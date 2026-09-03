#include "Graphemes.h"

#include "../common.h"
#include "charwidth.h"

namespace amber
{

bool IsCombining(char32_t cp)
{
    // The variation selectors are the grid's business, not ours: it records
    // them as CellEmojiVS / CellTextVS flags and re-emits them on copy.
    if (cp == 0xFE0F || cp == 0xFE0E)
        return false;
    // U+200D ZERO WIDTH JOINER is width 0 but is deliberately not treated as
    // combining — see the note in the header about ZWJ emoji and wcwidth.
    if (cp == 0x200D)
        return false;
    if (cp < 0x0300)
        return false;
    return TermCharWidth(cp) == 0;
}

char32_t ClusterTable::Intern(const std::u32string& text)
{
    if (text.size() < 2)
        return 0;                       // a lone base is not a cluster
    std::u32string key = text;
    if (key.size() > kMaxLength)
    {
        key.resize(kMaxLength);
        ++m_truncated;
    }
    auto it = m_index.find(key);
    if (it != m_index.end())
        return it->second;
    if (m_items.size() >= kMaxClusters)
    {
        ++m_refused;
        return 0;
    }
    const char32_t alias =
        kClusterAliasFirst + static_cast<char32_t>(m_items.size());
    if (alias > kClusterAliasLast)
    {
        ++m_refused;
        return 0;
    }
    m_items.push_back(key);
    m_index.emplace(std::move(key), alias);
    return alias;
}

const std::u32string& ClusterTable::Text(char32_t alias) const
{
    static const std::u32string none;
    if (!IsClusterAlias(alias))
        return none;
    const size_t i = static_cast<size_t>(alias - kClusterAliasFirst);
    return i < m_items.size() ? m_items[i] : none;
}

void ClusterTable::Clear()
{
    m_items.clear();
    m_index.clear();
    m_refused = 0;
    m_truncated = 0;
}

ClusterTable& Clusters()
{
    static ClusterTable table;
    return table;
}

void AppendClusterUtf8(std::string& out, char32_t cp)
{
    if (!IsClusterAlias(cp))
    {
        AppendUtf8(out, cp);
        return;
    }
    const std::u32string& text = Clusters().Text(cp);
    if (text.empty())
    {
        // An alias with nothing behind it can only mean the table was cleared
        // under a grid that still references it. Emit the replacement
        // character rather than a private-use code point that would paste as
        // a box into another application.
        AppendUtf8(out, U'�');
        return;
    }
    for (char32_t c : text)
        AppendUtf8(out, c);
}

char32_t ClusterBase(char32_t cp)
{
    if (!IsClusterAlias(cp))
        return cp;
    const std::u32string& text = Clusters().Text(cp);
    return text.empty() ? U'�' : text[0];
}

} // namespace amber
