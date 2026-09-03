// CommandBlockTests.cpp — semantic command blocks over OSC 133.
//
// Two halves. The first drives the real VtParser with real escape sequences
// and asserts the marks that come out, because that is where malformed and
// missing marks actually arrive. The second tests the block model itself —
// summaries, trimming, the notification policy — which is pure and needs no
// terminal.
//
// The rule these are all defending: a block is metadata REFERENCING grid
// rows. Nothing here stores output text, and nothing a block does may change
// what the grid says.
#include <catch2/catch_test_macros.hpp>

#include "../src/sessions/CommandBlocks.h"
#include "../src/term/grid.h"
#include "../src/term/vtparser.h"

#include <string>
#include <vector>

using namespace amber;

namespace
{

struct Mark
{
    char kind;
    int code;
    bool hasCode;
};

// Feeds a string through a real parser and collects the marks it emits.
std::vector<Mark> MarksFrom(const std::string& stream)
{
    Grid g;
    g.Init(40, 8);
    VtParser p(g);
    std::vector<Mark> out;
    p.SetMarkSink([&out](char k, int c, bool h) { out.push_back({ k, c, h }); });
    p.Feed(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());
    return out;
}

CommandBlock Done(const char* cmd, int lines, double secs, int exitCode)
{
    CommandBlock b;
    b.id = 1;
    b.command = cmd;
    b.lines = lines;
    b.hasOutput = lines > 0;
    b.outputFirst = 10;
    b.outputLast = 10 + static_cast<uint64_t>(lines > 0 ? lines - 1 : 0);
    b.promptRow = 9;
    b.inputRow = 9;
    b.durationSec = secs;
    b.exitCode = exitCode;
    b.hasExit = true;
    return b;
}

std::string Utf8(const std::u32string& s)
{
    std::string out;
    for (char32_t cp : s)
    {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace

// ------------------------------------------------------------- mark parsing
TEST_CASE("a well-formed A/B/C/D sequence produces four marks", "[blocks][osc133]")
{
    const auto m = MarksFrom("\x1b]133;A\x07"
                             "user@host:~$ \x1b]133;B\x07"
                             "make -j8\r\n\x1b]133;C\x07"
                             "building...\r\n"
                             "\x1b]133;D;0\x07");
    REQUIRE(m.size() == 4);
    CHECK(m[0].kind == 'A');
    CHECK(m[1].kind == 'B');
    CHECK(m[2].kind == 'C');
    CHECK(m[3].kind == 'D');
    CHECK(m[3].code == 0);
    CHECK(m[3].hasCode);
}

TEST_CASE("a bare 133;D reports no status rather than a fabricated success",
          "[blocks][osc133]")
{
    // This is the difference between "the command ended" and "the command
    // succeeded". Reporting 0 here would put a made-up result into the block,
    // the journal, the exit flash and the taskbar.
    const auto m = MarksFrom("\x1b]133;D\x07");
    REQUIRE(m.size() == 1);
    CHECK(m[0].kind == 'D');
    CHECK_FALSE(m[0].hasCode);

    SECTION("an explicit zero IS a status")
    {
        const auto z = MarksFrom("\x1b]133;D;0\x07");
        REQUIRE(z.size() == 1);
        CHECK(z[0].hasCode);
        CHECK(z[0].code == 0);
    }
    SECTION("a non-zero status survives, including a signal-shaped one")
    {
        for (const char* s : { "\x1b]133;D;1\x07", "\x1b]133;D;130\x07",
                               "\x1b]133;D;255\x07" })
        {
            const auto v = MarksFrom(s);
            REQUIRE(v.size() == 1);
            CHECK(v[0].hasCode);
            CHECK(v[0].code > 0);
        }
    }
}

TEST_CASE("malformed marks never become a status", "[blocks][osc133]")
{
    SECTION("a non-numeric status is not a status")
    {
        for (const char* s : { "\x1b]133;D;\x07", "\x1b]133;D;abc\x07",
                               "\x1b]133;D;;\x07", "\x1b]133;D;\x07" })
        {
            const auto v = MarksFrom(s);
            REQUIRE(v.size() == 1);
            CHECK(v[0].kind == 'D');
            CHECK_FALSE(v[0].hasCode);
        }
    }
    SECTION("an unknown mark letter is still delivered, for the app to ignore")
    {
        const auto v = MarksFrom("\x1b]133;X\x07\x1b]133;P;k=i\x07");
        REQUIRE(v.size() == 2);
        CHECK(v[0].kind == 'X');
        CHECK(v[1].kind == 'P');
        CHECK_FALSE(v[0].hasCode);
    }
    SECTION("a truncated introducer emits nothing")
    {
        CHECK(MarksFrom("\x1b]133\x07").empty());
        CHECK(MarksFrom("\x1b]133;\x07").empty());
        CHECK(MarksFrom("\x1b]13;A\x07").empty());
    }
    SECTION("ST-terminated marks work as well as BEL-terminated ones")
    {
        const auto v = MarksFrom("\x1b]133;A\x1b\\\x1b]133;D;3\x1b\\");
        REQUIRE(v.size() == 2);
        CHECK(v[0].kind == 'A');
        CHECK(v[1].kind == 'D');
        CHECK(v[1].code == 3);
        CHECK(v[1].hasCode);
    }
}

TEST_CASE("missing marks still yield a usable sequence", "[blocks][osc133]")
{
    SECTION("C and D with no A and no B")
    {
        const auto m = MarksFrom("\x1b]133;C\x07out\r\n\x1b]133;D;0\x07");
        REQUIRE(m.size() == 2);
        CHECK(m[0].kind == 'C');
        CHECK(m[1].kind == 'D');
    }
    SECTION("A with no D before the next A — a shell that died mid-command")
    {
        const auto m = MarksFrom("\x1b]133;A\x07\x1b]133;C\x07\x1b]133;A\x07");
        REQUIRE(m.size() == 3);
        CHECK(m[2].kind == 'A');
    }
    SECTION("a D with no C at all")
    {
        const auto m = MarksFrom("\x1b]133;D;1\x07");
        REQUIRE(m.size() == 1);
        CHECK(m[0].kind == 'D');
    }
}

TEST_CASE("marks do not disturb the grid they arrive in", "[blocks][osc133]")
{
    // The core rule: a block is metadata. The escape sequences that create
    // one must leave no trace in the terminal text.
    Grid a, b;
    a.Init(20, 4);
    b.Init(20, 4);
    VtParser pa(a), pb(b);
    pa.SetMarkSink([](char, int, bool) {});
    const std::string plain = "hello world";
    const std::string marked = "\x1b]133;A\x07hel\x1b]133;B\x07lo \x1b]133;C\x07world"
                               "\x1b]133;D;0\x07";
    pa.Feed(reinterpret_cast<const uint8_t*>(marked.data()), marked.size());
    pb.Feed(reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
    for (int c = 0; c < 20; ++c)
        CHECK(a.AbsCell(a.ScrollbackSize(), c).cp ==
              b.AbsCell(b.ScrollbackSize(), c).cp);
    CHECK(a.CurX() == b.CurX());
    CHECK(a.CurY() == b.CurY());
}

// ------------------------------------------------------------------ format
TEST_CASE("durations and sizes are formatted the way the summary reads them",
          "[blocks]")
{
    CHECK(FormatDuration(0.0) == "0ms");
    CHECK(FormatDuration(0.25) == "250ms");
    CHECK(FormatDuration(4.2) == "4.2s");
    CHECK(FormatDuration(64.0) == "1m04s");
    CHECK(FormatDuration(7325.0) == "2h02m");
    CHECK(FormatDuration(-5.0) == "0ms");        // never a negative time

    CHECK(FormatBytes(0) == "0 B");
    CHECK(FormatBytes(812) == "812 B");
    CHECK(FormatBytes(4200) == "4.1 KB");
    CHECK(FormatBytes(5ull * 1024 * 1024) == "5.0 MB");
    CHECK(FormatBytes(3ull * 1024 * 1024 * 1024) == "3.0 GB");
}

TEST_CASE("an outcome is never invented", "[blocks]")
{
    CommandBlock b = Done("ls", 3, 0.1, 0);
    CHECK(FormatOutcome(b) == "ok");
    CHECK(b.Succeeded());
    CHECK_FALSE(b.Unknown());

    b.exitCode = 2;
    CHECK(FormatOutcome(b) == "exit 2");
    CHECK(b.Failed());

    SECTION("no reported status is its own outcome, not a success")
    {
        b.hasExit = false;
        CHECK(FormatOutcome(b) == "no exit status");
        CHECK(b.Unknown());
        CHECK_FALSE(b.Succeeded());
        CHECK_FALSE(b.Failed());
    }
    SECTION("interrupted is its own outcome too")
    {
        b.hasExit = true;
        b.exitCode = 0;
        b.interrupted = true;
        CHECK(FormatOutcome(b) == "interrupted");
        CHECK(b.Unknown());
        CHECK_FALSE(b.Succeeded());
    }
    SECTION("running is not an outcome at all")
    {
        b.running = true;
        CHECK(FormatOutcome(b) == "running");
    }
}

TEST_CASE("a folded summary reports only what was measured", "[blocks][fold]")
{
    CommandBlock b = Done("make -j8", 1284, 92.5, 0);
    b.bytes = 48213;
    b.cwd = "/srv/app";
    const std::string s = Utf8(BuildSummary(b, false, ""));
    CHECK(s.find("make -j8") != std::string::npos);
    CHECK(s.find("1284 lines") != std::string::npos);
    CHECK(s.find("47.1 KB") != std::string::npos);
    CHECK(s.find("1m32s") != std::string::npos);
    CHECK(s.find("ok") != std::string::npos);
    CHECK(s.find("/srv/app") == std::string::npos);   // not asked for

    SECTION("the directory appears only when asked for")
    {
        CHECK(Utf8(BuildSummary(b, true, "")).find("/srv/app") != std::string::npos);
    }
    SECTION("an uncounted size is omitted, not printed as zero")
    {
        b.bytes = 0;
        CHECK(Utf8(BuildSummary(b, false, "")).find(" B]") == std::string::npos);
        CHECK(Utf8(BuildSummary(b, false, "")).find("0 B") == std::string::npos);
    }
    SECTION("one line is not \"1 lines\"")
    {
        b.lines = 1;
        CHECK(Utf8(BuildSummary(b, false, "")).find("1 line,") != std::string::npos);
    }
    SECTION("a first line is included when offered")
    {
        const std::string with = Utf8(BuildSummary(b, false, "error: no rule to make"));
        CHECK(with.find("error: no rule to make") != std::string::npos);
    }
    SECTION("a bookmarked block is flagged in its own summary")
    {
        b.bookmarked = true;
        CHECK(Utf8(BuildSummary(b, false, "")).find("\xE2\x9A\x91") != std::string::npos);
    }
    SECTION("a failure says which code, never just \"failed\"")
    {
        b.exitCode = 137;
        CHECK(Utf8(BuildSummary(b, false, "")).find("exit 137") != std::string::npos);
    }
}

TEST_CASE("a very long command cannot run the summary away", "[blocks][fold]")
{
    CommandBlock b = Done(std::string(4000, 'x').c_str(), 10, 1.0, 0);
    const std::u32string s = BuildSummary(b, true, std::string(4000, 'y'));
    // Bounded: the summary occupies one row and must not be built to
    // thousands of code points just because the command was.
    CHECK(s.size() < 300);
    CHECK(Utf8(s).find("\xE2\x80\xA6") != std::string::npos);   // marked as cut
}

TEST_CASE("a command whose text is not valid UTF-8 still yields a summary",
          "[blocks][fold]")
{
    CommandBlock b;
    b.command = "echo \xFF\xFE bad";
    b.lines = 1;
    b.hasOutput = true;
    b.hasExit = true;
    const std::u32string s = BuildSummary(b, false, "");
    CHECK_FALSE(s.empty());
    CHECK(Utf8(s).find("echo") != std::string::npos);
}

// ------------------------------------------------------------------- query
TEST_CASE("a row resolves to the block that owns it", "[blocks]")
{
    std::vector<CommandBlock> v;
    {
        CommandBlock a;
        a.id = 1; a.promptRow = 10; a.inputRow = 10;
        a.outputFirst = 11; a.outputLast = 20; a.hasOutput = true;
        v.push_back(a);
        CommandBlock b;
        b.id = 2; b.promptRow = 21; b.inputRow = 21;
        b.outputFirst = 22; b.outputLast = 22; b.hasOutput = true;
        v.push_back(b);
        CommandBlock c;      // ran, printed nothing
        c.id = 3; c.promptRow = 23; c.inputRow = 23;
        v.push_back(c);
    }
    CHECK(BlockIndexAtRow(v, 9) == -1);      // before the first prompt
    CHECK(BlockIndexAtRow(v, 10) == 0);      // the prompt row belongs to it
    CHECK(BlockIndexAtRow(v, 15) == 0);
    CHECK(BlockIndexAtRow(v, 20) == 0);
    CHECK(BlockIndexAtRow(v, 21) == 1);
    CHECK(BlockIndexAtRow(v, 23) == 2);      // a block with no output is one row
    CHECK(BlockIndexAtRow(v, 24) == -1);

    CHECK(BlockAtRow(v, 15)->id == 1);
    CHECK(BlockAtRow(v, 99) == nullptr);
    CHECK(BlockById(v, 2)->promptRow == 21);
    CHECK(BlockById(v, 77) == nullptr);
    CHECK(BlockById(v, 0) == nullptr);       // 0 is "no block", never a match
}

// ------------------------------------------------------------------- trim
TEST_CASE("blocks are dropped when the rows they name are trimmed", "[blocks][trim]")
{
    std::vector<CommandBlock> v;
    for (int i = 0; i < 5; ++i)
    {
        CommandBlock b;
        b.id = static_cast<uint64_t>(i + 1);
        b.promptRow = static_cast<uint64_t>(i * 10);
        b.inputRow = b.promptRow;
        b.outputFirst = b.promptRow + 1;
        b.outputLast = b.promptRow + 8;
        b.hasOutput = true;
        v.push_back(b);
    }
    CHECK(TrimBlocks(v, 0) == 0);
    CHECK(v.size() == 5);
    // Scrollback now starts at row 20: the first two blocks are gone.
    CHECK(TrimBlocks(v, 20) == 2);
    REQUIRE(v.size() == 3);
    CHECK(v.front().id == 3);

    SECTION("a block still partly addressable survives")
    {
        // Block 3 covers 20..28. With the oldest row at 25 it is half gone,
        // but the half that remains is still worth pointing at.
        CHECK(TrimBlocks(v, 25) == 0);
        CHECK(v.front().id == 3);
    }
    SECTION("a bookmark does not keep a trimmed block alive")
    {
        // Deliberate: a bookmark that outlived its rows would be a durable
        // reference to text that no longer exists.
        for (CommandBlock& b : v)
            b.bookmarked = true;
        CHECK(TrimBlocks(v, 1000) == 3);
        CHECK(v.empty());
    }
    SECTION("a block with no output is trimmed on its prompt row")
    {
        std::vector<CommandBlock> one;
        CommandBlock b;
        b.id = 1; b.promptRow = 4; b.inputRow = 4;
        one.push_back(b);
        CHECK(TrimBlocks(one, 4) == 0);
        CHECK(TrimBlocks(one, 5) == 1);
    }
}

// ------------------------------------------------- notification policy
TEST_CASE("a fast command never notifies", "[blocks][notify]")
{
    CommandBlock b = Done("ls", 2, 0.4, 0);
    for (NotifyOn m : { NotifyOn::Off, NotifyOn::Success, NotifyOn::Failure,
                        NotifyOn::Both })
        CHECK_FALSE(ShouldNotifyCompletion(m, 30, b));
}

TEST_CASE("the notification policy respects the mode", "[blocks][notify]")
{
    CommandBlock ok = Done("make", 100, 120.0, 0);
    CommandBlock bad = Done("make", 100, 120.0, 1);

    CHECK_FALSE(ShouldNotifyCompletion(NotifyOn::Off, 30, ok));
    CHECK_FALSE(ShouldNotifyCompletion(NotifyOn::Off, 30, bad));

    CHECK(ShouldNotifyCompletion(NotifyOn::Success, 30, ok));
    CHECK_FALSE(ShouldNotifyCompletion(NotifyOn::Success, 30, bad));

    CHECK_FALSE(ShouldNotifyCompletion(NotifyOn::Failure, 30, ok));
    CHECK(ShouldNotifyCompletion(NotifyOn::Failure, 30, bad));

    CHECK(ShouldNotifyCompletion(NotifyOn::Both, 30, ok));
    CHECK(ShouldNotifyCompletion(NotifyOn::Both, 30, bad));

    SECTION("a threshold of zero notifies about everything that finished")
    {
        CommandBlock quick = Done("ls", 1, 0.01, 0);
        CHECK(ShouldNotifyCompletion(NotifyOn::Both, 0, quick));
    }
    SECTION("exactly at the threshold counts")
    {
        CommandBlock edge = Done("sleep 30", 0, 30.0, 0);
        CHECK(ShouldNotifyCompletion(NotifyOn::Success, 30, edge));
    }
    SECTION("a running command is never reported as finished")
    {
        CommandBlock run = Done("tail -f", 0, 900.0, 0);
        run.running = true;
        CHECK_FALSE(ShouldNotifyCompletion(NotifyOn::Both, 30, run));
    }
}

TEST_CASE("an unknown outcome counts as not-succeeded, and says so",
          "[blocks][notify]")
{
    CommandBlock lost = Done("make", 40, 300.0, 0);
    lost.interrupted = true;
    // It did not succeed, so "failure only" is the mode that wants to hear.
    CHECK_FALSE(ShouldNotifyCompletion(NotifyOn::Success, 30, lost));
    CHECK(ShouldNotifyCompletion(NotifyOn::Failure, 30, lost));
    CHECK(ShouldNotifyCompletion(NotifyOn::Both, 30, lost));
    // And the text never claims to know how it ended.
    const std::string t = CompletionText(lost);
    CHECK(t.find("interrupted") != std::string::npos);
    CHECK(t.find("does not know") != std::string::npos);
    CHECK(t.find("exit") == std::string::npos);

    SECTION("no reported status is the same kind of unknown")
    {
        CommandBlock quiet = Done("deploy", 5, 200.0, 0);
        quiet.hasExit = false;
        CHECK_FALSE(ShouldNotifyCompletion(NotifyOn::Success, 30, quiet));
        CHECK(ShouldNotifyCompletion(NotifyOn::Failure, 30, quiet));
        CHECK(CompletionText(quiet).find("no exit status") != std::string::npos);
    }
}

TEST_CASE("completion text names the command, the time and the real status",
          "[blocks][notify]")
{
    CommandBlock ok = Done("cargo build --release", 900, 305.0, 0);
    const std::string t = CompletionText(ok);
    CHECK(t.find("cargo build --release") != std::string::npos);
    CHECK(t.find("5m05s") != std::string::npos);

    CommandBlock bad = Done("cargo test", 12, 61.0, 101);
    const std::string f = CompletionText(bad);
    CHECK(f.find("exit 101") != std::string::npos);
    CHECK(f.find("failed") != std::string::npos);

    SECTION("a command with no text still reads as a sentence")
    {
        CommandBlock anon = Done("", 1, 99.0, 0);
        CHECK(CompletionText(anon).find("a command") == 0);
    }
    SECTION("a very long command is clipped, not passed through whole")
    {
        CommandBlock big = Done(std::string(500, 'z').c_str(), 1, 99.0, 0);
        CHECK(CompletionText(big).size() < 200);
    }
}

TEST_CASE("notify mode names are all distinct", "[blocks][notify]")
{
    CHECK(std::string(NotifyOnName(NotifyOn::Off)) == "off");
    CHECK(std::string(NotifyOnName(NotifyOn::Success)) != NotifyOnName(NotifyOn::Failure));
    CHECK(std::string(NotifyOnName(NotifyOn::Both)) != NotifyOnName(NotifyOn::Success));
}

// ------------------------------------------------------- large-output shape
TEST_CASE("a block over a very large output stays a fixed-size object",
          "[blocks][perf]")
{
    // The point of the model: a block references rows, so a million lines of
    // output costs the same as one. Nothing here grows with the output.
    CommandBlock tiny = Done("echo hi", 1, 0.01, 0);
    CommandBlock huge = Done("yes | head -10000000", 10000000, 60.0, 0);
    huge.bytes = 20ull * 1000 * 1000;
    huge.summary = BuildSummary(huge, false, "");
    tiny.summary = BuildSummary(tiny, false, "");
    // Both summaries fit a row; the block itself has no output storage.
    CHECK(huge.summary.size() < 300);
    CHECK(Utf8(huge.summary).find("10000000 lines") != std::string::npos);
    CHECK(huge.outputLast - huge.outputFirst + 1 == 10000000);
}
