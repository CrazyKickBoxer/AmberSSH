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
    std::string profileId;        // ConnectionProfile::id
    std::string splitProfileId;   // empty when the tab is not split
    bool splitVertical = true;    // side by side, else stacked
};

struct Workspace
{
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
