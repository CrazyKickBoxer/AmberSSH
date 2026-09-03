// Workspaces.h — a named set of sessions, reopened in one click.
//
// A workspace records which profiles were open, in what order, and which of
// them were split. Restoring it opens the same set again, so the four boxes
// you touch every morning are one item on the jump list rather than four trips
// through the connection manager.
//
// Stored as JSON Lines under %LOCALAPPDATA%\AmberSSH\workspaces.jsonl, one
// workspace per line, for the same reason the command journal is: appends are
// cheap and a truncated write can only cost the last entry.
#pragma once

#include <string>
#include <vector>

namespace amber
{

struct WorkspaceTab
{
    std::string profileId;        // ConnectionProfile::id — the tab's pane 0
    // ---- schema 1 (still written, still read) --------------------------
    // A single split, which is all the old pane model could express. Kept so
    // a workspace saved by an older build restores, and so an older build
    // can still restore one saved by this one.
    std::string splitProfileId;   // empty when the tab is not split
    bool splitVertical = true;    // side by side, else stacked
    // ---- schema 2 -------------------------------------------------------
    // Every pane after the first, in pane-id order, and the layout tree over
    // all of them as PaneLayout::Serialize writes it. When `layout` is empty
    // the schema-1 fields are what describe the tab.
    std::vector<std::string> paneProfileIds;
    std::string layout;
    // Which pane had focus, as an index into {pane 0} + paneProfileIds.
    int focusPane = 0;
    // Read-only panes, by the same index. Restored: a pane the user locked
    // for watching a production log should come back locked.
    std::vector<int> readOnlyPanes;
    // Broadcast targets are deliberately NOT stored. Restoring a workspace
    // that starts typing into four production hosts at once is not a feature,
    // and the spec says so; the set is always empty on load.
};

struct Workspace
{
    // 1 = the original single-split form; 2 = pane trees. A file written by
    // an older build has no version field and is read as 1.
    static constexpr int kSchemaVersion = 2;
    int version = kSchemaVersion;
    std::string name;
    std::vector<WorkspaceTab> tabs;
};

class WorkspaceStore
{
public:
    // Empty path = memory only, which is what the tests use.
    void UseFile(const std::string& path);
    void Load();

    const std::vector<Workspace>& All() const { return m_items; }
    const Workspace* Find(const std::string& name) const;
    // Replaces any workspace of the same name.
    void Put(Workspace w);
    bool Remove(const std::string& name);

    static std::string ToLine(const Workspace& w);
    static bool FromLine(const std::string& line, Workspace& out);

    static constexpr size_t kMaxWorkspaces = 64;

private:
    std::string FilePath() const;
    void Save() const;

    std::vector<Workspace> m_items;
    bool m_loaded = false;
    std::string m_file;
    bool m_memoryOnly = false;
};

} // namespace amber
