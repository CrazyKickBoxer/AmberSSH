#include "AmberXController.h"

#include <cstdio>
#include <string>

#include "control/Handshake.h"

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

    std::wstring cmd = L"\"" + HostExePath() + L"\" --pipe " + m_srv.name +
                       L" --secret-handle " + std::to_wstring(reinterpret_cast<uintptr_t>(rd)) +
                       L" --parent " + std::to_wstring(GetCurrentProcessId());

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

        // ---- an X client, by hand ---------------------------------------
        // Everything below is the X11 wire protocol, little-endian, written
        // out so the gate depends on no X library. What the server sends
        // back on the channel is read through `recv`, which sets aside the
        // host's own status frames and remembers any HostError.
        std::vector<uint8_t> inbuf;
        std::string hostErrors;
        auto recv = [&](size_t want, DWORD ms) -> bool
        {
            const ULONGLONG until = GetTickCount64() + ms;
            while (inbuf.size() < want)
            {
                const ULONGLONG now = GetTickCount64();
                if (now >= until)
                    return false;
                Frame r;
                const PipeRead pr = c.Poll(r, static_cast<DWORD>(until - now));
                if (pr != PipeRead::Ok)
                    return false;
                if (r.type == MsgType::ChannelData && r.channel == 1)
                    inbuf.insert(inbuf.end(), r.payload.begin(), r.payload.end());
                else if (r.type == MsgType::HostError)
                {
                    std::string t;
                    ParseHostError(r.payload, t);
                    hostErrors += t + "; ";
                }
                else if (r.type == MsgType::ChannelClose && r.channel == 1)
                    return false;
            }
            return true;
        };
        auto take = [&](size_t n) { inbuf.erase(inbuf.begin(), inbuf.begin() + static_cast<std::ptrdiff_t>(n)); };
        auto u16 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
        auto u32 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); };
        auto put16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); };
        auto put32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); };
        auto send = [&](const std::vector<uint8_t>& req) { return c.SendData(1, req.data(), req.size()); };

        // setup: 'l', pad, 11.0, auth name + cookie
        {
            std::vector<uint8_t> s;
            s.push_back('l'); s.push_back(0);
            put16(s, 11); put16(s, 0);
            put16(s, 18); put16(s, 16); put16(s, 0);
            const char* name = "MIT-MAGIC-COOKIE-1";
            s.insert(s.end(), name, name + 18); s.push_back(0); s.push_back(0);
            s.insert(s.end(), 16, 0x42);
            line("send X11 setup", send(s));
        }
        uint32_t root = 0, ridBase = 0, whitePixel = 0, rootDepth = 0, rootW = 0, rootH = 0;
        bool setupOk = recv(8, 5000);
        if (setupOk)
        {
            const uint8_t status = inbuf[0];
            const uint32_t extra = u16(&inbuf[6]) * 4;
            setupOk = recv(8 + extra, 5000);
            if (setupOk && status == 1)
            {
                const uint8_t* p = inbuf.data() + 8;   // xConnSetup
                ridBase = u32(p + 4);
                const uint32_t nVendor = u16(p + 16);
                const uint8_t numFormats = p[21];
                const uint8_t* q = p + 32 + ((nVendor + 3) & ~3u) + numFormats * 8;   // first root
                root = u32(q);
                whitePixel = u32(q + 8);
                rootW = u16(q + 20);
                rootH = u16(q + 22);
                rootDepth = q[38];
                line("setup accepted", true,
                     "root=0x" + std::to_string(root) + " depth=" + std::to_string(rootDepth) +
                     " " + std::to_string(rootW) + "x" + std::to_string(rootH));
            }
            else if (setupOk)
            {
                std::string reason(reinterpret_cast<const char*>(inbuf.data()) + 8, inbuf[1]);
                line("setup accepted", false, "status=" + std::to_string(status) + " " + reason);
            }
            take(8 + extra);
        }
        if (!setupOk)
            line("setup accepted", false, "no reply  " + hostErrors);

        if (root && rootDepth == 24 && rootW == 1280 && rootH == 800)
        {
            const uint32_t wid = ridBase + 1, gc = ridBase + 2;
            std::vector<uint8_t> r;
            // CreateWindow: amber background, 200x120 at (10,10)
            r = { 1, 0 }; put16(r, 8 + 2); put32(r, wid); put32(r, root);
            put16(r, 10); put16(r, 10); put16(r, 200); put16(r, 120); put16(r, 0);
            put16(r, 1); put32(r, 0); put32(r, 0x2 | 0x800);
            put32(r, 0x00ff8800); put32(r, 0x8000 | 0x4 | 0x1);
            line("CreateWindow", send(r));
            // MapWindow
            r = { 8, 0 }; put16(r, 2); put32(r, wid);
            line("MapWindow", send(r));
            // CreateGC with a dark foreground
            r = { 55, 0 }; put16(r, 4 + 1); put32(r, gc); put32(r, wid); put32(r, 0x4); put32(r, 0x00203040);
            line("CreateGC", send(r));
            // PolyFillRectangle 80x50 at (20,20)
            r = { 70, 0 }; put16(r, 3 + 2); put32(r, wid); put32(r, gc);
            put16(r, 20); put16(r, 20); put16(r, 80); put16(r, 50);
            line("PolyFillRectangle", send(r));
            // GetGeometry: a reply proves the round trip
            r = { 14, 0 }; put16(r, 2); put32(r, wid);
            line("GetGeometry", send(r));
            // GetImage of one pixel inside the rectangle: proves fb drew it
            r = { 73, 2 }; put16(r, 5); put32(r, wid); put16(r, 25); put16(r, 25); put16(r, 1); put16(r, 1); put32(r, 0xffffffff);
            line("GetImage", send(r));

            // replies and events, in any order the server chooses
            bool sawGeometry = false, sawExpose = false, sawPixel = false;
            std::string pixel;
            const ULONGLONG until = GetTickCount64() + 5000;
            while ((!sawGeometry || !sawPixel) && GetTickCount64() < until)
            {
                if (!recv(32, 1000))
                    break;
                const uint8_t type = inbuf[0] & 0x7f;
                if (type == 1)   // reply
                {
                    const uint32_t extraLen = u32(&inbuf[4]) * 4;
                    if (!recv(32 + extraLen, 3000))
                        break;
                    const uint32_t seq = u16(&inbuf[2]);
                    if (seq == 5)   // GetGeometry: root@8 x@12 y@14 w@16 h@18
                    {
                        sawGeometry = u16(&inbuf[16]) == 200 && u16(&inbuf[18]) == 120 && u32(&inbuf[8]) == root;
                        line("GetGeometry reply", sawGeometry,
                             std::to_string(u16(&inbuf[16])) + "x" + std::to_string(u16(&inbuf[18])) +
                             " at " + std::to_string(u16(&inbuf[12])) + "," + std::to_string(u16(&inbuf[14])) +
                             " depth=" + std::to_string(inbuf[1]));
                    }
                    else if (seq == 6)   // GetImage
                    {
                        const uint32_t px = extraLen >= 4 ? (u32(&inbuf[32]) & 0xffffff) : 0xdeadbeef;
                        sawPixel = true;
                        char hex[16];
                        snprintf(hex, sizeof hex, "0x%06x", px);
                        pixel = hex;
                        line("GetImage pixel == foreground", px == 0x203040, pixel);
                    }
                    take(32 + extraLen);
                }
                else if (type == 0)   // error
                {
                    line("X error", false, "code=" + std::to_string(inbuf[1]) + " seq=" + std::to_string(u16(&inbuf[2])) +
                                           " major=" + std::to_string(inbuf[10]));
                    take(32);
                }
                else   // event
                {
                    if (type == 12)
                        sawExpose = true;
                    take(32);
                }
            }
            line("Expose event", sawExpose);
            if (!sawGeometry)
                line("GetGeometry reply", false, "none  " + hostErrors);
            if (!sawPixel)
                line("GetImage pixel == foreground", false, "none  " + hostErrors);

            // ---- input in: real window messages, X events out ----------
            // The display window is on this desktop; post it the messages a
            // user would generate. The client window at (10,10) 200x120
            // selected ButtonPress and KeyPress, so a click at (50,50) and a
            // press of the 'a' key (scan code 0x1E, evdev 30, X keycode 38)
            // must come back on the channel as those events.
            HWND disp = FindWindowW(L"AmberXDisplay", nullptr);
            line("display window exists", disp != nullptr);
            bool sawButton = false, sawKey = false;
            if (disp)
            {
                PostMessageW(disp, WM_MOUSEMOVE, 0, MAKELPARAM(50, 50));
                PostMessageW(disp, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 50));
                PostMessageW(disp, WM_LBUTTONUP, 0, MAKELPARAM(50, 50));
                PostMessageW(disp, WM_MOUSEMOVE, 0, MAKELPARAM(150, 100));
                PostMessageW(disp, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(150, 100));
                PostMessageW(disp, WM_LBUTTONUP, 0, MAKELPARAM(150, 100));
                PostMessageW(disp, WM_KEYDOWN, 'A', static_cast<LPARAM>((0x1E << 16) | 1));
                PostMessageW(disp, WM_KEYUP, 'A', static_cast<LPARAM>((0x1E << 16) | 1 | (1u << 30) | (1u << 31)));
                const ULONGLONG inUntil = GetTickCount64() + 5000;
                while ((!sawButton || !sawKey) && GetTickCount64() < inUntil)
                {
                    if (!recv(32, 1000))
                        break;
                    const uint8_t type = inbuf[0] & 0x7f;
                    if (type == 1)
                    {
                        const uint32_t extraLen = u32(&inbuf[4]) * 4;
                        if (!recv(32 + extraLen, 3000))
                            break;
                        take(32 + extraLen);
                        continue;
                    }
                    if (type == 4 && inbuf[1] == 1)   // ButtonPress, button 1
                    {
                        // root@20,22 event@24,26; the window is at (10,10)
                        const int rx = u16(&inbuf[20]), ry = u16(&inbuf[22]);
                        const int ex = u16(&inbuf[24]), ey = u16(&inbuf[26]);
                        const bool first = !sawButton;
                        const int wantX = first ? 50 : 150, wantY = first ? 50 : 100;
                        sawButton = true;
                        line(first ? "ButtonPress event #1" : "ButtonPress event #2",
                             rx == wantX && ry == wantY && ex == wantX - 10 && ey == wantY - 10,
                             "root " + std::to_string(rx) + "," + std::to_string(ry) +
                             " window " + std::to_string(ex) + "," + std::to_string(ey) +
                             " (posted " + std::to_string(wantX) + "," + std::to_string(wantY) + ")");
                    }
                    else if (type == 2 && inbuf[1] == 38)   // KeyPress, keycode 38
                        sawKey = true;
                    take(32);
                }
            }
            if (!sawButton)
                line("ButtonPress event", false, "none");
            line("KeyPress event (keycode 38)", sawKey);

            // QueryPointer: where does the server itself believe the pointer is?
            {
                std::vector<uint8_t> q = { 38, 0 }; put16(q, 2); put32(q, root);
                send(q);
                const ULONGLONG qUntil = GetTickCount64() + 3000;
                bool sawQuery = false;
                while (!sawQuery && GetTickCount64() < qUntil)
                {
                    if (!recv(32, 1000))
                        break;
                    const uint8_t type = inbuf[0] & 0x7f;
                    const uint32_t extraLen = (type == 1) ? u32(&inbuf[4]) * 4 : 0;
                    if (type == 1 && recv(32 + extraLen, 3000) && inbuf[1] <= 1 && u16(&inbuf[2]) == 7)
                    {
                        sawQuery = true;
                        line("QueryPointer after clicks", u16(&inbuf[16]) == 150 && u16(&inbuf[18]) == 100,
                             "root " + std::to_string(u16(&inbuf[16])) + "," + std::to_string(u16(&inbuf[18])) +
                             " (last move posted 150,100)");
                    }
                    take(32 + extraLen);
                }
                if (!sawQuery)
                    line("QueryPointer after clicks", false, "no reply");
            }

            // AMBERX_PREVIEW_HOLD_MS keeps the display window up so a capture
            // script can photograph what the native window shows.
            if (const char* hold = getenv("AMBERX_PREVIEW_HOLD_MS"))
                Sleep(static_cast<DWORD>(atoi(hold)));
        }
        const uint8_t one = 0;
        line("data on closed channel refused", !c.SendData(2, &one, 1));

        line("close channel 1", c.CloseChannel(1));
        st = false;
        for (int i = 0; i < 4 && !st; ++i)
            st = c.Poll(f, 3000) == PipeRead::Ok && f.type == MsgType::HostStatus &&
                 ParseHostStatus(f.payload, open, bytes, cookie);
        line("status after close", st && open == 0,
             st ? "open=" + std::to_string(open) + " bytesIn=" + std::to_string(bytes) : "no status");
        line("double close refused", !c.CloseChannel(1));
        line("no host errors", hostErrors.empty(), hostErrors);

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
