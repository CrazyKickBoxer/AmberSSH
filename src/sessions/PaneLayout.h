// PaneLayout.h — the recursive pane tree for one tab.
//
// What was here before: a tab was one Session with an optional second one in
// `Session::pane`, split 50/50 either vertically or horizontally, one level
// deep. Every renderer path special-cased "the session" and "its pane" with
// hardcoded offsets.
//
// This is a real binary layout tree instead: a leaf holds a pane id, a split
// holds two children, an orientation and a ratio. Arbitrary nesting works
// because either child of a split can itself be a split.
//
// Deliberately free of Windows, sessions and the renderer. The tree stores
// pane IDs, not Session pointers, so every operation the spec asks for —
// split, close, resize, swap, move, rotate, zoom, restore — is a pure
// transformation that can be tested exhaustively without a terminal. The app
// maps an id back to a session.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace amber
{

// A pane's identity within its tab. Stable for the pane's life, never reused
// while the tab is open, so a broadcast set or a focus reference cannot come
// to mean a different pane than the one it was pointed at.
using PaneId = int;
constexpr PaneId kNoPane = -1;

enum class SplitDir { Vertical = 0, Horizontal = 1 };   // side by side / stacked

// A rectangle in CELLS, which is the only unit the terminal has. Splitting
// works in cells so a pane's column and row count is exact rather than a
// rounded fraction of pixels.
struct PaneRect
{
    int col = 0, row = 0, cols = 0, rows = 0;
    bool Contains(int c, int r) const
    {
        return c >= col && c < col + cols && r >= row && r < row + rows;
    }
};

struct PaneNode
{
    // Leaf.
    PaneId pane = kNoPane;
    // Split (both children non-null exactly when this is not a leaf).
    SplitDir dir = SplitDir::Vertical;
    float ratio = 0.5f;                  // first child's share of the axis
    std::unique_ptr<PaneNode> first, second;

    bool IsLeaf() const { return !first && !second; }
};

// Every split consumes one row or column for its divider, so a pane can never
// be smaller than this and a split is refused when there is not room.
constexpr int kMinPaneCols = 8;
constexpr int kMinPaneRows = 3;
// The ratio is clamped so a pane cannot be dragged out of existence.
constexpr float kMinRatio = 0.1f;
constexpr float kMaxRatio = 0.9f;

class PaneLayout
{
public:
    PaneLayout() = default;
    PaneLayout(const PaneLayout&) = delete;
    PaneLayout& operator=(const PaneLayout&) = delete;

    // A tab always has at least one pane. Reset makes `root` the only one.
    void Reset(PaneId root);
    bool Empty() const { return !m_root; }
    // Panes in layout order, left-to-right then top-to-bottom.
    std::vector<PaneId> Panes() const;
    size_t Count() const;

    // --- geometry ---------------------------------------------------------
    // Every pane's rect inside a grid of `cols` x `rows` cells. Dividers take
    // one cell each and belong to no pane. The zoomed pane, if any, gets the
    // whole area and everything else gets an empty rect.
    std::vector<std::pair<PaneId, PaneRect>> Rects(int cols, int rows) const;
    // The rect of one pane, or an empty rect when it is not visible.
    PaneRect RectOf(PaneId id, int cols, int rows) const;
    // The pane at a cell, or kNoPane for a divider or outside.
    PaneId PaneAt(int col, int row, int cols, int rows) const;
    // The divider whose cell this is, identified by the split node it belongs
    // to; returns false when the cell is not on a divider. `vertical` says
    // which way the divider runs, for the resize cursor.
    bool DividerAt(int col, int row, int cols, int rows, bool& vertical,
                   PaneId& before, PaneId& after) const;

    // --- structure --------------------------------------------------------
    // Splits `target`, putting `newPane` on the far side. Returns false when
    // the target is unknown or there is not room for two panes.
    bool Split(PaneId target, PaneId newPane, SplitDir dir, int cols, int rows);
    // Removes a pane; its sibling takes the whole space. Returns false for
    // the last pane — a tab always has one.
    bool Close(PaneId id);
    // Exchanges two panes' positions, leaving the tree shape alone.
    bool Swap(PaneId a, PaneId b);
    // Flips the orientation of the split that contains `id`.
    bool Rotate(PaneId id);
    // Moves `id` next to `neighbour`, splitting it in `dir`. Used by the
    // keyboard "move pane" commands and by a pane drag.
    bool Move(PaneId id, PaneId neighbour, SplitDir dir, int cols, int rows);
    // Nudges the divider that separates `id` from its sibling. `delta` is a
    // fraction of the axis; positive grows `id`.
    bool Resize(PaneId id, float delta);
    // Sets the ratio of the split containing `id` directly, for a drag.
    bool SetRatio(PaneId id, float ratio);
    float RatioOf(PaneId id) const;

    // --- zoom -------------------------------------------------------------
    // Zoom is a view state, never a structural change: the tree is untouched,
    // so restoring returns the exact previous layout by construction rather
    // than by remembering it.
    void SetZoom(PaneId id) { m_zoom = id; }
    PaneId Zoom() const { return m_zoom; }
    void ClearZoom() { m_zoom = kNoPane; }
    bool Zoomed() const { return m_zoom != kNoPane && Has(m_zoom); }

    bool Has(PaneId id) const;

    // --- navigation -------------------------------------------------------
    // The pane in a direction from `from`, by geometry rather than by tree
    // shape: the nearest pane whose rect lies that way and overlaps on the
    // other axis. kNoPane when there is none.
    enum class Dir { Left, Right, Up, Down };
    PaneId Neighbour(PaneId from, Dir d, int cols, int rows) const;
    // Layout order, wrapping. delta +1 = next.
    PaneId Cycle(PaneId from, int delta) const;

    // --- serialisation ----------------------------------------------------
    // A compact form for the workspace file: leaves are their pane index in
    // `order`, splits are "V<ratio>(a,b)" / "H<ratio>(a,b)". Chosen over JSON
    // nesting so an old reader that does not understand it can skip one
    // string rather than fail to parse a whole workspace.
    std::string Serialize(const std::vector<PaneId>& order) const;
    // Rebuilds from that form. `order` maps an index back to a pane id.
    // Returns false on anything malformed, leaving the layout untouched.
    bool Deserialize(const std::string& text, const std::vector<PaneId>& order);

private:
    static void Collect(const PaneNode* n, std::vector<PaneId>& out);
    static void Walk(const PaneNode* n, const PaneRect& r,
                     std::vector<std::pair<PaneId, PaneRect>>& out);
    static void WalkDividers(const PaneNode* n, const PaneRect& r,
                             std::vector<std::tuple<PaneRect, bool, PaneId, PaneId>>& out);
    // The split node directly above the leaf holding `id`, and which side it
    // is on. Returns nullptr for the root leaf.
    PaneNode* ParentOf(PaneId id, bool* isFirst = nullptr) const;
    PaneNode* LeafOf(PaneId id) const;

    std::unique_ptr<PaneNode> m_root;
    PaneId m_zoom = kNoPane;
};

} // namespace amber
