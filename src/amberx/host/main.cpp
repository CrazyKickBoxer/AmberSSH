// AmberXHost — the isolated per-session process that contains the X server.
//
// This file establishes everything around the server — the process boundary,
// the authenticated pipe, the lifetime rules — and then hands over. With the
// X.Org core built in (AMBERX_HAVE_SERVER), the Windows backend takes the
// pipe and runs the server on its own thread; without it, the loop below
// counts frames so the transport can be exercised on its own.
//
// Rules this process keeps regardless of what it grows into:
//   * it never logs a cookie, a nonce, a secret or a byte of channel data;
//   * it exits when its parent does, whether or not the Job Object got there
//     first;
//   * it opens no listener of any kind.
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../control/Handshake.h"
#include "../control/Pipe.h"
#include "../control/Protocol.h"
#ifdef AMBERX_HAVE_SERVER
#include "WinBackend.h"
#endif

using namespace amber::amberx;

namespace
{

struct Args
{
    std::wstring pipe;
    HANDLE secretHandle = nullptr;
    DWORD parentPid = 0;
    int width = 1280;
    int height = 800;
    uint32_t display = 0;
    std::string keymap;     // an .xkm path, or empty for the built-in map
    bool rootless = true;
    std::string identity, sigil;
    std::string mode = "X11 FORWARDED";
    int skin = 0;
};

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool ParseArgs(int argc, wchar_t** argv, Args& a)
{
    for (int i = 1; i < argc; i += 2)
    {
        const std::wstring k = argv[i];
        const std::wstring v = (i + 1 < argc) ? argv[i + 1] : L"";
        if (k == L"--pipe")
            a.pipe = v;
        else if (k == L"--secret-handle")
            a.secretHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(v.c_str(), nullptr, 10)));
        else if (k == L"--parent")
            a.parentPid = static_cast<DWORD>(wcstoul(v.c_str(), nullptr, 10));
        else if (k == L"--geometry")
        {
            if (swscanf_s(v.c_str(), L"%dx%d", &a.width, &a.height) != 2)
                return false;
        }
        else if (k == L"--display")
            a.display = static_cast<uint32_t>(wcstoul(v.c_str(), nullptr, 10));
        else if (k == L"--keymap")
            a.keymap = Narrow(v);
        else if (k == L"--rootful")
        {
            a.rootless = false;
            --i;   // a flag, not a pair
        }
        else if (k == L"--identity")
            a.identity = Narrow(v);
        else if (k == L"--mode")
            a.mode = Narrow(v);
        else if (k == L"--sigil")
            a.sigil = Narrow(v);
        else if (k == L"--skin")
            a.skin = static_cast<int>(wcstol(v.c_str(), nullptr, 10));
        else
            return false;
    }
    if (a.width < 64 || a.height < 64 || a.width > 16384 || a.height > 16384)
        return false;
    return !a.pipe.empty() && a.secretHandle && a.parentPid;
}

// The secret arrives on an inherited anonymous-pipe handle, never on the
// command line or in the environment, so it is not readable from a process
// listing. Read exactly the expected size and close the handle at once.
bool ReadSecret(HANDLE h, std::vector<uint8_t>& secret)
{
    secret.assign(kSecretBytes, 0);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, secret.data(), static_cast<DWORD>(secret.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != secret.size())
    {
        secret.assign(secret.size(), 0);
        secret.clear();
        return false;
    }
    return true;
}

bool ParentGone(HANDLE parent)
{
    return parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0;
}

// Ends the process when the parent goes away, even while the server is busy
// or blocked: a second line of defence behind the Job Object.
DWORD WINAPI ParentWatch(LPVOID p)
{
    HANDLE parent = static_cast<HANDLE>(p);
    if (parent)
        WaitForSingleObject(parent, INFINITE);
    ExitProcess(0);
}

#ifndef AMBERX_HAVE_SERVER
bool SendStatus(FramedPipe& p, const ChannelTable& t, uint64_t bytesIn, bool cookieSet)
{
    Frame f;
    f.type = MsgType::HostStatus;
    f.channel = kControlChannel;
    f.payload = MakeHostStatus(static_cast<uint32_t>(t.Count()), bytesIn, cookieSet);
    return p.WriteFrame(f);
}

bool SendError(FramedPipe& p, const char* what)
{
    Frame f;
    f.type = MsgType::HostError;
    f.channel = kControlChannel;
    f.payload = MakeHostError(what);   // a fixed description, never an echo
    return p.WriteFrame(f);
}
#endif

} // namespace

// The job's DIE_ON_UNHANDLED_EXCEPTION ends the process without a dialog;
// this runs first and leaves one line in the log saying where. Nothing here
// touches the heap or any server state.
LONG WINAPI CrashLine(EXCEPTION_POINTERS* ep)
{
    const auto* r = ep ? ep->ExceptionRecord : nullptr;
    if (r)
    {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(r->ExceptionAddress), &mod);
        wchar_t name[MAX_PATH] = L"?";
        if (mod)
            GetModuleFileNameW(mod, name, MAX_PATH);
        const uintptr_t off = reinterpret_cast<uintptr_t>(r->ExceptionAddress) - reinterpret_cast<uintptr_t>(mod);
        fprintf(stderr, "AmberXHost: unhandled exception 0x%08lx at %ls+0x%llx", r->ExceptionCode, name,
                static_cast<unsigned long long>(off));
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
            fprintf(stderr, " (%s 0x%llx)", r->ExceptionInformation[0] ? "write" : "read",
                    static_cast<unsigned long long>(r->ExceptionInformation[1]));
        fputc('\n', stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int wmain(int argc, wchar_t** argv)
{
    SetUnhandledExceptionFilter(CrashLine);
    // An X server's pixels are the pixels: without this, a scaled desktop
    // makes Windows render the display window enlarged and blurred, and
    // divides every mouse coordinate in its queue by the scale factor.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Args a;
    if (!ParseArgs(argc, argv, a))
    {
        fputs("AmberXHost: not for direct use — launched by AmberSSH\n", stderr);
        return 64;
    }

    std::vector<uint8_t> secret;
    if (!ReadSecret(a.secretHandle, secret))
        return 65;

    // SYNCHRONIZE only: enough to notice the parent has exited, not enough
    // to do anything to it.
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, a.parentPid);

    std::string err;
    HANDLE h = ConnectClientPipe(a.pipe, 10000, err);
    if (h == INVALID_HANDLE_VALUE)
        return 66;
    FramedPipe pipe;
    pipe.Adopt(h);

    // ---- handshake: the host does not speak until the controller has ----
    HostHandshake hs;
    if (hs.Begin(secret, RandomBytes(kNonceBytes)).state == Step::State::Failed)
        return 67;
    secret.assign(secret.size(), 0);   // the machine has its own copy now
    while (!hs.Done() && !hs.Failed())
    {
        Frame f;
        const PipeRead r = pipe.ReadFrame(f, 10000);
        if (r != PipeRead::Ok)
            return 68;
        const Step s = hs.OnFrame(f);
        if (s.hasSend && !pipe.WriteFrame(s.send))
            return 69;
        if (s.state == Step::State::Failed)
            return 70;
    }

    if (parent)
        CloseHandle(CreateThread(nullptr, 0, ParentWatch, parent, 0, nullptr));

#ifdef AMBERX_HAVE_SERVER
    // ---- the server -------------------------------------------------------
    HostOptions opt;
    opt.rootless = a.rootless;
    opt.width = a.width;
    opt.height = a.height;
    opt.display = a.display;
    opt.keymap = a.keymap;
    opt.identity = a.identity.empty() ? "AmberSSH session" : a.identity;
    opt.mode = a.mode;
    opt.sigil = a.sigil;
    opt.skin = a.skin;
    if (!BackendInit(pipe, opt, err))
    {
        fprintf(stderr, "AmberXHost: %s\n", err.c_str());
        return 74;
    }
    return BackendRun();
#else
    // ---- transport-only loop: counts frames, contains no X11 --------------
    ChannelTable channels;
    uint64_t bytesIn = 0;
    bool cookieSet = false;
    std::vector<uint8_t> cookie;
    uint32_t display = 0;

    for (;;)
    {
        if (ParentGone(parent))
            return 0;
        Frame f;
        const PipeRead r = pipe.ReadFrame(f, 1000);
        if (r == PipeRead::Timeout)
            continue;
        if (r == PipeRead::Closed)
            return 0;
        if (r == PipeRead::Bad)
            return 71;

        switch (f.type)
        {
        case MsgType::SetCookie:
            if (!ParseSetCookie(f.payload, cookie, display))
                SendError(pipe, "cookie payload malformed");
            else
            {
                cookieSet = true;   // the value is kept, never printed
                SendStatus(pipe, channels, bytesIn, cookieSet);
            }
            break;
        case MsgType::ChannelOpen:
            if (!channels.Open(f.channel))
                SendError(pipe, "channel refused");
            else
                SendStatus(pipe, channels, bytesIn, cookieSet);
            break;
        case MsgType::ChannelData:
            if (!channels.IsOpen(f.channel))
                SendError(pipe, "data on a channel that is not open");
            else
                bytesIn += f.payload.size();   // counted, never inspected
            break;
        case MsgType::ChannelClose:
            if (!channels.Close(f.channel))
                SendError(pipe, "close of a channel that is not open");
            else
                SendStatus(pipe, channels, bytesIn, cookieSet);
            break;
        case MsgType::Shutdown:
            SendStatus(pipe, channels, bytesIn, cookieSet);
            return 0;
        default:
            // Hello/HelloAck/AuthProof after the handshake, or a host-only
            // type arriving from the controller: a protocol error.
            SendError(pipe, "unexpected message after handshake");
            return 72;
        }
    }
#endif
}
