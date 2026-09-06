// RemoteName.h — every filename that arrives from a server, checked before it
// is allowed to become part of a local path.
//
// A directory listing is attacker-controlled data. The name in it decides
// where a download is written, so it is the one place in the SFTP browser
// where a remote host can reach the local filesystem. This is the gate, and
// it is pure so the whole matrix can be tested without a server.
//
// The rule is REJECT, not repair. A name that could mean anything other than
// "one file in this directory" is refused and the entry is skipped with a
// reason the user can read. Repairing a hostile name invents a filename the
// server never sent, and every interesting bypass lives in the repair.
#pragma once

#include <string>

namespace amber
{

// Why a server-supplied name was refused. Ok is the only accepted value.
enum class NameCheck
{
    Ok,
    Empty,
    Dot,              // "." or ".." — the directory itself, or its parent
    Separator,        // contains '/' or '\': more than one path component
    DriveOrStream,    // contains ':': a drive letter, or an NTFS data stream
    Wildcard,         // '*' or '?': a pattern, not a name
    Control,          // a control character, NUL included
    Reserved,         // CON, PRN, AUX, NUL, COM1..9, LPT1..9
    TrailingDotSpace, // Windows strips these silently, so two names collide
    TooLong,
    // A Unicode directional override or isolate. These are invisible and
    // reverse the text after them, so "evil‮gnp.exe" is displayed as
    // "evilexe.png": the extension the reader sees is not the extension the
    // system acts on. Arabic and Hebrew names do not need them — those
    // scripts carry their own direction — so refusing the explicit overrides
    // costs nothing legitimate.
    BidiOverride,
};

// One path component, exactly as the server sent it (UTF-8). Nothing is
// trimmed or decoded first: the name is judged as it arrived.
NameCheck CheckRemoteName(const std::string& name);

// A '/'-separated relative path from a server — the shape the sync planner
// works in — checked one component at a time. Empty, absolute, and any
// component CheckRemoteName refuses, are all refused. `badComponent` receives
// the component that failed when the result is not Ok.
NameCheck CheckRemotePath(const std::string& path, std::string* badComponent = nullptr);

// A short phrase naming the rule that refused it, for the status line.
const char* NameCheckReason(NameCheck c);

// True when `child` is `root` itself or sits underneath it.
//
// Lexical only: separators are normalised, "." is dropped and ".." pops a
// segment, then the two are compared case-insensitively the way Windows
// compares paths. It touches no disk, so it neither knows nor cares whether
// either path exists.
//
// What it therefore does NOT do: resolve junctions, symlinks or substituted
// drives. A directory junction already present inside `root` and pointing
// elsewhere will pass. This is the second line of defence behind
// CheckRemoteName, not a replacement for it.
bool PathWithin(const std::wstring& root, const std::wstring& child);

} // namespace amber
