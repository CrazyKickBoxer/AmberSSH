// CompatFixtureTests.cpp — Stage 4's compatibility regression harness.
//
// Each fixture is a captured byte stream of the kind a real program emits,
// fed through the real VtParser, with the assertion made against the
// CANONICAL GRID STATE and its metadata rather than against a screenshot.
// That is the point: a rendering change must be free to alter every pixel,
// and none of these may notice; a change to the text model must break them.
//
// Where a stream comes from a specific program the comment says so, because
// the value of a fixture is that it is what that program actually sends.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "../src/term/Graphemes.h"
#include "../src/term/grid.h"
#include "../src/term/vtparser.h"

#include <string>
#include <vector>

using namespace amber;

namespace
{

// A parser, its grid, and everything the sinks saw — the whole observable
// result of feeding a stream.
struct Harness
{
    Grid grid;
    VtParser parser{ grid };

    std::vector<std::string> titles;
    std::vector<std::string> cwds;
    std::vector<std::string> clipboard;
    std::vector<std::string> replies;
    struct Mark { char kind; int code; bool hasCode; };
    std::vector<Mark> marks;
    int images = 0;
    int imageCols = 0, imageRows = 0;

    explicit Harness(int cols = 40, int rows = 10)
    {
        // The image decoders go through WIC, which needs an apartment. The
        // app initialises COM on its UI thread; a test binary has to say so
        // itself or every PNG decode fails for a reason that has nothing to
        // do with the protocol under test.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        grid.Init(cols, rows);
        parser.SetTitleSink([this](const std::string& t) { titles.push_back(t); });
        parser.SetCwdSink([this](const std::string& d) { cwds.push_back(d); });
        parser.SetClipboardSink([this](const std::string& b) { clipboard.push_back(b); });
        parser.SetMarkSink([this](char k, int c, bool h) { marks.push_back({ k, c, h }); });
        parser.SetWriter([this](const char* d, size_t n) { replies.emplace_back(d, n); });
        parser.SetImageSink([this](DecodedImage&& img, int c, int r) {
            ++images;
            imageCols = c;
            imageRows = r;
            return (img.h > 0 && r <= 0) ? 1 : r;
        });
    }

    void Feed(const std::string& s)
    {
        parser.Feed(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    // Feeds in fragments, so a fixture also proves the stream survives being
    // split at every byte boundary — which is what a socket does.
    void FeedSplit(const std::string& s, size_t chunk)
    {
        for (size_t i = 0; i < s.size(); i += chunk)
            Feed(s.substr(i, chunk));
    }
    std::string Line(int row) const
    {
        return grid.GetText(row, 0, row, grid.Cols() - 1);
    }
    std::string Screen() const
    {
        return grid.GetText(0, 0, grid.Rows() - 1, grid.Cols() - 1);
    }
    // GetText joins rows with CRLF, so a blank screen is never an empty
    // string. "Nothing was printed" is the question these fixtures actually
    // ask, and this is what it means.
    bool Blank() const
    {
        for (char ch : Screen())
            if (ch != '\r' && ch != '\n' && ch != ' ')
                return false;
        return true;
    }
};

const std::string ESC = "\x1b";
const std::string BEL = "\x07";
const std::string ST = "\x1b\\";

} // namespace

// ------------------------------------------------------------------ unicode
TEST_CASE("a UTF-8 sequence split across reads decodes as one character",
          "[compat][unicode]")
{
    // The socket boundary case: 16 KB reads land mid-sequence constantly.
    for (size_t chunk = 1; chunk <= 4; ++chunk)
    {
        Harness h;
        h.FeedSplit("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z", chunk);
        const std::string line = h.Line(0);
        INFO("chunk size " << chunk);
        CHECK(line == "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z");
        CHECK(h.parser.Utf8Errors() == 0);
    }
}

TEST_CASE("combining marks attach to their base instead of being dropped",
          "[compat][unicode]")
{
    // This is the Stage 4 correctness fix. Before it, every one of these
    // pasted back as the bare base letter.
    SECTION("a single acute accent")
    {
        Harness h;
        h.Feed("cafe\xCC\x81");          // e + U+0301
        CHECK(h.Line(0) == "cafe\xCC\x81");
        CHECK(h.grid.CurX() == 4);       // still four columns wide
    }
    SECTION("several marks stack on one base")
    {
        Harness h;
        h.Feed("a\xCC\x81\xCC\x88\xCC\xA7");
        CHECK(h.Line(0) == "a\xCC\x81\xCC\x88\xCC\xA7");
        CHECK(h.grid.CurX() == 1);
    }
    SECTION("a mark arriving split across reads still attaches")
    {
        Harness h;
        h.FeedSplit("e\xCC\x81", 1);
        CHECK(h.Line(0) == "e\xCC\x81");
    }
    SECTION("a mark with nothing before it is discarded, not crashed on")
    {
        Harness h;
        h.Feed("\xCC\x81");
        CHECK(h.grid.CurX() == 0);
        CHECK(h.Line(0).empty());
    }
    SECTION("the cell count still matches what wcwidth would say")
    {
        // The hard constraint: the server positions the cursor with wcwidth,
        // which gives a combining mark zero columns. A cluster must not move
        // the cursor, or every redraw after it desyncs.
        Harness a, b;
        a.Feed("abc\xCC\x81\xCC\x88""def");
        b.Feed("abcdef");
        CHECK(a.grid.CurX() == b.grid.CurX());
    }
}

TEST_CASE("a regional indicator pair becomes one flag in two cells",
          "[compat][unicode]")
{
    Harness h;
    h.Feed("\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7");   // U+1F1EC U+1F1E7 = GB
    CHECK(h.Line(0) == "\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7");
    // Two columns: exactly what two width-1 regional indicators occupy, so
    // joining them changed the rendering and not the layout.
    CHECK(h.grid.CurX() == 2);
    CHECK((h.grid.ViewCell(0, 0).flags & CellWideLead) != 0);
    CHECK((h.grid.ViewCell(0, 1).flags & CellWideTail) != 0);

    SECTION("a lone regional indicator stays one narrow cell")
    {
        Harness one;
        one.Feed("\xF0\x9F\x87\xAC");
        CHECK(one.grid.CurX() == 1);
        CHECK_FALSE((one.grid.ViewCell(0, 0).flags & CellWideLead) != 0);
    }
    SECTION("three in a row pair up and leave the third alone")
    {
        Harness three;
        three.Feed("\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7\xF0\x9F\x87\xAB");
        CHECK(three.grid.CurX() == 3);
        CHECK(three.Line(0) == "\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7\xF0\x9F\x87\xAB");
    }
}

TEST_CASE("malformed UTF-8 cannot put an invalid scalar in a cell",
          "[compat][unicode][security]")
{
    SECTION("an overlong encoding is refused")
    {
        // C0 80 is the classic overlong NUL, used to sneak a byte past a
        // filter. It decoded to U+0000 and reached the grid before Stage 4.
        Harness h;
        h.Feed("a\xC0\x80z");
        CHECK(h.parser.Utf8Errors() > 0);
        const std::string line = h.Line(0);
        CHECK(line.find('\0') == std::string::npos);
        CHECK(line.front() == 'a');
        CHECK(line.back() == 'z');
    }
    SECTION("an overlong three-byte form is refused")
    {
        Harness h;
        h.Feed("\xE0\x80\xAF");          // overlong '/'
        CHECK(h.parser.Utf8Errors() > 0);
        CHECK(h.Line(0).find('/') == std::string::npos);
    }
    SECTION("a UTF-16 surrogate half is refused")
    {
        Harness h;
        h.Feed("\xED\xA0\x80");          // U+D800, not a scalar value
        CHECK(h.parser.Utf8Errors() > 0);
        for (int c = 0; c < h.grid.Cols(); ++c)
        {
            const char32_t cp = h.grid.ViewCell(0, c).cp;
            CHECK_FALSE((cp >= 0xD800 && cp <= 0xDFFF));
        }
    }
    SECTION("a truncated sequence resynchronises on the next character")
    {
        Harness h;
        h.Feed("a\xE2\x82z");            // two bytes of a three-byte sequence
        CHECK(h.parser.Utf8Errors() > 0);
        const std::string line = h.Line(0);
        CHECK(line.front() == 'a');
        CHECK(line.back() == 'z');       // the 'z' survived
    }
    SECTION("a stray continuation byte is one replacement, not a lost stream")
    {
        Harness h;
        h.Feed("a\x80\x80z");
        CHECK(h.parser.Utf8Errors() >= 2);
        CHECK(h.Line(0).back() == 'z');
    }
    SECTION("a wall of garbage neither hangs nor corrupts the grid")
    {
        Harness h;
        std::string junk;
        for (int i = 0; i < 5000; ++i)
            junk += static_cast<char>(0x80 + (i % 0x7F));
        h.Feed(junk);
        h.Feed("ok");
        CHECK(h.grid.Rows() == 10);
        CHECK(h.Line(h.grid.CurY()).find("ok") != std::string::npos);
    }
}

TEST_CASE("wide characters survive cursor movement and overwriting",
          "[compat][unicode]")
{
    SECTION("a CJK character occupies two cells")
    {
        Harness h;
        h.Feed("\xE6\x97\xA5\xE6\x9C\xAC");    // 日本
        CHECK(h.grid.CurX() == 4);
        CHECK((h.grid.ViewCell(0, 0).flags & CellWideLead) != 0);
        CHECK((h.grid.ViewCell(0, 1).flags & CellWideTail) != 0);
        CHECK(h.Line(0) == "\xE6\x97\xA5\xE6\x9C\xAC");
    }
    SECTION("overwriting the lead blanks the tail")
    {
        Harness h;
        h.Feed("\xE6\x97\xA5");
        h.Feed(ESC + "[H" + "x");         // home, then a narrow character
        CHECK(h.Line(0) == "x");
        CHECK_FALSE((h.grid.ViewCell(0, 1).flags & CellWideTail) != 0);
    }
    SECTION("overwriting the tail blanks the lead")
    {
        Harness h;
        h.Feed("\xE6\x97\xA5");
        h.Feed(ESC + "[1;2H" + "x");
        const std::string line = h.Line(0);
        CHECK(line.find("\xE6\x97\xA5") == std::string::npos);
        CHECK(line.find('x') != std::string::npos);
    }
    SECTION("a wide character at the last column wraps rather than splitting")
    {
        Harness h(5, 4);
        h.Feed("abcd\xE6\x97\xA5");
        CHECK(h.grid.CurY() == 1);
        CHECK(h.Line(1) == "\xE6\x97\xA5");
    }
    SECTION("erasing a line leaves no orphaned half")
    {
        Harness h;
        h.Feed("ab\xE6\x97\xA5" "cd");
        h.Feed(ESC + "[1;3H" + ESC + "[K");   // erase from inside the wide char
        for (int c = 0; c < h.grid.Cols(); ++c)
        {
            const Cell& cell = h.grid.ViewCell(0, c);
            const bool lead = (cell.flags & CellWideLead) != 0;
            const bool tail = (cell.flags & CellWideTail) != 0;
            if (lead)
                CHECK((h.grid.ViewCell(0, c + 1).flags & CellWideTail) != 0);
            if (tail)
                CHECK((h.grid.ViewCell(0, c - 1).flags & CellWideLead) != 0);
        }
    }
}

TEST_CASE("a variation selector changes presentation, not layout",
          "[compat][unicode]")
{
    Harness h;
    h.Feed("\xE2\x9D\xA4\xEF\xB8\x8F");   // U+2764 U+FE0F
    CHECK((h.grid.ViewCell(0, 0).flags & CellEmojiVS) != 0);
    // Re-emitted on copy so the colour form survives a paste elsewhere.
    CHECK(h.Line(0) == "\xE2\x9D\xA4\xEF\xB8\x8F");
}

// -------------------------------------------------------------------- OSC 7
TEST_CASE("OSC 7 reports the working directory", "[compat][osc7]")
{
    Harness h;
    h.Feed(ESC + "]7;file://host/srv/app" + BEL);
    REQUIRE(h.cwds.size() == 1);
    CHECK(h.cwds[0] == "/srv/app");

    SECTION("ST terminates it as well as BEL")
    {
        Harness s;
        s.Feed(ESC + "]7;file://host/tmp" + ST);
        REQUIRE(s.cwds.size() == 1);
        CHECK(s.cwds[0] == "/tmp");
    }
}

// -------------------------------------------------------------------- OSC 8
TEST_CASE("OSC 8 keeps the target separate from the visible text",
          "[compat][osc8]")
{
    Harness h;
    h.Feed(ESC + "]8;;https://example.com/page" + BEL + "click here" +
           ESC + "]8;;" + BEL + " plain");
    // The visible text is only the label — the URI is nowhere in the grid.
    CHECK(h.Line(0) == "click here plain");
    const uint16_t id = h.grid.ViewCell(0, 0).link;
    REQUIRE(id != 0);
    CHECK(h.parser.LinkUri(id) == "https://example.com/page");
    // The link ends where the empty OSC 8 said it did.
    CHECK(h.grid.ViewCell(0, 9).link == id);
    CHECK(h.grid.ViewCell(0, 11).link == 0);
}

TEST_CASE("an OSC 8 target carrying control characters is refused entirely",
          "[compat][osc8][security]")
{
    // Stripping would be worse than refusing: a stripped URI is a DIFFERENT
    // target from the one the server named, and it would be the one opened.
    //
    // DEL is the byte that actually reaches the buffer. Everything below 0x20
    // is intercepted by the C0 handler before the string state ever sees it,
    // and ESC terminates the string outright — so those cannot enter a URI by
    // construction. The check exists for DEL and as an invariant that holds
    // whatever the parser does upstream.
    SECTION("a DEL in the target rejects the link")
    {
        Harness h;
        h.Feed(ESC + "]8;;https://example.com/a\x7f" "b" + BEL + "x");
        CHECK(h.grid.ViewCell(0, 0).link == 0);
        CHECK(h.parser.LinkRejects() > 0);
    }
    SECTION("an embedded ESC ends the string; it does not extend the URI")
    {
        // The payload after the ESC becomes an OSC of its own, and the link
        // that WAS stored stops at the escape.
        Harness h;
        h.Feed(ESC + "]8;;https://example.com/" + ESC + "]0;title" + BEL + "x");
        const uint16_t id = h.grid.ViewCell(0, 0).link;
        if (id)
            CHECK(h.parser.LinkUri(id) == "https://example.com/");
    }
    SECTION("whatever is stored never contains a control character")
    {
        // The invariant, stated over the awkward inputs together.
        for (const char* bad : { "https://example.com/a\rb",
                                 "https://example.com/a\nb",
                                 "https://example.com/a\x7f" "b",
                                 "https://example.com/a\x01" "b" })
        {
            Harness h;
            h.Feed(ESC + "]8;;" + bad + BEL + "x");
            INFO(bad);
            const uint16_t id = h.grid.ViewCell(0, 0).link;
            if (!id)
                continue;                 // refused outright: also fine
            for (unsigned char ch : h.parser.LinkUri(id))
                CHECK(((ch >= 0x20 && ch != 0x7F)));
        }
    }
}

TEST_CASE("an oversized OSC 8 target is dropped", "[compat][osc8][security]")
{
    Harness h;
    h.Feed(ESC + "]8;;https://example.com/" + std::string(4000, 'a') + BEL + "x");
    CHECK(h.grid.ViewCell(0, 0).link == 0);
}

// ------------------------------------------------------------------ OSC 133
TEST_CASE("OSC 133 marks arrive without disturbing the grid", "[compat][osc133]")
{
    Harness a, b;
    a.Feed(ESC + "]133;A" + BEL + "$ " + ESC + "]133;B" + BEL + "ls" + "\r\n" +
           ESC + "]133;C" + BEL + "file.txt\r\n" + ESC + "]133;D;0" + BEL);
    b.Feed("$ ls\r\nfile.txt\r\n");
    CHECK(a.Screen() == b.Screen());
    REQUIRE(a.marks.size() == 4);
    CHECK(a.marks[3].hasCode);
    CHECK(a.marks[3].code == 0);
}

// ------------------------------------------------------------- alt screen
TEST_CASE("the alternate screen is entered, kept separate, and restored",
          "[compat][altscreen]")
{
    // The ?1049 sequence vim, less, htop and tmux all use.
    Harness h;
    h.Feed("main screen text\r\n");
    h.Feed(ESC + "[?1049h");
    h.Feed(ESC + "[2J" + ESC + "[H" + "alternate content");
    CHECK(h.Line(0) == "alternate content");
    CHECK(h.Screen().find("main screen text") == std::string::npos);
    h.Feed(ESC + "[?1049l");
    CHECK(h.Screen().find("main screen text") != std::string::npos);
    CHECK(h.Screen().find("alternate content") == std::string::npos);
}

TEST_CASE("the alternate screen contributes nothing to scrollback",
          "[compat][altscreen]")
{
    Harness h(20, 4);
    h.Feed("a\r\nb\r\nc\r\n");
    const uint64_t before = h.grid.TotalPushed();
    h.Feed(ESC + "[?1049h");
    for (int i = 0; i < 20; ++i)
        h.Feed("alt line\r\n");
    CHECK(h.grid.TotalPushed() == before);
    h.Feed(ESC + "[?1049l");
}

// --------------------------------------------------------------- less / vim
TEST_CASE("a less session leaves the screen it found", "[compat][less]")
{
    // What `less` actually emits: smcup, paint, a status line in reverse
    // video, then rmcup on quit.
    Harness h(20, 5);
    h.Feed("prompt$ less file\r\n");
    h.Feed(ESC + "[?1049h" + ESC + "[H" + ESC + "[2J");
    h.Feed("line one\r\nline two\r\n");
    h.Feed(ESC + "[7m" + "(END)" + ESC + "[27m");
    CHECK(h.Screen().find("(END)") != std::string::npos);
    h.Feed(ESC + "[?1049l");
    CHECK(h.Screen().find("(END)") == std::string::npos);
    CHECK(h.Screen().find("prompt$ less file") != std::string::npos);
}

TEST_CASE("a vim-style full redraw addresses cells absolutely", "[compat][vim]")
{
    Harness h(20, 5);
    h.Feed(ESC + "[?1049h" + ESC + "[2J");
    // Absolute positioning, a tilde column, and a modeline — the shape of a
    // vim repaint.
    for (int r = 2; r <= 4; ++r)
        h.Feed(ESC + "[" + std::to_string(r) + ";1H~");
    h.Feed(ESC + "[1;1H" + "hello world");
    h.Feed(ESC + "[5;1H" + ESC + "[7m" + "\"f\" 1L, 12C" + ESC + "[0m");
    CHECK(h.Line(0) == "hello world");
    CHECK(h.Line(1) == "~");
    CHECK(h.Line(4) == "\"f\" 1L, 12C");
}

// ------------------------------------------------------------------- colour
TEST_CASE("git-style SGR colour lands on the right cells", "[compat][color]")
{
    Harness h;
    // What `git diff --color` sends for an added line.
    h.Feed(ESC + "[32m" + "+added" + ESC + "[m" + " " + ESC + "[31m" + "-gone" +
           ESC + "[m");
    CHECK(h.Line(0) == "+added -gone");
    CHECK(h.grid.ViewCell(0, 0).fg != h.grid.ViewCell(0, 7).fg);
    // The reset really reset: the space between them is default-coloured.
    CHECK(h.grid.ViewCell(0, 6).fg == amber::kColorDefault);
}

TEST_CASE("a truecolor gradient keeps every distinct value", "[compat][color]")
{
    Harness h(64, 4);
    std::string s;
    for (int i = 0; i < 32; ++i)
        s += ESC + "[38;2;" + std::to_string(i * 8) + ";" +
             std::to_string(255 - i * 8) + ";128m#";
    h.Feed(s);
    // 32 different foreground colours, none of them equal to its neighbour.
    for (int i = 1; i < 32; ++i)
        CHECK(h.grid.ViewCell(0, i).fg != h.grid.ViewCell(0, i - 1).fg);

    SECTION("the colon sub-parameter form parses the same way")
    {
        Harness c;
        c.Feed(ESC + "[38:2::10:20:30m" + "x");
        CHECK(c.grid.ViewCell(0, 0).fg != amber::kColorDefault);
    }
}

// ------------------------------------------------------------------- modes
TEST_CASE("bracketed paste is a mode, not text", "[compat][paste]")
{
    Harness h;
    CHECK_FALSE(h.parser.Modes().bracketedPaste);
    h.Feed(ESC + "[?2004h");
    CHECK(h.parser.Modes().bracketedPaste);
    CHECK(h.Line(0).empty());            // the sequence printed nothing
    h.Feed(ESC + "[?2004l");
    CHECK_FALSE(h.parser.Modes().bracketedPaste);
}

TEST_CASE("mouse tracking modes are recorded and left off by default",
          "[compat][mouse]")
{
    Harness h;
    CHECK(h.parser.Modes().mouseMode == 0);
    h.Feed(ESC + "[?1000h");
    CHECK(h.parser.Modes().mouseMode == 1000);
    h.Feed(ESC + "[?1002h");
    CHECK(h.parser.Modes().mouseMode == 1002);
    h.Feed(ESC + "[?1006h");
    CHECK(h.parser.Modes().mouseSgr);
    h.Feed(ESC + "[?1003h");
    CHECK(h.parser.Modes().mouseMode == 1003);
    h.Feed(ESC + "[?1003l");
    CHECK(h.parser.Modes().mouseMode == 0);
    CHECK(h.Blank());           // none of it printed
}

TEST_CASE("a tmux-style scroll region moves only its own rows",
          "[compat][tmux]")
{
    Harness h(10, 6);
    h.Feed("r0\r\nr1\r\nr2\r\nr3\r\nr4\r\nr5");
    h.Feed(ESC + "[2;5r");               // DECSTBM rows 2..5
    h.Feed(ESC + "[5;1H\n");             // line feed at the bottom of it
    CHECK(h.Line(0) == "r0");            // outside the region: untouched
    CHECK(h.Line(5) == "r5");
    CHECK(h.Line(1) == "r2");            // inside: scrolled up by one
}

// ------------------------------------------------------------------ graphics
TEST_CASE("an iTerm2 inline image is decoded and placed", "[compat][image]")
{
    // A 1x1 PNG, which is what the fixture needs to be: the assertion is
    // about the protocol, not the pixels.
    const std::string png1x1 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmM"
        "IQAAAABJRU5ErkJggg==";
    Harness h;
    h.Feed(ESC + "]1337;File=inline=1;width=4;height=2:" + png1x1 + BEL);
    CHECK(h.images == 1);
    CHECK(h.imageCols == 4);
    CHECK(h.imageRows == 2);

    SECTION("without inline=1 it is a file transfer, and is ignored")
    {
        Harness f;
        f.Feed(ESC + "]1337;File=name=Zg==;size=1:" + png1x1 + BEL);
        CHECK(f.images == 0);
    }
    SECTION("pixel and percentage sizes fall back to the natural size")
    {
        Harness p;
        p.Feed(ESC + "]1337;File=inline=1;width=100px;height=50%:" + png1x1 + BEL);
        CHECK(p.images == 1);
        CHECK(p.imageCols == 0);
        CHECK(p.imageRows == 0);
    }
    SECTION("other OSC 1337 commands are ignored rather than acted on")
    {
        Harness v;
        v.Feed(ESC + "]1337;SetUserVar=foo=YmFy" + BEL);
        v.Feed(ESC + "]1337;CurrentDir=/etc" + BEL);
        CHECK(v.images == 0);
        CHECK(v.cwds.empty());           // NOT taken as a directory report
        CHECK(v.Blank());
    }
    SECTION("a corrupt payload is counted, not crashed on")
    {
        Harness bad;
        bad.Feed(ESC + "]1337;File=inline=1:bm90IGEgcG5n" + BEL);
        CHECK(bad.images == 0);
        CHECK(bad.parser.ImageDecodeFails() > 0);
    }
}

TEST_CASE("a Kitty graphics payload is bounded and never printed",
          "[compat][image][security]")
{
    Harness h;
    // Raw RGBA with dimensions that would be 16 GB if believed.
    h.Feed(ESC + "_Gf=32,s=100000,v=100000,a=T;AAAA" + ST);
    CHECK(h.images == 0);
    CHECK(h.Blank());           // the payload did not leak as text

    SECTION("a plausible raw image is accepted")
    {
        Harness ok;
        // 2x2 RGBA = 16 bytes -> base64 of 16 zero bytes.
        ok.Feed(ESC + "_Gf=32,s=2,v=2,a=T;AAAAAAAAAAAAAAAAAAAAAA==" + ST);
        CHECK(ok.images == 1);
    }
}

TEST_CASE("a Sixel payload cannot allocate without bound",
          "[compat][image][security]")
{
    Harness h;
    // A raster attribute claiming an enormous canvas, then one pixel.
    h.Feed(ESC + "Pq\"1;1;60000;60000#0~" + ST);
    // Either it decoded something small or it refused; what it must not do is
    // honour the declared size.
    CHECK(h.Blank());
}

TEST_CASE("an unterminated control string cannot grow without bound",
          "[compat][security]")
{
    Harness h;
    h.Feed(ESC + "]0;");
    for (int i = 0; i < 200; ++i)
        h.Feed(std::string(100000, 'A'));   // 20 MB with no terminator
    h.Feed(BEL);
    // The parser caps the buffer; the grid is untouched and we are still here.
    CHECK(h.Blank());
    h.Feed("after");
    CHECK(h.Line(h.grid.CurY()).find("after") != std::string::npos);
}

// --------------------------------------------------------------- clipboard
TEST_CASE("OSC 52 writes the clipboard but never reads it",
          "[compat][osc52][security]")
{
    Harness h;
    h.Feed(ESC + "]52;c;aGVsbG8=" + BEL);
    REQUIRE(h.clipboard.size() == 1);
    CHECK(h.clipboard[0] == "aGVsbG8=");

    SECTION("a query is dropped, so a remote host cannot read the clipboard")
    {
        Harness q;
        q.Feed(ESC + "]52;c;?" + BEL);
        CHECK(q.clipboard.empty());
        CHECK(q.replies.empty());
    }
}

// ----------------------------------------------------------------- scrolling
TEST_CASE("scrollback ids stay stable while lines fall off the end",
          "[compat][scrollback]")
{
    Harness h(10, 4);
    h.grid.SetScrollbackMax(5);
    for (int i = 0; i < 20; ++i)
        h.Feed("line" + std::to_string(i) + "\r\n");
    // TotalPushed counts every line ever pushed, not what is retained.
    CHECK(h.grid.TotalPushed() >= 17);
    CHECK(h.grid.ScrollbackSize() == 5);
    // The oldest addressable row is TotalPushed - ScrollbackSize, and the
    // newest text is still on screen.
    CHECK(h.Screen().find("line19") != std::string::npos);
}
