// AnsiParserTests.cpp — VT100/xterm escape-sequence state machine.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "term/grid.h"
#include "term/vtparser.h"

namespace
{

struct Harness
{
    Grid grid;
    VtParser parser{ grid };
    std::string replies;
    std::string title;

    Harness(int cols = 20, int rows = 6)
    {
        grid.Init(cols, rows);
        parser.SetWriter([this](const char* d, size_t n) { replies.append(d, n); });
        parser.SetTitleSink([this](const std::string& t) { title = t; });
    }

    void Feed(std::string_view s)
    {
        parser.Feed(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    // Feeds one byte at a time — proves the state machine survives arbitrary
    // chunk boundaries, which is how bytes actually arrive from a socket.
    void FeedSplit(std::string_view s)
    {
        for (char c : s)
        {
            uint8_t b = static_cast<uint8_t>(c);
            parser.Feed(&b, 1);
        }
    }

    std::string Row(int r) const
    {
        std::string out;
        for (int c = 0; c < grid.Cols(); ++c)
        {
            char32_t cp = grid.ViewCell(r, c).cp;
            out.push_back(cp < 128 ? static_cast<char>(cp) : '?');
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }
};

} // namespace

TEST_CASE("plain text lands in the grid", "[parser]")
{
    Harness h;
    h.Feed("hello");
    REQUIRE(h.Row(0) == "hello");
    REQUIRE(h.grid.CurX() == 5);
    REQUIRE(h.grid.CurY() == 0);
}

TEST_CASE("carriage return and line feed", "[parser]")
{
    Harness h;
    h.Feed("ab\r\ncd");
    REQUIRE(h.Row(0) == "ab");
    REQUIRE(h.Row(1) == "cd");
}

TEST_CASE("backspace moves the cursor without erasing", "[parser]")
{
    Harness h;
    h.Feed("abc\b\bX");
    REQUIRE(h.Row(0) == "aXc");
}

TEST_CASE("cursor positioning is 1-based and clamped", "[parser]")
{
    Harness h;
    h.Feed("\x1b[3;5HX");
    REQUIRE(h.grid.CurY() == 2);
    REQUIRE(h.Row(2) == "    X");

    h.Feed("\x1b[999;999HY");           // clamps to the last cell
    REQUIRE(h.grid.CurY() == h.grid.Rows() - 1);
}

TEST_CASE("cursor movement sequences", "[parser]")
{
    Harness h;
    h.Feed("\x1b[5;5H");
    h.Feed("\x1b[2A");  REQUIRE(h.grid.CurY() == 2);
    h.Feed("\x1b[1B");  REQUIRE(h.grid.CurY() == 3);
    h.Feed("\x1b[2C");  REQUIRE(h.grid.CurX() == 6);
    h.Feed("\x1b[3D");  REQUIRE(h.grid.CurX() == 3);
}

TEST_CASE("erase display and erase line", "[parser]")
{
    Harness h;
    h.Feed("aaaa\r\nbbbb\r\ncccc");
    h.Feed("\x1b[2;1H\x1b[K");          // erase to end of line 2
    REQUIRE(h.Row(1).empty());
    REQUIRE(h.Row(2) == "cccc");

    h.Feed("\x1b[2J");                  // erase whole display
    REQUIRE(h.Row(0).empty());
    REQUIRE(h.Row(2).empty());
}

TEST_CASE("insert and delete characters", "[parser]")
{
    Harness h;
    h.Feed("abcdef\x1b[1;1H");
    h.Feed("\x1b[2P");                  // delete 2 chars
    REQUIRE(h.Row(0) == "cdef");
    h.Feed("\x1b[2@");                  // insert 2 blanks
    REQUIRE(h.Row(0) == "  cdef");
}

TEST_CASE("insert and delete lines", "[parser]")
{
    Harness h;
    h.Feed("one\r\ntwo\r\nthree\x1b[2;1H");
    h.Feed("\x1b[1L");                  // insert a line at row 2
    REQUIRE(h.Row(0) == "one");
    REQUIRE(h.Row(1).empty());
    REQUIRE(h.Row(2) == "two");

    h.Feed("\x1b[1M");                  // delete it again
    REQUIRE(h.Row(1) == "two");
}

TEST_CASE("SGR sets and resets intensity", "[parser][sgr]")
{
    Harness h;
    h.Feed("\x1b[1mB\x1b[0mn");
    const Cell& bold = h.grid.ViewCell(0, 0);
    const Cell& normal = h.grid.ViewCell(0, 1);
    REQUIRE((bold.attr & AttrBold) != 0);
    REQUIRE((normal.attr & AttrBold) == 0);
}

TEST_CASE("SGR truecolor and 256-colour are accepted", "[parser][sgr]")
{
    Harness h;
    h.Feed("\x1b[38;2;255;255;255mW");
    h.Feed("\x1b[38;5;236mD");
    REQUIRE(h.grid.ViewCell(0, 0).fg == amber::ColRgb(255, 255, 255));
    REQUIRE(h.grid.ViewCell(0, 1).fg == amber::ColPalette(236));
}

TEST_CASE("reverse video swaps foreground and background", "[parser][sgr]")
{
    Harness h;
    h.Feed("\x1b[7mR");
    REQUIRE((h.grid.ViewCell(0, 0).attr & AttrInverse) != 0);
}

TEST_CASE("alternate screen is entered and left", "[parser]")
{
    Harness h;
    h.Feed("main");
    h.Feed("\x1b[?1049h");
    REQUIRE(h.grid.AltActive());
    h.Feed("alt");
    REQUIRE(h.Row(0) == "alt");
    h.Feed("\x1b[?1049l");
    REQUIRE_FALSE(h.grid.AltActive());
    REQUIRE(h.Row(0) == "main");        // main screen content survived
}

TEST_CASE("cursor visibility mode", "[parser]")
{
    Harness h;
    h.Feed("\x1b[?25l");
    REQUIRE_FALSE(h.grid.CursorVisible());
    h.Feed("\x1b[?25h");
    REQUIRE(h.grid.CursorVisible());
}

TEST_CASE("bracketed paste mode toggles", "[parser]")
{
    Harness h;
    REQUIRE_FALSE(h.parser.Modes().bracketedPaste);
    h.Feed("\x1b[?2004h");
    REQUIRE(h.parser.Modes().bracketedPaste);
    h.Feed("\x1b[?2004l");
    REQUIRE_FALSE(h.parser.Modes().bracketedPaste);
}

TEST_CASE("application cursor keys mode toggles", "[parser]")
{
    Harness h;
    h.Feed("\x1b[?1h");
    REQUIRE(h.parser.Modes().appCursorKeys);
    h.Feed("\x1b[?1l");
    REQUIRE_FALSE(h.parser.Modes().appCursorKeys);
}

TEST_CASE("device status report answers back", "[parser]")
{
    Harness h;
    h.Feed("\x1b[6n");                  // cursor position report
    REQUIRE(h.replies.find("\x1b[") != std::string::npos);
    REQUIRE(h.replies.back() == 'R');
}

TEST_CASE("primary device attributes answers back", "[parser]")
{
    Harness h;
    h.Feed("\x1b[c");
    REQUIRE(h.replies.find("\x1b[?") != std::string::npos);
}

TEST_CASE("OSC 0 sets the window title", "[parser][osc]")
{
    Harness h;
    h.Feed("\x1b]0;my title\x07");
    REQUIRE(h.title == "my title");
}

TEST_CASE("OSC terminated by ST is accepted", "[parser][osc]")
{
    Harness h;
    h.Feed("\x1b]2;via st\x1b\\");
    REQUIRE(h.title == "via st");
}

TEST_CASE("an over-long OSC payload cannot grow without bound", "[parser][osc][fuzz]")
{
    Harness h;
    std::string huge = "\x1b]0;";
    huge.append(200000, 'A');
    huge.push_back('\x07');
    REQUIRE_NOTHROW(h.Feed(huge));
    // Whatever the parser keeps, it must not have adopted a 200 KB title.
    REQUIRE(h.title.size() < 100000);
}

TEST_CASE("escape sequences split across chunks still parse", "[parser][fuzz]")
{
    Harness h;
    h.FeedSplit("\x1b[3;5HX");
    REQUIRE(h.grid.CurY() == 2);
    REQUIRE(h.Row(2) == "    X");
}

TEST_CASE("a complete but absurd sequence does not corrupt later output",
          "[parser][fuzz]")
{
    // Overflowing parameter values, and far more parameters than the fixed
    // param array holds. The sequence is well formed, so the parser must
    // return to ground and print what follows.
    Harness h;
    h.Feed("\x1b[999999999999;;;;;;;;;;;;;;;;;;;;;;;;m");
    h.Feed("ok");
    REQUIRE(h.Row(0).find("ok") != std::string::npos);
}

TEST_CASE("a truncated CSI consumes exactly one following final byte",
          "[parser][fuzz]")
{
    // Correct VT behaviour, not a defect: after ESC [ the next byte in the
    // final range terminates the sequence. xterm swallows it the same way.
    Harness h;
    h.Feed("\x1b[");      // truncated introducer
    h.Feed("ok");         // 'o' terminates the CSI; 'k' prints
    REQUIRE(h.Row(0) == "k");
}

TEST_CASE("UTF-8 multibyte input decodes", "[parser][utf8]")
{
    Harness h;
    h.Feed("\xC3\xA9");                 // U+00E9
    REQUIRE(h.grid.ViewCell(0, 0).cp == U'é');
}

TEST_CASE("UTF-8 split across chunks decodes", "[parser][utf8]")
{
    Harness h;
    h.FeedSplit("\xE2\x82\xAC");        // U+20AC EURO SIGN, one byte at a time
    REQUIRE(h.grid.ViewCell(0, 0).cp == U'€');
}

TEST_CASE("invalid UTF-8 does not throw or hang", "[parser][utf8][fuzz]")
{
    Harness h;
    REQUIRE_NOTHROW(h.Feed("\xFF\xFE\x80\x80"));
    REQUIRE_NOTHROW(h.Feed("ok"));
}

TEST_CASE("scroll region confines scrolling", "[parser]")
{
    Harness h;
    h.Feed("\x1b[2;3r");                // region = rows 2..3
    h.Feed("\x1b[1;1Htop");
    h.Feed("\x1b[3;1Hxx\n");            // newline at region bottom scrolls region
    REQUIRE(h.Row(0) == "top");         // row 1 is outside the region: untouched
}

TEST_CASE("save and restore cursor", "[parser]")
{
    Harness h;
    h.Feed("\x1b[4;7H\x1b[s");
    h.Feed("\x1b[1;1H");
    h.Feed("\x1b[u");
    REQUIRE(h.grid.CurY() == 3);
    REQUIRE(h.grid.CurX() == 6);
}

TEST_CASE("tab advances to the next tab stop", "[parser]")
{
    Harness h(40, 4);
    h.Feed("a\tb");
    REQUIRE(h.grid.CurX() == 9);        // default stops every 8 columns
}

TEST_CASE("mouse reporting modes are tracked", "[parser][mouse]")
{
    Harness h;
    REQUIRE(h.parser.Modes().mouseMode == 0);
    h.Feed("\x1b[?1000h");
    REQUIRE(h.parser.Modes().mouseMode == 1000);
    h.Feed("\x1b[?1006h");              // SGR extended coordinates
    REQUIRE(h.parser.Modes().mouseSgr);
    h.Feed("\x1b[?1002h");              // upgrade to click+drag
    REQUIRE(h.parser.Modes().mouseMode == 1002);
    h.Feed("\x1b[?1002l");
    REQUIRE(h.parser.Modes().mouseMode == 0);
    h.Feed("\x1b[?1006l");
    REQUIRE_FALSE(h.parser.Modes().mouseSgr);
    // Legacy encodings are consumed without affecting the mode.
    h.Feed("\x1b[?1005h\x1b[?1015h");
    REQUIRE(h.parser.Modes().mouseMode == 0);
}

TEST_CASE("OSC 52 delivers base64 clipboard payloads", "[parser][osc52]")
{
    Harness h;
    std::string clip;
    h.parser.SetClipboardSink([&](const std::string& b64) { clip = b64; });

    h.Feed("\x1b]52;c;aGVsbG8=\x07");           // BEL-terminated
    REQUIRE(clip == "aGVsbG8=");                // "hello"

    clip.clear();
    h.FeedSplit("\x1b]52;;d29ybGQ=\x1b\\");     // ST-terminated, split feed
    REQUIRE(clip == "d29ybGQ=");                // "world"

    // Queries must never reach the sink — the remote cannot read the
    // local clipboard.
    clip.clear();
    h.Feed("\x1b]52;c;?\x07");
    REQUIRE(clip.empty());

    // Titles still work through the shared dispatch.
    h.Feed("\x1b]0;my title\x07");
    REQUIRE(h.title == "my title");
}
