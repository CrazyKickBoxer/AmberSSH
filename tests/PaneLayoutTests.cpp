// PaneLayoutTests.cpp — the recursive pane tree.
//
// The tree is pure, so the properties that matter can be asserted
// exhaustively rather than sampled: panes never overlap, they never leave the
// grid, every operation leaves exactly the panes it should, and zoom is a
// view state that restores the layout by construction.
#include <catch2/catch_test_macros.hpp>

#include "../src/sessions/PaneLayout.h"

#include <algorithm>
#include <set>
#include <string>

using namespace amber;

namespace
{

// The invariant every layout must satisfy, checked after every mutation in
// the tests below: no two panes share a cell, and none is outside the grid.
void CheckDisjoint(const PaneLayout& l, int cols, int rows)
{
    const auto rects = l.Rects(cols, rows);
    for (size_t i = 0; i < rects.size(); ++i)
    {
        const PaneRect& a = rects[i].second;
        if (a.cols <= 0 || a.rows <= 0)
            continue;                     // hidden by zoom
        CHECK(a.col >= 0);
        CHECK(a.row >= 0);
        CHECK(a.col + a.cols <= cols);
        CHECK(a.row + a.rows <= rows);
        for (size_t j = i + 1; j < rects.size(); ++j)
        {
            const PaneRect& b = rects[j].second;
            if (b.cols <= 0 || b.rows <= 0)
                continue;
            const bool overlap = a.col < b.col + b.cols && b.col < a.col + a.cols &&
                                 a.row < b.row + b.rows && b.row < a.row + a.rows;
            CHECK_FALSE(overlap);
        }
    }
}

} // namespace

TEST_CASE("a fresh layout is one pane filling the grid", "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Count() == 1);
    const auto rects = l.Rects(80, 24);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].first == 1);
    CHECK(rects[0].second.col == 0);
    CHECK(rects[0].second.row == 0);
    CHECK(rects[0].second.cols == 80);
    CHECK(rects[0].second.rows == 24);
    CHECK_FALSE(l.Close(1));              // a tab always has one pane
}

TEST_CASE("a vertical split divides the columns and keeps a divider",
          "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 81, 24));
    const auto rects = l.Rects(81, 24);
    REQUIRE(rects.size() == 2);
    // 81 columns = 40 + a divider + 40.
    CHECK(rects[0].second.cols == 40);
    CHECK(rects[1].second.cols == 40);
    CHECK(rects[1].second.col == 41);
    CHECK(rects[0].second.rows == 24);
    CHECK(rects[1].second.rows == 24);
    CheckDisjoint(l, 81, 24);

    SECTION("the divider cell belongs to neither pane")
    {
        CHECK(l.PaneAt(39, 5, 81, 24) == 1);
        CHECK(l.PaneAt(40, 5, 81, 24) == kNoPane);
        CHECK(l.PaneAt(41, 5, 81, 24) == 2);
        bool vertical = false;
        PaneId before = kNoPane, after = kNoPane;
        REQUIRE(l.DividerAt(40, 5, 81, 24, vertical, before, after));
        CHECK(vertical);
        CHECK(before == 1);
        CHECK(after == 2);
    }
}

TEST_CASE("splits nest arbitrarily", "[panes]")
{
    // The thing the old one-level model could not do: split a pane that is
    // itself one side of a split, repeatedly, on both sides.
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 160, 48));
    REQUIRE(l.Split(2, 3, SplitDir::Horizontal, 160, 48));
    REQUIRE(l.Split(1, 4, SplitDir::Horizontal, 160, 48));
    REQUIRE(l.Split(4, 5, SplitDir::Vertical, 160, 48));
    CHECK(l.Count() == 5);
    const std::vector<PaneId> panes = l.Panes();
    CHECK(std::set<PaneId>(panes.begin(), panes.end()).size() == 5);
    CheckDisjoint(l, 160, 48);

    SECTION("every pane is reachable by the cell it occupies")
    {
        for (const auto& [pid, r] : l.Rects(160, 48))
        {
            REQUIRE(r.cols > 0);
            REQUIRE(r.rows > 0);
            CHECK(l.PaneAt(r.col, r.row, 160, 48) == pid);
        }
    }
}

TEST_CASE("a deeply nested layout stays disjoint and inside the grid",
          "[panes][stress]")
{
    PaneLayout l;
    l.Reset(0);
    PaneId next = 1;
    // Keep splitting the most recently created pane, alternating direction,
    // until the tree refuses for want of room.
    PaneId target = 0;
    for (int i = 0; i < 40; ++i)
    {
        const SplitDir d = (i % 2) ? SplitDir::Horizontal : SplitDir::Vertical;
        if (!l.Split(target, next, d, 200, 60))
            break;
        target = next++;
    }
    CHECK(l.Count() > 4);                 // it really did nest
    CheckDisjoint(l, 200, 60);
    // Every pane is still big enough to be a terminal.
    for (const auto& [pid, r] : l.Rects(200, 60))
    {
        INFO("pane " << pid);
        CHECK(r.cols >= 1);
        CHECK(r.rows >= 1);
    }
}

TEST_CASE("a split is refused rather than making a pane too small",
          "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    // kMinPaneCols * 2 + 1 columns is the smallest that can hold two.
    CHECK_FALSE(l.Split(1, 2, SplitDir::Vertical, kMinPaneCols * 2, 24));
    CHECK(l.Count() == 1);
    CHECK(l.Split(1, 2, SplitDir::Vertical, kMinPaneCols * 2 + 1, 24));
    CHECK(l.Count() == 2);

    SECTION("horizontally too")
    {
        PaneLayout h;
        h.Reset(1);
        CHECK_FALSE(h.Split(1, 2, SplitDir::Horizontal, 80, kMinPaneRows * 2));
        CHECK(h.Split(1, 2, SplitDir::Horizontal, 80, kMinPaneRows * 2 + 1));
    }
    SECTION("a split refused deep in the tree does not disturb it")
    {
        // 30 columns splits into 14 and 15; neither can hold two panes of
        // kMinPaneCols with a divider between them, so the second split is
        // judged on the space its target actually has and refused.
        PaneLayout d;
        d.Reset(1);
        REQUIRE(d.Split(1, 2, SplitDir::Vertical, 30, 24));
        const auto before = d.Rects(30, 24);
        CHECK_FALSE(d.Split(2, 3, SplitDir::Vertical, 30, 24));
        const auto after = d.Rects(30, 24);
        REQUIRE(before.size() == after.size());
        for (size_t i = 0; i < before.size(); ++i)
        {
            CHECK(before[i].first == after[i].first);
            CHECK(before[i].second.cols == after[i].second.cols);
        }
    }
    SECTION("a duplicate pane id is refused")
    {
        PaneLayout d;
        d.Reset(1);
        REQUIRE(d.Split(1, 2, SplitDir::Vertical, 80, 24));
        CHECK_FALSE(d.Split(1, 2, SplitDir::Vertical, 80, 24));
        CHECK(d.Count() == 2);
    }
}

TEST_CASE("closing a pane gives its space to its sibling subtree", "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 160, 48));
    REQUIRE(l.Split(2, 3, SplitDir::Horizontal, 160, 48));
    REQUIRE(l.Close(1));
    // 2 and 3 now share the whole grid, still stacked.
    CHECK(l.Count() == 2);
    CheckDisjoint(l, 160, 48);
    const auto rects = l.Rects(160, 48);
    for (const auto& [pid, r] : rects)
        CHECK(r.cols == 160);

    SECTION("closing down to one pane leaves it filling the grid")
    {
        REQUIRE(l.Close(3));
        REQUIRE(l.Count() == 1);
        const auto one = l.Rects(160, 48);
        REQUIRE(one.size() == 1);
        CHECK(one[0].second.cols == 160);
        CHECK(one[0].second.rows == 48);
        CHECK_FALSE(l.Close(one[0].first));
    }
    SECTION("closing an unknown pane changes nothing")
    {
        CHECK_FALSE(l.Close(99));
        CHECK(l.Count() == 2);
    }
}

TEST_CASE("repeated split and close leaves no residue", "[panes][stress]")
{
    PaneLayout l;
    l.Reset(0);
    PaneId next = 1;
    for (int round = 0; round < 200; ++round)
    {
        const std::vector<PaneId> before = l.Panes();
        const PaneId target = before[static_cast<size_t>(round) % before.size()];
        const SplitDir d = (round % 3) ? SplitDir::Vertical : SplitDir::Horizontal;
        if (l.Split(target, next, d, 120, 40))
        {
            ++next;
            CHECK(l.Count() == before.size() + 1);
        }
        if (l.Count() > 3)
        {
            const std::vector<PaneId> now = l.Panes();
            REQUIRE(l.Close(now[now.size() / 2]));
        }
        CheckDisjoint(l, 120, 40);
    }
    // Whatever it ended up as, it is still a valid layout of distinct panes.
    const std::vector<PaneId> end = l.Panes();
    CHECK(std::set<PaneId>(end.begin(), end.end()).size() == end.size());
    CHECK(l.Count() >= 1);
}

TEST_CASE("resize moves the divider and is clamped both ways", "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 101, 24));
    const int wide = l.RectOf(1, 101, 24).cols;
    REQUIRE(l.Resize(1, 0.2f));           // grow pane 1
    CHECK(l.RectOf(1, 101, 24).cols > wide);
    CheckDisjoint(l, 101, 24);

    SECTION("positive delta grows the named pane whichever side it is on")
    {
        PaneLayout r;
        r.Reset(1);
        REQUIRE(r.Split(1, 2, SplitDir::Vertical, 101, 24));
        const int was = r.RectOf(2, 101, 24).cols;
        REQUIRE(r.Resize(2, 0.2f));
        CHECK(r.RectOf(2, 101, 24).cols > was);
    }
    SECTION("a resize storm cannot push a pane out of existence")
    {
        for (int i = 0; i < 500; ++i)
            l.Resize(1, 0.5f);
        CheckDisjoint(l, 101, 24);
        CHECK(l.RatioOf(1) <= kMaxRatio + 1e-6f);
        CHECK(l.RectOf(2, 101, 24).cols > 0);
        for (int i = 0; i < 1000; ++i)
            l.Resize(1, -0.5f);
        CHECK(l.RatioOf(1) >= kMinRatio - 1e-6f);
        CHECK(l.RectOf(1, 101, 24).cols > 0);
        CheckDisjoint(l, 101, 24);
    }
    SECTION("the only pane cannot be resized")
    {
        PaneLayout one;
        one.Reset(7);
        CHECK_FALSE(one.Resize(7, 0.1f));
        CHECK_FALSE(one.SetRatio(7, 0.3f));
    }
}

TEST_CASE("zoom is a view state and restores the exact layout", "[panes][zoom]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 160, 48));
    REQUIRE(l.Split(2, 3, SplitDir::Horizontal, 160, 48));
    REQUIRE(l.Resize(1, 0.15f));
    const auto before = l.Rects(160, 48);

    l.SetZoom(2);
    CHECK(l.Zoomed());
    const auto zoomed = l.Rects(160, 48);
    // The zoomed pane has everything; the others have nothing.
    for (const auto& [pid, r] : zoomed)
    {
        if (pid == 2)
        {
            CHECK(r.cols == 160);
            CHECK(r.rows == 48);
        }
        else
        {
            CHECK(r.cols == 0);
            CHECK(r.rows == 0);
        }
    }
    CheckDisjoint(l, 160, 48);
    CHECK(l.Count() == 3);                // nothing was destroyed

    l.ClearZoom();
    const auto after = l.Rects(160, 48);
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        CHECK(before[i].first == after[i].first);
        CHECK(before[i].second.col == after[i].second.col);
        CHECK(before[i].second.row == after[i].second.row);
        CHECK(before[i].second.cols == after[i].second.cols);
        CHECK(before[i].second.rows == after[i].second.rows);
    }

    SECTION("closing the zoomed pane clears the zoom")
    {
        l.SetZoom(2);
        REQUIRE(l.Close(2));
        CHECK_FALSE(l.Zoomed());
    }
    SECTION("splitting while zoomed drops the zoom, so the new pane is visible")
    {
        l.SetZoom(2);
        REQUIRE(l.Split(2, 4, SplitDir::Vertical, 160, 48));
        CHECK_FALSE(l.Zoomed());
        CHECK(l.RectOf(4, 160, 48).cols > 0);
    }
    SECTION("zooming a pane that does not exist is not zoomed")
    {
        l.SetZoom(99);
        CHECK_FALSE(l.Zoomed());
        CHECK(l.Rects(160, 48).size() == 3);
    }
}

TEST_CASE("swap exchanges positions without changing the shape", "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 160, 48));
    REQUIRE(l.Split(2, 3, SplitDir::Horizontal, 160, 48));
    const PaneRect r1 = l.RectOf(1, 160, 48);
    const PaneRect r3 = l.RectOf(3, 160, 48);
    REQUIRE(l.Swap(1, 3));
    CHECK(l.RectOf(3, 160, 48).col == r1.col);
    CHECK(l.RectOf(3, 160, 48).cols == r1.cols);
    CHECK(l.RectOf(1, 160, 48).col == r3.col);
    CHECK(l.Count() == 3);
    CheckDisjoint(l, 160, 48);
    CHECK_FALSE(l.Swap(1, 1));
    CHECK_FALSE(l.Swap(1, 99));
}

TEST_CASE("rotate flips the split that holds a pane", "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 120, 40));
    CHECK(l.RectOf(1, 120, 40).rows == 40);      // side by side
    REQUIRE(l.Rotate(1));
    CHECK(l.RectOf(1, 120, 40).cols == 120);     // now stacked
    CheckDisjoint(l, 120, 40);
    PaneLayout one;
    one.Reset(5);
    CHECK_FALSE(one.Rotate(5));
}

TEST_CASE("move relocates a pane and never loses it", "[panes]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 160, 48));
    REQUIRE(l.Split(2, 3, SplitDir::Horizontal, 160, 48));
    REQUIRE(l.Move(3, 1, SplitDir::Horizontal, 160, 48));
    CHECK(l.Count() == 3);
    const std::vector<PaneId> panes = l.Panes();
    CHECK(std::set<PaneId>(panes.begin(), panes.end()).size() == 3);
    CheckDisjoint(l, 160, 48);
    // 3 is now stacked with 1, so they share a column band.
    CHECK(l.RectOf(3, 160, 48).col == l.RectOf(1, 160, 48).col);

    SECTION("moving onto itself or an unknown pane is refused")
    {
        CHECK_FALSE(l.Move(1, 1, SplitDir::Vertical, 160, 48));
        CHECK_FALSE(l.Move(1, 99, SplitDir::Vertical, 160, 48));
        CHECK(l.Count() == 3);
    }
    SECTION("a move the destination has no room for keeps every pane")
    {
        PaneLayout t;
        t.Reset(1);
        REQUIRE(t.Split(1, 2, SplitDir::Vertical, 40, 10));
        // Too narrow to split again: the move fails and both panes remain.
        t.Move(2, 1, SplitDir::Vertical, 40, 10);
        CHECK(t.Count() == 2);
        const std::vector<PaneId> got = t.Panes();
        CHECK(std::set<PaneId>(got.begin(), got.end()) == std::set<PaneId>{ 1, 2 });
    }
}

TEST_CASE("neighbour navigation follows geometry, not tree shape", "[panes]")
{
    // 1 | 2      with 2 split so 2 is above 3
    //   | 3
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 160, 48));
    REQUIRE(l.Split(2, 3, SplitDir::Horizontal, 160, 48));
    CHECK(l.Neighbour(1, PaneLayout::Dir::Right, 160, 48) != kNoPane);
    CHECK(l.Neighbour(2, PaneLayout::Dir::Left, 160, 48) == 1);
    CHECK(l.Neighbour(3, PaneLayout::Dir::Left, 160, 48) == 1);
    CHECK(l.Neighbour(2, PaneLayout::Dir::Down, 160, 48) == 3);
    CHECK(l.Neighbour(3, PaneLayout::Dir::Up, 160, 48) == 2);
    CHECK(l.Neighbour(1, PaneLayout::Dir::Left, 160, 48) == kNoPane);
    CHECK(l.Neighbour(1, PaneLayout::Dir::Up, 160, 48) == kNoPane);

    SECTION("cycling visits every pane and wraps")
    {
        std::set<PaneId> seen;
        PaneId at = 1;
        for (int i = 0; i < 3; ++i)
        {
            seen.insert(at);
            at = l.Cycle(at, 1);
        }
        CHECK(seen.size() == 3);
        CHECK(at == 1);                   // wrapped back
        CHECK(l.Cycle(1, -1) == l.Panes().back());
    }
}

TEST_CASE("a layout round-trips through its serialised form", "[panes][workspace]")
{
    PaneLayout l;
    l.Reset(10);
    REQUIRE(l.Split(10, 20, SplitDir::Vertical, 200, 60));
    REQUIRE(l.Split(20, 30, SplitDir::Horizontal, 200, 60));
    REQUIRE(l.Split(10, 40, SplitDir::Horizontal, 200, 60));
    REQUIRE(l.Resize(10, 0.13f));
    const std::vector<PaneId> order = l.Panes();
    const std::string text = l.Serialize(order);
    CHECK(text.find('V') != std::string::npos);
    CHECK(text.find('H') != std::string::npos);

    PaneLayout back;
    back.Reset(10);
    REQUIRE(back.Deserialize(text, order));
    const auto a = l.Rects(200, 60);
    const auto b = back.Rects(200, 60);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        CHECK(a[i].first == b[i].first);
        CHECK(a[i].second.col == b[i].second.col);
        CHECK(a[i].second.row == b[i].second.row);
        CHECK(a[i].second.cols == b[i].second.cols);
        CHECK(a[i].second.rows == b[i].second.rows);
    }
}

TEST_CASE("a malformed layout string is refused and changes nothing",
          "[panes][workspace][security]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 80, 24));
    const std::vector<PaneId> order = l.Panes();
    const auto before = l.Rects(80, 24);

    for (const char* bad : { "", "V", "V0.5", "V0.5(", "V0.5(0", "V0.5(0,",
                             "V0.5(0,1", "V0.5(0,1))", "X0.5(0,1)", "0,1",
                             "V0.5(0,0)",           // a pane twice
                             "V0.5(0,9)",           // an index out of range
                             "V0.5(0,1)junk" })
    {
        INFO(bad);
        CHECK_FALSE(l.Deserialize(bad, order));
    }
    // Untouched by every rejection.
    const auto after = l.Rects(80, 24);
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i)
        CHECK(before[i].first == after[i].first);

    SECTION("a deeply nested string cannot overflow the parser")
    {
        std::string deep;
        for (int i = 0; i < 500; ++i)
            deep += "V0.5(0,";
        deep += "1";
        for (int i = 0; i < 500; ++i)
            deep += ")";
        CHECK_FALSE(l.Deserialize(deep, order));
    }
    SECTION("a huge string is refused before it is parsed")
    {
        CHECK_FALSE(l.Deserialize(std::string(100000, 'V'), order));
    }
    SECTION("a zoom is never restored")
    {
        PaneLayout z;
        z.Reset(1);
        REQUIRE(z.Split(1, 2, SplitDir::Vertical, 80, 24));
        z.SetZoom(2);
        const std::vector<PaneId> o = z.Panes();
        const std::string s = z.Serialize(o);
        PaneLayout r;
        r.Reset(1);
        REQUIRE(r.Deserialize(s, o));
        CHECK_FALSE(r.Zoomed());
    }
}

TEST_CASE("rects follow the grid size, so a window resize repositions panes",
          "[panes][resize]")
{
    PaneLayout l;
    l.Reset(1);
    REQUIRE(l.Split(1, 2, SplitDir::Vertical, 200, 60));
    REQUIRE(l.Split(2, 3, SplitDir::Horizontal, 200, 60));
    // A resize storm: every size from tiny to large, checking the invariant
    // at each step. This is what a window drag produces.
    for (int cols = 20; cols <= 300; cols += 7)
        for (int rows = 6; rows <= 80; rows += 11)
        {
            CheckDisjoint(l, cols, rows);
            int totalCells = 0;
            for (const auto& [pid, r] : l.Rects(cols, rows))
                totalCells += r.cols * r.rows;
            // Panes plus dividers account for the whole grid, never more.
            CHECK(totalCells <= cols * rows);
        }
}
