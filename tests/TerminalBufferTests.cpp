// TerminalBufferTests.cpp — cell grid, wrapping, scrolling, scrollback, resize.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "term/grid.h"

namespace
{

Cell Brush()
{
    Cell c;
    c.cp = U' ';
    return c;   // default fg/bg — the amber-on-black terminal defaults
}

void Put(Grid& g, std::string_view text)
{
    Cell b = Brush();
    for (char ch : text)
        g.PutChar(static_cast<char32_t>(ch), b, false);
}

std::string Row(const Grid& g, int r)
{
    std::string out;
    for (int c = 0; c < g.Cols(); ++c)
    {
        char32_t cp = g.ViewCell(r, c).cp;
        out.push_back(cp < 128 ? static_cast<char>(cp) : '?');
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

} // namespace

TEST_CASE("init produces a blank grid of the requested size", "[grid]")
{
    Grid g;
    g.Init(80, 25);
    REQUIRE(g.Cols() == 80);
    REQUIRE(g.Rows() == 25);
    REQUIRE(g.CurX() == 0);
    REQUIRE(g.CurY() == 0);
    REQUIRE(Row(g, 0).empty());
}

TEST_CASE("writing advances the cursor", "[grid]")
{
    Grid g;
    g.Init(10, 4);
    Put(g, "abc");
    REQUIRE(Row(g, 0) == "abc");
    REQUIRE(g.CurX() == 3);
}

TEST_CASE("autowrap moves to the next line at the margin", "[grid][wrap]")
{
    Grid g;
    g.Init(4, 3);
    g.SetAutowrap(true);
    Put(g, "abcdef");
    REQUIRE(Row(g, 0) == "abcd");
    REQUIRE(Row(g, 1) == "ef");
}

TEST_CASE("autowrap disabled clamps at the last column", "[grid][wrap]")
{
    Grid g;
    g.Init(4, 3);
    g.SetAutowrap(false);
    Put(g, "abcdef");
    REQUIRE(g.CurY() == 0);
    REQUIRE(Row(g, 1).empty());
}

TEST_CASE("line feed at the bottom scrolls and fills scrollback", "[grid][scroll]")
{
    Grid g;
    g.Init(8, 3);
    Put(g, "r0"); g.CarriageReturn(); g.LineFeed();
    Put(g, "r1"); g.CarriageReturn(); g.LineFeed();
    Put(g, "r2"); g.CarriageReturn(); g.LineFeed();
    Put(g, "r3");

    REQUIRE(Row(g, 2) == "r3");
    REQUIRE(g.ScrollbackSize() >= 1);
}

TEST_CASE("reverse index at the top scrolls down", "[grid][scroll]")
{
    Grid g;
    g.Init(8, 3);
    Put(g, "top");
    g.SetCursor(0, 0);
    g.ReverseIndex();
    REQUIRE(Row(g, 1) == "top");
}

TEST_CASE("erase display clears the requested span", "[grid][erase]")
{
    Grid g;
    g.Init(8, 3);
    Put(g, "aaa"); g.CarriageReturn(); g.LineFeed();
    Put(g, "bbb"); g.CarriageReturn(); g.LineFeed();
    Put(g, "ccc");

    g.SetCursor(0, 1);
    g.EraseDisplay(0, Brush());          // erase from cursor down
    REQUIRE(Row(g, 0) == "aaa");
    REQUIRE(Row(g, 1).empty());
    REQUIRE(Row(g, 2).empty());
}

TEST_CASE("erase line modes", "[grid][erase]")
{
    Grid g;
    g.Init(8, 2);
    Put(g, "abcdefg");
    g.SetCursor(3, 0);
    g.EraseLine(0, Brush());             // to end of line
    REQUIRE(Row(g, 0) == "abc");

    Put(g, "");
    g.SetCursor(0, 0);
    Put(g, "abcdefg");
    g.SetCursor(3, 0);
    g.EraseLine(1, Brush());             // from start of line
    REQUIRE(Row(g, 0).substr(0, 4) == "    ");
}

TEST_CASE("insert and delete characters shift the line", "[grid]")
{
    Grid g;
    g.Init(10, 2);
    Put(g, "abcdef");
    g.SetCursor(0, 0);
    g.DeleteChars(2, Brush());
    REQUIRE(Row(g, 0) == "cdef");
    g.InsertChars(2, Brush());
    REQUIRE(Row(g, 0) == "  cdef");
}

TEST_CASE("insert and delete lines shift the screen", "[grid]")
{
    Grid g;
    g.Init(8, 4);
    Put(g, "l0"); g.CarriageReturn(); g.LineFeed();
    Put(g, "l1"); g.CarriageReturn(); g.LineFeed();
    Put(g, "l2");

    g.SetCursor(0, 1);
    g.InsertLines(1, Brush());
    REQUIRE(Row(g, 1).empty());
    REQUIRE(Row(g, 2) == "l1");

    g.DeleteLines(1, Brush());
    REQUIRE(Row(g, 1) == "l1");
}

TEST_CASE("scroll region limits scrolling", "[grid][scroll]")
{
    Grid g;
    g.Init(8, 5);
    Put(g, "keep");
    g.SetScrollRegion(1, 3);
    g.SetCursor(0, 3);
    g.LineFeed();                        // scrolls only rows 1..3
    REQUIRE(Row(g, 0) == "keep");
}

TEST_CASE("alternate screen preserves the main buffer", "[grid][alt]")
{
    Grid g;
    g.Init(8, 3);
    Put(g, "main");
    g.EnterAlt();
    REQUIRE(g.AltActive());
    REQUIRE(Row(g, 0).empty());
    Put(g, "alt");
    REQUIRE(Row(g, 0) == "alt");
    g.ExitAlt();
    REQUIRE_FALSE(g.AltActive());
    REQUIRE(Row(g, 0) == "main");
}

TEST_CASE("save and restore cursor round-trips", "[grid]")
{
    Grid g;
    g.Init(20, 6);
    g.SetCursor(7, 3);
    g.SaveCursor();
    g.SetCursor(0, 0);
    g.RestoreCursor();
    REQUIRE(g.CurX() == 7);
    REQUIRE(g.CurY() == 3);
}

TEST_CASE("resize keeps the grid consistent", "[grid][resize]")
{
    Grid g;
    g.Init(20, 6);
    Put(g, "hello");
    g.Resize(40, 12);
    REQUIRE(g.Cols() == 40);
    REQUIRE(g.Rows() == 12);
    REQUIRE(Row(g, 0).substr(0, 5) == "hello");

    g.Resize(10, 3);                     // shrink
    REQUIRE(g.Cols() == 10);
    REQUIRE(g.Rows() == 3);
    REQUIRE(g.CurX() < g.Cols());
    REQUIRE(g.CurY() < g.Rows());
}

TEST_CASE("tab stops default to every eight columns", "[grid][tabs]")
{
    Grid g;
    g.Init(40, 3);
    g.Tab();
    REQUIRE(g.CurX() == 8);
    g.Tab();
    REQUIRE(g.CurX() == 16);
}

TEST_CASE("custom tab stop is honoured and clearable", "[grid][tabs]")
{
    Grid g;
    g.Init(40, 3);
    g.ClearTabStop(true);                // clear all
    g.SetCursor(5, 0);
    g.SetTabStop();
    g.SetCursor(0, 0);
    g.Tab();
    REQUIRE(g.CurX() == 5);
}

TEST_CASE("scrollback view scrolls and snaps back", "[grid][scrollback]")
{
    Grid g;
    g.Init(8, 2);
    for (int i = 0; i < 10; ++i)
    {
        Put(g, "x");
        g.CarriageReturn();
        g.LineFeed();
    }
    REQUIRE(g.ScrollbackSize() > 0);

    g.ScrollView(3);
    REQUIRE(g.ViewOffset() > 0);
    g.SnapView();
    REQUIRE(g.ViewOffset() == 0);
}

TEST_CASE("GetText extracts a rectangular span", "[grid][text]")
{
    Grid g;
    g.Init(10, 3);
    Put(g, "abcdef");
    std::string s = g.GetText(0, 1, 0, 3);
    REQUIRE(s == "bcd");
}

TEST_CASE("ResetAll returns the grid to a clean state", "[grid]")
{
    Grid g;
    g.Init(10, 3);
    Put(g, "dirty");
    g.EnterAlt();
    g.ResetAll();
    REQUIRE_FALSE(g.AltActive());
    REQUIRE(g.CurX() == 0);
    REQUIRE(g.CurY() == 0);
}
