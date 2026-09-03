#include "ConPty.h"

#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <mutex>

#pragma comment(lib, "shell32.lib")

namespace amber
{
namespace
{

std::string NarrowW(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

std::wstring EnvVar(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
}

bool FileExists(const std::wstring& p)
{
    if (p.empty())
        return false;
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The first of `candidates` that exists, else an empty string.
std::wstring FirstExisting(std::initializer_list<std::wstring> candidates)
{
    for (const std::wstring& c : candidates)
        if (FileExists(c))
            return c;
    return {};
}

// Runs a console program and captures its stdout. Used only for the WSL
// probe, which prints a short list and exits; a hung child is abandoned
// after the timeout rather than blocking shell discovery for ever.
bool CaptureOutput(const std::wstring& cmdline, std::wstring& out, DWORD timeoutMs)
{
    out.clear();
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);           // our copy; the child holds the other
    if (!ok)
    {
        CloseHandle(rd);
        return false;
    }

    // Bounded: peek before every read so a child that never exits, or one
    // that hands its write end to something that outlives it, cannot block
    // this thread. Discovery runs during startup and when a menu opens, so
    // an unbounded read here is a hang in the user interface.
    std::string bytes;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr))
            break;                                   // pipe closed: child done
        if (avail > 0)
        {
            char buf[4096];
            DWORD n = 0;
            if (!ReadFile(rd, buf, (std::min)(avail, static_cast<DWORD>(sizeof(buf))), &n,
                          nullptr) || n == 0)
                break;
            bytes.append(buf, n);
            continue;                                // drain before waiting
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
        {
            // Exited with nothing more buffered: one last peek, then stop.
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                break;
            continue;
        }
        if (GetTickCount64() >= deadline)
        {
            TerminateProcess(pi.hProcess, 1);        // hung: do not wait on it
            break;
        }
        Sleep(15);
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // wsl.exe writes UTF-16LE. Treat an odd length as a truncated read.
    out.assign(reinterpret_cast<const wchar_t*>(bytes.data()), bytes.size() / sizeof(wchar_t));
    return true;
}

} // namespace

// ---------------------------------------------------------------- discovery
std::vector<std::string> ParseWslList(const std::wstring& raw)
{
    std::vector<std::string> names;
    size_t i = 0;
    // Skip a UTF-16 byte-order mark; wsl.exe emits one on some builds.
    if (!raw.empty() && raw[0] == L'\uFEFF')
        i = 1;
    while (i <= raw.size())
    {
        size_t end = raw.find(L'\n', i);
        std::wstring line = raw.substr(i, end == std::wstring::npos ? std::wstring::npos : end - i);
        i = (end == std::wstring::npos) ? raw.size() + 1 : end + 1;
        // Strip CR, the "(Default)" suffix some locales print, and padding.
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' ||
                                 line.back() == L'\t' || line.back() == L'\0'))
            line.pop_back();
        size_t a = line.find_first_not_of(L" \t\0", 0, 3);
        if (a == std::wstring::npos)
            continue;
        line = line.substr(a);
        if (line.empty())
            continue;
        // A distribution name never contains a space ("Ubuntu-22.04",
        // "kali-linux", "openSUSE-Leap-15.6"), but every localised header and
        // error sentence wsl.exe can print does. Without this an install with
        // WSL absent would list "Windows Subsystem for Linux has no installed
        // distributions." in the menu as though it were a shell.
        if (line.find(L' ') != std::wstring::npos)
            continue;
        names.push_back(NarrowW(line));
    }
    return names;
}

std::vector<std::string> DiscoverWslDistros()
{
    static std::mutex mx;
    static bool probed = false;
    static std::vector<std::string> cached;
    std::lock_guard<std::mutex> lk(mx);
    if (probed)
        return cached;
    probed = true;

    // System32 even from a 32-bit process: Sysnative avoids the redirector.
    std::wstring wsl = FirstExisting({ EnvVar(L"SystemRoot") + L"\\Sysnative\\wsl.exe",
                                       EnvVar(L"SystemRoot") + L"\\System32\\wsl.exe" });
    if (wsl.empty())
        return cached;
    std::wstring out;
    if (!CaptureOutput(L"\"" + wsl + L"\" --list --quiet", out, 4000))
        return cached;
    cached = ParseWslList(out);
    return cached;
}

std::wstring ResolveExecutable(const std::wstring& exe)
{
    if (exe.empty())
        return exe;
    std::wstring expanded(MAX_PATH * 2, L'\0');
    DWORD n = ExpandEnvironmentStringsW(exe.c_str(), expanded.data(),
                                        static_cast<DWORD>(expanded.size()));
    if (n > 0 && n <= expanded.size())
        expanded.resize(n - 1);
    else
        expanded = exe;
    if (expanded.find(L'\\') != std::wstring::npos || expanded.find(L'/') != std::wstring::npos)
        return expanded;
    // A bare name: ask the loader where PATH puts it.
    wchar_t found[MAX_PATH * 2];
    wchar_t* filePart = nullptr;
    DWORD got = SearchPathW(nullptr, expanded.c_str(), L".exe", static_cast<DWORD>(std::size(found)),
                            found, &filePart);
    if (got > 0 && got < std::size(found))
        return std::wstring(found, got);
    return expanded;
}

std::vector<LocalShell> DiscoverLocalShells()
{
    std::vector<LocalShell> out;
    const std::wstring sysRoot = EnvVar(L"SystemRoot");
    const std::wstring pf = EnvVar(L"ProgramFiles");
    const std::wstring pf86 = EnvVar(L"ProgramFiles(x86)");
    const std::wstring localApp = EnvVar(L"LOCALAPPDATA");

    // PowerShell 7 installs per-machine or per-user; check both before PATH.
    std::wstring pwsh = FirstExisting({ pf + L"\\PowerShell\\7\\pwsh.exe",
                                        pf + L"\\PowerShell\\7-preview\\pwsh.exe",
                                        localApp + L"\\Microsoft\\WindowsApps\\pwsh.exe" });
    if (pwsh.empty())
    {
        std::wstring viaPath = ResolveExecutable(L"pwsh.exe");
        if (FileExists(viaPath))
            pwsh = viaPath;
    }
    if (!pwsh.empty())
        out.push_back({ "pwsh", "PowerShell 7", pwsh, L"-NoLogo", false, {} });

    std::wstring ps = sysRoot + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (FileExists(ps))
        out.push_back({ "powershell", "Windows PowerShell", ps, L"-NoLogo", false, {} });

    std::wstring cmd = sysRoot + L"\\System32\\cmd.exe";
    if (FileExists(cmd))
        out.push_back({ "cmd", "Command Prompt", cmd, L"", false, {} });

    std::wstring bash = FirstExisting({ pf + L"\\Git\\bin\\bash.exe",
                                        pf86 + L"\\Git\\bin\\bash.exe",
                                        localApp + L"\\Programs\\Git\\bin\\bash.exe" });
    if (!bash.empty())
        out.push_back({ "gitbash", "Git Bash", bash, L"--login -i", false, {} });

    std::wstring wsl = FirstExisting({ sysRoot + L"\\Sysnative\\wsl.exe",
                                       sysRoot + L"\\System32\\wsl.exe" });
    if (!wsl.empty())
    {
        for (const std::string& d : DiscoverWslDistros())
        {
            LocalShell s;
            s.key = "wsl:" + d;
            s.name = "WSL \xC2\xB7 " + d;          // "WSL · Ubuntu"
            s.exe = wsl;
            s.wsl = true;
            s.distro = d;
            std::wstring wide(d.begin(), d.end());
            // Distribution names are ASCII in practice; widen defensively for
            // the rest by round-tripping through the ANSI code page.
            int need = MultiByteToWideChar(CP_UTF8, 0, d.c_str(), -1, nullptr, 0);
            if (need > 1)
            {
                wide.assign(static_cast<size_t>(need - 1), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, d.c_str(), -1, wide.data(), need);
            }
            s.args = L"--distribution \"" + wide + L"\"";
            out.push_back(std::move(s));
        }
    }
    return out;
}

bool ResolveShellByKey(const std::string& key, LocalShell& out)
{
    if (key.empty())
        return false;
    for (const LocalShell& s : DiscoverLocalShells())
    {
        if (s.key == key)
        {
            out = s;
            return true;
        }
    }
    return false;
}

std::wstring ShellIntegrationArgs(const std::string& shellKey)
{
    // Each of these emits OSC 133 prompt marks and OSC 7 for the working
    // directory, for this session only. They are passed as launch arguments,
    // so nothing on disk is touched and closing the tab ends the effect.
    if (shellKey == "pwsh" || shellKey == "powershell")
    {
        // A prompt function wrapping whatever prompt the user already has.
        return LR"(-NoExit -Command "$global:__amberOld=$function:prompt; )"
               LR"(function global:prompt { $c=$?; $e=if($c){0}else{1}; )"
               LR"(Write-Host -NoNewline ([char]27 + ']133;D;' + $e + [char]7); )"
               LR"(Write-Host -NoNewline ([char]27 + ']7;file://' + $env:COMPUTERNAME + '/' + )"
               LR"(($PWD.Path -replace '\\','/') + [char]7); )"
               LR"(Write-Host -NoNewline ([char]27 + ']133;A' + [char]7); )"
               LR"($p = & $global:__amberOld; )"
               LR"(Write-Host -NoNewline ([char]27 + ']133;B' + [char]7); $p }")";
    }
    if (shellKey == "gitbash" || shellKey.rfind("wsl:", 0) == 0)
    {
        // bash/zsh: PROMPT_COMMAND and PS0/PS1 wrappers, via --rcfile-free
        // -c so no dotfile is read or written.
        return LR"(-c "export PROMPT_COMMAND='printf \"\\033]133;D;%s\\007\\033]7;file://%s%s\\007\\033]133;A\\007\" \"$?\" \"$HOSTNAME\" \"$PWD\"'; )"
               LR"(export PS0='\\033]133;C\\007'; exec \"$SHELL\" -i")";
    }
    return {};   // cmd.exe has no prompt hook that can carry escape sequences
}

// ------------------------------------------------------------------- ConPty
ConPty::~ConPty() { Close(); }

bool ConPty::Start(const ConPtyConfig& cfg, std::string& err)
{
    err.clear();
    HANDLE inRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &m_inWrite, nullptr, 0) ||
        !CreatePipe(&m_outRead, &outWrite, nullptr, 0))
    {
        err = "could not create the console pipes";
        if (inRead) CloseHandle(inRead);
        if (outWrite) CloseHandle(outWrite);
        Close();
        return false;
    }

    COORD size;
    size.X = (std::max<short>)(cfg.cols, 1);
    size.Y = (std::max<short>)(cfg.rows, 1);
    HRESULT hr = CreatePseudoConsole(size, inRead, outWrite, 0, &m_pc);
    // The pseudoconsole duplicates what it needs; our ends of the child's
    // pipes go now, or the child never sees EOF.
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (FAILED(hr))
    {
        err = "the Windows pseudoconsole is unavailable on this system";
        Close();
        return false;
    }

    // Size the attribute list, then attach the pseudoconsole to it.
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    std::vector<char> attrStorage(attrBytes);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrBytes) ||
        !UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_pc, sizeof(m_pc),
                                   nullptr, nullptr))
    {
        err = "could not attach the pseudoconsole to the child process";
        Close();
        return false;
    }

    std::wstring exe = ResolveExecutable(cfg.exe);
    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!cfg.args.empty())
        cmdline += L" " + cfg.args;

    // Environment: ours, plus the profile's overrides, as a double-NUL block.
    std::wstring envBlock;
    LPVOID envPtr = nullptr;
    if (!cfg.env.empty())
    {
        LPWCH ours = GetEnvironmentStringsW();
        for (LPWCH p = ours; p && *p;)
        {
            std::wstring entry(p);
            p += entry.size() + 1;
            // A profile override of the same name replaces ours.
            size_t eq = entry.find(L'=');
            bool overridden = false;
            if (eq != std::wstring::npos && eq > 0)
            {
                std::wstring key = entry.substr(0, eq);
                for (const std::wstring& o : cfg.env)
                {
                    size_t oeq = o.find(L'=');
                    if (oeq != std::wstring::npos &&
                        _wcsicmp(o.substr(0, oeq).c_str(), key.c_str()) == 0)
                    {
                        overridden = true;
                        break;
                    }
                }
            }
            if (!overridden)
                envBlock += entry + L'\0';
        }
        if (ours)
            FreeEnvironmentStringsW(ours);
        for (const std::wstring& o : cfg.env)
            if (o.find(L'=') != std::wstring::npos)
                envBlock += o + L'\0';
        envBlock += L'\0';
        envPtr = envBlock.data();
    }

    std::wstring cwd = cfg.cwd;
    if (cwd.empty())
    {
        PWSTR home = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &home)) && home)
            cwd = home;
        if (home)
            CoTaskMemFree(home);
    }
    else
    {
        std::wstring expanded(MAX_PATH * 2, L'\0');
        DWORD n = ExpandEnvironmentStringsW(cwd.c_str(), expanded.data(),
                                            static_cast<DWORD>(expanded.size()));
        if (n > 0 && n <= expanded.size())
            cwd = expanded.substr(0, n - 1);
    }
    if (!cwd.empty() && GetFileAttributesW(cwd.c_str()) == INVALID_FILE_ATTRIBUTES)
        cwd.clear();   // a stale directory must not stop the shell opening

    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                             envPtr, cwd.empty() ? nullptr : cwd.c_str(),
                             &si.StartupInfo, &pi);
    DWORD launchErr = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok)
    {
        err = "could not start " + NarrowW(exe) + " (";
        err += launchErr == ERROR_FILE_NOT_FOUND ? "not found"
             : launchErr == ERROR_ACCESS_DENIED  ? "access denied"
                                                 : "error " + std::to_string(launchErr);
        err += ")";
        Close();
        return false;
    }
    m_process = pi.hProcess;
    m_thread = pi.hThread;

    // A job object so closing the tab takes the shell's children with it —
    // otherwise a backgrounded process keeps the console host alive.
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (m_job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
        AssignProcessToJobObject(m_job, m_process);
    }
    return true;
}

size_t ConPty::Read(void* buf, size_t len)
{
    if (!m_outRead || len == 0)
        return 0;
    DWORD n = 0;
    if (!ReadFile(m_outRead, buf, static_cast<DWORD>(len), &n, nullptr))
        return 0;      // broken pipe: the child is gone
    return n;
}

bool ConPty::Write(const void* buf, size_t len)
{
    if (!m_inWrite || len == 0)
        return m_inWrite != nullptr;
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len)
    {
        DWORD n = 0;
        if (!WriteFile(m_inWrite, p + sent, static_cast<DWORD>(len - sent), &n, nullptr) || n == 0)
            return false;
        sent += n;
    }
    return true;
}

bool ConPty::Resize(short cols, short rows)
{
    if (!m_pc)
        return false;
    COORD c;
    c.X = (std::max<short>)(cols, 1);
    c.Y = (std::max<short>)(rows, 1);
    return SUCCEEDED(ResizePseudoConsole(m_pc, c));
}

bool ConPty::Alive() const
{
    if (!m_process)
        return false;
    return WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

DWORD ConPty::ExitCode() const
{
    DWORD code = STILL_ACTIVE;
    if (m_process)
        GetExitCodeProcess(m_process, &code);
    return code;
}

void ConPty::RequestExit()
{
    if (m_exitRequested)
        return;
    m_exitRequested = true;
    // EOF on stdin is what tells a shell to leave; closing the pseudoconsole
    // then releases the console host. The read side stays open so the reader
    // thread drains the last output and sees a clean EOF.
    if (m_inWrite)
    {
        CloseHandle(m_inWrite);
        m_inWrite = nullptr;
    }
    if (m_pc)
    {
        ClosePseudoConsole(m_pc);
        m_pc = nullptr;
    }
}

void ConPty::Terminate()
{
    if (m_job)
    {
        TerminateJobObject(m_job, 1);
        return;
    }
    if (m_process)
        TerminateProcess(m_process, 1);
}

void ConPty::Close()
{
    // Order matters: stdin, then the pseudoconsole (which unblocks the child
    // and lets the console host retire), then our read end, then the handles.
    if (m_inWrite)
    {
        CloseHandle(m_inWrite);
        m_inWrite = nullptr;
    }
    if (m_pc)
    {
        ClosePseudoConsole(m_pc);
        m_pc = nullptr;
    }
    if (m_outRead)
    {
        CloseHandle(m_outRead);
        m_outRead = nullptr;
    }
    if (m_thread)
    {
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_process)
    {
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_job)
    {
        CloseHandle(m_job);      // KILL_ON_JOB_CLOSE ends the tree
        m_job = nullptr;
    }
    m_exitRequested = false;
}

} // namespace amber
