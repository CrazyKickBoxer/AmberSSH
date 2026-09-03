// Graphemes.h — grapheme clusters in a one-code-point-per-cell terminal.
//
// The problem Stage 4 found: a `Cell` held exactly one `char32_t`, and every
// zero-width code point was DISCARDED. "e" + U+0301 rendered and copied as
// "e"; Hebrew points, Arabic harakat, Thai tones and Devanagari matras were
// all silently dropped. That is text corruption, not a rendering nicety.
//
// The fix has to respect a hard constraint stated in charwidth.h: the number
// of CELLS a sequence occupies must keep agreeing with the server's wcwidth,
// or every erase and redraw after it desyncs. So the cell count stays exactly
// what it was; what changes is that a cell can now carry a whole cluster.
//
// Clusters are interned by content and addressed by an alias code point in
// Supplementary Private Use Area-A. A cell stores the alias in its `cp`, so
// nothing downstream needed a wider Cell or a second lookup path — the glyph
// atlas, the particle pipeline and the GPU cell data are all keyed by
// char32_t and keep working unchanged. Copy and the renderer expand the
// alias; everything else treats it as an opaque code point.
//
// Deliberately NOT merged: ZWJ emoji sequences and skin-tone modifiers. glibc
// wcwidth gives each emoji in a ZWJ sequence its own two columns, so the
// server lays out a family emoji as six columns. Joining it into one cell
// would render it beautifully and then desync the cursor from the shell,
// which is the one failure mode this terminal cannot afford. Regional
// indicator pairs ARE merged, because both halves are width 1 either way, so
// the flag occupies the same two columns whether joined or not.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace amber
{

// Supplementary PUA-A. Nothing legitimate arrives in this range over a
// terminal, and the plane is reserved for exactly this kind of private use.
constexpr char32_t kClusterAliasFirst = 0xF0000;
constexpr char32_t kClusterAliasLast  = 0xFFFFD;

inline bool IsClusterAlias(char32_t cp)
{
    return cp >= kClusterAliasFirst && cp <= kClusterAliasLast;
}

// True for a code point that attaches to the preceding cell rather than
// occupying one of its own: combining marks, joiners and the like. The
// variation selectors are excluded — the grid records those as flags, which
// predates this and keeps working.
bool IsCombining(char32_t cp);

// True for U+1F1E6..U+1F1FF, the halves of a flag.
inline bool IsRegionalIndicator(char32_t cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

class ClusterTable
{
public:
    // A hostile server can emit unbounded distinct sequences, so the table is
    // capped. Past the cap Intern returns 0 and the caller keeps the base
    // character alone — degraded, never unbounded.
    static constexpr size_t kMaxClusters = 4096;
    // A cluster longer than this is truncated. Unicode's own recommendation
    // for a "stream-safe" string is 30 non-starters; 16 is generous for real
    // text and bounds a server that sends ten thousand marks on one base.
    static constexpr size_t kMaxLength = 16;

    // Interns `text` (a base code point followed by its marks) and returns
    // its alias, or 0 when the table is full or the text is not a cluster.
    char32_t Intern(const std::u32string& text);

    // The cluster behind an alias. Empty for anything that is not one.
    const std::u32string& Text(char32_t alias) const;

    size_t Count() const { return m_items.size(); }
    // Interns refused because the table was full — a diagnostics counter.
    size_t Refused() const { return m_refused; }
    // Clusters truncated at kMaxLength.
    size_t Truncated() const { return m_truncated; }
    void Clear();

private:
    std::vector<std::u32string> m_items;
    std::unordered_map<std::u32string, char32_t> m_index;
    size_t m_refused = 0;
    size_t m_truncated = 0;
};

// One table for the process. Clusters are content-addressed, so two sessions
// showing the same accented text share a slot, and an alias means the same
// thing everywhere it is seen.
ClusterTable& Clusters();

// Appends `cp` as UTF-8, expanding a cluster alias back into the code points
// it stands for. Every path that turns cells into text goes through this, so
// a copy, a search or a log gets the real characters back.
void AppendClusterUtf8(std::string& out, char32_t cp);

// The base code point of a cluster (its first element), or `cp` itself. Used
// where only the base matters — width, and the "is this an emoji" question.
char32_t ClusterBase(char32_t cp);

} // namespace amber
