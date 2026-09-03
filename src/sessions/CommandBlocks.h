// CommandBlocks.h — a command, as metadata pointing at rows of the grid.
//
// Stage 3's rule, and the reason nothing in this file owns any text: a block
// is a REFERENCE into the canonical scrollback, never a second copy of it and
// never a second terminal model. Selection, search, copy, the alternate
// screen and escape processing all keep working on the grid; blocks only say
// which rows meant what.
//
// Everything here is built from the OSC 133 marks AmberSSH already receives
// (A prompt start, B input start, C output start, D exit status) plus OSC 7
// for the directory. There is no second shell parser.
//
// Pure and deterministic — no Windows, no terminal, no renderer — so the
// summary text, the trimming rules and the notification policy are all
// unit-testable.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

// A row id is Grid::TotalPushed() + screen row: monotonic, and stable while
// the row lives. It is NOT durable — the row falls out of scrollback when
// enough output pushes past it, and a block that references trimmed rows is
// dropped rather than left pointing at nothing.
using RowId = uint64_t;

struct CommandBlock
{
    uint64_t id = 0;              // session-local, monotonic; 0 = invalid
    // Provenance, shared with the command journal so the two never become
    // separate histories.
    std::string sessionKey;       // profile id, or the session label

    // --- the ranges this block references -------------------------------
    RowId promptRow = 0;          // OSC 133 A — where the prompt began
    RowId inputRow = 0;           // OSC 133 B — where the user's typing began
    int   inputCol = -1;          // column of that mark, -1 = unknown
    RowId outputFirst = 0;        // first output row, inclusive
    RowId outputLast = 0;         // last output row, inclusive
    bool  hasOutput = false;      // false: the command printed nothing

    // --- what it was ----------------------------------------------------
    std::string command;          // lifted between B and the cursor at C
    std::string cwd;              // OSC 7 at the time it started

    // --- how it went ----------------------------------------------------
    int64_t startedAt = 0;        // Unix seconds
    int64_t endedAt = 0;          // Unix seconds, 0 while running
    double  durationSec = 0.0;
    int     exitCode = 0;
    bool    hasExit = false;      // false = AmberSSH never saw a status
    bool    interrupted = false;  // the link died mid-command
    bool    running = false;      // between C and D
    int     lines = 0;            // output rows
    uint64_t bytes = 0;           // output bytes, when they were countable

    // --- local state, never sent anywhere -------------------------------
    bool collapsed = false;
    bool bookmarked = false;
    std::u32string summary;       // rendered in place of a collapsed block

    // A block whose outcome AmberSSH genuinely knows.
    bool Succeeded() const { return hasExit && !interrupted && exitCode == 0; }
    bool Failed() const { return hasExit && !interrupted && exitCode != 0; }
    // Neither: still running, interrupted, or the shell never reported.
    bool Unknown() const { return !hasExit || interrupted; }
};

// ------------------------------------------------------------------ format
// "1.2s", "340ms", "3m04s" — the same vocabulary the journal overlay uses.
std::string FormatDuration(double seconds);
// "812 B", "4.1 KB", "2.3 MB". Only ever shown when bytes were counted.
std::string FormatBytes(uint64_t bytes);
// "exit 1", "ok", "interrupted", "running" — never a fabricated status.
std::string FormatOutcome(const CommandBlock& b);

// The one-line summary drawn in place of a collapsed block, as code points.
// `firstLine` is optional (empty to omit) and is the first non-blank line of
// the output, which is usually the useful part of a long build log.
std::u32string BuildSummary(const CommandBlock& b, bool showCwd,
                            const std::string& firstLine);

// ------------------------------------------------------------------- query
// The block containing `row` anywhere between its prompt and its last output
// row, or nullptr. Blocks do not overlap, so the first hit is the answer.
const CommandBlock* BlockAtRow(const std::vector<CommandBlock>& blocks, RowId row);
CommandBlock* BlockAtRow(std::vector<CommandBlock>& blocks, RowId row);
// Index of that block, or -1.
int BlockIndexAtRow(const std::vector<CommandBlock>& blocks, RowId row);
// Lookup by the id carried in a journal entry.
const CommandBlock* BlockById(const std::vector<CommandBlock>& blocks, uint64_t id);

// ------------------------------------------------------------------- trim
// Drops blocks whose rows have fallen out of the scrollback. `oldest` is the
// lowest row id the grid can still address.
//
// A bookmarked block is dropped too, and that is deliberate: a bookmark that
// outlived its rows would be a durable reference to text that no longer
// exists. Bookmarks are session-local and last exactly as long as the rows
// they point at. Returns how many were dropped, so the UI can say so.
size_t TrimBlocks(std::vector<CommandBlock>& blocks, RowId oldest);

// ------------------------------------------------------- notification policy
enum class NotifyOn { Off = 0, Success = 1, Failure = 2, Both = 3 };

const char* NotifyOnName(NotifyOn n);

// Whether a finished command deserves a completion notification.
//
//  - Off, or shorter than the threshold: no. A fast command never notifies,
//    which is the whole point of the threshold.
//  - An unknown outcome (no exit status, or interrupted) counts as "did not
//    succeed", so Failure and Both report it — with wording that says the
//    outcome is unknown rather than inventing one.
bool ShouldNotifyCompletion(NotifyOn mode, int thresholdSec, const CommandBlock& b);

// The text of that notification. Never claims a status it does not have.
std::string CompletionText(const CommandBlock& b);

} // namespace amber
