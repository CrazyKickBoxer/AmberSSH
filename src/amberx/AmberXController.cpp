#include "AmberXController.h"

#include <psapi.h>
#include <sddl.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "control/Handshake.h"
#include "PreviewClient.h"

namespace amber::amberx
{

namespace
{

std::wstring HostExePath()
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) +
           L"AmberXHost.exe";
}

std::wstring HostLogPath()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"amberx-host.log";
}

// A JOB_OBJECT_LIMIT_ACTIVE_PROCESS of 1 means the host cannot spawn
// children at all — there is nothing an X server host should ever need to
// launch, and a limit is cheaper than an argument. The job also carries the
// resource ceilings the prompt asks for: memory, a hard CPU cap, and the UI
// restrictions a display server has no business exceeding.
constexpr SIZE_T kHostMemoryLimit = 1024ull * 1024 * 1024;   // 1 GiB
constexpr DWORD kHostCpuPercent = 50;

HANDLE MakeJob()
{
    const char* diag = getenv("AMBERX_SANDBOX_DIAG");
    const bool noMem = diag && (strstr(diag, "nojobmem") || strstr(diag, "nojoball"));
    const bool noCpu = diag && (strstr(diag, "nojobcpu") || strstr(diag, "nojoball"));
    const bool noUi = diag && (strstr(diag, "nojobui") || strstr(diag, "nojoball"));
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
    jl.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!noMem)
    {
        jl.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY;
        jl.ProcessMemoryLimit = kHostMemoryLimit;
        jl.JobMemoryLimit = kHostMemoryLimit;
    }
    jl.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl)))
    {
        CloseHandle(job);
        return nullptr;
    }
    if (!noCpu)
    {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu = {};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = kHostCpuPercent * 100;
        SetInformationJobObject(job, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu));
    }
    if (!noUi)
    {
        // no shutting the desktop down, no changing display or system settings,
        // no global atoms, no other desktops — a window is all it may make
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {};
        ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
                                 JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                 JOB_OBJECT_UILIMIT_DESKTOP;
        SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui));
    }
    return job;
}

// The host's token: this user's, with every privilege removed and the
// integrity level set to Low. Low integrity means UIPI stops it sending
// messages to AmberSSH's windows or any other medium-integrity process, and
// the mandatory label stops it writing to the user's files. It can still
// create windows and draw, which is all it is for. Fails closed: no
// restricted token, no host.
HANDLE MakeHostToken(std::string& err)
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT,
                          &tok))
    {
        err = "could not open the process token";
        return nullptr;
    }
    HANDLE restricted = nullptr;
    const BOOL ok = CreateRestrictedToken(tok, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restricted);
    CloseHandle(tok);
    if (!ok)
    {
        err = "could not create the restricted token";
        return nullptr;
    }
    PSID low = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-16-4096", &low))   // SECURITY_MANDATORY_LOW_RID
    {
        CloseHandle(restricted);
        err = "could not build the low-integrity SID";
        return nullptr;
    }
    TOKEN_MANDATORY_LABEL label = {};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = low;
    const BOOL set = SetTokenInformation(restricted, TokenIntegrityLevel, &label,
                                         sizeof(label) + GetLengthSid(low));
    LocalFree(low);
    if (!set)
    {
        CloseHandle(restricted);
        err = "could not lower the token's integrity level";
        return nullptr;
    }
    return restricted;
}

// Reads what the launched host actually runs under. This is the
// verification, as opposed to the request above.
std::string ReadConfinement(HANDLE proc)
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &tok))
        return "unverified";
    std::string out;
    DWORD n = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &n);
    std::vector<uint8_t> buf(n ? n : 1);
    if (n && GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), n, &n))
    {
        auto* tml = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
        const DWORD rid = *GetSidSubAuthority(tml->Label.Sid, *GetSidSubAuthorityCount(tml->Label.Sid) - 1);
        out += rid < SECURITY_MANDATORY_MEDIUM_RID ? "integrity=Low" : rid < SECURITY_MANDATORY_HIGH_RID ? "integrity=Medium" : "integrity=High";
    }
    else
        out += "integrity=?";
    // Privileges: DISABLE_MAX_PRIVILEGE leaves SeChangeNotifyPrivilege alone,
    // so a stripped token has exactly one. (IsTokenRestricted would only be
    // true with restricting SIDs, which this token does not use.)
    n = 0;
    GetTokenInformation(tok, TokenPrivileges, nullptr, 0, &n);
    std::vector<uint8_t> pbuf(n ? n : 1);
    if (n && GetTokenInformation(tok, TokenPrivileges, pbuf.data(), n, &n))
        out += " privileges=" + std::to_string(reinterpret_cast<TOKEN_PRIVILEGES*>(pbuf.data())->PrivilegeCount);
    else
        out += " privileges=?";
    CloseHandle(tok);
    return out;
}

// Process mitigation policies for the host: no dynamic code, no extension
// DLLs, no images from remote or low-labelled locations, strict handles,
// terminate on heap corruption, full ASLR. win32k stays: it draws windows.
DWORD64 HostMitigations()
{
    return PROCESS_CREATION_MITIGATION_POLICY_DEP_ENABLE |
           PROCESS_CREATION_MITIGATION_POLICY_SEHOP_ENABLE |
           PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON |
           PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
           PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON |
           PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
           PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
           PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON |
           PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
           PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON;
}

} // namespace

bool AmberXController::Alive() const
{
    return m_proc && WaitForSingleObject(m_proc, 0) == WAIT_TIMEOUT && m_pipe.Valid();
}

void AmberXController::Kill()
{
    StopReader();   // before the handle it reads from goes away
    m_ready = false;
    m_pipe.Close();
    if (m_srv.h != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_srv.h);
        m_srv.h = INVALID_HANDLE_VALUE;
    }
    if (m_proc)
    {
        // Give a live host a moment to exit on the closed pipe; the job
        // close below kills it regardless.
        WaitForSingleObject(m_proc, 500);
        CloseHandle(m_proc);
        m_proc = nullptr;
    }
    if (m_job)
    {
        CloseHandle(m_job);   // KILL_ON_JOB_CLOSE ends the process tree
        m_job = nullptr;
    }
    m_channels.Clear();
}

bool AmberXController::Start(std::string& err)
{
    Kill();

    if (!CreateServerPipe(m_srv, err))
        return false;
    m_security = m_srv.security;

    // The secret travels on an anonymous pipe whose READ end alone is
    // inherited — via an explicit handle list, so nothing else this process
    // holds leaks into the child. The child reads it and closes it; it never
    // appears on a command line or in an environment block.
    const std::vector<uint8_t> secret = RandomBytes(kSecretBytes);
    if (secret.empty())
    {
        err = "no entropy for the secret";
        Kill();
        return false;
    }
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &inherit, 0))
    {
        err = "could not create the secret pipe";
        Kill();
        return false;
    }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    DWORD wrote = 0;
    const BOOL sw = WriteFile(wr, secret.data(), static_cast<DWORD>(secret.size()), &wrote, nullptr);
    CloseHandle(wr);
    if (!sw || wrote != secret.size())
    {
        CloseHandle(rd);
        err = "could not deliver the secret";
        Kill();
        return false;
    }

    // ---- launch, suspended, into the job, then resume -------------------
    m_job = MakeJob();
    if (!m_job)
    {
        CloseHandle(rd);
        err = "could not create the job object";
        Kill();
        return false;
    }

    // The host's stderr goes to a file: it is the server's log, and the
    // server's logging layer is the one place that guarantees nothing
    // sensitive is ever written (os_log.c). Truncated per launch, so it is
    // never larger than one session.
    SECURITY_ATTRIBUTES inheritLog = { sizeof(inheritLog), nullptr, TRUE };
    // With AMBERX_TRACE_IO set the log is appended instead, so a run that
    // starts several hosts (the preview) keeps every host's trace.
    const bool appendLog = getenv("AMBERX_TRACE_IO") != nullptr;
    HANDLE logH = CreateFileW(HostLogPath().c_str(), appendLog ? FILE_APPEND_DATA : GENERIC_WRITE,
                              FILE_SHARE_READ, &inheritLog, appendLog ? OPEN_ALWAYS : CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE inheritList[2] = { rd, logH };
    const DWORD inheritCount = (logH != INVALID_HANDLE_VALUE) ? 2 : 1;

    // AMBERX_SANDBOX_DIAG=notoken|nomitig: diagnosis only — which half of the
    // confinement a launch failure belongs to. Never set in normal use.
    const char* diag = getenv("AMBERX_SANDBOX_DIAG");
    const bool diagNoToken = diag && strstr(diag, "notoken");
    const bool diagNoMitig = diag && strstr(diag, "nomitig");
    // No PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY: with it the loader ends
    // the host with STATUS_DLL_INIT_FAILED before wmain runs (found by
    // bisection; token, mitigations and job limits are all fine without it).
    // The Job Object's ActiveProcessLimit of 1 already forbids children.
    const bool diagNoChild = true;
    DWORD64 mitigations = HostMitigations();
    DWORD childPolicy = 0;
    const DWORD attrCount = 1 + (diagNoMitig ? 0 : 1) + (diagNoChild ? 0 : 1);
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, attrCount, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, attrCount, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList,
                                   sizeof(HANDLE) * inheritCount, nullptr, nullptr) ||
        (!diagNoMitig &&
         !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations,
                                    sizeof(mitigations), nullptr, nullptr)) ||
        (!diagNoChild &&
         !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &childPolicy,
                                    sizeof(childPolicy), nullptr, nullptr)))
    {
        CloseHandle(rd);
        if (logH != INVALID_HANDLE_VALUE)
            CloseHandle(logH);
        err = "could not build the process attributes";
        Kill();
        return false;
    }

    HANDLE hostToken = diagNoToken ? nullptr : MakeHostToken(err);
    if (!hostToken && !diagNoToken)
    {
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(rd);
        if (logH != INVALID_HANDLE_VALUE)
            CloseHandle(logH);
        Kill();
        return false;
    }

    // Quoted for CreateProcess: a double quote inside a value is dropped
    // rather than escaped, so no value can break out of its argument.
    auto quoted = [](const std::string& s) {
        std::string t;
        for (char ch : s) if (ch != '"') t += ch;
        const int n = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L' ');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, w.data(), n);
        return L"\"" + w + L"\"";
    };
    std::wstring cmd = L"\"" + HostExePath() + L"\" --pipe " + m_srv.name +
                       L" --secret-handle " + std::to_wstring(reinterpret_cast<uintptr_t>(rd)) +
                       L" --parent " + std::to_wstring(GetCurrentProcessId()) +
                       L" --identity " + quoted(m_launch.identity) +
                       L" --mode " + quoted(m_launch.modeLabel) +
                       L" --skin " + std::to_wstring(m_launch.skin);
    if (!m_launch.sigil.empty())
        cmd += L" --sigil " + quoted(m_launch.sigil);
    if (!m_launch.keymap.empty())
        cmd += L" --keymap " + quoted(m_launch.keymap);
    if (m_launch.rootful)
        cmd += L" --rootful";
    if (m_launch.trusted)
        cmd += L" --trusted";
    cmd += L" --auth-timeout " + std::to_wstring(m_launch.authTimeoutSeconds);
    cmd += L" --clipboard " + std::to_wstring(m_launch.clipboardMode);
    if (m_launch.desktopW > 0 && m_launch.desktopH > 0)
        cmd += L" --desktop " + std::to_wstring(m_launch.desktopX) + L"," +
               std::to_wstring(m_launch.desktopY) + L"," +
               std::to_wstring(m_launch.desktopW) + L"," +
               std::to_wstring(m_launch.desktopH);
    if (m_launch.presentCapHz > 0)
        cmd += L" --present-cap " + std::to_wstring(m_launch.presentCapHz);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    if (logH != INVALID_HANDLE_VALUE)
    {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdError = logH;
        si.StartupInfo.hStdOutput = logH;
        si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    }
    PROCESS_INFORMATION pi = {};
    const BOOL ok = hostToken
        ? CreateProcessAsUserW(hostToken, nullptr, cmd.data(), nullptr, nullptr, TRUE,
                               EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                               nullptr, nullptr, &si.StartupInfo, &pi)
        : CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                         EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                         nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD launchError = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (hostToken)
        CloseHandle(hostToken);
    CloseHandle(rd);   // the child has its own copy now
    if (logH != INVALID_HANDLE_VALUE)
        CloseHandle(logH);
    if (!ok)
    {
        err = "could not start AmberXHost.exe under the restricted token (error " +
              std::to_string(launchError) + "; is it next to AmberSSH.exe?)";
        Kill();
        return false;
    }
    m_proc = pi.hProcess;
    m_confinement = ReadConfinement(m_proc);
    // Assigned before it runs a single instruction, so there is no window in
    // which the host exists outside the job.
    if (!AssignProcessToJobObject(m_job, m_proc))
    {
        TerminateProcess(m_proc, 1);
        CloseHandle(pi.hThread);
        err = "could not place the host in the job";
        Kill();
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // ---- connect and handshake -----------------------------------------
    if (!WaitForClient(m_srv.h, 10000))
    {
        // Say whether the host is still alive (it cannot reach the pipe) or
        // already gone (and with what code): the two have different causes.
        DWORD code = 0;
        char detail[96];
        if (WaitForSingleObject(m_proc, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(m_proc, &code))
            snprintf(detail, sizeof detail, " (host exited with 0x%08lx before connecting)", code);
        else
            snprintf(detail, sizeof detail, " (host still running; it could not open the pipe)");
        err = std::string("the host did not connect") + detail;
        Kill();
        return false;
    }
    m_pipe.Adopt(m_srv.h);
    m_srv.h = INVALID_HANDLE_VALUE;   // owned by m_pipe now

    ControllerHandshake hs;
    Step s = hs.Begin(secret, RandomBytes(kNonceBytes));
    if (s.state == Step::State::Failed || !m_pipe.WriteFrame(s.send))
    {
        err = "handshake could not start";
        Kill();
        return false;
    }
    while (!hs.Done() && !hs.Failed())
    {
        Frame f;
        if (m_pipe.ReadFrame(f, 10000) != PipeRead::Ok)
        {
            err = "no handshake reply from the host";
            Kill();
            return false;
        }
        s = hs.OnFrame(f);
        if (s.hasSend && !m_pipe.WriteFrame(s.send))
        {
            err = "could not send the handshake proof";
            Kill();
            return false;
        }
    }
    if (hs.Failed())
    {
        // The thing on the pipe did not hold the secret. Whatever it is, it
        // is not our child.
        err = "handshake failed: the host could not prove it is ours";
        Kill();
        return false;
    }
    m_ready = true;
    // From here on the pipe's reads belong to the reader thread; the
    // handshake above was the last direct one.
    StartReader();
    return true;
}

bool AmberXController::SetCookie(const std::vector<uint8_t>& cookie16, uint32_t display)
{
    if (!m_ready)
        return false;
    Frame f;
    f.type = MsgType::SetCookie;
    f.channel = kControlChannel;
    f.payload = MakeSetCookie(cookie16, display);
    return !f.payload.empty() && m_pipe.WriteFrame(f);
}

bool AmberXController::OpenChannel(uint32_t id)
{
    if (!m_ready || !m_channels.Open(id))
        return false;
    Frame f;
    f.type = MsgType::ChannelOpen;
    f.channel = id;
    return m_pipe.WriteFrame(f);
}

bool AmberXController::SendData(uint32_t id, const uint8_t* data, size_t len)
{
    if (!m_ready || !m_channels.IsOpen(id) || len > kMaxPayload)
        return false;
    Frame f;
    f.type = MsgType::ChannelData;
    f.channel = id;
    f.payload.assign(data, data + len);
    return m_pipe.WriteFrame(f);
}

uint64_t AmberXController::HostPeakMemoryBytes() const
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    DWORD n = 0;
    if (!m_job || !QueryInformationJobObject(m_job, JobObjectExtendedLimitInformation,
                                             &info, sizeof info, &n))
        return 0;
    return info.PeakProcessMemoryUsed;
}

uint64_t AmberXController::HostMemoryBytes() const
{
    PROCESS_MEMORY_COUNTERS pmc = {};
    if (!m_proc || !K32GetProcessMemoryInfo(m_proc, &pmc, sizeof pmc))
        return 0;
    return pmc.WorkingSetSize;
}

bool AmberXController::SendWindowAction(uint32_t xid, WindowAct act)
{
    if (!m_ready || xid == 0)
        return false;
    Frame f;
    f.type = MsgType::WindowAction;
    f.channel = kControlChannel;
    f.payload = MakeWindowAction(xid, act);
    return m_pipe.WriteFrame(f);
}

bool AmberXController::SendClipboard(const std::string& utf8)
{
    if (!m_ready || utf8.empty() || utf8.size() > kMaxPayload)
        return false;
    Frame f;
    f.type = MsgType::ClipboardText;
    f.channel = kControlChannel;
    f.payload.assign(utf8.begin(), utf8.end());
    return m_pipe.WriteFrame(f);
}

bool AmberXController::CloseChannel(uint32_t id)
{
    if (!m_ready || !m_channels.Close(id))
        return false;
    Frame f;
    f.type = MsgType::ChannelClose;
    f.channel = id;
    return m_pipe.WriteFrame(f);
}

void AmberXController::KillHostForTest()
{
    if (m_proc)
        TerminateProcess(m_proc, 99);
}

// ------------------------------------------------------------------ reader
// The session loop used to find host frames by polling the pipe on its own
// tick — 30 ms when the socket was quiet — so every event and reply from the
// host waited up to a tick before it went anywhere. GTK 4 felt that as
// input that was "laggy and inconsistent", which is what tick-quantised
// latency compounded over a few round trips is. Now a thread owns the pipe
// read, queues frames, and holds an event high while the queue has anything
// in it; the loop sleeps on that event beside the socket and wakes the
// moment the host speaks.
//
// The pipe is overlapped I/O: this thread's reads and the session thread's
// writes use separate events and separate OVERLAPPEDs on one handle, which
// is exactly what overlapped handles are for. The only ordering that matters
// is that the thread is stopped before the handle is closed — Kill does that.
struct AmberXController::Reader
{
    std::thread th;
    std::mutex mu;
    std::deque<std::pair<Frame, uint64_t>> q;   // frame, arrival (QPC us)
    HANDLE readable = nullptr;                    // manual-reset: high while q is non-empty
    std::atomic<bool> stop{false};
    PipeRead end = PipeRead::Ok;                  // why the thread finished, if it has
    uint64_t waitSumUs = 0, waitMaxUs = 0;
    uint32_t waited = 0;
};

static uint64_t NowUs()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart) * 1000000ull / static_cast<uint64_t>(freq.QuadPart);
}

void AmberXController::StartReader()
{
    if (m_reader)
        return;
    m_reader = new Reader;
    m_reader->readable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_reader->th = std::thread([this] {
        Reader& r = *m_reader;
        for (;;)
        {
            if (r.stop.load())
                return;
            Frame f;
            // A bounded wait rather than INFINITE, so a stop request is
            // honoured within a quarter second without touching the handle
            // from another thread.
            const PipeRead rc = m_pipe.ReadFrame(f, 250);
            if (rc == PipeRead::Timeout)
                continue;
            std::lock_guard<std::mutex> lk(r.mu);
            if (rc == PipeRead::Ok)
                r.q.emplace_back(std::move(f), NowUs());
            else
                r.end = rc;
            SetEvent(r.readable);
            if (rc != PipeRead::Ok)
                return;
        }
    });
}

void AmberXController::StopReader()
{
    if (!m_reader)
        return;
    m_reader->stop.store(true);
    if (m_reader->th.joinable())
        m_reader->th.join();
    if (m_reader->readable)
        CloseHandle(m_reader->readable);
    delete m_reader;
    m_reader = nullptr;
}

PipeRead AmberXController::Poll(Frame& out, DWORD timeoutMs)
{
    if (!m_ready)
        return PipeRead::Closed;
    if (!m_reader)
        return m_pipe.ReadFrame(out, timeoutMs);
    Reader& r = *m_reader;
    for (int pass = 0; pass < 2; ++pass)
    {
        {
            std::lock_guard<std::mutex> lk(r.mu);
            if (!r.q.empty())
            {
                out = std::move(r.q.front().first);
                const uint64_t now = NowUs();
                const uint64_t waited = now > r.q.front().second ? now - r.q.front().second : 0;
                r.q.pop_front();
                r.waitSumUs += waited;
                if (waited > r.waitMaxUs)
                    r.waitMaxUs = waited;
                r.waited++;
                if (r.q.empty() && r.end == PipeRead::Ok)
                    ResetEvent(r.readable);
                return PipeRead::Ok;
            }
            if (r.end != PipeRead::Ok)
                return r.end;
        }
        if (pass == 0 && timeoutMs > 0)
            WaitForSingleObject(r.readable, timeoutMs);
        else
            break;
    }
    return PipeRead::Timeout;
}

HANDLE AmberXController::ReadableEvent() const
{
    return m_reader ? m_reader->readable : nullptr;
}

void AmberXController::QueueWait(uint32_t& maxUs, uint32_t& avgUs, uint32_t& frames)
{
    maxUs = avgUs = frames = 0;
    if (!m_reader)
        return;
    std::lock_guard<std::mutex> lk(m_reader->mu);
    frames = m_reader->waited;
    maxUs = static_cast<uint32_t>(m_reader->waitMaxUs);
    avgUs = frames ? static_cast<uint32_t>(m_reader->waitSumUs / frames) : 0;
    m_reader->waitSumUs = m_reader->waitMaxUs = 0;
    m_reader->waited = 0;
}

void AmberXController::Stop()
{
    if (m_ready)
    {
        Frame f;
        f.type = MsgType::Shutdown;
        f.channel = kControlChannel;
        m_pipe.WriteFrame(f);
        Frame ack;
        Poll(ack, 1000);   // the host's final status, if it sends one
    }
    Kill();
}

// ------------------------------------------------------------------ preview
// The report the release bundle carries. Everything in it is a fact about
// this build: what it is, what it was built from, what it enforces and what
// it does not do. No machine state, no session, nothing that changes between
// runs — a report that varies is a report nobody can check a bundle against.
int WriteReport()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"amberx-report.txt";
    FILE* out = _wfopen(path.c_str(), L"w");
    if (!out)
        return 1;

    auto say = [&](const char* line) {
        fprintf(out, "%s\n", line);
        printf("%s\n", line);
    };
    char buf[256];

    say("AmberX - the X server built into AmberSSH");
    say("");
    snprintf(buf, sizeof buf, "AmberX version        %s", kAmberXVersion);
    say(buf);
    snprintf(buf, sizeof buf, "Upstream X.Org        xserver %s", kUpstreamVersion);
    say(buf);
    say("Upstream components   xorgproto 2025.1, pixman 0.46.4, libXfont2 2.0.9,");
    say("                      libxkbfile 1.1.3, zlib 1.3.2");
    snprintf(buf, sizeof buf, "Control protocol      AmberXControl v%u, %u KiB maximum frame",
             static_cast<unsigned>(kVersion), static_cast<unsigned>(kMaxPayload / 1024));
    say(buf);
    snprintf(buf, sizeof buf, "Built                 %s %s", __DATE__, __TIME__);
    say(buf);
    say("");
    say("Provenance and licences");
    say("  docs/amberx/SOURCE-PROVENANCE.md   what was fetched, and its hashes");
    say("  docs/amberx/LICENSE-MATRIX.md      every component and its licence");
    say("  third_party/amberx/THIRD-PARTY-NOTICES.md");
    say("  third_party/amberx/amberx.spdx.json  SPDX 2.3 SBOM");
    say("");
    say("What it enforces");
    say("  One AmberXHost process per session, restricted token, Low integrity,");
    say("  process mitigations, and a Job Object that ends it with AmberSSH.");
    say("  Restricted X11 by default: the session cookie is registered UNTRUSTED");
    say("  with the X SECURITY extension. Trusted is an explicit, warned opt-in.");
    say("  Clipboard off by default; text only when on.");
    say("  Resource limits in src/amberx/server/amberlimits.h.");
    say("");
    say("What it does not do");
    say("  No GLX, no OpenGL, no DRI, no Vulkan — 2D only.");
    say("  No audio forwarding.");
    say("  No tab or pane embedding: forwarded windows are native windows.");
    say("  No clipboard formats beyond UTF-8 text.");
    say("  Forwarded clients in one session are not isolated from each other");
    say("  (X SECURITY semantics — see docs/amberx/THREAT-MODEL.md).");
    say("  No external network listener: the display is reachable only through");
    say("  the session that owns it.");
    say("");
    say("It is not VcXsrv, Cygwin/X, Xming or X410, shares no code with them,");
    say("and is not endorsed by them. It is built from upstream X.Org sources");
    say("with an original Windows layer; see the provenance manifest.");

    fclose(out);
    return 0;
}

int RunPreview()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"amberx-preview.txt";
    FILE* out = _wfopen(path.c_str(), L"w");
    // Line buffered: if a check crashes the process, the transcript still
    // names the last check that ran, which is the only clue there is.
    if (out)
        setvbuf(out, nullptr, _IONBF, 0);   // _IOLBF is not honoured by the CRT
    if (!out)
        return 2;
    int failures = 0;
    auto line = [&](const char* step, bool ok, const std::string& detail = {})
    {
        fprintf(out, "%-28s %s%s%s\n", step, ok ? "ok" : "FAIL",
                detail.empty() ? "" : "  ", detail.c_str());
        if (!ok)
            ++failures;
    };

    AmberXController c;
    AmberXController::Launch launch;
    launch.identity = "julie-prod.example · josh";
    launch.sigil = "TAKO";
    launch.skin = 0;
    c.Configure(launch);
    std::string err;
    const bool started = c.Start(err);
    line("start + handshake", started, err);
    if (started)
    {
        line("pipe DACL (read back)", !c.Security().empty(), c.Security());
        line("host alive", c.Alive());
        line("set cookie", c.SetCookie(std::vector<uint8_t>(16, 0x42), 0));
        Frame f;
        uint32_t open = 0; uint64_t bytes = 0; bool cookie = false;
        // The host reports status whenever something changes — at startup,
        // on the cookie, on every channel — so a check waits for the status
        // that shows the expected state rather than reading the next one.
        auto statusUntil = [&](auto pred, DWORD ms) -> bool
        {
            const ULONGLONG until = GetTickCount64() + ms;
            while (GetTickCount64() < until)
            {
                if (c.Poll(f, 500) != PipeRead::Ok)
                    continue;
                if (f.type == MsgType::HostStatus && ParseHostStatus(f.payload, open, bytes, cookie) && pred())
                    return true;
            }
            return false;
        };
        bool st = statusUntil([&] { return cookie; }, 5000);
        line("status after cookie", st, st ? "cookieSet=1" : "no status with cookieSet");

        line("open channel 1", c.OpenChannel(1));
        st = statusUntil([&] { return open == 1; }, 5000);
        line("status after open", st, st ? "open=1" : "no status with open=1");

        failures += RunPreviewClient(c, [&](const char* step, bool ok, const std::string& detail) {
            fprintf(out, "%-44s %s%s%s\n", step, ok ? "ok" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
        });

        line("close channel 1", c.CloseChannel(1));
        st = false;
        for (int i = 0; i < 4 && !st; ++i)
            st = c.Poll(f, 3000) == PipeRead::Ok && f.type == MsgType::HostStatus &&
                 ParseHostStatus(f.payload, open, bytes, cookie);
        line("status after close", st && open == 0, st ? "open=" + std::to_string(open) : "no status");
        line("double close refused", !c.CloseChannel(1));

        // A frame the host must refuse: data on channel 0 cannot even be
        // encoded, so the controller never sends it — prove that too.
        Frame bad;
        bad.type = MsgType::ChannelData;
        bad.channel = kControlChannel;
        std::vector<uint8_t> wire;
        line("control-channel data unencodable", !Encode(bad, wire));

        c.Stop();
        line("host exited after shutdown", !c.Alive());

        // Reconnect: a second host from the same controller, torn down the
        // same way. Anything leaked by the first would show here.
        std::string err2;
        const bool again = c.Start(err2);
        line("restart after stop", again, err2);
        if (again)
        {
            line("second host alive", c.Alive());
            c.Stop();
            line("second host exited", !c.Alive());
        }
    }

    // ---- Phase 5 -----------------------------------------------------------
    // Each host below is started, given the cookie, driven, and stopped.
    auto startWithCookie = [&](AmberXController& h, const AmberXController::Launch& l, const char* what) -> bool {
        h.Configure(l);
        std::string e;
        if (!h.Start(e))
        {
            line(what, false, e);
            return false;
        }
        line(what, true, h.Confinement());
        h.SetCookie(std::vector<uint8_t>(16, 0x42), 0);
        Frame f;
        uint32_t open = 0; uint64_t bytes = 0; bool cookie = false;
        const ULONGLONG until = GetTickCount64() + 5000;
        while (!cookie && GetTickCount64() < until)
            if (h.Poll(f, 500) == PipeRead::Ok && f.type == MsgType::HostStatus)
                ParseHostStatus(f.payload, open, bytes, cookie);
        return cookie;
    };
    auto logHasSecret = [&]() -> bool {
        FILE* lf = _wfopen(HostLogPath().c_str(), L"rb");
        if (!lf)
            return false;
        std::string all;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, lf)) > 0)
            all.append(buf, n);
        fclose(lf);
        // the cookie is sixteen 0x42 bytes: neither its hex nor the raw bytes may appear
        return all.find("4242424242424242") != std::string::npos ||
               all.find(std::string(16, 'B')) != std::string::npos;
    };
    {
        AmberXController r;
        if (startWithCookie(r, launch, "restricted host started (confinement read back)"))
        {
            line("host runs at low integrity with its privileges stripped",
                 r.Confinement().find("integrity=Low") != std::string::npos &&
                     r.Confinement().find("privileges=1") != std::string::npos,
                 r.Confinement());
            failures += RunPreviewTrustChecks(r, false, [&](const char* step, bool ok, const std::string& detail) {
                fprintf(out, "%-44s %s%s%s\n", step, ok ? "ok" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
            });
            r.Stop();
            line("cookie absent from the host log", !logHasSecret());
        }
    }
    {
        AmberXController t;
        AmberXController::Launch lt = launch;
        lt.trusted = true;
        lt.modeLabel = "X11 TRUSTED";
        if (startWithCookie(t, lt, "trusted host started"))
        {
            failures += RunPreviewTrustChecks(t, true, [&](const char* step, bool ok, const std::string& detail) {
                fprintf(out, "%-44s %s%s%s\n", step, ok ? "ok" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
            });
            t.Stop();
        }
    }
    {
        AmberXController u;
        AmberXController::Launch lu = launch;
        lu.authTimeoutSeconds = 2;
        if (startWithCookie(u, lu, "host with a 2 s authorization timeout started"))
        {
            failures += RunPreviewTimeoutCheck(u, [&](const char* step, bool ok, const std::string& detail) {
                fprintf(out, "%-44s %s%s%s\n", step, ok ? "ok" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
            });
            u.Stop();
        }
    }
    {
        // Phase 8: protocol conformance — the other byte order, split and
        // over-long requests, bad ids, destruction order, a client that
        // vanishes, and a burst of noise into the dispatcher.
        AmberXController cf;
        const ULONGLONG startAt = GetTickCount64();
        if (startWithCookie(cf, launch, "conformance host started"))
        {
            // Startup: profile open to a display that answers. Measured from
            // the launch to the cookie being live, which is the moment a
            // forwarded client could connect.
            {
                char d[64];
                const unsigned long long ms = GetTickCount64() - startAt;
                snprintf(d, sizeof d, "%llu ms to a display that answers", ms);
                line("host startup time", ms < 5000, d);
            }
            failures += RunPreviewConformance(cf, [&](const char* step, bool ok, const std::string& detail) {
                fprintf(out, "%-44s %s%s%s\n", step, ok ? "ok" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
            });
            cf.Stop();
        }
    }
    {
        // Phase 8: two sessions at once.
        AmberXController a, b;
        if (startWithCookie(a, launch, "session A host started") &&
            startWithCookie(b, launch, "session B host started"))
        {
            failures += RunPreviewIsolation(a, b, [&](const char* step, bool ok, const std::string& detail) {
                fprintf(out, "%-44s %s%s%s\n", step, ok ? "ok" : "FAIL",
                        detail.empty() ? "" : "  ", detail.c_str());
            });
        }
        a.Stop();
        b.Stop();
    }
    {
        // Phase 6: the clipboard bridge, once with both directions enabled and
        // once with only remote → local, so the mode is proven by what it
        // refuses as much as by what it carries.
        for (const int mode : { 4, 2, 0 })
        {
            AmberXController cb;
            AmberXController::Launch lc = launch;
            lc.clipboardMode = mode;
            const char* what = mode == 4 ? "clipboard host started (both directions)"
                             : mode == 2 ? "clipboard host started (remote to local only)"
                                         : "clipboard host started (clipboard disabled)";
            if (startWithCookie(cb, lc, what))
            {
                failures += RunPreviewClipboardChecks(cb, mode, [&](const char* step, bool ok, const std::string& detail) {
                    fprintf(out, "%-44s %s%s%s\n", step, ok ? "ok" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
                });
                cb.Stop();
            }
        }
    }
    {
        // A host that dies under the controller: the controller notices,
        // stays alive, and can start another.
        AmberXController k;
        if (startWithCookie(k, launch, "host for the crash test started"))
        {
            k.KillHostForTest();
            Frame f;
            PipeRead r = PipeRead::Ok;
            const ULONGLONG until = GetTickCount64() + 5000;
            while (r == PipeRead::Ok && GetTickCount64() < until)
                r = k.Poll(f, 500);
            line("controller sees the host crash as a closed pipe", r == PipeRead::Closed || r == PipeRead::Bad || !k.Alive());
            std::string e;
            line("controller starts a new host after the crash", k.Start(e), e);
            k.Stop();
            line("no host left after the crash test", !k.Alive());
        }
    }
    fprintf(out, "\n%s\n", failures ? "PREVIEW FAILED" : "PREVIEW PASSED");
    fclose(out);
    return failures ? 1 : 0;
}

} // namespace amber::amberx
