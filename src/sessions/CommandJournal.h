// CommandJournal.h — a searchable record of every command run through
// AmberSSH, built from the OSC 133 shell-integration marks the terminal
// already receives.
//
// Each entry is one command: what was typed, on which host, in which remote
// directory, how long it ran and what it exited with. The data was previously
// captured and discarded (the marks drove the tide bars and the exit flash and
// were then forgotten); this keeps it.
//
// Stored as JSON Lines under %LOCALAPPDATA%\AmberSSH\journal.jsonl so appends
// are cheap and a truncated write can only ever lose the last line.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

struct JournalEntry
{
    std::string host;        // session caption / profile label
    std::string cwd;         // remote working directory (OSC 7), may be empty
    std::string command;     // the text the user actually typed
    int exitCode = 0;        // OSC 133;D code; -1 when the session ended first
    // The link died while this command was running. AmberSSH never saw an
    // exit status and will not invent one: exitCode stays -1 and means
    // "unknown", not "failed".
    bool interrupted = false;
    double durationSec = 0.0;
    int64_t startedAt = 0;   // Unix seconds, for "3h ago"
};

class CommandJournal
{
public:
    // Where the journal lives. Defaults to %LOCALAPPDATA%\AmberSSH\journal.jsonl
    // on first use. An EMPTY path means memory only, with no reads or writes —
    // which is what the tests use so they never touch the real history.
    // Changing the file discards what is held and reloads from the new one.
    void UseFile(const std::string& path);

    // Reads the file if it exists, replacing whatever is held. Idempotent, and
    // never throws: a corrupt or partly written line is skipped, because
    // losing the journal must never block startup. Add() calls this itself, so
    // history can never be truncated by writing before reading.
    void Load();

    // Appends one command and persists it. Commands that begin with a space
    // are dropped, mirroring the shell's own HISTCONTROL=ignorespace
    // convention — that is the established way to say "do not record this
    // one", and it is the escape hatch for a command with a secret in it.
    void Add(JournalEntry e);

    // Newest first.
    const std::vector<JournalEntry>& Entries() const { return m_entries; }
    void Remove(size_t index);
    void Clear();

    // Fuzzy subsequence match over "command host cwd", best first. An empty
    // query returns everything in recency order.
    std::vector<size_t> Search(const std::string& query) const;

    // Ceiling on retained entries; the oldest are dropped past it.
    static constexpr size_t kMaxEntries = 5000;

    // Serialisation, exposed for the tests.
    static std::string ToLine(const JournalEntry& e);
    static bool FromLine(const std::string& line, JournalEntry& out);

private:
    void Save() const;

    std::string FilePath() const;

    std::vector<JournalEntry> m_entries;   // newest at the front
    bool m_loaded = false;
    std::string m_file;                    // empty + m_memoryOnly = no I/O
    bool m_memoryOnly = false;
};

} // namespace amber
