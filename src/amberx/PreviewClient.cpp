// PreviewClient.cpp — the X clients inside --preview-amberx.
//
// Everything here is the X11 wire protocol, little-endian, written out by
// hand so the gate depends on no X library. It drives a running AmberXHost
// through control channels exactly as forwarded clients would, and checks
// the Phase 2, 3 and 4 gates: setup with the cookie, drawing and reading
// pixels back, input in both directions, rootless frames with the right
// title, owner, close and focus behaviour, the identity strip outside the
// X pixels, and a second concurrent client whose bytes arrive one at a
// time.
#include "PreviewClient.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "control/Protocol.h"

namespace amber::amberx
{

namespace
{

using Line = std::function<void(const char*, bool, const std::string&)>;

struct XClient
{
    AmberXController& c;
    Line line;
    uint32_t ch;                // the control channel this client lives on
    bool fragmented = false;    // deliver every request one byte per frame
    std::vector<uint8_t> inbuf;
    std::string hostErrors;
    uint32_t seq = 0;           // of the last request sent
    std::vector<std::vector<uint8_t>> events;   // 32-byte events set aside
    std::vector<std::vector<uint8_t>>* others = nullptr;   // frames for other channels
    uint32_t root = 0, ridBase = 0, rootW = 0, rootH = 0, rootDepth = 0;

    XClient(AmberXController& ctl, Line l, uint32_t channel) : c(ctl), line(std::move(l)), ch(channel) {}

    static uint32_t u16(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }
    static uint32_t u32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); }
    static void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); }
    static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); }
    static void pad4(std::vector<uint8_t>& v) { while (v.size() % 4) v.push_back(0); }

    // Raw bytes to the channel: in frame-sized pieces, or one byte per frame.
    bool sendRaw(const std::vector<uint8_t>& bytes)
    {
        if (fragmented)
        {
            for (uint8_t b : bytes)
                if (!c.SendData(ch, &b, 1))
                    return false;
            return true;
        }
        size_t off = 0;
        while (off < bytes.size())
        {
            const size_t n = std::min(bytes.size() - off, static_cast<size_t>(kMaxPayload));
            if (!c.SendData(ch, bytes.data() + off, n))
                return false;
            off += n;
        }
        return true;
    }

    // A BIG-REQUESTS request: length field 0, then the 32-bit length in
    // 4-byte units. The client must have enabled the extension first.
    bool sendBig(std::vector<uint8_t> req)
    {
        req[2] = 0;
        req[3] = 0;
        const uint32_t units = static_cast<uint32_t>((req.size() + 4) / 4);
        std::vector<uint8_t> ext(4);
        for (int i = 0; i < 4; ++i)
            ext[i] = static_cast<uint8_t>((units >> (8 * i)) & 0xff);
        req.insert(req.begin() + 4, ext.begin(), ext.end());
        ++seq;
        return sendRaw(req);
    }

    // The X error code for request `s`, or 0 if it produced none: a probe
    // with a reply is sent afterwards, and whichever of "error for s" and
    // "the probe's reply or error" arrives first decides. The probe is a
    // GetGeometry of the root — readable by every client, trusted or not —
    // so the probe itself can never be what is refused. -1 when nothing
    // arrives.
    int errorFor(uint32_t s)
    {
        std::vector<uint8_t> probe = { 14, 0, 0, 0 };
        put32(probe, root);
        send(probe);
        const uint32_t probeSeq = seq;
        const ULONGLONG until = GetTickCount64() + 5000;
        while (GetTickCount64() < until)
        {
            std::vector<uint8_t> m;
            const int k = next(m, 1000);
            if (k < 0)
                continue;
            if (k == 0 && (u16(&m[2]) & 0xffff) == (s & 0xffff))
                return m[1];
            if ((k == 1 || k == 0) && (u16(&m[2]) & 0xffff) == (probeSeq & 0xffff))
                return 0;
            if (k == 2)
                events.push_back(m);
        }
        return -1;
    }

    // Reads channel bytes until `want` are buffered, setting aside the
    // host's own status frames and remembering any HostError. Frames for
    // another client's channel are handed to that client through `others`.
    bool recv(size_t want, DWORD ms)
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
            if (r.type == MsgType::ChannelData && r.channel == ch)
                inbuf.insert(inbuf.end(), r.payload.begin(), r.payload.end());
            else if (r.type == MsgType::ChannelData && others)
                others->push_back(r.payload);
            else if (r.type == MsgType::HostError)
            {
                std::string t;
                ParseHostError(r.payload, t);
                hostErrors += t + "; ";
            }
            else if (r.type == MsgType::ChannelClose && r.channel == ch)
                return false;
        }
        return true;
    }
    void take(size_t n) { inbuf.erase(inbuf.begin(), inbuf.begin() + static_cast<std::ptrdiff_t>(n)); }

    bool send(std::vector<uint8_t> req)
    {
        // the length field is in 4-byte units and lives at bytes 2-3
        req[2] = static_cast<uint8_t>((req.size() / 4) & 0xff);
        req[3] = static_cast<uint8_t>(((req.size() / 4) >> 8) & 0xff);
        ++seq;
        return sendRaw(req);
    }

    // Pulls one message off the stream. Returns 1 reply, 2 event, 0 error, -1 nothing.
    int next(std::vector<uint8_t>& msg, DWORD ms)
    {
        if (!recv(32, ms))
            return -1;
        const uint8_t type = inbuf[0] & 0x7f;
        if (type == 1)
        {
            const uint32_t extra = u32(&inbuf[4]) * 4;
            if (!recv(32 + extra, 3000))
                return -1;
            msg.assign(inbuf.begin(), inbuf.begin() + 32 + extra);
            take(32 + extra);
            return 1;
        }
        msg.assign(inbuf.begin(), inbuf.begin() + 32);
        take(32);
        return type == 0 ? 0 : 2;
    }

    // Waits for the reply to request `s`, queueing events seen on the way.
    bool reply(uint32_t s, std::vector<uint8_t>& out, DWORD ms = 5000)
    {
        const ULONGLONG until = GetTickCount64() + ms;
        while (GetTickCount64() < until)
        {
            std::vector<uint8_t> m;
            const int k = next(m, 1000);
            if (k < 0)
                continue;
            if (k == 1 && (u16(&m[2]) & 0xffff) == (s & 0xffff))
            {
                out = m;
                return true;
            }
            if (k == 2)
                events.push_back(m);
            if (k == 0)
                line("X error", false, "code=" + std::to_string(m[1]) + " seq=" + std::to_string(u16(&m[2])) +
                                       " major=" + std::to_string(m[10]));
        }
        return false;
    }

    // The window an event names: device events (Key/Button/Motion/Enter/
    // Leave, types 2-8) carry time at byte 4 and the event window at 12;
    // the others carry their window at 4.
    static uint32_t eventWindow(const std::vector<uint8_t>& e)
    {
        const uint8_t t = e[0] & 0x7f;
        return (t >= 2 && t <= 8) ? u32(&e[12]) : u32(&e[4]);
    }

    // Waits for an event of `type` on `window` (0 = any), queueing others.
    bool event(uint8_t type, uint32_t window, std::vector<uint8_t>& out, DWORD ms = 5000)
    {
        for (size_t i = 0; i < events.size(); ++i)
        {
            const std::vector<uint8_t>& e = events[i];
            if ((e[0] & 0x7f) == type && (window == 0 || eventWindow(e) == window))
            {
                out = e;
                events.erase(events.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        const ULONGLONG until = GetTickCount64() + ms;
        while (GetTickCount64() < until)
        {
            std::vector<uint8_t> m;
            const int k = next(m, 1000);
            if (k == 2)
            {
                if ((m[0] & 0x7f) == type && (window == 0 || eventWindow(m) == window))
                {
                    out = m;
                    return true;
                }
                events.push_back(m);
            }
        }
        return false;
    }

    // The connection setup: 'l', pad, 11.0, the auth name and the cookie.
    bool setup(const char* step)
    {
        std::vector<uint8_t> s;
        s.push_back('l'); s.push_back(0);
        put16(s, 11); put16(s, 0);
        put16(s, 18); put16(s, 16); put16(s, 0);
        const char* name = "MIT-MAGIC-COOKIE-1";
        s.insert(s.end(), name, name + 18); s.push_back(0); s.push_back(0);
        s.insert(s.end(), 16, 0x42);
        if (!sendRaw(s))
        {
            line(step, false, "send failed");
            return false;
        }
        bool ok = recv(8, 5000);
        if (!ok)
        {
            line(step, false, "no reply  " + hostErrors);
            return false;
        }
        const uint8_t status = inbuf[0];
        const uint32_t extra = u16(&inbuf[6]) * 4;
        ok = recv(8 + extra, 5000);
        if (ok && status == 1)
        {
            const uint8_t* p = inbuf.data() + 8;
            ridBase = u32(p + 4);
            const uint32_t nVendor = u16(p + 16);
            const uint8_t numFormats = p[21];
            const uint8_t* q = p + 32 + ((nVendor + 3) & ~3u) + numFormats * 8;
            root = u32(q);
            rootW = u16(q + 20);
            rootH = u16(q + 22);
            rootDepth = q[38];
            line(step, true, "root=0x" + std::to_string(root) + " depth=" + std::to_string(rootDepth) +
                             " " + std::to_string(rootW) + "x" + std::to_string(rootH));
        }
        else if (ok)
        {
            std::string reason(reinterpret_cast<const char*>(inbuf.data()) + 8, inbuf[1]);
            line(step, false, "status=" + std::to_string(status) + " " + reason);
        }
        else
            line(step, false, "short reply");
        take(8 + extra);
        return ok && status == 1 && root != 0;
    }

    uint32_t internAtom(const char* name)
    {
        std::vector<uint8_t> r = { 16, 0, 0, 0 };
        put16(r, static_cast<uint32_t>(strlen(name)));
        put16(r, 0);
        r.insert(r.end(), name, name + strlen(name));
        pad4(r);
        send(r);
        std::vector<uint8_t> rep;
        if (!reply(seq, rep))
            return 0;
        return u32(&rep[8]);
    }

    void changeProperty(uint32_t win, uint32_t prop, uint32_t type, int format, const void* data, size_t nItems)
    {
        std::vector<uint8_t> r = { 18, 0, 0, 0 };
        put32(r, win); put32(r, prop); put32(r, type);
        r.push_back(static_cast<uint8_t>(format)); r.push_back(0); r.push_back(0); r.push_back(0);
        put32(r, static_cast<uint32_t>(nItems));
        const uint8_t* p = static_cast<const uint8_t*>(data);
        r.insert(r.end(), p, p + nItems * (format / 8));
        pad4(r);
        send(r);
    }

    // GetProperty: returns the 32-bit items of a format-32 property
    std::vector<uint32_t> getProperty32(uint32_t win, uint32_t prop)
    {
        std::vector<uint8_t> r = { 20, 0, 0, 0 };
        put32(r, win); put32(r, prop); put32(r, 0); put32(r, 0); put32(r, 64);
        send(r);
        std::vector<uint8_t> rep;
        std::vector<uint32_t> out;
        if (!reply(seq, rep) || rep[1] != 32)
            return out;
        const uint32_t n = u32(&rep[16]);
        for (uint32_t i = 0; i < n && 32 + i * 4 + 4 <= rep.size(); ++i)
            out.push_back(u32(&rep[32 + i * 4]));
        return out;
    }

    // CreateWindow + _NET_WM_NAME + MapWindow, the common shape of a toplevel
    void createToplevel(uint32_t wid, int x, int y, int w, int h, uint32_t bg, uint32_t eventMask,
                        uint32_t aNetWmName, uint32_t aUtf8, const char* title)
    {
        std::vector<uint8_t> r = { 1, 0, 0, 0 }; put32(r, wid); put32(r, root);
        put16(r, x); put16(r, y); put16(r, w); put16(r, h); put16(r, 0);
        put16(r, 1); put32(r, 0); put32(r, 0x2 | 0x800);
        put32(r, bg); put32(r, eventMask);
        send(r);
        changeProperty(wid, aNetWmName, aUtf8, 8, title, strlen(title));
    }

    void mapWindow(uint32_t wid)
    {
        std::vector<uint8_t> r = { 8, 0, 0, 0 }; put32(r, wid);
        send(r);
    }
};

HWND FindFrameByTitle(const wchar_t* title)
{
    return FindWindowW(L"AmberXFrame", title);
}

HWND WaitFrame(const wchar_t* title, int tries = 30)
{
    for (int i = 0; i < tries; ++i)
    {
        HWND h = FindFrameByTitle(title);
        if (h)
            return h;
        Sleep(100);
    }
    return nullptr;
}

HWND ChildOfClass(HWND parent, const wchar_t* cls)
{
    return FindWindowExW(parent, nullptr, cls, nullptr);
}

} // namespace

int RunPreviewClient(AmberXController& c, const Line& line)
{
    int failures = 0;
    auto check = [&](const char* step, bool ok, const std::string& detail = {}) {
        line(step, ok, detail);
        if (!ok)
            ++failures;
    };
    XClient x(c, line, 1);
    std::vector<std::vector<uint8_t>> spill;   // frames for the second client seen by the first
    x.others = &spill;

    if (!x.setup("setup accepted") || x.rootDepth != 24)
        return failures + 1;

    // ---- atoms ----------------------------------------------------------------
    const uint32_t aNetWmName = x.internAtom("_NET_WM_NAME");
    const uint32_t aUtf8 = x.internAtom("UTF8_STRING");
    const uint32_t aWmProtocols = x.internAtom("WM_PROTOCOLS");
    const uint32_t aWmDelete = x.internAtom("WM_DELETE_WINDOW");
    const uint32_t aWmTransientFor = x.internAtom("WM_TRANSIENT_FOR");
    const uint32_t aNetClientList = x.internAtom("_NET_CLIENT_LIST");
    const uint32_t aNetWmCheck = x.internAtom("_NET_SUPPORTING_WM_CHECK");
    check("InternAtom", aNetWmName && aUtf8 && aWmProtocols && aWmDelete && aWmTransientFor && aNetClientList && aNetWmCheck);

    // ---- window A: amber, 200x120 at (10,10), titled, closable -------------
    const uint32_t wid = x.ridBase + 1, gc = x.ridBase + 2, dlg = x.ridBase + 3;
    const uint32_t eventMask = 0x8000 /*Exposure*/ | 0x4 /*ButtonPress*/ | 0x1 /*KeyPress*/ |
                               0x200000 /*FocusChange*/ | 0x20000 /*StructureNotify*/;
    x.createToplevel(wid, 10, 10, 200, 120, 0x00ff8800, eventMask, aNetWmName, aUtf8, "AmberX preview");
    const uint32_t protocols[1] = { aWmDelete };
    x.changeProperty(wid, aWmProtocols, 4 /*ATOM*/, 32, protocols, 1);
    x.mapWindow(wid);
    check("CreateWindow + MapWindow", true);
    std::vector<uint8_t> r;
    r = { 55, 0, 0, 0 }; XClient::put32(r, gc); XClient::put32(r, wid); XClient::put32(r, 0x4); XClient::put32(r, 0x00203040);
    check("CreateGC", x.send(r));
    r = { 70, 0, 0, 0 }; XClient::put32(r, wid); XClient::put32(r, gc);
    XClient::put16(r, 20); XClient::put16(r, 20); XClient::put16(r, 80); XClient::put16(r, 50);
    check("PolyFillRectangle", x.send(r));

    std::vector<uint8_t> rep, ev;
    r = { 14, 0, 0, 0 }; XClient::put32(r, wid);
    x.send(r);
    bool ok = x.reply(x.seq, rep);
    // the size is the client's; the position is the manager's — a frame
    // asked to sit at (10,10) is placed inside the work area, so it may move
    check("GetGeometry reply", ok && XClient::u16(&rep[16]) == 200 && XClient::u16(&rep[18]) == 120,
          ok ? std::to_string(XClient::u16(&rep[16])) + "x" + std::to_string(XClient::u16(&rep[18])) +
               " at " + std::to_string(XClient::u16(&rep[12])) + "," + std::to_string(XClient::u16(&rep[14]))
             : "none");
    r = { 73, 2, 0, 0 }; XClient::put32(r, wid); XClient::put16(r, 25); XClient::put16(r, 25); XClient::put16(r, 1); XClient::put16(r, 1); XClient::put32(r, 0xffffffff);
    x.send(r);
    ok = x.reply(x.seq, rep);
    {
        const uint32_t px = (ok && rep.size() >= 36) ? (XClient::u32(&rep[32]) & 0xffffff) : 0xdeadbeef;
        char hex[16];
        snprintf(hex, sizeof hex, "0x%06x", px);
        check("GetImage pixel == foreground", px == 0x203040, hex);
    }
    check("Expose event", x.event(12, wid, ev));

    // ---- the native frame -------------------------------------------------------
    HWND frame = WaitFrame(L"AmberX preview");
    check("native frame titled from _NET_WM_NAME", frame != nullptr);
    HWND view = frame ? ChildOfClass(frame, L"AmberXView") : nullptr;
    HWND strip = frame ? ChildOfClass(frame, L"AmberXStrip") : nullptr;
    check("frame has a view child", view != nullptr);
    check("frame has the identity strip", strip != nullptr);
    if (view && strip)
    {
        RECT vr, sr;
        GetWindowRect(view, &vr);
        GetWindowRect(strip, &sr);
        check("strip sits above the X pixels, outside them",
              sr.bottom <= vr.top && (sr.bottom - sr.top) >= 16 && (vr.right - vr.left) == 200 && (vr.bottom - vr.top) == 120,
              "strip " + std::to_string(sr.bottom - sr.top) + "px, view " + std::to_string(vr.right - vr.left) + "x" + std::to_string(vr.bottom - vr.top));
    }

    // ---- input in through the view --------------------------------------------
    if (view)
    {
        PostMessageW(view, WM_MOUSEMOVE, 0, MAKELPARAM(40, 40));
        PostMessageW(view, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
        PostMessageW(view, WM_LBUTTONUP, 0, MAKELPARAM(40, 40));
        PostMessageW(view, WM_KEYDOWN, 'A', static_cast<LPARAM>((0x1E << 16) | 1));
        PostMessageW(view, WM_KEYUP, 'A', static_cast<LPARAM>((0x1E << 16) | 1 | (1u << 30) | (1u << 31)));
        ok = x.event(4, wid, ev);
        // window-relative must be the posted point; root = window position + it
        check("ButtonPress at the posted point", ok && ev[1] == 1 && XClient::u16(&ev[24]) == 40 && XClient::u16(&ev[26]) == 40,
              ok ? "window " + std::to_string(XClient::u16(&ev[24])) + "," + std::to_string(XClient::u16(&ev[26])) +
                   " root " + std::to_string(XClient::u16(&ev[20])) + "," + std::to_string(XClient::u16(&ev[22]))
                 : "none");
        ok = x.event(2, wid, ev);
        check("KeyPress keycode 38", ok && ev[1] == 38);
    }

    // ---- window B: a transient dialog owned by A ----------------------------------
    x.createToplevel(dlg, 300, 300, 120, 80, 0x00e0e0e0, eventMask, aNetWmName, aUtf8, "AmberX dialog");
    x.changeProperty(dlg, aWmTransientFor, 33 /*WINDOW*/, 32, &wid, 1);
    x.mapWindow(dlg);
    HWND dframe = WaitFrame(L"AmberX dialog");
    check("transient dialog has its own frame", dframe != nullptr);
    check("dialog frame is owned by A's frame", dframe && frame && GetWindow(dframe, GW_OWNER) == frame);

    // ---- the WM's root properties -------------------------------------------------
    {
        std::vector<uint32_t> list = x.getProperty32(x.root, aNetClientList);
        const bool hasA = std::find(list.begin(), list.end(), wid) != list.end();
        const bool hasB = std::find(list.begin(), list.end(), dlg) != list.end();
        check("_NET_CLIENT_LIST lists both windows", hasA && hasB, std::to_string(list.size()) + " entries");
        std::vector<uint32_t> chk = x.getProperty32(x.root, aNetWmCheck);
        check("_NET_SUPPORTING_WM_CHECK set", !chk.empty() && chk[0] != 0);
    }

    // ---- focus: activating the native frame focuses the X window -------------------
    if (frame)
    {
        PostMessageW(frame, WM_ACTIVATE, WA_ACTIVE, 0);
        ok = x.event(9, wid, ev);
        check("FocusIn after native activation", ok);
    }

    // ---- native move: ConfigureNotify with the new position ---------------------------
    if (frame && view)
    {
        // the settled position first (placement may have moved the window)
        r = { 14, 0, 0, 0 }; XClient::put32(r, wid);
        x.send(r);
        int bx = 10, by = 10;
        if (x.reply(x.seq, rep))
        {
            bx = static_cast<int16_t>(XClient::u16(&rep[12]));
            by = static_cast<int16_t>(XClient::u16(&rep[14]));
        }
        // placement's own ConfigureNotify may still be queued; only the
        // one caused by the move below counts
        while (x.event(22, wid, ev, 300)) {}
        RECT fr;
        GetWindowRect(frame, &fr);
        SetWindowPos(frame, nullptr, fr.left + 60, fr.top + 30, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ok = x.event(22, wid, ev, 5000);
        const int nx = ok ? static_cast<int16_t>(XClient::u16(&ev[16])) : -1;
        const int ny = ok ? static_cast<int16_t>(XClient::u16(&ev[18])) : -1;
        check("ConfigureNotify after native move", ok && nx == bx + 60 && ny == by + 30,
              ok ? std::to_string(nx) + "," + std::to_string(ny) + " (expected " + std::to_string(bx + 60) + "," +
                   std::to_string(by + 30) + ")" : "none");
    }

    // ---- a second client on its own channel, one byte per frame ----------------------
    // Phase 4: multiple simultaneous X11 channels, arbitrary fragmentation.
    // Everything this client sends — the setup packet included — arrives at
    // the host as one-byte frames; the server's request framer must
    // reassemble it exactly.
    check("open channel 2", c.OpenChannel(2));
    XClient y(c, line, 2);
    y.fragmented = true;
    y.others = &spill;
    if (y.setup("second client setup, fragmented") && y.rootDepth == 24)
    {
        const uint32_t wid2 = y.ridBase + 1;
        y.createToplevel(wid2, 500, 200, 160, 100, 0x0040c0ff, eventMask, aNetWmName, aUtf8, "AmberX second client");
        y.mapWindow(wid2);
        HWND f2 = WaitFrame(L"AmberX second client");
        check("second client's window has its own frame", f2 != nullptr);
        r = { 14, 0, 0, 0 }; XClient::put32(r, wid2);
        y.send(r);
        ok = y.reply(y.seq, rep);
        check("second client GetGeometry over fragmented frames", ok && XClient::u16(&rep[16]) == 160 && XClient::u16(&rep[18]) == 100);
        // the first client is unaffected and sees three managed windows
        std::vector<uint32_t> list = x.getProperty32(x.root, aNetClientList);
        check("_NET_CLIENT_LIST now lists three windows", list.size() == 3, std::to_string(list.size()) + " entries");
        check("first client still answers", x.internAtom("WM_CLASS") != 0);
        // closing the second channel takes its window with it
        c.CloseChannel(2);
        bool gone = false;
        for (int i = 0; i < 30 && !gone; ++i)
        {
            gone = FindFrameByTitle(L"AmberX second client") == nullptr;
            if (!gone)
                Sleep(100);
        }
        check("closing channel 2 removes its frame", gone);
    }
    else
        failures += 4;

    // ---- close: the native close button becomes WM_DELETE_WINDOW --------------------
    if (frame)
    {
        PostMessageW(frame, WM_CLOSE, 0, 0);
        ok = x.event(33, wid, ev);
        check("WM_DELETE_WINDOW on native close",
              ok && XClient::u32(&ev[8]) == aWmProtocols && XClient::u32(&ev[12]) == aWmDelete);
        check("frame still open until the client acts", IsWindow(frame) != 0);
    }

    check("no X errors and no host errors", x.hostErrors.empty() && y.hostErrors.empty(), x.hostErrors + y.hostErrors);
    if (const char* hold = getenv("AMBERX_PREVIEW_HOLD_MS"))
        Sleep(static_cast<DWORD>(atoi(hold)));
    return failures;
}

// ---- Phase 5: what restricted and trusted enforce, and what limits refuse ----
int RunPreviewTrustChecks(AmberXController& c, bool trusted, const Line& line)
{
    int failures = 0;
    const std::string tag = trusted ? " [trusted]" : " [restricted]";
    auto check = [&](const std::string& step, bool ok, const std::string& detail = {}) {
        line((step + tag).c_str(), ok, detail);
        if (!ok)
            ++failures;
    };
    if (!c.OpenChannel(1))
    {
        check("open channel", false);
        return failures;
    }
    XClient x(c, line, 1);
    if (!x.setup((std::string("setup") + tag).c_str()))
        return failures + 1;

    const uint32_t eventMask = 0x8000 | 0x20000;
    const uint32_t wid = x.ridBase + 1;
    const uint32_t aNetWmName = x.internAtom("_NET_WM_NAME");
    const uint32_t aUtf8 = x.internAtom("UTF8_STRING");
    x.createToplevel(wid, 40, 40, 120, 80, 0x00888888, eventMask, aNetWmName, aUtf8,
                     trusted ? "AmberX trusted probe" : "AmberX restricted probe");
    x.mapWindow(wid);

    // 1. Writing a property on the ROOT window — a trusted client's window.
    //    Untrusted clients may read it, never write it: BadAccess.
    std::vector<uint8_t> r;
    {
        const char v = 'x';
        x.changeProperty(x.root, 39 /*WM_NAME*/, 31 /*STRING*/, 8, &v, 1);
        const int e = x.errorFor(x.seq);
        check("ChangeProperty on the root window", trusted ? e == 0 : e == 10 /*BadAccess*/,
              "error " + std::to_string(e) + (trusted ? " (expected none)" : " (expected 10 BadAccess)"));
    }
    // 2. A keyboard grab: the SECURITY extension's device policy.
    {
        r = { 31, 0, 0, 0 }; XClient::put32(r, wid); XClient::put32(r, 0); r.push_back(1); r.push_back(1); r.push_back(0); r.push_back(0);
        x.send(r);
        std::vector<uint8_t> rep;
        const uint32_t s = x.seq;
        std::vector<uint8_t> m;
        int status = -1;
        const ULONGLONG until = GetTickCount64() + 5000;
        while (status < 0 && GetTickCount64() < until)
        {
            const int k = x.next(m, 1000);
            if (k == 1 && (XClient::u16(&m[2]) & 0xffff) == (s & 0xffff)) status = 100 + m[1];   // reply: 100 + grab status
            else if (k == 0 && (XClient::u16(&m[2]) & 0xffff) == (s & 0xffff)) status = m[1];    // error code
            else if (k == 2) x.events.push_back(m);
        }
        line((std::string("GrabKeyboard outcome") + tag).c_str(), true,
             status >= 100 ? "reply, status " + std::to_string(status - 100) : "error " + std::to_string(status));
    }
    // 3. Exhaustion: a pixmap beyond the limit is refused with BadAlloc,
    //    and the server keeps answering.
    {
        r = { 53, 24, 0, 0 }; XClient::put32(r, x.ridBase + 2); XClient::put32(r, wid); XClient::put16(r, 20000); XClient::put16(r, 20000);
        x.send(r);
        const int e = x.errorFor(x.seq);
        check("20000x20000 pixmap refused", e == 11 /*BadAlloc*/, "error " + std::to_string(e));
    }
    // 4. Exhaustion: a 2 MiB property (over the 1 MiB limit) through
    //    BIG-REQUESTS is refused with BadAlloc before it is stored.
    {
        std::vector<uint8_t> q = { 98, 0, 0, 0 }; XClient::put16(q, 12); XClient::put16(q, 0);
        const char* ext = "BIG-REQUESTS"; q.insert(q.end(), ext, ext + 12);
        x.send(q);
        std::vector<uint8_t> rep;
        uint8_t major = 0;
        if (x.reply(x.seq, rep) && rep[8])
            major = rep[9];
        bool enabled = false;
        if (major)
        {
            q = { major, 0, 0, 0 };
            x.send(q);
            enabled = x.reply(x.seq, rep);
        }
        check("BIG-REQUESTS enabled", enabled);
        if (enabled)
        {
            std::vector<uint8_t> big = { 18, 0, 0, 0 };
            XClient::put32(big, wid); XClient::put32(big, aNetWmName); XClient::put32(big, 31);
            big.push_back(8); big.push_back(0); big.push_back(0); big.push_back(0);
            const uint32_t n = 2u * 1024 * 1024;
            XClient::put32(big, n);
            big.resize(big.size() + n, 'A');
            x.sendBig(big);
            const int e = x.errorFor(x.seq);
            check("2 MiB property refused", e == 11, "error " + std::to_string(e));
        }
    }
    // 5. Exhaustion: an atom flood trips the rate limit, and the server is
    //    still there afterwards.
    {
        int refused = 0;
        // Only messages for the flood's own sequence range count: an earlier
        // errorFor probe can leave its reply queued, and it must not be
        // mistaken for one of the flood's answers.
        const uint32_t firstSeq = (x.seq + 1) & 0xffff;
        auto inFlood = [&](const std::vector<uint8_t>& m) {
            const uint32_t d = (XClient::u16(&m[2]) - firstSeq) & 0xffff;
            return d < 2100;
        };
        for (int i = 0; i < 2100; ++i)
        {
            char name[48];
            snprintf(name, sizeof name, "amberx-flood-%d-%u", i, static_cast<unsigned>(GetTickCount64() & 0xffff));
            std::vector<uint8_t> q = { 16, 0, 0, 0 };
            XClient::put16(q, static_cast<uint32_t>(strlen(name))); XClient::put16(q, 0);
            q.insert(q.end(), name, name + strlen(name));
            XClient::pad4(q);
            x.send(q);
        }
        // drain: count errors among the replies
        const ULONGLONG until = GetTickCount64() + 15000;
        int seen = 0;
        while (seen < 2100 && GetTickCount64() < until)
        {
            std::vector<uint8_t> m;
            const int k = x.next(m, 1000);
            if (k == 1) { if (inFlood(m)) ++seen; }
            else if (k == 0) { if (inFlood(m)) { ++seen; ++refused; } }
            else if (k == 2) x.events.push_back(m);
            else break;
        }
        check("atom flood rate-limited", refused > 0 && seen == 2100,
              std::to_string(refused) + " of " + std::to_string(seen) + " refused");
        r = { 14, 0, 0, 0 }; XClient::put32(r, wid);
        x.send(r);
        std::vector<uint8_t> rep;
        check("server still answers after the flood", x.reply(x.seq, rep));
    }
    c.CloseChannel(1);
    return failures;
}

// ---- Phase 5: an untrusted authorization that expires ------------------------
int RunPreviewTimeoutCheck(AmberXController& c, const Line& line)
{
    int failures = 0;
    auto check = [&](const char* step, bool ok, const std::string& detail = {}) {
        line(step, ok, detail);
        if (!ok)
            ++failures;
    };
    c.OpenChannel(1);
    {
        XClient x(c, line, 1);
        check("client before the timeout", x.setup("setup with a 2 s authorization"));
        c.CloseChannel(1);
    }
    Sleep(3500);   // past the 2 s timeout, with no client holding the cookie
    c.OpenChannel(2);
    {
        XClient y(c, line, 2);
        std::vector<uint8_t> s;
        s.push_back('l'); s.push_back(0);
        XClient::put16(s, 11); XClient::put16(s, 0);
        XClient::put16(s, 18); XClient::put16(s, 16); XClient::put16(s, 0);
        const char* name = "MIT-MAGIC-COOKIE-1";
        s.insert(s.end(), name, name + 18); s.push_back(0); s.push_back(0);
        s.insert(s.end(), 16, 0x42);
        y.sendRaw(s);
        bool refused = false;
        std::string reason;
        if (y.recv(8, 5000))
        {
            const uint8_t status = y.inbuf[0];
            const uint32_t extra = XClient::u16(&y.inbuf[6]) * 4;
            if (y.recv(8 + extra, 3000) && status == 0)
            {
                refused = true;
                reason.assign(reinterpret_cast<const char*>(y.inbuf.data()) + 8, y.inbuf[1]);
            }
        }
        check("client after the timeout is refused", refused, reason);
        c.CloseChannel(2);
    }
    return failures;
}

} // namespace amber::amberx
