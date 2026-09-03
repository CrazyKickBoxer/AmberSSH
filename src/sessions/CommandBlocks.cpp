#include "CommandBlocks.h"

#include <algorithm>
#include <cstdio>

namespace amber
{

namespace
{

// UTF-8 -> UTF-32, permissive: a malformed byte becomes U+FFFD rather than
// truncating the summary. Command text comes off the wire and can be
// anything.
void AppendUtf32(std::u32string& out, const std::string& utf8)
{
    for (size_t i = 0; i < utf8.size();)
    {
        const unsigned char b0 = static_cast<unsigned char>(utf8[i]);
        int n = 1;
        char32_t cp = b0;
        if (b0 >= 0xF0)      { cp = b0 & 0x07u; n = 4; }
        else if (b0 >= 0xE0) { cp = b0 & 0x0Fu; n = 3; }
        else if (b0 >= 0xC0) { cp = b0 & 0x1Fu; n = 2; }
        else if (b0 >= 0x80) { out.push_back(U'�'); ++i; continue; }
        if (i + static_cast<size_t>(n) > utf8.size())
        {
            out.push_back(U'�');
            break;
        }
        for (int k = 1; k < n; ++k)
        {
            const unsigned char bk = static_cast<unsigned char>(utf8[i + k]);
            if ((bk & 0xC0) != 0x80)
            {
                cp = U'�';
                n = k;
                break;
            }
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        out.push_back(cp);
        i += static_cast<size_t>(n);
    }
}

// Trims to at most `maxCp` code points, marking the cut.
void Clip(std::u32string& s, size_t maxCp)
{
    if (s.size() <= maxCp)
        return;
    s.resize(maxCp > 0 ? maxCp - 1 : 0);
    s.push_back(U'…');
}

} // namespace

// ------------------------------------------------------------------ format
std::string FormatDuration(double seconds)
{
    char b[32];
    if (seconds < 0.0)
        seconds = 0.0;
    if (seconds < 1.0)
        snprintf(b, sizeof(b), "%dms", static_cast<int>(seconds * 1000.0 + 0.5));
    else if (seconds < 60.0)
        snprintf(b, sizeof(b), "%.1fs", seconds);
    else if (seconds < 3600.0)
        snprintf(b, sizeof(b), "%dm%02ds", static_cast<int>(seconds) / 60,
                 static_cast<int>(seconds) % 60);
    else
        snprintf(b, sizeof(b), "%dh%02dm", static_cast<int>(seconds) / 3600,
                 (static_cast<int>(seconds) % 3600) / 60);
    return b;
}

std::string FormatBytes(uint64_t bytes)
{
    char b[32];
    if (bytes < 1024ull)
        snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024ull)
        snprintf(b, sizeof(b), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024ull * 1024ull)
        snprintf(b, sizeof(b), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        snprintf(b, sizeof(b), "%.1f GB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return b;
}

std::string FormatOutcome(const CommandBlock& b)
{
    if (b.running)
        return "running";
    if (b.interrupted)
        return "interrupted";
    if (!b.hasExit)
        return "no exit status";
    if (b.exitCode == 0)
        return "ok";
    char t[32];
    snprintf(t, sizeof(t), "exit %d", b.exitCode);
    return t;
}

std::u32string BuildSummary(const CommandBlock& b, bool showCwd,
                            const std::string& firstLine)
{
    std::u32string out;
    out.push_back(U'▸');        // a collapsed disclosure triangle
    out.push_back(U' ');
    if (b.bookmarked)
    {
        out.push_back(U'⚑');    // a flag: this one was marked
        out.push_back(U' ');
    }

    std::u32string cmd;
    AppendUtf32(cmd, b.command.empty() ? std::string("output") : b.command);
    Clip(cmd, 90);
    out += cmd;

    // The meta column. Only facts: a count that was counted, a duration that
    // was measured, a status the shell actually reported.
    std::string meta = "  [";
    meta += std::to_string(b.lines);
    meta += b.lines == 1 ? " line" : " lines";
    if (b.bytes > 0)
        meta += ", " + FormatBytes(b.bytes);
    meta += ", " + FormatDuration(b.durationSec);
    meta += ", " + FormatOutcome(b);
    if (showCwd && !b.cwd.empty())
        meta += ", " + b.cwd;
    meta += "]";
    std::u32string metaCp;
    AppendUtf32(metaCp, meta);
    Clip(metaCp, 120);
    out += metaCp;

    if (!firstLine.empty())
    {
        std::u32string fl;
        AppendUtf32(fl, "  " + firstLine);
        Clip(fl, 80);
        out += fl;
    }
    return out;
}

// ------------------------------------------------------------------- query
int BlockIndexAtRow(const std::vector<CommandBlock>& blocks, RowId row)
{
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        const CommandBlock& b = blocks[i];
        const RowId first = b.promptRow;
        const RowId last = b.hasOutput ? b.outputLast
                                       : std::max(b.inputRow, b.promptRow);
        if (row >= first && row <= last)
            return static_cast<int>(i);
    }
    return -1;
}

const CommandBlock* BlockAtRow(const std::vector<CommandBlock>& blocks, RowId row)
{
    const int i = BlockIndexAtRow(blocks, row);
    return i < 0 ? nullptr : &blocks[static_cast<size_t>(i)];
}

CommandBlock* BlockAtRow(std::vector<CommandBlock>& blocks, RowId row)
{
    const int i = BlockIndexAtRow(blocks, row);
    return i < 0 ? nullptr : &blocks[static_cast<size_t>(i)];
}

const CommandBlock* BlockById(const std::vector<CommandBlock>& blocks, uint64_t id)
{
    if (id == 0)
        return nullptr;
    for (const CommandBlock& b : blocks)
        if (b.id == id)
            return &b;
    return nullptr;
}

// ------------------------------------------------------------------- trim
size_t TrimBlocks(std::vector<CommandBlock>& blocks, RowId oldest)
{
    const size_t before = blocks.size();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                [oldest](const CommandBlock& b)
                                {
                                    // A block survives while any row it names
                                    // is still addressable.
                                    const RowId last =
                                        b.hasOutput ? b.outputLast
                                                    : std::max(b.inputRow, b.promptRow);
                                    return last < oldest;
                                }),
                 blocks.end());
    return before - blocks.size();
}

// ------------------------------------------------------- notification policy
const char* NotifyOnName(NotifyOn n)
{
    switch (n)
    {
    case NotifyOn::Off:     return "off";
    case NotifyOn::Success: return "success only";
    case NotifyOn::Failure: return "failure only";
    case NotifyOn::Both:    return "success and failure";
    }
    return "off";
}

bool ShouldNotifyCompletion(NotifyOn mode, int thresholdSec, const CommandBlock& b)
{
    if (mode == NotifyOn::Off)
        return false;
    if (b.running)
        return false;                        // not finished: nothing to report
    if (thresholdSec < 0)
        return false;
    if (b.durationSec < static_cast<double>(thresholdSec))
        return false;                        // a fast command never notifies

    if (b.Succeeded())
        return mode == NotifyOn::Success || mode == NotifyOn::Both;
    // Everything else did not succeed: a non-zero exit, an interrupted
    // command, or one the shell never reported a status for. The last two are
    // unknown rather than failed, and CompletionText says so.
    return mode == NotifyOn::Failure || mode == NotifyOn::Both;
}

std::string CompletionText(const CommandBlock& b)
{
    std::string cmd = b.command.empty() ? std::string("a command") : b.command;
    if (cmd.size() > 80)
        cmd = cmd.substr(0, 79) + "\xE2\x80\xA6";
    const std::string dur = FormatDuration(b.durationSec);
    if (b.interrupted)
        return cmd + " was interrupted after " + dur +
               " — AmberSSH does not know how it ended";
    if (!b.hasExit)
        return cmd + " ended after " + dur + " with no exit status reported";
    if (b.exitCode == 0)
        return cmd + " finished in " + dur;
    return cmd + " failed after " + dur + " (exit " + std::to_string(b.exitCode) + ")";
}

} // namespace amber
