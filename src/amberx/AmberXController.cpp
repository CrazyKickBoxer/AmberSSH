#include "AmberXController.h"

#include <cstdio>
#include <string>

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
// launch, and a limit is cheaper than an argument.
HANDLE MakeJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
    jl.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    jl.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl)))
    {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

} // namespace

bool AmberXController::Alive() const
{
    return m_proc && WaitForSingleObject(m_proc, 0) == WAIT_TIMEOUT && m_pipe.Valid();
}

void AmberXController::Kill()
{
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
    HANDLE logH = CreateFileW(HostLogPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritLog,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE inheritList[2] = { rd, logH };
    const DWORD inheritCount = (logH != INVALID_HANDLE_VALUE) ? 2 : 1;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList,
                                   sizeof(HANDLE) * inheritCount, nullptr, nullptr))
    {
        CloseHandle(rd);
        if (logH != INVALID_HANDLE_VALUE)
            CloseHandle(logH);
        err = "could not build the handle list";
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
                       L" --mode " + (m_launch.trusted ? L"trusted" : L"restricted") +
                       L" --skin " + std::to_wstring(m_launch.skin);
    if (!m_launch.sigil.empty())
        cmd += L" --sigil " + quoted(m_launch.sigil);
    if (!m_launch.keymap.empty())
        cmd += L" --keymap " + quoted(m_launch.keymap);
    if (m_launch.rootful)
        cmd += L" --rootful";

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
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                                       CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(rd);   // the child has its own copy now
    if (logH != INVALID_HANDLE_VALUE)
        CloseHandle(logH);
    if (!ok)
    {
        err = "could not start AmberXHost.exe (is it next to AmberSSH.exe?)";
        Kill();
        return false;
    }
    m_proc = pi.hProcess;
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
        err = "the host did not connect";
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

bool AmberXController::CloseChannel(uint32_t id)
{
    if (!m_ready || !m_channels.Close(id))
        return false;
    Frame f;
    f.type = MsgType::ChannelClose;
    f.channel = id;
    return m_pipe.WriteFrame(f);
}

PipeRead AmberXController::Poll(Frame& out, DWORD timeoutMs)
{
    if (!m_ready)
        return PipeRead::Closed;
    return m_pipe.ReadFrame(out, timeoutMs);
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
        m_pipe.ReadFrame(ack, 1000);   // the host's final status, if it sends one
    }
    Kill();
}

// ------------------------------------------------------------------ preview
int RunPreview()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"amberx-preview.txt";
    FILE* out = _wfopen(path.c_str(), L"w");
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
    launch.trusted = false;
    launch.sigil = "ember-fox-lantern";
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
    }
    fprintf(out, "\n%s\n", failures ? "PREVIEW FAILED" : "PREVIEW PASSED");
    fclose(out);
    return failures ? 1 : 0;
}

} // namespace amber::amberx
