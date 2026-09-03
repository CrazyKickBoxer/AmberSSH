// ConPty.h — local console sessions through the Windows pseudoconsole.
//
// A ConPty owns, under RAII, everything a local shell needs: the two pipe
// pairs, the HPCON, the attribute list and the child process. Destruction
// closes them in the one order that does not hang — write side first, then
// the pseudoconsole (which signals the child), then the read side — so a
// dropped ConPty can never leak a handle or leave a console host running.
//
// Nothing here knows about the terminal grid. The transport thread in
// SshSession::ThreadMainLocal pumps bytes between this and the same ring
// buffer every other protocol uses, so a local shell reaches the parser by
// exactly the path an SSH channel does.
#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace amber
{

// ---------------------------------------------------------------- discovery
// A console program AmberSSH can launch. `key` is stable and storable in a
// profile; `exe` and `args` are what actually runs.
struct LocalShell
{
    std::string key;        // "pwsh", "powershell", "cmd", "gitbash", "wsl:Ubuntu"
    std::string name;       // "PowerShell 7", "WSL · Ubuntu"
    std::wstring exe;       // fully qualified when found, else the bare name
    std::wstring args;      // arguments, may be empty
    bool wsl = false;
    std::string distro;     // WSL only: the distribution name
};

// Shells found on this machine, in menu order: PowerShell 7, Windows
// PowerShell, Command Prompt, Git Bash, then one entry per WSL distribution.
// Cheap enough to call when a menu opens; the WSL probe is cached.
std::vector<LocalShell> DiscoverLocalShells();

// The discovered shell with this key, or false when it is not on this
// machine. Resolution happens at launch, so a profile survives a shell being
// upgraded, moved, or a WSL distribution being installed after it was saved.
bool ResolveShellByKey(const std::string& key, LocalShell& out);

// The WSL distributions installed for this user. Names come from
// `wsl.exe --list --quiet`, which prints one distribution per line in UTF-16;
// the default distribution is reported separately by ParseWslList.
std::vector<std::string> DiscoverWslDistros();

// Splits the UTF-16 output of `wsl.exe --list --quiet` into distribution
// names. Exposed for testing: the encoding, the BOM and the CR endings are
// all easy to get wrong and none of them are visible in a debugger.
std::vector<std::string> ParseWslList(const std::wstring& raw);

// Expands %VARIABLES% and resolves a bare executable name against PATH.
// Returns the input unchanged when it cannot be resolved, so the launch
// error names what the user actually asked for.
std::wstring ResolveExecutable(const std::wstring& exe);

// ---------------------------------------------------------- shell integration
// A per-session command that makes a shell emit OSC 7 (working directory) and
// OSC 133 (prompt marks) for this session only. Nothing is written to the
// user's dotfiles — the arguments below are handed to the shell at launch and
// vanish with the process. Empty when the shell has no safe bootstrap.
std::wstring ShellIntegrationArgs(const std::string& shellKey);

// ------------------------------------------------------------------- ConPty
struct ConPtyConfig
{
    std::wstring exe;
    std::wstring args;
    std::wstring cwd;                 // empty = the user's profile directory
    std::vector<std::wstring> env;    // "NAME=value" overrides, added to ours
    short cols = 80;
    short rows = 24;
};

class ConPty
{
public:
    ConPty() = default;
    ~ConPty();
    ConPty(const ConPty&) = delete;
    ConPty& operator=(const ConPty&) = delete;

    // Creates the pseudoconsole and launches the child. On failure returns
    // false and fills `err` with a sentence naming the executable.
    bool Start(const ConPtyConfig& cfg, std::string& err);

    // Reads whatever the child has written. Blocks until at least one byte is
    // available or the pipe breaks; returns 0 at EOF (the child exited).
    size_t Read(void* buf, size_t len);
    // Writes keystrokes to the child. False when the pipe is gone.
    bool Write(const void* buf, size_t len);

    // Resizes the pseudoconsole. Safe from another thread and safe to call
    // with unchanged dimensions; a failure is not fatal to the session.
    bool Resize(short cols, short rows);

    // True while the child is alive.
    bool Alive() const;
    // The child's exit code once it has exited, or STILL_ACTIVE.
    DWORD ExitCode() const;

    // Closes the write side so the child sees EOF on stdin, then closes the
    // pseudoconsole, which is what asks a well-behaved shell to exit.
    void RequestExit();
    // Ends the child's process tree immediately. For a hung shell.
    void Terminate();
    // The read end, so the owning session can cancel a blocking read to wake
    // its reader thread. Ownership stays here: never close this.
    HANDLE ReadHandle() const { return m_outRead; }
    // Releases everything. Idempotent; the destructor calls it.
    void Close();

private:
    HPCON  m_pc = nullptr;
    HANDLE m_inWrite = nullptr;     // us -> child stdin
    HANDLE m_outRead = nullptr;     // child stdout -> us
    HANDLE m_process = nullptr;
    HANDLE m_thread = nullptr;
    HANDLE m_job = nullptr;         // kills the whole tree with the session
    bool   m_exitRequested = false;
};

} // namespace amber
