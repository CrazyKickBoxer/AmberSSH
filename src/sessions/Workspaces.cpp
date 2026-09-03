#include "Workspaces.h"

#include "../platform/Paths.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace amber
{
namespace
{

std::string Esc(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            o += '\\';
        if (static_cast<unsigned char>(c) < 0x20)
            continue;                 // ids and names never contain controls
        o += c;
    }
    return o;
}

// Reads "key":"value" out of the line, starting the search at `from` so
// repeated keys inside the tab array can be walked in order.
bool Field(const std::string& line, const char* key, size_t& from, std::string& out)
{
    const std::string pat = std::string("\"") + key + "\":\"";
    size_t p = line.find(pat, from);
    if (p == std::string::npos)
        return false;
    p += pat.size();
    out.clear();
    for (size_t i = p; i < line.size(); ++i)
    {
        if (line[i] == '\\' && i + 1 < line.size())
        {
            out += line[++i];
            continue;
        }
        if (line[i] == '"')
        {
            from = i + 1;
            return true;
        }
        out += line[i];
    }
    return false;
}

} // namespace

std::string WorkspaceStore::ToLine(const Workspace& w)
{
    std::ostringstream o;
    o << "{\"n\":\"" << Esc(w.name) << "\",\"t\":[";
    for (size_t i = 0; i < w.tabs.size(); ++i)
    {
        const WorkspaceTab& t = w.tabs[i];
        if (i)
            o << ',';
        o << "{\"p\":\"" << Esc(t.profileId) << "\",\"s\":\""
          << Esc(t.splitProfileId) << "\",\"v\":" << (t.splitVertical ? 1 : 0)
          << "}";
    }
    o << "]}";
    return o.str();
}

bool WorkspaceStore::FromLine(const std::string& line, Workspace& out)
{
    if (line.size() < 2 || line.front() != '{')
        return false;
    size_t from = 0;
    Workspace w;
    if (!Field(line, "n", from, w.name) || w.name.empty())
        return false;
    // Each tab contributes a "p" then an "s"; walking them in order keeps the
    // tab order, which is the whole point of a workspace.
    for (;;)
    {
        size_t probe = from;
        WorkspaceTab t;
        if (!Field(line, "p", probe, t.profileId))
            break;
        size_t afterP = probe;
        if (!Field(line, "s", probe, t.splitProfileId))
            t.splitProfileId.clear();
        size_t vpos = line.find("\"v\":", afterP);
        t.splitVertical = !(vpos != std::string::npos && vpos < probe + 8 &&
                            line[vpos + 4] == '0');
        if (t.profileId.empty())
            break;
        w.tabs.push_back(std::move(t));
        from = probe;
    }
    if (w.tabs.empty())
        return false;
    out = std::move(w);
    return true;
}

std::string WorkspaceStore::FilePath() const
{
    if (!m_file.empty())
        return m_file;
    try
    {
        return (DataRoot() / "workspaces.jsonl").string();
    }
    catch (...)
    {
        return {};
    }
}

void WorkspaceStore::UseFile(const std::string& path)
{
    m_file = path;
    m_memoryOnly = path.empty();
    m_loaded = false;
    m_items.clear();
    Load();
}

void WorkspaceStore::Load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    m_items.clear();          // replace, never append
    if (m_memoryOnly)
        return;
    const std::string path = FilePath();
    if (path.empty())
        return;
    try
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            Workspace w;
            if (FromLine(line, w))
                m_items.push_back(std::move(w));
        }
    }
    catch (...)
    {
        return;
    }
}

const Workspace* WorkspaceStore::Find(const std::string& name) const
{
    for (const Workspace& w : m_items)
        if (w.name == name)
            return &w;
    return nullptr;
}

void WorkspaceStore::Put(Workspace w)
{
    Load();
    if (w.name.empty() || w.tabs.empty())
        return;
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
    {
        if (it->name == w.name)
        {
            *it = std::move(w);
            Save();
            return;
        }
    }
    if (m_items.size() >= kMaxWorkspaces)
        m_items.erase(m_items.begin());
    m_items.push_back(std::move(w));
    Save();
}

bool WorkspaceStore::Remove(const std::string& name)
{
    Load();
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
    {
        if (it->name == name)
        {
            m_items.erase(it);
            Save();
            return true;
        }
    }
    return false;
}

void WorkspaceStore::Save() const
{
    if (m_memoryOnly)
        return;
    const std::string path = FilePath();
    if (path.empty())
        return;
    try
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        for (const Workspace& w : m_items)
            out << ToLine(w) << "\n";
    }
    catch (...)
    {
        return;
    }
}

} // namespace amber
