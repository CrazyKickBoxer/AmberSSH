// SelectionTests.cpp — text extraction for clipboard copy across the grid.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "term/grid.h"

namespace
{

Cell Brush()
{
    Cell c;
    c.cp = U' ';
    return c;   // default fg/bg
}

void Put(Grid& g, std::string_view text)
{
    Cell b = Brush();
    for (char ch : text)
        g.PutChar(static_cast<char32_t>(ch), b, false);
}

void PutLine(Grid& g, std::string_view text)
{
    Put(g, text);
    g.CarriageReturn();
    g.LineFeed();
}

} // namespace

TEST_CASE("single-line forward selection", "[selection]")
{
    Grid g;
    g.Init(20, 4);
    Put(g, "hello world");
    REQUIRE(g.GetText(0, 0, 0, 4) == "hello");
    REQUIRE(g.GetText(0, 6, 0, 10) == "world");
}

TEST_CASE("a one-cell selection yields one character", "[selection]")
{
    Grid g;
    g.Init(20, 4);
    Put(g, "abc");
    REQUIRE(g.GetText(0, 1, 0, 1) == "b");
}

TEST_CASE("multi-line selection joins rows with newlines", "[selection]")
{
    Grid g;
    g.Init(20, 4);
    PutLine(g, "first");
    PutLine(g, "second");
    Put(g, "third");

    std::string s = g.GetText(0, 0, 2, 4);
    REQUIRE(s.find("first") != std::string::npos);
    REQUIRE(s.find("second") != std::string::npos);
    REQUIRE(s.find('\n') != std::string::npos);
}

TEST_CASE("trailing blanks are not padded into the copy", "[selection]")
{
    Grid g;
    g.Init(20, 3);
    PutLine(g, "ab");
    Put(g, "cd");

    std::string s = g.GetText(0, 0, 1, 19);
    // Should not contain a run of many spaces from the empty right-hand side.
    REQUIRE(s.find("          ") == std::string::npos);
}

TEST_CASE("selection spanning the whole grid does not overrun", "[selection]")
{
    Grid g;
    g.Init(10, 3);
    PutLine(g, "0123456789");
    PutLine(g, "abcdefghij");
    Put(g, "ABCDEFGHIJ");

    REQUIRE_NOTHROW(g.GetText(0, 0, g.Rows() - 1, g.Cols() - 1));
    std::string s = g.GetText(0, 0, g.Rows() - 1, g.Cols() - 1);
    REQUIRE(s.find("0123456789") != std::string::npos);
    REQUIRE(s.find("ABCDEFGHIJ") != std::string::npos);
}

TEST_CASE("out-of-range selection coordinates are clamped, not crashes",
          "[selection][bounds]")
{
    Grid g;
    g.Init(10, 3);
    Put(g, "abc");
    REQUIRE_NOTHROW(g.GetText(-5, -5, 99, 99));
    REQUIRE_NOTHROW(g.GetText(2, 9, 0, 0));      // reversed / inverted
}

TEST_CASE("selection reads from scrollback after scrolling", "[selection][scrollback]")
{
    Grid g;
    g.Init(10, 2);
    PutLine(g, "history1");
    PutLine(g, "history2");
    PutLine(g, "visible1");
    Put(g, "visible2");

    REQUIRE(g.ScrollbackSize() > 0);
    g.ScrollView(2);
    std::string s = g.GetText(0, 0, 1, 9);
    REQUIRE_FALSE(s.empty());
    g.SnapView();
}

TEST_CASE("unicode content survives extraction", "[selection][utf8]")
{
    Grid g;
    g.Init(20, 3);
    Cell b = Brush();
    g.PutChar(U'é', b, false);
    g.PutChar(U'€', b, false);
    std::string s = g.GetText(0, 0, 0, 1);
    // Extracted as UTF-8: 2 bytes for U+00E9 plus 3 for U+20AC.
    REQUIRE(s.size() == 5);
}
