// WinBackend.cpp — the Windows half of AmberXHost: everything the X side
// asks for through amberwin.h, and nothing the X side can see.
//
// Three threads:
//   * the UI thread (wmain) owns every window and its message pump;
//   * the pipe thread reads AmberXControl frames from AmberSSH and turns them
//     into events and channel bytes;
//   * the server thread runs the X.Org core (amberx_server_main) and pulls
//     from the queues this file keeps.
//
// The X core is single-threaded by design, so every crossing is a queue plus
// one wake event: the UI and pipe threads only ever push; the server thread
// only ever pops. Frame operations the server thread requests are marshalled
// to the UI thread with SendMessage to a message-only window, because a
// window belongs to the thread that created it. Channel writes and status
// frames go out under one mutex because FramedPipe is not thread-safe and
// two frames must never interleave.
//
// Rootless frames (Phase 3): each top-level X window is an owner window
// carrying the identity strip — painted here, from AmberSSH's own chrome
// table, and never reachable by X drawing — and a view child that shows the
// frame's DIB. The X side draws straight into that DIB.
//
// Rules kept here: no listener of any kind; nothing logged that came off a
// channel; cookie bytes copied into the event and nowhere else.
#include <Windows.h>
#include <bcrypt.h>
#include <windowsx.h>
#include <shellscalingapi.h>

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
#include "../../ui/Chrome.h"
#include "WinBackend.h"
#include "WinKeymap.h"

extern "C" {
#include "../../../third_party/amberx/config-msvc/compat/dirent.h"
}

using namespace amber::amberx;

// ---- frames ------------------------------------------------------------------
struct amberwin_frame
{
    uint32_t xid = 0;
    HWND hwnd = nullptr;        // the owner window (caption, borders)
    HWND view = nullptr;        // the X pixels
    HWND strip = nullptr;       // the identity strip; null for override-redirect
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int stride = 0;
    int x = 0, y = 0, w = 1, h = 1;   // X screen coordinates / size of the view
    bool overrideRedirect = false;
    bool mapped = false;
    bool resizable = true;
    int minW = 0, minH = 0, maxW = 0, maxH = 0;
    amberwin_frame* owner = nullptr;
    bool modal = false;
    bool minimized = false, maximized = false, fullscreen = false;
    WINDOWPLACEMENT savedPlacement{};
    int settingPos = 0;         // >0 while we move it ourselves: no echo
    HICON icon = nullptr;
    int stripH = 0;
};

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
    std::string keymapPath, identity, mode, sigil;

    // events + channel bytes: pushed by UI/pipe threads, popped by the server
    std::mutex qmu;
    std::deque<amberwin_event> events;
    std::unordered_map<uint32_t, ChannelBuf> channels;
    // clipboard text from AmberSSH, waiting for the server thread to pull it;
    // at most one, because a clipboard has at most one current value
    std::vector<uint8_t> clipPending;
    HANDLE wake = nullptr;

    // outbound frames: server thread (data, status) and pipe thread (errors)
    std::mutex wmu;
    FramedPipe* pipe = nullptr;
    std::atomic<bool> pipeDown{false};

    // the rootful screen
    HWND hwnd = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int stride = 0;
    int width = 0, height = 0;
    std::mutex presentMu;

    // rootless
    HWND ops = nullptr;         // message-only window: frame ops run here
    DWORD uiThread = 0;
    HFONT stripFont = nullptr;
    int stripFontDpi = 0;

    std::atomic<bool> serverExited{false};
};

Backend g;
constexpr UINT WM_AMBER_OP = WM_APP + 7;
constexpr int kStripLogical = 22;

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

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
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

// ---- input shared by the rootful window and the views ---------------------------
void PushPointer(int sx, int sy)
{
    amberwin_event ev{};
    ev.type = AMBERWIN_EV_POINTER_MOVE;
    ev.x = sx;
    ev.y = sy;
    Push(ev);
}

void Button(int button, bool down)
{
    amberwin_event ev{};
    ev.type = AMBERWIN_EV_BUTTON;
    ev.button = button;
    ev.pressed = down ? 1 : 0;
    Push(ev);
}

// A wheel that reports finer than a notch (precision touchpads and most
// modern mice do) sends deltas smaller than WHEEL_DELTA. Truncating each
// message would throw those away and the surface would feel dead, so the
// remainder is carried and a button click is emitted per notch accumulated.
// The remainder is cleared when the direction reverses, which is what stops
// a slow scroll one way from paying for a slow scroll the other.
int g_wheelV, g_wheelH;

void Wheel(int delta, bool horizontal)
{
    int& acc = horizontal ? g_wheelH : g_wheelV;
    if ((acc > 0) != (delta > 0))
        acc = 0;
    acc += delta;
    while (acc >= WHEEL_DELTA)
    {
        acc -= WHEEL_DELTA;
        const int b = horizontal ? 7 : 4;
        Button(b, true);
        Button(b, false);
    }
    while (acc <= -WHEEL_DELTA)
    {
        acc += WHEEL_DELTA;
        const int b = horizontal ? 6 : 5;
        Button(b, true);
        Button(b, false);
    }
}

// Caps, Num and Scroll as Windows currently has them. Sent when a frame is
// activated, because that is the moment the X side's idea of the locks can
// be wrong: the user may have pressed Caps Lock in another application.
void PushLocks()
{
    amberwin_event ev{};
    ev.type = AMBERWIN_EV_LOCKS;
    ev.x = (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
    ev.y = (GetKeyState(VK_NUMLOCK) & 1) ? 1 : 0;
    ev.w = (GetKeyState(VK_SCROLL) & 1) ? 1 : 0;
    Push(ev);
}

// Windows sends AltGr as a left-control press immediately followed by a
// right-alt press, with nothing in the message to tell that control from a
// real one except that it arrives in the same tick. The X side treats
// right-alt as the level-three shift, so the phantom control has to go, and
// its release with it — otherwise the X side would see a control key that
// went up without ever going down, or worse, stay down forever.
//
// Returns true when the message should be dropped.
bool g_phantomCtrlHeld;

bool AltGrPhantom(WPARAM vk, bool ext, bool down)
{
    if (vk != VK_CONTROL || ext)
        return false;
    if (down)
    {
        MSG next;
        if (PeekMessageW(&next, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) &&
            (next.message == WM_KEYDOWN || next.message == WM_SYSKEYDOWN) &&
            next.wParam == VK_MENU && (next.lParam & (1 << 24)) != 0 &&
            next.time == static_cast<DWORD>(GetMessageTime()))
        {
            g_phantomCtrlHeld = true;
            return true;
        }
        return false;
    }
    if (g_phantomCtrlHeld)
    {
        g_phantomCtrlHeld = false;
        return true;
    }
    return false;
}

// Returns true when the message was consumed. Client coordinates become X
// screen coordinates through the window's screen position and the origin.
bool InputMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int originX, int originY)
{
    auto point = [&](int& sx, int& sy) {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &p);
        sx = p.x - originX;
        sy = p.y - originY;
    };
    switch (msg)
    {
    case WM_MOUSEMOVE:
    {
        int sx, sy;
        point(sx, sy);
        PushPointer(sx, sy);
        return true;
    }
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_MBUTTONUP:   case WM_RBUTTONUP:
    {
        int sx, sy;
        point(sx, sy);
        PushPointer(sx, sy);
        const bool down = (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN);
        const int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1
                    : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 2 : 3;
        // Capture while any button is down, and only then: releasing on the
        // first button up would drop a drag that started with two buttons,
        // and holding capture with none down would steal the pointer from
        // the rest of the desktop.
        static int held = 0;
        if (down)
        {
            if (++held == 1)
                SetCapture(hwnd);
        }
        else if (held > 0 && --held == 0)
            ReleaseCapture();
        Button(b, down);
        return true;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        Wheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
        return true;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        // bit 30: the key was already down — Windows' own autorepeat. XKB
        // repeats keys itself, so a repeated press is dropped here.
        if (down && (lp & (1 << 30)))
            return true;
        const UINT scan = (lp >> 16) & 0xff;
        const bool ext = (lp & (1 << 24)) != 0;
        if (AltGrPhantom(wp, ext, down))
            return true;
        const uint32_t code = EvdevFromScan(scan, ext, wp);
        if (code)
        {
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_KEY;
            ev.keycode = code;
            ev.pressed = down ? 1 : 0;
            Push(ev);
        }
        // Alt/F10 would otherwise open the system menu; Alt+F4 stays native
        return !((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wp == VK_F4);
    }
    }
    return false;
}

// ---- the rootful window -------------------------------------------------------
void PaintRootful(HWND hwnd)
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

LRESULT CALLBACK RootfulProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        PaintRootful(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
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
        amberwin_event ev{};
        ev.type = AMBERWIN_EV_SHUTDOWN;
        Push(ev);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    {
        // rootful: the window's client area is the root, origin (0,0)
        POINT o{ 0, 0 };
        ClientToScreen(hwnd, &o);
        if (InputMessage(hwnd, msg, wp, lp, o.x, o.y))
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- the identity strip -----------------------------------------------------------
// Painted from AmberSSH's chrome table so it follows the user's skin, and
// painted only here: it is a separate window, so no X drawing can reach it.
const amber::ChromeSpec& Spec()
{
    return amber::ChromeAt(g.cfg.skin);
}

COLORREF Rgb(uint32_t srgb)
{
    return RGB((srgb >> 16) & 0xff, (srgb >> 8) & 0xff, srgb & 0xff);
}

HFONT StripFont(int dpi)
{
    if (g.stripFont && g.stripFontDpi == dpi)
        return g.stripFont;
    if (g.stripFont)
        DeleteObject(g.stripFont);
    const amber::ChromeSpec& s = Spec();
    const wchar_t* face = s.uiFont ? s.uiFont : L"Segoe UI";
    const int px = -MulDiv(11, dpi, 72);
    g.stripFont = CreateFontW(px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, face);
    g.stripFontDpi = dpi;
    return g.stripFont;
}

int StripHeight(HWND hwnd)
{
    return MulDiv(kStripLogical, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

void PaintStrip(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    const amber::ChromeSpec& s = Spec();
    // Classic derives its colours from the terminal theme, which the host
    // does not have: a fixed dark ground with an amber accent stands in.
    const bool themed = !s.useThemeAccent;
    const COLORREF bg = themed ? Rgb(s.bg) : RGB(28, 28, 30);
    const COLORREF text = themed ? Rgb(s.text) : RGB(235, 235, 235);
    const COLORREF dim = themed ? Rgb(s.textDim) : RGB(160, 160, 160);
    const COLORREF accent = themed ? Rgb(s.neonA) : RGB(255, 136, 0);
    const COLORREF warn = themed ? Rgb(s.danger) : RGB(220, 60, 60);
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &r, b);
    DeleteObject(b);
    const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    RECT line{ r.left, r.bottom - MulDiv(2, dpi, 96), r.right, r.bottom };
    HBRUSH a = CreateSolidBrush(accent);
    FillRect(dc, &line, a);
    DeleteObject(a);

    HGDIOBJ oldFont = SelectObject(dc, StripFont(dpi));
    SetBkMode(dc, TRANSPARENT);
    std::wstring identity = Widen(g.identity), mode = Widen(g.mode), sigil = Widen(g.sigil);
    if (s.uppercase)
    {
        CharUpperW(identity.data());
        CharUpperW(mode.data());
    }
    // the mode word is coloured only when it is an enforced one
    const COLORREF modeColour = (g.mode == "RESTRICTED") ? accent : (g.mode == "TRUSTED") ? warn : text;
    const std::wstring dot = L"  ·  ";
    RECT tr = r;
    tr.left += MulDiv(10, dpi, 96);
    tr.bottom = line.top;
    auto draw = [&](const std::wstring& t, COLORREF c) {
        SetTextColor(dc, c);
        RECT calc = tr;
        DrawTextW(dc, t.c_str(), -1, &calc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(dc, t.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        tr.left += calc.right - calc.left;
    };
    draw(identity, text);
    draw(dot, dim);
    draw(mode, modeColour);
    if (!sigil.empty())
    {
        draw(dot, dim);
        draw(sigil, text);
    }
    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        PaintStrip(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        // dragging the strip moves the frame, as a caption would
        SendMessageW(GetParent(hwnd), WM_NCLBUTTONDOWN, HTCAPTION, lp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- frame windows ----------------------------------------------------------------
amberwin_frame* FrameOf(HWND hwnd)
{
    return reinterpret_cast<amberwin_frame*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

DWORD FrameStyle(const amberwin_frame* f)
{
    if (f->overrideRedirect || f->fullscreen)
        return WS_POPUP;
    DWORD s = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (f->resizable)
        s |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    return s;
}

DWORD FrameExStyle(const amberwin_frame* f)
{
    return f->overrideRedirect ? (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE) : 0;
}

// The frame's window rectangle on the desktop for a given view rectangle
// (X screen coordinates), accounting for the strip and the native borders.
RECT FrameRectFor(const amberwin_frame* f, int x, int y, int w, int h)
{
    RECT r{ x + g.cfg.desktop_x, y + g.cfg.desktop_y - f->stripH,
            x + g.cfg.desktop_x + w, y + g.cfg.desktop_y + h };
    AdjustWindowRectExForDpi(&r, FrameStyle(f), FALSE, FrameExStyle(f),
                             f->hwnd ? GetDpiForWindow(f->hwnd) : GetDpiForSystem());
    return r;
}

void LayoutChildren(amberwin_frame* f)
{
    RECT c;
    GetClientRect(f->hwnd, &c);
    const int cw = c.right - c.left, ch = c.bottom - c.top;
    if (f->strip)
        SetWindowPos(f->strip, nullptr, 0, 0, cw, f->stripH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(f->view, nullptr, 0, f->stripH, cw, ch - f->stripH > 0 ? ch - f->stripH : 1,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void AllocDib(amberwin_frame* f, int w, int h)
{
    if (f->dib)
    {
        DeleteObject(f->dib);
        f->dib = nullptr;
        f->bits = nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HDC dc = GetDC(nullptr);
    f->dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (f->dib && p)
    {
        memset(p, 0, static_cast<size_t>(w) * 4u * static_cast<size_t>(h));
        f->bits = p;
        f->stride = w * 4;
    }
}

void ReportConfigure(amberwin_frame* f)
{
    if (f->settingPos || !f->mapped || f->minimized)
        return;
    RECT vr;
    GetWindowRect(f->view, &vr);
    amberwin_event ev{};
    ev.type = AMBERWIN_EV_FRAME_CONFIGURE;
    ev.xid = f->xid;
    ev.x = vr.left - g.cfg.desktop_x;
    ev.y = vr.top - g.cfg.desktop_y;
    ev.w = vr.right - vr.left;
    ev.h = vr.bottom - vr.top;
    if (ev.x == f->x && ev.y == f->y && ev.w == f->w && ev.h == f->h)
        return;
    f->x = ev.x; f->y = ev.y;   // what the X side is about to be told
    Push(ev);
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    amberwin_frame* f = FrameOf(hwnd);
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CLOSE:
        if (f)
        {
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_FRAME_CLOSE;
            ev.xid = f->xid;
            Push(ev);
        }
        return 0;   // the X side decides; the window never closes on its own
    case WM_ACTIVATE:
        if (f)
        {
            const bool active = LOWORD(wp) != WA_INACTIVE;
            if (active)
            {
                SetFocus(f->view);
                // the locks may have been changed in another application
                // while this frame was not the one receiving keys
                PushLocks();
            }
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_FRAME_ACTIVATE;
            ev.xid = f->xid;
            ev.pressed = active ? 1 : 0;
            Push(ev);
        }
        return 0;
    case WM_INPUTLANGCHANGE:
        // the user switched keyboard layout: re-read it and tell the X side,
        // which rebuilds its map and notifies clients through XKB
        if (ReadCurrentLayout())
        {
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_KEYMAP;
            Push(ev);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_DISPLAYCHANGE:
    {
        amberwin_event ev{};
        ev.type = AMBERWIN_EV_MONITORS;
        Push(ev);
        return 0;
    }
    case WM_SIZE:
        if (f)
        {
            LayoutChildren(f);
            const bool mn = IsIconic(hwnd) != 0, mx = IsZoomed(hwnd) != 0;
            if (mn != f->minimized || mx != f->maximized)
            {
                f->minimized = mn;
                f->maximized = mx;
                amberwin_event ev{};
                ev.type = AMBERWIN_EV_FRAME_STATE;
                ev.xid = f->xid;
                ev.x = mn ? 1 : 0;
                ev.y = mx ? 1 : 0;
                Push(ev);
            }
        }
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (f)
        {
            LayoutChildren(f);
            ReportConfigure(f);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (f)
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            RECT z{ 0, 0, 0, 0 };
            AdjustWindowRectExForDpi(&z, FrameStyle(f), FALSE, FrameExStyle(f), GetDpiForWindow(hwnd));
            const int fw = z.right - z.left, fh = z.bottom - z.top + f->stripH;
            if (f->minW > 0) mmi->ptMinTrackSize.x = f->minW + fw;
            if (f->minH > 0) mmi->ptMinTrackSize.y = f->minH + fh;
            if (f->maxW > 0) mmi->ptMaxTrackSize.x = f->maxW + fw;
            if (f->maxH > 0) mmi->ptMaxTrackSize.y = f->maxH + fh;
        }
        return 0;
    case WM_DPICHANGED:
        if (f)
        {
            // keep the pixel size — X clients do not scale — accept the
            // position; the strip re-measures itself for the new DPI
            const RECT* sug = reinterpret_cast<const RECT*>(lp);
            f->stripH = f->strip ? StripHeight(hwnd) : 0;
            RECT r = FrameRectFor(f, f->x, f->y, f->w, f->h);
            f->settingPos++;
            SetWindowPos(hwnd, nullptr, sug->left, sug->top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            f->settingPos--;
            if (f->strip)
                InvalidateRect(f->strip, nullptr, TRUE);
            ReportConfigure(f);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    amberwin_frame* f = FrameOf(hwnd);
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (f && f->dib)
        {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, f->dib);
            const RECT& r = ps.rcPaint;
            BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, r.left, r.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    }
    if (InputMessage(hwnd, msg, wp, lp, g.cfg.desktop_x, g.cfg.desktop_y))
        return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- frame operations, executed on the UI thread ------------------------------------
enum class Op { Create, Destroy, Move, Resize, Restack, Unmap, Shape, Title, Icon, Hints,
                Transient, State, Activate, SetXid };

struct FrameOp
{
    Op op;
    amberwin_frame* f = nullptr;
    amberwin_frame* other = nullptr;
    int x = 0, y = 0, w = 0, h = 0;
    int a = 0, b = 0, c = 0, d = 0;
    uint32_t xid = 0;
    const char* text = nullptr;
    const int16_t* boxes = nullptr;
    const uint32_t* argb = nullptr;
    bool ok = false;
};

void ApplyStyle(amberwin_frame* f)
{
    SetWindowLongPtrW(f->hwnd, GWL_STYLE, static_cast<LONG_PTR>(FrameStyle(f)));
    SetWindowLongPtrW(f->hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(FrameExStyle(f)));
    f->settingPos++;
    SetWindowPos(f->hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    f->settingPos--;
}

void PlaceFrame(amberwin_frame* f, int x, int y, int w, int h)
{
    RECT r = FrameRectFor(f, x, y, w, h);
    f->settingPos++;
    SetWindowPos(f->hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    f->settingPos--;
}

void DoCreate(FrameOp& o)
{
    amberwin_frame* f = new amberwin_frame;
    f->xid = o.xid;
    f->x = o.x; f->y = o.y; f->w = o.w > 0 ? o.w : 1; f->h = o.h > 0 ? o.h : 1;
    f->overrideRedirect = o.a != 0;
    f->stripH = f->overrideRedirect ? 0 : MulDiv(kStripLogical, static_cast<int>(GetDpiForSystem()), 96);
    RECT r = FrameRectFor(f, f->x, f->y, f->w, f->h);
    // Placement: a client asks for its window at (x, y) with no idea that a
    // caption and the strip sit above it. A managed frame is kept inside the
    // work area of the monitor it lands on, as any window manager would; the
    // X side learns the settled position through the configure report.
    // Override-redirect windows (menus, tooltips) go exactly where asked.
    if (!f->overrideRedirect)
    {
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof mi };
        if (GetMonitorInfoW(mon, &mi))
        {
            const int fw = r.right - r.left, fh = r.bottom - r.top;
            if (r.left < mi.rcWork.left) { r.left = mi.rcWork.left; r.right = r.left + fw; }
            if (r.top < mi.rcWork.top) { r.top = mi.rcWork.top; r.bottom = r.top + fh; }
            if (r.right > mi.rcWork.right) { r.left = mi.rcWork.right - fw; r.right = mi.rcWork.right; }
            if (r.bottom > mi.rcWork.bottom) { r.top = mi.rcWork.bottom - fh; r.bottom = mi.rcWork.bottom; }
        }
    }
    f->settingPos++;
    f->hwnd = CreateWindowExW(FrameExStyle(f), L"AmberXFrame", L"X11 window", FrameStyle(f),
                              r.left, r.top, r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, GetModuleHandleW(nullptr), f);
    if (!f->hwnd)
    {
        delete f;
        return;
    }
    if (!f->overrideRedirect)
    {
        f->stripH = StripHeight(f->hwnd);
        f->strip = CreateWindowExW(0, L"AmberXStrip", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, f->stripH,
                                   f->hwnd, nullptr, GetModuleHandleW(nullptr), f);
    }
    f->view = CreateWindowExW(0, L"AmberXView", L"", WS_CHILD | WS_VISIBLE, 0, f->stripH, f->w, f->h,
                              f->hwnd, nullptr, GetModuleHandleW(nullptr), f);
    AllocDib(f, f->w, f->h);
    f->settingPos--;
    LayoutChildren(f);
    // where the view actually landed, in X screen coordinates; if placement
    // moved it, the X side is told once the frame is shown
    {
        RECT vr;
        GetWindowRect(f->view, &vr);
        f->x = vr.left - g.cfg.desktop_x;
        f->y = vr.top - g.cfg.desktop_y;
        if (f->x != o.x || f->y != o.y)
        {
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_FRAME_CONFIGURE;
            ev.xid = f->xid;
            ev.x = f->x; ev.y = f->y; ev.w = f->w; ev.h = f->h;
            Push(ev);
        }
    }
    o.f = f;
    o.ok = f->view && f->bits;
}

void DoDestroy(amberwin_frame* f)
{
    if (f->owner && f->modal)
        EnableWindow(f->owner->hwnd, TRUE);
    f->settingPos++;
    if (f->hwnd)
        DestroyWindow(f->hwnd);
    if (f->dib)
        DeleteObject(f->dib);
    if (f->icon)
        DestroyIcon(f->icon);
    delete f;
}

void DoState(amberwin_frame* f, int minimized, int maximized, int fullscreen, int urgent)
{
    if ((fullscreen != 0) != f->fullscreen)
    {
        f->fullscreen = fullscreen != 0;
        if (f->fullscreen)
        {
            f->savedPlacement.length = sizeof f->savedPlacement;
            GetWindowPlacement(f->hwnd, &f->savedPlacement);
            f->stripH = 0;
            ApplyStyle(f);
            HMONITOR mon = MonitorFromWindow(f->hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof mi };
            GetMonitorInfoW(mon, &mi);
            f->settingPos++;
            SetWindowPos(f->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
            f->settingPos--;
            if (f->strip)
                ShowWindow(f->strip, SW_HIDE);
        }
        else
        {
            f->stripH = f->strip ? StripHeight(f->hwnd) : 0;
            ApplyStyle(f);
            if (f->strip)
                ShowWindow(f->strip, SW_SHOW);
            f->settingPos++;
            SetWindowPlacement(f->hwnd, &f->savedPlacement);
            f->settingPos--;
        }
        LayoutChildren(f);
        ReportConfigure(f);
    }
    if (f->mapped && !f->fullscreen)
    {
        if (minimized && !IsIconic(f->hwnd))
            ShowWindow(f->hwnd, SW_MINIMIZE);
        else if (!minimized && IsIconic(f->hwnd))
            ShowWindow(f->hwnd, SW_RESTORE);
        if (maximized && !IsZoomed(f->hwnd) && !minimized)
            ShowWindow(f->hwnd, SW_MAXIMIZE);
        else if (!maximized && IsZoomed(f->hwnd))
            ShowWindow(f->hwnd, SW_RESTORE);
    }
    if (urgent)
    {
        FLASHWINFO fi{ sizeof fi, f->hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&fi);
    }
}

void RunOp(FrameOp& o)
{
    amberwin_frame* f = o.f;
    switch (o.op)
    {
    case Op::Create:
        DoCreate(o);
        return;
    case Op::Destroy:
        DoDestroy(f);
        break;
    case Op::Move:
        f->x = o.x; f->y = o.y;
        PlaceFrame(f, f->x, f->y, f->w, f->h);
        break;
    case Op::Resize:
        f->x = o.x; f->y = o.y; f->w = o.w > 0 ? o.w : 1; f->h = o.h > 0 ? o.h : 1;
        AllocDib(f, f->w, f->h);
        PlaceFrame(f, f->x, f->y, f->w, f->h);
        LayoutChildren(f);
        InvalidateRect(f->view, nullptr, FALSE);
        break;
    case Op::Restack:
    {
        const bool wasMapped = f->mapped;
        f->mapped = true;
        HWND after = o.other && o.other->hwnd ? o.other->hwnd : HWND_TOP;
        UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
        if (f->overrideRedirect || wasMapped)
            flags |= SWP_NOACTIVATE;
        f->settingPos++;
        SetWindowPos(f->hwnd, after, 0, 0, 0, 0, flags);
        f->settingPos--;
        if (f->owner && f->modal)
            EnableWindow(f->owner->hwnd, FALSE);
        break;
    }
    case Op::Unmap:
        f->mapped = false;
        f->settingPos++;
        ShowWindow(f->hwnd, SW_HIDE);
        f->settingPos--;
        if (f->owner && f->modal)
            EnableWindow(f->owner->hwnd, TRUE);
        break;
    case Op::Shape:
        if (o.a == 0)
            SetWindowRgn(f->view, nullptr, TRUE);
        else
        {
            HRGN rgn = CreateRectRgn(0, 0, 0, 0);
            for (int i = 0; i < o.a; ++i)
            {
                HRGN r = CreateRectRgn(o.boxes[i * 4], o.boxes[i * 4 + 1], o.boxes[i * 4 + 2], o.boxes[i * 4 + 3]);
                CombineRgn(rgn, rgn, r, RGN_OR);
                DeleteObject(r);
            }
            SetWindowRgn(f->view, rgn, TRUE);   // the window owns rgn now
        }
        break;
    case Op::Title:
        SetWindowTextW(f->hwnd, Widen(o.text ? o.text : "").c_str());
        break;
    case Op::Icon:
    {
        if (f->icon)
        {
            DestroyIcon(f->icon);
            f->icon = nullptr;
        }
        if (o.w > 0 && o.h > 0 && o.argb)
        {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = o.w;
            bi.bmiHeader.biHeight = -o.h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* p = nullptr;
            HDC dc = GetDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (color && p)
            {
                memcpy(p, o.argb, static_cast<size_t>(o.w) * 4u * static_cast<size_t>(o.h));
                HBITMAP mask = CreateBitmap(o.w, o.h, 1, 1, nullptr);
                ICONINFO ii{ TRUE, 0, 0, mask, color };
                f->icon = CreateIconIndirect(&ii);
                DeleteObject(mask);
            }
            if (color)
                DeleteObject(color);
        }
        SendMessageW(f->hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(f->icon));
        SendMessageW(f->hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(f->icon));
        break;
    }
    case Op::Hints:
    {
        f->minW = o.a; f->minH = o.b; f->maxW = o.c; f->maxH = o.d;
        const bool resizable = o.x != 0;
        if (resizable != f->resizable)
        {
            f->resizable = resizable;
            ApplyStyle(f);
            PlaceFrame(f, f->x, f->y, f->w, f->h);
        }
        break;
    }
    case Op::Transient:
        if (f->owner && f->modal && f->owner != o.other)
            EnableWindow(f->owner->hwnd, TRUE);
        f->owner = o.other;
        f->modal = o.a != 0;
        SetWindowLongPtrW(f->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(f->owner ? f->owner->hwnd : nullptr));
        if (f->owner && f->modal && f->mapped)
            EnableWindow(f->owner->hwnd, FALSE);
        break;
    case Op::State:
        DoState(f, o.a, o.b, o.c, o.d);
        break;
    case Op::Activate:
        if (f->mapped)
        {
            if (IsIconic(f->hwnd))
                ShowWindow(f->hwnd, SW_RESTORE);
            SetForegroundWindow(f->hwnd);
        }
        break;
    case Op::SetXid:
        f->xid = o.xid;
        break;
    }
    o.ok = true;
}

LRESULT CALLBACK OpsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_AMBER_OP)
    {
        RunOp(*reinterpret_cast<FrameOp*>(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Runs a frame op on the UI thread and waits for it. Safe to call from the
// UI thread itself (runs inline) and from the server thread (SendMessage
// blocks until the UI thread has executed it).
void Marshal(FrameOp& o)
{
    if (GetCurrentThreadId() == g.uiThread)
        RunOp(o);
    else if (g.ops)
        SendMessageW(g.ops, WM_AMBER_OP, 0, reinterpret_cast<LPARAM>(&o));
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
        case MsgType::ClipboardText:
        {
            // AmberSSH has already applied the session's clipboard policy to
            // this text; the X side applies its own copy of it again.
            {
                std::lock_guard<std::mutex> lk(g.qmu);
                g.clipPending.assign(f.payload.begin(), f.payload.end());
            }
            amberwin_event ev{};
            ev.type = AMBERWIN_EV_CLIPBOARD;
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
    PostThreadMessageW(g.uiThread, WM_QUIT, static_cast<WPARAM>(rc), 0);
    return static_cast<DWORD>(rc);
}

} // namespace

// ---- WinBackend.h ---------------------------------------------------------------
namespace amber::amberx
{

bool BackendInit(FramedPipe& pipe, const HostOptions& opt, std::string& err)
{
    g.pipe = &pipe;
    g.keymapPath = opt.keymap;
    g.identity = opt.identity;
    g.mode = opt.mode;
    g.sigil = opt.sigil;
    g.uiThread = GetCurrentThreadId();
    g.cfg.rootless = opt.rootless ? 1 : 0;
    g.cfg.depth = 24;
    g.cfg.display = opt.display;
    g.cfg.keymap_path = g.keymapPath.empty() ? nullptr : g.keymapPath.c_str();
    g.cfg.identity = g.identity.c_str();
    g.cfg.mode = g.mode.c_str();
    g.cfg.skin = opt.skin;
    g.cfg.trusted = opt.trusted ? 1 : 0;
    g.cfg.auth_timeout_seconds = opt.authTimeout;
    g.cfg.clipboard_mode = opt.clipboard;
    // read the layout before the server thread starts: XKB asks for the
    // keymap while initialising the keyboard device, which is early
    ReadCurrentLayout();
    g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.wake)
    {
        err = "CreateEvent failed";
        return false;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto reg = [&](const wchar_t* cls, WNDPROC proc, UINT style, HCURSOR cur) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = cur;
        wc.lpszClassName = cls;
        wc.style = style;
        return RegisterClassW(&wc) != 0;
    };
    HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    if (!reg(L"AmberXDisplay", RootfulProc, CS_OWNDC, arrow) ||
        !reg(L"AmberXFrame", FrameProc, 0, arrow) ||
        !reg(L"AmberXView", ViewProc, CS_OWNDC, arrow) ||
        !reg(L"AmberXStrip", StripProc, 0, arrow) ||
        !reg(L"AmberXOps", OpsProc, 0, nullptr))
    {
        err = "RegisterClass failed";
        return false;
    }

    if (opt.rootless)
    {
        // the X screen is the virtual desktop; its origin may be negative
        g.cfg.desktop_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g.cfg.desktop_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g.cfg.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g.cfg.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (g.cfg.width < 64 || g.cfg.height < 64)
        {
            g.cfg.width = 1280;
            g.cfg.height = 800;
        }
        g.ops = CreateWindowExW(0, L"AmberXOps", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        if (!g.ops)
        {
            err = "could not create the ops window";
            return false;
        }
        return true;
    }

    g.cfg.width = opt.width;
    g.cfg.height = opt.height;
    RECT r{ 0, 0, opt.width, opt.height };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    wchar_t title[64];
    swprintf_s(title, L"AmberX :%u", opt.display);
    g.hwnd = CreateWindowExW(0, L"AmberXDisplay", title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
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
    if (g.hwnd)
        ShowWindow(g.hwnd, SW_SHOWNORMAL);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (!g.serverExited)
    {
        amberwin_event ev{};
        ev.type = AMBERWIN_EV_SHUTDOWN;
        Push(ev);
        // the server may be inside a SendMessage to this thread; keep
        // pumping while it winds down
        const ULONGLONG until = GetTickCount64() + 5000;
        while (!g.serverExited && GetTickCount64() < until)
        {
            if (MsgWaitForMultipleObjects(1, &serverThread, FALSE, 100, QS_ALLINPUT) == WAIT_OBJECT_0)
                break;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageW(&m);
        }
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

// ---- frames ----------------------------------------------------------------------
amberwin_frame* amberwin_frame_create(uint32_t xid, int x, int y, int w, int h, int override_redirect)
{
    FrameOp o{ Op::Create };
    o.xid = xid; o.x = x; o.y = y; o.w = w; o.h = h; o.a = override_redirect;
    Marshal(o);
    return o.ok ? o.f : nullptr;
}

void amberwin_frame_destroy(amberwin_frame* f)
{
    if (!f) return;
    FrameOp o{ Op::Destroy }; o.f = f; Marshal(o);
}

void amberwin_frame_move(amberwin_frame* f, int x, int y)
{
    if (!f) return;
    FrameOp o{ Op::Move }; o.f = f; o.x = x; o.y = y; Marshal(o);
}

void amberwin_frame_resize(amberwin_frame* f, int x, int y, int w, int h)
{
    if (!f) return;
    FrameOp o{ Op::Resize }; o.f = f; o.x = x; o.y = y; o.w = w; o.h = h; Marshal(o);
}

void amberwin_frame_restack(amberwin_frame* f, amberwin_frame* above)
{
    if (!f) return;
    FrameOp o{ Op::Restack }; o.f = f; o.other = above; Marshal(o);
}

void amberwin_frame_unmap(amberwin_frame* f)
{
    if (!f) return;
    FrameOp o{ Op::Unmap }; o.f = f; Marshal(o);
}

void amberwin_frame_set_shape(amberwin_frame* f, int nboxes, const int16_t* boxes)
{
    if (!f) return;
    FrameOp o{ Op::Shape }; o.f = f; o.a = nboxes; o.boxes = boxes; Marshal(o);
}

void* amberwin_frame_bits(amberwin_frame* f, int* stride_bytes)
{
    if (!f) return nullptr;
    *stride_bytes = f->stride;
    return f->bits;
}

void amberwin_frame_present(amberwin_frame* f, int x, int y, int w, int h)
{
    if (!f || !f->view) return;
    if (w < 0 || h < 0)
        InvalidateRect(f->view, nullptr, FALSE);
    else
    {
        RECT r{ x, y, x + w, y + h };
        InvalidateRect(f->view, &r, FALSE);
    }
}

void amberwin_frame_set_title(amberwin_frame* f, const char* utf8)
{
    if (!f) return;
    FrameOp o{ Op::Title }; o.f = f; o.text = utf8; Marshal(o);
}

void amberwin_frame_set_icon(amberwin_frame* f, int w, int h, const uint32_t* argb)
{
    if (!f) return;
    FrameOp o{ Op::Icon }; o.f = f; o.w = w; o.h = h; o.argb = argb; Marshal(o);
}

void amberwin_frame_set_hints(amberwin_frame* f, int min_w, int min_h, int max_w, int max_h, int resizable)
{
    if (!f) return;
    FrameOp o{ Op::Hints }; o.f = f; o.a = min_w; o.b = min_h; o.c = max_w; o.d = max_h; o.x = resizable;
    Marshal(o);
}

void amberwin_frame_set_transient(amberwin_frame* f, amberwin_frame* owner, int modal)
{
    if (!f) return;
    FrameOp o{ Op::Transient }; o.f = f; o.other = owner; o.a = modal; Marshal(o);
}

void amberwin_frame_set_state(amberwin_frame* f, int minimized, int maximized, int fullscreen, int urgent)
{
    if (!f) return;
    FrameOp o{ Op::State }; o.f = f; o.a = minimized; o.b = maximized; o.c = fullscreen; o.d = urgent;
    Marshal(o);
}

void amberwin_frame_activate(amberwin_frame* f)
{
    if (!f) return;
    FrameOp o{ Op::Activate }; o.f = f; Marshal(o);
}

void amberwin_frame_set_xid(amberwin_frame* f, uint32_t xid)
{
    if (!f) return;
    FrameOp o{ Op::SetXid }; o.f = f; o.xid = xid; Marshal(o);
}

// ---- events and channels ------------------------------------------------------------
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

// The monitor topology, in X screen coordinates. A monitor to the left of
// the primary one has a negative Windows x and a non-negative one here,
// because the X screen's origin is the top-left of the virtual desktop.
// Physical size comes from each monitor's own DPI, which is how a client
// on a 150 % monitor and one on a 100 % monitor learn they differ.
int amberwin_monitors(amberwin_monitor* out, int cap)
{
    struct Collect
    {
        amberwin_monitor* out;
        int cap;
        int n;
    } c{ out, cap, 0 };

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        Collect& c = *reinterpret_cast<Collect*>(lp);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(mon, &mi))
            return TRUE;
        const int index = c.n++;
        if (!c.out || index >= c.cap)
            return TRUE;
        amberwin_monitor& m = c.out[index];
        m = {};
        m.x = mi.rcMonitor.left - g.cfg.desktop_x;
        m.y = mi.rcMonitor.top - g.cfg.desktop_y;
        m.w = mi.rcMonitor.right - mi.rcMonitor.left;
        m.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        UINT dx = 96, dy = 96;
        if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0)
            dx = dy = 96;
        m.dpi = static_cast<int32_t>(dx);
        // 25.4 mm to the inch; a client that divides pixels by millimetres
        // gets the monitor's real scale back
        m.mm_w = static_cast<int32_t>(m.w * 254 / (dx * 10));
        m.mm_h = static_cast<int32_t>(m.h * 254 / ((dy ? dy : dx) * 10));
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, m.name,
                            static_cast<int>(sizeof m.name), nullptr, nullptr);
        m.name[sizeof m.name - 1] = '\0';
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));

    if (c.n == 0)
    {
        // no monitor at all should be impossible; report the screen so the
        // X side never has to handle a display with no outputs
        if (out && cap > 0)
        {
            out[0] = {};
            out[0].w = g.cfg.width;
            out[0].h = g.cfg.height;
            out[0].dpi = 96;
            out[0].mm_w = g.cfg.width * 254 / 960;
            out[0].mm_h = g.cfg.height * 254 / 960;
            out[0].primary = 1;
            strcpy_s(out[0].name, "AmberX");
        }
        return 1;
    }
    return c.n;
}

// ---- clipboard ----------------------------------------------------------------
// The host never touches the Windows clipboard: at low integrity it could
// not read it if it wanted to, and it should not want to. It is a relay
// between the X side and AmberSSH, which owns the policy and the clipboard
// itself. Text is held here only between arriving and being pulled.
uint32_t amberwin_clipboard_pull(char* buf, uint32_t cap)
{
    std::lock_guard<std::mutex> lk(g.qmu);
    if (g.clipPending.empty() || g.clipPending.size() > cap)
    {
        g.clipPending.clear();      // consumed either way: never a stale paste
        return 0;
    }
    const uint32_t n = static_cast<uint32_t>(g.clipPending.size());
    memcpy(buf, g.clipPending.data(), n);
    buf[n] = '\0';
    g.clipPending.clear();
    return n;
}

void amberwin_clipboard_push(const char* utf8, uint32_t len)
{
    if (!utf8 || len == 0 || len > kMaxPayload)
        return;
    SendFrame(MsgType::ClipboardText, kControlChannel,
              std::vector<uint8_t>(utf8, utf8 + len));
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

// ---- libXfont2's POSIX leftovers -----------------------------------------------
// libXfont2 calls strcasecmp by its POSIX name. Inside X translation units
// os.h maps it to xstrcasecmp; this unit has no os.h, so it carries the
// plain name.
int strcasecmp(const char* a, const char* b)
{
    return _stricmp(a, b);
}

} // extern "C"
