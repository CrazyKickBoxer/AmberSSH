// WinBackend.cpp — the Windows half of AmberXHost: everything the X side
// asks for through amberwin.h, and nothing the X side can see.
//
// Three threads:
//   * the UI thread (wmain) owns the window and its message pump;
//   * the pipe thread reads AmberXControl frames from AmberSSH and turns them
//     into events and channel bytes;
//   * the server thread runs the X.Org core (amberx_server_main) and pulls
//     from the queues this file keeps.
//
// The X core is single-threaded by design, so every crossing is a queue plus
// one wake event: the UI and pipe threads only ever push; the server thread
// only ever pops. Channel writes and status frames go out under one mutex
// because FramedPipe is not thread-safe and two frames must never interleave.
//
// Rules kept here: no listener of any kind; nothing logged that came off a
// channel; cookie bytes copied into the event and nowhere else.
#include <Windows.h>
#include <bcrypt.h>
#include <windowsx.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../control/Pipe.h"
#include "../control/Protocol.h"
#include "../server/amberwin.h"
#include "WinBackend.h"

extern "C" {
#include "../../../third_party/amberx/config-msvc/compat/dirent.h"
}

using namespace amber::amberx;

namespace
{

// ---- shared state ----------------------------------------------------------
struct ChannelBuf
{
    std::vector<uint8_t> data;
    size_t rd = 0;
    bool closedByPeer = false;
};

struct Backend
{
    amberwin_config cfg{};
    std::string keymapPath;

    // events + channel bytes: pushed by UI/pipe threads, popped by the server
    std::mutex qmu;
    std::deque<amberwin_event> events;
    std::unordered_map<uint32_t, ChannelBuf> channels;
    HANDLE wake = nullptr;

    // outbound frames: server thread (data, status) and pipe thread (errors)
    std::mutex wmu;
    FramedPipe* pipe = nullptr;
    std::atomic<bool> pipeDown{false};

    // the screen
    HWND hwnd = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int stride = 0;
    int width = 0, height = 0;
    std::mutex presentMu;

    std::atomic<bool> serverExited{false};
};

Backend g;

void Wake()
{
    if (g.wake)
        SetEvent(g.wake);
}

void Push(const amberwin_event& ev)
{
    {
        std::lock_guard<std::mutex> lk(g.qmu);
        g.events.push_back(ev);
    }
    Wake();
}

bool SendFrame(MsgType type, uint32_t channel, std::vector<uint8_t> payload)
{
    std::lock_guard<std::mutex> lk(g.wmu);
    if (!g.pipe || g.pipeDown)
        return false;
    Frame f;
    f.type = type;
    f.channel = channel;
    f.payload = std::move(payload);
    if (!g.pipe->WriteFrame(f))
    {
        g.pipeDown = true;
        return false;
    }
    return true;
}

// ---- keycodes ----------------------------------------------------------------
// Windows reports PC/AT set-1 scan codes. For the main block those are the
// evdev codes; the E0-prefixed keys are numbered differently by evdev and
// need the table. A key not listed is dropped rather than guessed.
uint32_t EvdevFromScan(UINT scan, bool extended, WPARAM vk)
{
    if (vk == VK_PAUSE)
        return 119;
    if (!extended)
        return (scan >= 1 && scan <= 88) ? scan : 0;
    switch (scan)
    {
    case 0x1C: return 96;   // KP_Enter
    case 0x1D: return 97;   // Control_R
    case 0x35: return 98;   // KP_Divide
    case 0x37: return 99;   // Print
    case 0x38: return 100;  // Alt_R
    case 0x47: return 102;  // Home
    case 0x48: return 103;  // Up
    case 0x49: return 104;  // Prior
    case 0x4B: return 105;  // Left
    case 0x4D: return 106;  // Right
    case 0x4F: return 107;  // End
    case 0x50: return 108;  // Down
    case 0x51: return 109;  // Next
    case 0x52: return 110;  // Insert
    case 0x53: return 111;  // Delete
    case 0x5B: return 125;  // Super_L
    case 0x5C: return 126;  // Super_R
    case 0x5D: return 127;  // Menu
    case 0x45: return 69;   // Num_Lock
    default:   return 0;
    }
}

// ---- the window ---------------------------------------------------------------
void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (g.dib)
    {
        std::lock_guard<std::mutex> lk(g.presentMu);
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.dib);
        const RECT& r = ps.rcPaint;
        BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, r.left, r.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    EndPaint(hwnd, &ps);
}

void Button(int button, bool down)
{
    amberwin_event ev{};
    ev.type = AMBERWIN_EV_BUTTON;
    ev.button = button;
    ev.pressed = down ? 1 : 0;
    Push(ev);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        amberwin_event ev{};
        ev.type = AMBERWIN_EV_POINTER_MOVE;
        ev.x = GET_X_LPARAM(lp);
        ev.y = GET_Y_LPARAM(lp);
        Push(ev);
        return 0;
    }
    case WM_LBUTTONDOWN: SetCapture(hwnd); Button(1, true);  return 0;
    case WM_LBUTTONUP:   ReleaseCapture(); Button(1, false); return 0;
    case WM_MBUTTONDOWN: SetCapture(hwnd); Button(2, true);  return 0;
    case WM_MBUTTONUP:   ReleaseCapture(); Button(2, false); return 0;
    case WM_RBUTTONDOWN: SetCapture(hwnd); Button(3, true);  return 0;
    case WM_RBUTTONUP:   ReleaseCapture(); Button(3, false); return 0;
    case WM_MOUSEWHEEL:
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const int b = delta > 0 ? 4 : 5;
        for (int n = abs(delta) / WHEEL_DELTA; n > 0; --n)
        {
            Button(b, true);
            Button(b, false);
        }
        return 0;
    }
    case WM_MOUSEHWHEEL:
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const int b = delta > 0 ? 7 : 6;
        for (int n = abs(delta) / WHEEL_DELTA; n > 0; --n)
        {
            Button(b, true);
            Button(b, false);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        // bit 30: the key was already down — Windows' own autorepeat. XKB
        // repeats keys itself, so a repeated press is dropped here.
        if (down && (lp & (1 << 30)))
            return 0;
        const UINT scan = (lp >> 16) & 0xff;
        const bool ext = (lp & (1 << 24)) != 0;
        const uint32_t code = EvdevFromScan(scan, ext, wp);
        if (code)
        {
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_KEY;
            ev.keycode = code;
            ev.pressed = down ? 1 : 0;
            Push(ev);
        }
        // Alt/F10 would otherwise open the (absent) system menu
        return (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wp != VK_F4 ? 0
               : DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    {
        amberwin_event ev{};
        ev.type = AMBERWIN_EV_FOCUS;
        ev.pressed = (msg == WM_SETFOCUS) ? 1 : 0;
        Push(ev);
        return 0;
    }
    case WM_CLOSE:
    {
        // closing the window ends the display: tell the server, which
        // finishes its generation and returns; the pump exits when it has.
        amberwin_event ev{};
        ev.type = AMBERWIN_EV_SHUTDOWN;
        Push(ev);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- the pipe thread ----------------------------------------------------------
DWORD WINAPI PipeThread(LPVOID)
{
    for (;;)
    {
        Frame f;
        const PipeRead r = g.pipe->ReadFrame(f, 1000);
        if (r == PipeRead::Timeout)
        {
            if (g.serverExited)
                return 0;
            continue;
        }
        if (r == PipeRead::Closed || r == PipeRead::Bad)
        {
            g.pipeDown = true;
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_SHUTDOWN;
            Push(ev);
            return 0;
        }
        switch (f.type)
        {
        case MsgType::SetCookie:
        {
            std::vector<uint8_t> cookie;
            uint32_t display = 0;
            if (!ParseSetCookie(f.payload, cookie, display) || cookie.size() != 16)
            {
                SendFrame(MsgType::HostError, kControlChannel, MakeHostError("cookie payload malformed"));
                break;
            }
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_COOKIE;
            ev.display = display;
            memcpy(ev.cookie, cookie.data(), 16);
            Push(ev);
            std::fill(cookie.begin(), cookie.end(), 0);
            break;
        }
        case MsgType::ChannelOpen:
        {
            bool ok;
            {
                std::lock_guard<std::mutex> lk(g.qmu);
                ok = f.channel != 0 && g.channels.size() < ChannelTable::kMaxChannels &&
                     g.channels.find(f.channel) == g.channels.end();
                if (ok)
                    g.channels.emplace(f.channel, ChannelBuf{});
            }
            if (!ok)
            {
                SendFrame(MsgType::HostError, kControlChannel, MakeHostError("channel refused"));
                break;
            }
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_CHANNEL_OPEN;
            ev.channel = f.channel;
            Push(ev);
            break;
        }
        case MsgType::ChannelData:
        {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(g.qmu);
                auto it = g.channels.find(f.channel);
                if (it != g.channels.end() && !it->second.closedByPeer)
                {
                    ChannelBuf& b = it->second;
                    // compact once the read cursor has passed the halfway mark
                    if (b.rd > 0 && b.rd >= b.data.size() / 2)
                    {
                        b.data.erase(b.data.begin(), b.data.begin() + static_cast<std::ptrdiff_t>(b.rd));
                        b.rd = 0;
                    }
                    b.data.insert(b.data.end(), f.payload.begin(), f.payload.end());
                    ok = true;
                }
            }
            if (ok)
                Wake();
            else
                SendFrame(MsgType::HostError, kControlChannel, MakeHostError("data on a channel that is not open"));
            break;
        }
        case MsgType::ChannelClose:
        {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(g.qmu);
                auto it = g.channels.find(f.channel);
                if (it != g.channels.end())
                {
                    it->second.closedByPeer = true;
                    ok = true;
                }
            }
            if (!ok)
            {
                SendFrame(MsgType::HostError, kControlChannel, MakeHostError("close of a channel that is not open"));
                break;
            }
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_CHANNEL_CLOSE;
            ev.channel = f.channel;
            Push(ev);
            break;
        }
        case MsgType::Shutdown:
        {
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_SHUTDOWN;
            Push(ev);
            return 0;
        }
        default:
            SendFrame(MsgType::HostError, kControlChannel, MakeHostError("unexpected message after handshake"));
            break;
        }
    }
}

DWORD WINAPI ServerThread(LPVOID)
{
    const char* argv[] = { "AmberXHost", nullptr };
    const int rc = amberx_server_main(1, const_cast<char**>(argv));
    g.serverExited = true;
    if (g.hwnd)
        PostMessageW(g.hwnd, WM_DESTROY, 0, 0);
    return static_cast<DWORD>(rc);
}

} // namespace

// ---- WinBackend.h ---------------------------------------------------------------
namespace amber::amberx
{

bool BackendInit(FramedPipe& pipe, int width, int height, uint32_t display,
                 const std::string& keymapPath, std::string& err)
{
    g.pipe = &pipe;
    g.keymapPath = keymapPath;
    g.cfg.width = width;
    g.cfg.height = height;
    g.cfg.depth = 24;
    g.cfg.display = display;
    g.cfg.keymap_path = g.keymapPath.empty() ? nullptr : g.keymapPath.c_str();
    g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.wake)
    {
        err = "CreateEvent failed";
        return false;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AmberXDisplay";
    wc.style = CS_OWNDC;
    if (!RegisterClassW(&wc))
    {
        err = "RegisterClass failed";
        return false;
    }
    RECT r{ 0, 0, width, height };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    wchar_t title[64];
    swprintf_s(title, L"AmberX :%u", display);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.hwnd)
    {
        err = "CreateWindow failed";
        return false;
    }
    return true;
}

int BackendRun()
{
    HANDLE pipeThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    HANDLE serverThread = CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
    if (!pipeThread || !serverThread)
        return 73;
    ShowWindow(g.hwnd, SW_SHOWNORMAL);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // the server has returned (it posted WM_DESTROY) or the window was
    // torn down; give the server a moment to finish its generation
    if (!g.serverExited)
    {
        amberwin_event ev{};
        ev.type = AMBERWIN_EV_SHUTDOWN;
        Push(ev);
        WaitForSingleObject(serverThread, 5000);
    }
    DWORD rc = 0;
    GetExitCodeThread(serverThread, &rc);
    g.serverExited = true;
    WaitForSingleObject(pipeThread, 2000);
    CloseHandle(serverThread);
    CloseHandle(pipeThread);
    return static_cast<int>(rc);
}

} // namespace amber::amberx

// ---- amberwin.h: the C surface ----------------------------------------------------
extern "C" {

const amberwin_config* amberwin_get_config(void)
{
    return &g.cfg;
}

uint32_t amberwin_now_ms(void)
{
    return static_cast<uint32_t>(GetTickCount64());
}

uint64_t amberwin_now_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart) * 1000000ull / static_cast<uint64_t>(f.QuadPart);
}

int amberwin_random(void* buf, size_t len)
{
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 1 : 0;
}

void amberwin_log(int level, const char* text)
{
    static const char* tag[] = { "II", "WW", "EE", "FF" };
    if (level < 0 || level > 3)
        level = 2;
    fprintf(stderr, "AmberX (%s) %s\n", tag[level], text);
    if (level >= AMBERWIN_LOG_ERROR)
        SendFrame(MsgType::HostError, kControlChannel, MakeHostError(text));
}

void amberwin_exit(int code)
{
    ExitProcess(static_cast<UINT>(code));
}

int amberwin_screen_create(int width, int height, void** bits, int* stride_bytes)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;    // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!dib || !p)
        return 0;
    memset(p, 0, static_cast<size_t>(width) * 4u * static_cast<size_t>(height));
    g.dib = dib;
    g.bits = p;
    g.stride = width * 4;
    g.width = width;
    g.height = height;
    *bits = p;
    *stride_bytes = g.stride;
    return 1;
}

void amberwin_screen_present(int x, int y, int w, int h)
{
    if (!g.hwnd)
        return;
    RECT r{ x, y, x + w, y + h };
    InvalidateRect(g.hwnd, &r, FALSE);
}

void amberwin_bell(int percent)
{
    if (percent > 0)
        MessageBeep(MB_OK);
}

int amberwin_wait(int timeout_ms)
{
    {
        std::lock_guard<std::mutex> lk(g.qmu);
        if (!g.events.empty())
            return 1;
        for (auto& kv : g.channels)
            if (kv.second.data.size() > kv.second.rd)
                return 1;
    }
    const DWORD t = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    return WaitForSingleObject(g.wake, t) == WAIT_OBJECT_0 ? 1 : 0;
}

int amberwin_next_event(amberwin_event* ev)
{
    std::lock_guard<std::mutex> lk(g.qmu);
    if (g.events.empty())
        return 0;
    *ev = g.events.front();
    g.events.pop_front();
    return 1;
}

int amberwin_readable_channels(uint32_t* ids, int cap)
{
    std::lock_guard<std::mutex> lk(g.qmu);
    int n = 0;
    for (auto& kv : g.channels)
    {
        if (n >= cap)
            break;
        if (kv.second.data.size() > kv.second.rd)
            ids[n++] = kv.first;
    }
    return n;
}

int amberwin_channel_read(uint32_t ch, void* buf, size_t cap)
{
    std::lock_guard<std::mutex> lk(g.qmu);
    auto it = g.channels.find(ch);
    if (it == g.channels.end())
        return -1;
    ChannelBuf& b = it->second;
    const size_t avail = b.data.size() - b.rd;
    if (avail == 0)
        return b.closedByPeer ? -1 : 0;
    const size_t n = avail < cap ? avail : cap;
    memcpy(buf, b.data.data() + b.rd, n);
    b.rd += n;
    return static_cast<int>(n);
}

int amberwin_channel_write(uint32_t ch, const void* buf, size_t len)
{
    {
        std::lock_guard<std::mutex> lk(g.qmu);
        auto it = g.channels.find(ch);
        if (it == g.channels.end() || it->second.closedByPeer)
            return -1;
    }
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (len > 0)
    {
        const size_t n = len < kMaxPayload ? len : kMaxPayload;
        if (!SendFrame(MsgType::ChannelData, ch, std::vector<uint8_t>(p, p + n)))
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

void amberwin_channel_close(uint32_t ch)
{
    bool tellPeer = false;
    {
        std::lock_guard<std::mutex> lk(g.qmu);
        auto it = g.channels.find(ch);
        if (it != g.channels.end())
        {
            tellPeer = !it->second.closedByPeer;
            g.channels.erase(it);
        }
    }
    if (tellPeer)
        SendFrame(MsgType::ChannelClose, ch, {});
}

void amberwin_report(uint32_t open_clients, uint64_t bytes_in, int cookie_set)
{
    SendFrame(MsgType::HostStatus, kControlChannel,
              MakeHostStatus(open_clients, bytes_in, cookie_set != 0));
}

// ---- SHA-1 for the core's authorization ids (os/xsha1.h) ---------------------
// upstream os/xsha1.c has a CryptoAPI backend, but it reaches <windows.h>
// from inside an X translation unit; this is the same thing on this side.
void* x_sha1_init(void)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        return nullptr;
    }
    BCryptCloseAlgorithmProvider(alg, 0);   // the hash keeps its own reference
    return h;
}

int x_sha1_update(void* ctx, void* data, int size)
{
    return BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(ctx), static_cast<PUCHAR>(data),
                          static_cast<ULONG>(size), 0) == 0 ? 1 : 0;
}

int x_sha1_final(void* ctx, unsigned char result[20])
{
    const bool ok = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(ctx), result, 20, 0) == 0;
    BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(ctx));
    return ok ? 1 : 0;
}

// ---- dirent for libXfont2's directory scan (config-msvc/compat/dirent.h) ------
struct AmberWinDir
{
    HANDLE h = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAA fd{};
    bool first = true;
    bool done = false;
    struct dirent ent{};
};

DIR* opendir(const char* name)
{
    std::string pat = name;
    if (!pat.empty() && pat.back() != '/' && pat.back() != '\\')
        pat += '\\';
    pat += '*';
    AmberWinDir* d = new AmberWinDir;
    d->h = FindFirstFileA(pat.c_str(), &d->fd);
    if (d->h == INVALID_HANDLE_VALUE)
    {
        delete d;
        return nullptr;
    }
    return d;
}

struct dirent* readdir(DIR* dir)
{
    AmberWinDir* d = dir;
    if (!d || d->done)
        return nullptr;
    if (!d->first)
    {
        if (!FindNextFileA(d->h, &d->fd))
        {
            d->done = true;
            return nullptr;
        }
    }
    d->first = false;
    strncpy_s(d->ent.d_name, d->fd.cFileName, _TRUNCATE);
    return &d->ent;
}

int closedir(DIR* dir)
{
    AmberWinDir* d = dir;
    if (!d)
        return -1;
    if (d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    delete d;
    return 0;
}

} // extern "C"

// ---- libXfont2's POSIX leftovers -----------------------------------------------
// libXfont2 calls strcasecmp by its POSIX name. Inside X translation units os.h maps both to x-prefixed versions;
// this unit has no os.h, so it can carry the plain names.
extern "C" {
int strcasecmp(const char* a, const char* b)
{
    return _stricmp(a, b);
}
}
