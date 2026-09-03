#include "PaneLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace amber
{

namespace
{

// Splitting works in cells, so the first child's size is a whole number and
// the second gets the remainder — no pane is ever a rounded fraction of a
// cell, and the two always add up to the parent minus the divider.
void SplitRect(const PaneRect& r, SplitDir dir, float ratio, PaneRect& a,
               PaneRect& b)
{
    ratio = std::clamp(ratio, kMinRatio, kMaxRatio);
    a = b = r;
    if (dir == SplitDir::Vertical)
    {
        const int avail = std::max(0, r.cols - 1);          // one divider column
        int first = static_cast<int>(std::lround(avail * ratio));
        first = std::clamp(first, 0, avail);
        a.cols = first;
        b.col = r.col + first + 1;
        b.cols = avail - first;
    }
    else
    {
        const int avail = std::max(0, r.rows - 1);          // one divider row
        int first = static_cast<int>(std::lround(avail * ratio));
        first = std::clamp(first, 0, avail);
        a.rows = first;
        b.row = r.row + first + 1;
        b.rows = avail - first;
    }
}

} // namespace

void PaneLayout::Reset(PaneId root)
{
    m_root = std::make_unique<PaneNode>();
    m_root->pane = root;
    m_zoom = kNoPane;
}

void PaneLayout::Collect(const PaneNode* n, std::vector<PaneId>& out)
{
    if (!n)
        return;
    if (n->IsLeaf())
    {
        if (n->pane != kNoPane)
            out.push_back(n->pane);
        return;
    }
    Collect(n->first.get(), out);
    Collect(n->second.get(), out);
}

std::vector<PaneId> PaneLayout::Panes() const
{
    std::vector<PaneId> out;
    Collect(m_root.get(), out);
    return out;
}

size_t PaneLayout::Count() const
{
    return Panes().size();
}

bool PaneLayout::Has(PaneId id) const
{
    if (id == kNoPane)
        return false;
    const std::vector<PaneId> all = Panes();
    return std::find(all.begin(), all.end(), id) != all.end();
}

// ------------------------------------------------------------------ geometry
void PaneLayout::Walk(const PaneNode* n, const PaneRect& r,
                      std::vector<std::pair<PaneId, PaneRect>>& out)
{
    if (!n)
        return;
    if (n->IsLeaf())
    {
        out.emplace_back(n->pane, r);
        return;
    }
    PaneRect a, b;
    SplitRect(r, n->dir, n->ratio, a, b);
    Walk(n->first.get(), a, out);
    Walk(n->second.get(), b, out);
}

std::vector<std::pair<PaneId, PaneRect>> PaneLayout::Rects(int cols, int rows) const
{
    std::vector<std::pair<PaneId, PaneRect>> out;
    if (!m_root)
        return out;
    const PaneRect full{ 0, 0, std::max(0, cols), std::max(0, rows) };
    // Zoom gives one pane the whole area and everyone else nothing. The tree
    // is not modified, which is why restoring is exact.
    if (m_zoom != kNoPane && Has(m_zoom))
    {
        for (PaneId id : Panes())
            out.emplace_back(id, id == m_zoom ? full : PaneRect{});
        return out;
    }
    Walk(m_root.get(), full, out);
    return out;
}

PaneRect PaneLayout::RectOf(PaneId id, int cols, int rows) const
{
    for (const auto& [pid, r] : Rects(cols, rows))
        if (pid == id)
            return r;
    return {};
}

PaneId PaneLayout::PaneAt(int col, int row, int cols, int rows) const
{
    for (const auto& [pid, r] : Rects(cols, rows))
        if (r.cols > 0 && r.rows > 0 && r.Contains(col, row))
            return pid;
    return kNoPane;
}

void PaneLayout::WalkDividers(
    const PaneNode* n, const PaneRect& r,
    std::vector<std::tuple<PaneRect, bool, PaneId, PaneId>>& out)
{
    if (!n || n->IsLeaf())
        return;
    PaneRect a, b;
    SplitRect(r, n->dir, n->ratio, a, b);
    // The divider is the cell line between the two children.
    PaneRect d;
    if (n->dir == SplitDir::Vertical)
        d = PaneRect{ a.col + a.cols, r.row, 1, r.rows };
    else
        d = PaneRect{ r.col, a.row + a.rows, r.cols, 1 };
    std::vector<PaneId> left, right;
    Collect(n->first.get(), left);
    Collect(n->second.get(), right);
    out.emplace_back(d, n->dir == SplitDir::Vertical,
                     left.empty() ? kNoPane : left.back(),
                     right.empty() ? kNoPane : right.front());
    WalkDividers(n->first.get(), a, out);
    WalkDividers(n->second.get(), b, out);
}

bool PaneLayout::DividerAt(int col, int row, int cols, int rows, bool& vertical,
                           PaneId& before, PaneId& after) const
{
    if (!m_root || Zoomed())
        return false;
    std::vector<std::tuple<PaneRect, bool, PaneId, PaneId>> divs;
    WalkDividers(m_root.get(), PaneRect{ 0, 0, std::max(0, cols), std::max(0, rows) },
                 divs);
    for (const auto& [r, vert, a, b] : divs)
    {
        if (!r.Contains(col, row))
            continue;
        vertical = vert;
        before = a;
        after = b;
        return true;
    }
    return false;
}

// ----------------------------------------------------------------- structure
PaneNode* PaneLayout::LeafOf(PaneId id) const
{
    if (!m_root || id == kNoPane)
        return nullptr;
    // Iterative, so a pathological tree cannot overflow the stack.
    std::vector<PaneNode*> stack{ m_root.get() };
    while (!stack.empty())
    {
        PaneNode* n = stack.back();
        stack.pop_back();
        if (n->IsLeaf())
        {
            if (n->pane == id)
                return n;
            continue;
        }
        stack.push_back(n->first.get());
        stack.push_back(n->second.get());
    }
    return nullptr;
}

PaneNode* PaneLayout::ParentOf(PaneId id, bool* isFirst) const
{
    if (!m_root || id == kNoPane)
        return nullptr;
    std::vector<PaneNode*> stack{ m_root.get() };
    while (!stack.empty())
    {
        PaneNode* n = stack.back();
        stack.pop_back();
        if (n->IsLeaf())
            continue;
        if (n->first->IsLeaf() && n->first->pane == id)
        {
            if (isFirst)
                *isFirst = true;
            return n;
        }
        if (n->second->IsLeaf() && n->second->pane == id)
        {
            if (isFirst)
                *isFirst = false;
            return n;
        }
        stack.push_back(n->first.get());
        stack.push_back(n->second.get());
    }
    return nullptr;
}

bool PaneLayout::Split(PaneId target, PaneId newPane, SplitDir dir, int cols,
                       int rows)
{
    if (newPane == kNoPane || Has(newPane))
        return false;
    PaneNode* leaf = LeafOf(target);
    if (!leaf)
        return false;
    // Refuse rather than create a pane too small to be a terminal. Checked
    // against the target's CURRENT rect, so a split deep in the tree is
    // judged on the space it actually has.
    const PaneRect r = RectOf(target, cols, rows);
    if (dir == SplitDir::Vertical)
    {
        if (r.cols < kMinPaneCols * 2 + 1)
            return false;
    }
    else if (r.rows < kMinPaneRows * 2 + 1)
        return false;

    auto a = std::make_unique<PaneNode>();
    auto b = std::make_unique<PaneNode>();
    a->pane = target;
    b->pane = newPane;
    leaf->pane = kNoPane;
    leaf->dir = dir;
    leaf->ratio = 0.5f;
    leaf->first = std::move(a);
    leaf->second = std::move(b);
    // A split while zoomed would leave the new pane invisible with no way to
    // reach it; the zoom is dropped instead.
    m_zoom = kNoPane;
    return true;
}

bool PaneLayout::Close(PaneId id)
{
    if (!m_root || Count() <= 1)
        return false;
    bool isFirst = false;
    PaneNode* parent = ParentOf(id, &isFirst);
    if (!parent)
        return false;
    // The sibling subtree replaces the split, so closing a pane inside a
    // nested layout collapses exactly one level.
    std::unique_ptr<PaneNode> keep =
        isFirst ? std::move(parent->second) : std::move(parent->first);
    parent->first.reset();
    parent->second.reset();
    parent->pane = keep->pane;
    parent->dir = keep->dir;
    parent->ratio = keep->ratio;
    parent->first = std::move(keep->first);
    parent->second = std::move(keep->second);
    if (m_zoom == id)
        m_zoom = kNoPane;
    return true;
}

bool PaneLayout::Swap(PaneId a, PaneId b)
{
    if (a == b)
        return false;
    PaneNode* na = LeafOf(a);
    PaneNode* nb = LeafOf(b);
    if (!na || !nb)
        return false;
    std::swap(na->pane, nb->pane);
    return true;
}

bool PaneLayout::Rotate(PaneId id)
{
    PaneNode* parent = ParentOf(id);
    if (!parent)
        return false;
    parent->dir = parent->dir == SplitDir::Vertical ? SplitDir::Horizontal
                                                    : SplitDir::Vertical;
    return true;
}

bool PaneLayout::Move(PaneId id, PaneId neighbour, SplitDir dir, int cols,
                      int rows)
{
    if (id == neighbour || !Has(id) || !Has(neighbour) || Count() <= 1)
        return false;
    // Remove it, then re-split the destination. Doing it in that order means
    // the space freed by the removal is available to the new split, so moving
    // a pane into a tight corner behaves the same as splitting there would.
    if (!Close(id))
        return false;
    if (Split(neighbour, id, dir, cols, rows))
        return true;
    // The destination could not take it. Put it back beside whatever is left
    // rather than losing the pane: a failed move must not destroy anything.
    const std::vector<PaneId> rest = Panes();
    if (rest.empty())
        return false;
    for (PaneId host : rest)
    {
        if (Split(host, id, SplitDir::Vertical, cols, rows) ||
            Split(host, id, SplitDir::Horizontal, cols, rows))
            return false;   // restored, but the move did not happen
    }
    // Nothing had room at all. The tree is smaller than it was, which is the
    // one case where a caller must re-add the pane itself.
    return false;
}

bool PaneLayout::Resize(PaneId id, float delta)
{
    bool isFirst = false;
    PaneNode* parent = ParentOf(id, &isFirst);
    if (!parent)
        return false;
    // Positive delta always grows the named pane, whichever side it is on.
    const float next = parent->ratio + (isFirst ? delta : -delta);
    parent->ratio = std::clamp(next, kMinRatio, kMaxRatio);
    return true;
}

bool PaneLayout::SetRatio(PaneId id, float ratio)
{
    PaneNode* parent = ParentOf(id);
    if (!parent)
        return false;
    parent->ratio = std::clamp(ratio, kMinRatio, kMaxRatio);
    return true;
}

float PaneLayout::RatioOf(PaneId id) const
{
    PaneNode* parent = ParentOf(id);
    return parent ? parent->ratio : 1.0f;
}

// ---------------------------------------------------------------- navigation
PaneId PaneLayout::Neighbour(PaneId from, Dir d, int cols, int rows) const
{
    // Geometry, not tree shape: what the user means by "the pane to the left"
    // is the one that looks left, however the tree happens to be nested.
    const auto rects = Rects(cols, rows);
    PaneRect me{};
    bool found = false;
    for (const auto& [pid, r] : rects)
        if (pid == from)
        {
            me = r;
            found = true;
        }
    if (!found || me.cols <= 0)
        return kNoPane;
    PaneId best = kNoPane;
    int bestGap = 0;
    for (const auto& [pid, r] : rects)
    {
        if (pid == from || r.cols <= 0 || r.rows <= 0)
            continue;
        int gap = 0;
        bool ok = false;
        switch (d)
        {
        case Dir::Left:
            ok = r.col + r.cols <= me.col &&
                 r.row < me.row + me.rows && r.row + r.rows > me.row;
            gap = me.col - (r.col + r.cols);
            break;
        case Dir::Right:
            ok = r.col >= me.col + me.cols &&
                 r.row < me.row + me.rows && r.row + r.rows > me.row;
            gap = r.col - (me.col + me.cols);
            break;
        case Dir::Up:
            ok = r.row + r.rows <= me.row &&
                 r.col < me.col + me.cols && r.col + r.cols > me.col;
            gap = me.row - (r.row + r.rows);
            break;
        case Dir::Down:
            ok = r.row >= me.row + me.rows &&
                 r.col < me.col + me.cols && r.col + r.cols > me.col;
            gap = r.row - (me.row + me.rows);
            break;
        }
        if (!ok)
            continue;
        if (best == kNoPane || gap < bestGap)
        {
            best = pid;
            bestGap = gap;
        }
    }
    return best;
}

PaneId PaneLayout::Cycle(PaneId from, int delta) const
{
    const std::vector<PaneId> all = Panes();
    if (all.empty())
        return kNoPane;
    auto it = std::find(all.begin(), all.end(), from);
    if (it == all.end())
        return all.front();
    const int n = static_cast<int>(all.size());
    int i = static_cast<int>(it - all.begin()) + delta;
    i = ((i % n) + n) % n;
    return all[static_cast<size_t>(i)];
}

// -------------------------------------------------------------- serialisation
namespace
{

void Write(const PaneNode* n, const std::vector<PaneId>& order, std::string& out)
{
    if (!n)
        return;
    if (n->IsLeaf())
    {
        auto it = std::find(order.begin(), order.end(), n->pane);
        // A pane that is not in `order` cannot be named, so it is written as
        // index 0; the caller builds `order` from Panes() and never hits this.
        out += std::to_string(it == order.end() ? 0 : (it - order.begin()));
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%c%.3f(", n->dir == SplitDir::Vertical ? 'V' : 'H',
             n->ratio);
    out += buf;
    Write(n->first.get(), order, out);
    out += ',';
    Write(n->second.get(), order, out);
    out += ')';
}

// Recursive-descent parse of the same grammar, bounded in depth so a hostile
// or corrupt workspace line cannot overflow the stack.
constexpr int kMaxParseDepth = 32;

std::unique_ptr<PaneNode> Read(const std::string& s, size_t& i,
                               const std::vector<PaneId>& order, int depth,
                               bool& bad)
{
    if (bad || depth > kMaxParseDepth || i >= s.size())
    {
        bad = true;
        return nullptr;
    }
    const char c = s[i];
    if (c == 'V' || c == 'H')
    {
        auto n = std::make_unique<PaneNode>();
        n->dir = (c == 'V') ? SplitDir::Vertical : SplitDir::Horizontal;
        ++i;
        // ratio
        size_t start = i;
        while (i < s.size() && (isdigit(static_cast<unsigned char>(s[i])) ||
                                s[i] == '.' || s[i] == '-'))
            ++i;
        n->ratio = std::clamp(static_cast<float>(atof(s.substr(start, i - start).c_str())),
                              kMinRatio, kMaxRatio);
        if (i >= s.size() || s[i] != '(')
        {
            bad = true;
            return nullptr;
        }
        ++i;
        n->first = Read(s, i, order, depth + 1, bad);
        if (bad || i >= s.size() || s[i] != ',')
        {
            bad = true;
            return nullptr;
        }
        ++i;
        n->second = Read(s, i, order, depth + 1, bad);
        if (bad || i >= s.size() || s[i] != ')')
        {
            bad = true;
            return nullptr;
        }
        ++i;
        return n;
    }
    if (isdigit(static_cast<unsigned char>(c)))
    {
        size_t start = i;
        while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])))
            ++i;
        const size_t idx = static_cast<size_t>(atoi(s.substr(start, i - start).c_str()));
        if (idx >= order.size())
        {
            bad = true;
            return nullptr;
        }
        auto n = std::make_unique<PaneNode>();
        n->pane = order[idx];
        return n;
    }
    bad = true;
    return nullptr;
}

} // namespace

std::string PaneLayout::Serialize(const std::vector<PaneId>& order) const
{
    std::string out;
    Write(m_root.get(), order, out);
    return out;
}

bool PaneLayout::Deserialize(const std::string& text,
                             const std::vector<PaneId>& order)
{
    if (text.empty() || order.empty() || text.size() > 4096)
        return false;
    size_t i = 0;
    bool bad = false;
    std::unique_ptr<PaneNode> root = Read(text, i, order, 0, bad);
    // Trailing junk is a malformed line, not a layout with extra bytes.
    if (bad || !root || i != text.size())
        return false;
    // Every pane must appear exactly once, or the layout would hide a live
    // session or show one twice.
    std::vector<PaneId> got;
    Collect(root.get(), got);
    std::vector<PaneId> sortedGot = got, sortedWant = order;
    std::sort(sortedGot.begin(), sortedGot.end());
    std::sort(sortedWant.begin(), sortedWant.end());
    if (sortedGot != sortedWant)
        return false;
    m_root = std::move(root);
    m_zoom = kNoPane;      // never restore a zoom: the spec says unzoomed
    return true;
}

} // namespace amber
