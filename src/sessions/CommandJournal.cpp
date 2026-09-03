#include "CommandJournal.h"

#include "../platform/Paths.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace amber
{
namespace
{

std::string JsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            }
            else
                o += static_cast<char>(c);
        }
    }
    return o;
}

// Minimal field reader: the file is only ever written by ToLine, so a full
// JSON parser would be weight for nothing. Unknown or reordered fields are
// tolerated; anything malformed makes the caller skip the line.
bool FieldStr(const std::string& line, const char* key, std::string& out)
{
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = line.find(pat);
    if (p == std::string::npos)
        return false;
    p += pat.size();
    out.clear();
    for (size_t i = p; i < line.size(); ++i)
    {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size())
        {
            char n = line[++i];
            switch (n)
            {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
                // Only control-character escapes are ever written.
                if (i + 4 < line.size())
                {
                    int v = std::stoi(line.substr(i + 1, 4), nullptr, 16);
                    if (v > 0 && v < 0x80)
                        out += static_cast<char>(v);
                    i += 4;
                }
                break;
            default: out += n; break;
            }
            continue;
        }
        if (c == '"')
            return true;
        out += c;
    }
    return false;
}

bool FieldNum(const std::string& line, const char* key, double& out)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = line.find(pat);
    if (p == std::string::npos)
        return false;
    p += pat.size();
    try
    {
        out = std::stod(line.substr(p));
    }
    catch (...)
    {
        return false;
    }
    return true;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::string CommandJournal::ToLine(const JournalEntry& e)
{
    std::ostringstream o;
    o << "{\"t\":" << e.startedAt
      << ",\"d\":" << e.durationSec
      << ",\"x\":" << e.exitCode
      << ",\"i\":" << (e.interrupted ? 1 : 0)
      << ",\"b\":" << e.blockId
      << ",\"k\":\"" << JsonEscape(e.sessionKey)
      << "\",\"h\":\"" << JsonEscape(e.host)
      << "\",\"w\":\"" << JsonEscape(e.cwd)
      << "\",\"c\":\"" << JsonEscape(e.command) << "\"}";
    return o.str();
}

bool CommandJournal::FromLine(const std::string& line, JournalEntry& out)
{
    if (line.size() < 2 || line.front() != '{')
        return false;
    JournalEntry e;
    if (!FieldStr(line, "c", e.command) || e.command.empty())
        return false;
    FieldStr(line, "h", e.host);
    FieldStr(line, "w", e.cwd);
    double v = 0.0;
    if (FieldNum(line, "t", v))
        e.startedAt = static_cast<int64_t>(v);
    if (FieldNum(line, "d", v))
        e.durationSec = v;
    if (FieldNum(line, "x", v))
        e.exitCode = static_cast<int>(v);
    if (FieldNum(line, "i", v))
        e.interrupted = v != 0.0;
    if (FieldNum(line, "b", v))
        e.blockId = static_cast<uint64_t>(v < 0.0 ? 0.0 : v);
    FieldStr(line, "k", e.sessionKey);
    out = std::move(e);
    return true;
}

std::string CommandJournal::FilePath() const
{
    if (!m_file.empty())
        return m_file;
    try
    {
        return (DataRoot() / "journal.jsonl").string();
    }
    catch (...)
    {
        return {};
    }
}

void CommandJournal::UseFile(const std::string& path)
{
    m_file = path;
    m_memoryOnly = path.empty();
    m_loaded = false;
    m_entries.clear();
    Load();
}

void CommandJournal::Load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    // Defensive: loading must REPLACE, never append. Adding before loading
    // would otherwise produce every entry twice, and the Save() that follows
    // would write the duplicates back to disk.
    m_entries.clear();
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
            JournalEntry e;
            if (FromLine(line, e))
                m_entries.push_back(std::move(e));
        }
    }
    catch (...)
    {
        // A missing or unreadable journal is not worth failing startup over.
        return;
    }
    // The file is written oldest-first; the in-memory list is newest-first.
    std::reverse(m_entries.begin(), m_entries.end());
    if (m_entries.size() > kMaxEntries)
        m_entries.resize(kMaxEntries);
}

void CommandJournal::Add(JournalEntry e)
{
    // Adding before loading would make the Save() below truncate the file to
    // just this session's commands, destroying the existing history. Load
    // first, always — it is a no-op once done.
    Load();
    // Leading space means "do not record", exactly as in the shell.
    if (e.command.empty() || e.command.front() == ' ')
        return;
    while (!e.command.empty() &&
           (e.command.back() == ' ' || e.command.back() == '\t'))
        e.command.pop_back();
    if (e.command.empty())
        return;
    // Collapse an immediate repeat into one entry rather than a run of them.
    if (!m_entries.empty() && m_entries.front().command == e.command &&
        m_entries.front().host == e.host)
        m_entries.erase(m_entries.begin());
    m_entries.insert(m_entries.begin(), std::move(e));
    if (m_entries.size() > kMaxEntries)
        m_entries.resize(kMaxEntries);
    Save();
}

void CommandJournal::Remove(size_t index)
{
    if (index >= m_entries.size())
        return;
    m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(index));
    Save();
}

void CommandJournal::Clear()
{
    m_entries.clear();
    Save();
}

void CommandJournal::Save() const
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
        // Oldest first on disk, so a tail of the file is the recent history.
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
            out << ToLine(*it) << "\n";
    }
    catch (...)
    {
        return;
    }
}

std::vector<size_t> CommandJournal::Search(const std::string& query) const
{
    const std::string q = Lower(query);
    std::vector<std::pair<int, size_t>> scored;
    scored.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const JournalEntry& e = m_entries[i];
        // The command carries the most weight, but the host and directory are
        // searchable too, so "web nginx" finds it on the right box.
        const std::string hay =
            Lower(e.command + " " + e.host + " " + e.cwd);
        int score = 0;
        if (!q.empty())
        {
            size_t from = 0;
            int prev = -2;
            bool ok = true;
            for (char qc : q)
            {
                if (qc == ' ')
                    continue;
                size_t f = hay.find(qc, from);
                if (f == std::string::npos)
                {
                    ok = false;
                    break;
                }
                score += 10;
                if (static_cast<int>(f) == prev + 1)
                    score += 8;                       // adjacent run
                if (f == 0 || hay[f - 1] == ' ' || hay[f - 1] == '/')
                    score += 6;                       // word start
                if (f < e.command.size())
                    score += 4;                       // hit in the command
                prev = static_cast<int>(f);
                from = f + 1;
            }
            if (!ok)
                continue;
            score -= static_cast<int>(hay.size()) / 16;
        }
        // Recency breaks ties: i is already newest-first.
        score -= static_cast<int>(i / 64);
        scored.push_back({ score, i });
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<size_t> out;
    out.reserve(scored.size());
    for (const auto& s : scored)
        out.push_back(s.second);
    return out;
}

} // namespace amber
