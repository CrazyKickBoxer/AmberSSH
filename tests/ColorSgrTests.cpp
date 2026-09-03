// ColorSgrTests.cpp — end-to-end ANSI color model: SGR parsing in every form,
// chunk-split safety at every byte boundary, selective resets, palette
// resolution, scrollback fidelity, and escape-free copy.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "term/grid.h"
#include "term/palette.h"
#include "term/vtparser.h"

using amber::ColPalette;
using amber::ColRgb;
using amber::kColorDefault;

namespace
{

struct Harness
{
    Grid grid;
    VtParser parser{ grid };
    Harness() { grid.Init(40, 10); }
    void Feed(const std::string& s)
    {
        parser.Feed(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
};

} // namespace

TEST_CASE("standard and bright palette foregrounds", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[31mr\x1b[96mc\x1b[39md");
    REQUIRE(h.grid.ViewCell(0, 0).fg == ColPalette(1));
    REQUIRE(h.grid.ViewCell(0, 1).fg == ColPalette(14));
    REQUIRE(h.grid.ViewCell(0, 2).fg == kColorDefault);
}

TEST_CASE("standard and bright palette backgrounds", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[44mb\x1b[102mg\x1b[49md");
    REQUIRE(h.grid.ViewCell(0, 0).bg == ColPalette(4));
    REQUIRE(h.grid.ViewCell(0, 1).bg == ColPalette(10));
    REQUIRE(h.grid.ViewCell(0, 2).bg == kColorDefault);
}

TEST_CASE("256-color and truecolor, semicolon form", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[38;5;208mA\x1b[48;5;17mB\x1b[38;2;12;200;99mC\x1b[48;2;1;2;3mD");
    REQUIRE(h.grid.ViewCell(0, 0).fg == ColPalette(208));
    REQUIRE(h.grid.ViewCell(0, 1).bg == ColPalette(17));
    REQUIRE(h.grid.ViewCell(0, 2).fg == ColRgb(12, 200, 99));
    REQUIRE(h.grid.ViewCell(0, 3).bg == ColRgb(1, 2, 3));
}

TEST_CASE("truecolor colon forms with and without colorspace id", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[38:2:10:20:30mA");
    h.Feed("\x1b[38:2::40:50:60mB");
    h.Feed("\x1b[38:5:129mC");
    REQUIRE(h.grid.ViewCell(0, 0).fg == ColRgb(10, 20, 30));
    REQUIRE(h.grid.ViewCell(0, 1).fg == ColRgb(40, 50, 60));
    REQUIRE(h.grid.ViewCell(0, 2).fg == ColPalette(129));
}

TEST_CASE("multiple SGR commands in one sequence", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[1;4;38;5;46;48;2;9;8;7mX");
    const Cell& c = h.grid.ViewCell(0, 0);
    REQUIRE((c.attr & AttrBold) != 0);
    REQUIRE((c.attr & AttrUnderline) != 0);
    REQUIRE(c.fg == ColPalette(46));
    REQUIRE(c.bg == ColRgb(9, 8, 7));
}

TEST_CASE("selective resets leave unrelated attributes alone", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[1;3;4;5;7;9;31;44m");
    h.Feed("\x1b[24mA");   // underline off only
    const Cell& a = h.grid.ViewCell(0, 0);
    REQUIRE((a.attr & AttrUnderline) == 0);
    REQUIRE((a.attr & AttrBold) != 0);
    REQUIRE((a.attr & AttrItalic) != 0);
    REQUIRE((a.attr & AttrBlink) != 0);
    REQUIRE((a.attr & AttrInverse) != 0);
    REQUIRE((a.attr & AttrStrike) != 0);
    REQUIRE(a.fg == ColPalette(1));
    REQUIRE(a.bg == ColPalette(4));

    h.Feed("\x1b[22;23;25;27;29mB");   // clear the rest, keep colors
    const Cell& b = h.grid.ViewCell(0, 1);
    REQUIRE((b.attr & (AttrBold | AttrItalic | AttrBlink | AttrInverse |
                       AttrStrike)) == 0);
    REQUIRE(b.fg == ColPalette(1));
    REQUIRE(b.bg == ColPalette(4));
}

TEST_CASE("full attribute set parses", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[2mF\x1b[0m\x1b[8mH\x1b[0m\x1b[21mD");
    REQUIRE((h.grid.ViewCell(0, 0).attr & AttrDim) != 0);
    REQUIRE((h.grid.ViewCell(0, 1).attr & AttrConceal) != 0);
    REQUIRE((h.grid.ViewCell(0, 2).attr & AttrDblUnder) != 0);
}

TEST_CASE("underline color SGR 58/59", "[color][sgr]")
{
    Harness h;
    h.Feed("\x1b[4m\x1b[58;2;45;226;230mU");
    REQUIRE(h.grid.ViewCell(0, 0).ul == ColRgb(45, 226, 230));
    h.Feed("\x1b[59mV");
    REQUIRE(h.grid.ViewCell(0, 1).ul == kColorDefault);
}

TEST_CASE("escape sequences split at every byte boundary", "[color][chunks]")
{
    const std::string seq = "\x1b[1;38;2;255;45;146;48;5;17mZ";
    for (size_t split = 1; split < seq.size(); ++split)
    {
        Harness h;
        h.Feed(seq.substr(0, split));
        h.Feed(seq.substr(split));
        const Cell& c = h.grid.ViewCell(0, 0);
        INFO("split at byte " << split);
        REQUIRE(c.cp == U'Z');
        REQUIRE((c.attr & AttrBold) != 0);
        REQUIRE(c.fg == ColRgb(255, 45, 146));
        REQUIRE(c.bg == ColPalette(17));
    }
}

TEST_CASE("inverse flag is recorded and resolution swaps colors", "[color]")
{
    Harness h;
    h.Feed("\x1b[7mR");
    const Cell& c = h.grid.ViewCell(0, 0);
    REQUIRE((c.attr & AttrInverse) != 0);
    // The renderer swaps *resolved* colors; verify the resolution helpers.
    const amber::Palette16& pal = amber::PaletteAmberMiami();
    uint32_t fg = amber::ResolveCellColor(c.fg, pal, 0xFFB000);
    uint32_t bg = amber::ResolveCellColor(c.bg, pal, 0x000000);
    REQUIRE(fg == 0xFFB000);   // default fg resolves to amber
    REQUIRE(bg == 0x000000);   // default bg resolves to black
}

TEST_CASE("xterm-256 cube and grayscale resolution is canonical", "[color]")
{
    const amber::Palette16& pal = amber::PaletteAmberMiami();
    REQUIRE(amber::Resolve256(16, pal) == 0x000000);
    REQUIRE(amber::Resolve256(196, pal) == 0xFF0000);   // 16+180 = pure red
    REQUIRE(amber::Resolve256(46, pal) == 0x00FF00);
    REQUIRE(amber::Resolve256(21, pal) == 0x0000FF);
    REQUIRE(amber::Resolve256(232, pal) == 0x080808);
    REQUIRE(amber::Resolve256(255, pal) == 0xEEEEEE);
    REQUIRE(amber::Resolve256(3, pal) == pal.c[3]);
}

TEST_CASE("scrollback preserves colors and attributes", "[color][scrollback]")
{
    Harness h;
    h.Feed("\x1b[38;2;1;2;3;1mtop\x1b[0m\r\n");
    for (int i = 0; i < 12; ++i)
        h.Feed("filler\r\n");
    REQUIRE(h.grid.ScrollbackSize() > 0);
    h.grid.ScrollView(h.grid.ScrollbackSize());
    bool found = false;
    for (int r = 0; r < h.grid.Rows() && !found; ++r)
    {
        const Cell& c = h.grid.ViewCell(r, 0);
        if (c.cp == U't')
        {
            REQUIRE(c.fg == ColRgb(1, 2, 3));
            REQUIRE((c.attr & AttrBold) != 0);
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("copied text contains no escape bytes", "[color][copy]")
{
    Harness h;
    h.Feed("\x1b[31mred\x1b[0m and \x1b[38;2;0;255;0mgreen\x1b[0m");
    std::string text = h.grid.GetText(0, 0, 0, h.grid.Cols() - 1);
    REQUIRE(text == "red and green");
    REQUIRE(text.find('\x1b') == std::string::npos);
}

TEST_CASE("wrapping preserves the active color", "[color][wrap]")
{
    Harness h;
    h.Feed("\x1b[38;5;99m");
    for (int i = 0; i < 45; ++i)   // wider than the 40-column grid
        h.Feed("x");
    REQUIRE(h.grid.ViewCell(1, 2).fg == ColPalette(99));
}

TEST_CASE("erase fills carry the brush background", "[color][erase]")
{
    Harness h;
    h.Feed("\x1b[48;5;17m\x1b[2J");
    REQUIRE(h.grid.ViewCell(5, 5).bg == ColPalette(17));
    REQUIRE(h.grid.ViewCell(5, 5).cp == U' ');
}
