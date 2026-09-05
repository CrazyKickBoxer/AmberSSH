// app_vnc.cpp — the VNC tab inside App: how it starts, how its worker's
// events reach the tab, how the desktop is drawn, and where input goes.
//
// A VNC tab is a Session carrying a VncTab (sessions/VncTab.h), so every
// tab-lifecycle path in app.cpp applies to it unchanged; what is here is
// only the places where a desktop diverges from a terminal.
#include "app.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "vnc/Keysyms.h"

using amber::Session;
using amber::VncTab;
namespace vnc = amber::vnc;

namespace
{

bool InputAllowed(const VncTab& t)
{
    return t.session && !t.session->ViewOnly() && t.session->State() == vnc::VncState::Connected;
}

// Screen px -> framebuffer px through the placement. Returns whether the
// point is inside the picture; the coordinates are clamped either way, so
// a drag that leaves the picture keeps reporting its edge.
bool ToFramebuffer(const VncTab& t, int px, int py, int& fx, int& fy)
{
    if (t.fbW == 0 || t.fbH == 0 || t.scale <= 0.0f)
        return false;
    const float x = (static_cast<float>(px) - t.dstX) / t.scale;
    const float y = (static_cast<float>(py) - t.dstY) / t.scale;
    const bool inside = x >= 0.0f && y >= 0.0f && x < static_cast<float>(t.fbW) && y < static_cast<float>(t.fbH);
    fx = std::clamp(static_cast<int>(std::floor(x)), 0, static_cast<int>(t.fbW) - 1);
    fy = std::clamp(static_cast<int>(std::floor(y)), 0, static_cast<int>(t.fbH) - 1);
    return inside;
}

// UTF-8 onto the Windows clipboard. The UI thread only; opening the
// clipboard can block on whichever application holds it.
void SetClipboardUtf8(HWND owner, const std::string& utf8)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0 || !OpenClipboard(owner))
        return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(n + 1) * sizeof(wchar_t));
    if (h)
    {
        wchar_t* w = static_cast<wchar_t*>(GlobalLock(h));
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w, n);
        w[n] = 0;
        GlobalUnlock(h);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, h))
            GlobalFree(h);
    }
    CloseClipboard();
}

} // namespace

amber::VncTab* App::VncActive()
{
    if (!HasSession())
        return nullptr;
    return Cur().vnc.get();
}

bool App::StartVncSession(Session& s, amber::ConnectionRequest& req)
{
    auto tab = std::make_unique<VncTab>();
    tab->session = std::make_unique<vnc::VncSession>();
    tab->desk = std::make_unique<DesktopParticles>();
    if (!tab->desk->Init(m_device, m_shaders))
    {
        s.state = amber::SessionState::Error;
        s.status = "VNC: the particle desktop could not initialise";
        return false;
    }

    const amber::ConnectionProfile& p = req.profile;
    vnc::VncConfig cfg;
    cfg.host = p.host;
    cfg.port = p.port > 0 ? p.port : 5900;
    cfg.password = req.password.Reveal();
    cfg.viewOnly = p.vncViewOnly;
    cfg.encodings = p.vncEncodings;
    cfg.wantCursor = p.vncCursorMode == 0;
    cfg.tls = p.vncTls != 0;

    if (!p.vncViaProfileId.empty())
    {
        // The SSH session to tunnel through must already be up: the forward
        // is added to a live worker, and the bound port comes back as its
        // ForwardUp event, relayed in PumpSshEvents.
        Session* via = nullptr;
        for (auto& sp : m_sessions)
            if (sp->profile.id == p.vncViaProfileId && sp->profile.protocol == amber::Protocol::Ssh &&
                sp->ssh.Running())
            {
                via = sp.get();
                break;
            }
        if (!via)
        {
            s.state = amber::SessionState::Error;
            s.status = "VNC: the SSH session to tunnel through is not connected";
            amber::ScrubString(cfg.password);
            return false;
        }
        SshSession* ssh = &via->ssh;
        cfg.addForward = [ssh](const std::string& spec) { ssh->AddForward(spec); };
        tab->viaProfileId = via->profile.id;
    }

    s.state = amber::SessionState::Connecting;
    s.status = "connecting...";
    const bool ok = tab->session->Start(cfg);
    amber::ScrubString(cfg.password);
    tab->tunnelSpec = tab->session->TunnelSpec();
    if (!ok)
    {
        s.state = amber::SessionState::Error;
        s.status = "VNC: the connection could not be started";
    }
    s.vnc = std::move(tab);
    return ok;
}

void App::PumpVncEvents()
{
    for (auto& sp : m_sessions)
    {
        Session& s = *sp;
        if (!s.vnc || !s.vnc->session)
            continue;
        VncTab& t = *s.vnc;
        vnc::VncEvent ev;
        while (t.session->PollEvent(ev))
        {
            switch (ev.type)
            {
            case vnc::VncEvent::Type::Status:
                s.status = ev.text;
                if (t.session->State() == vnc::VncState::Authenticating)
                    s.state = amber::SessionState::Authenticating;
                else if (t.session->State() == vnc::VncState::Reconnecting)
                    s.state = amber::SessionState::Reconnecting;
                break;
            case vnc::VncEvent::Type::Connected:
                s.state = amber::SessionState::Connected;
                s.status.clear();
                s.everConnected = true;
                t.desktopName = ev.text;
                t.heldKeysyms.clear();
                t.heldButtons = 0;
                if (HasSession() && &s == &Cur())
                    SetWindowTextW(m_hwnd, WideFromUtf8(TitleFor(s)).c_str());
                break;
            case vnc::VncEvent::Type::Resized:
                AddNotice(s, 0, "desktop resized to " + ev.text);
                break;
            case vnc::VncEvent::Type::CutText:
                // an echo of what we sent is not news; anything else goes on
                // the Windows clipboard and is remembered so it is not sent back
                if (ev.text == t.lastToServer)
                    break;
                t.lastFromServer = ev.text;
                SetClipboardUtf8(m_hwnd, ev.text);
                break;
            case vnc::VncEvent::Type::Bell:
                RingBell(s, HasSession() && &s == &Cur());
                break;
            case vnc::VncEvent::Type::Closed:
                s.state = amber::SessionState::Disconnected;
                s.status = ev.text;
                t.heldKeysyms.clear();
                t.heldButtons = 0;
                break;
            case vnc::VncEvent::Type::Error:
                s.state = amber::SessionState::Error;
                s.status = "VNC: " + ev.text;
                AddNotice(s, 2, s.status);
                break;
            case vnc::VncEvent::Type::AuthFailed:
                s.state = amber::SessionState::Error;
                s.status = "VNC: " + ev.text;
                AddNotice(s, 2, "VNC authentication failed: " + ev.text);
                break;
            }
        }
        // the approximate input-to-picture figure: the next completed update
        // after an input was sent
        const vnc::VncStats st = t.session->GetStats();
        if (st.updates != t.updatesSeen)
        {
            if (t.lastInputSentAt >= 0.0)
            {
                t.approxInputToPictureMs = static_cast<float>((m_time - t.lastInputSentAt) * 1000.0);
                t.lastInputSentAt = -1.0;
            }
            t.updatesSeen = st.updates;
        }
        if (m_time - t.bytesAt >= 1.0)
        {
            t.bytesPerSec = t.bytesAt > 0.0 ? static_cast<float>((st.bytesIn - t.bytesInPrev) / (m_time - t.bytesAt)) : 0.0f;
            t.bytesInPrev = st.bytesIn;
            t.bytesAt = m_time;
        }
    }
}

void App::RenderVncPasses(ID3D12GraphicsCommandList* cl, FrameContext& frame)
{
    VncTab* tp = VncActive();
    if (!tp || !tp->desk || !tp->session)
        return;
    VncTab& t = *tp;
    const amber::ConnectionProfile& prof = Cur().profile;
    t.desk->Begin(cl);

    vnc::Damage d;
    const bool have = t.session->TakeDamage(d);
    const uint32_t fbW = have ? d.width : t.fbW;
    const uint32_t fbH = have ? d.height : t.fbH;
    if (have && (fbW != t.fbW || fbH != t.fbH || !t.desk->Ready()))
    {
        if (t.desk->Configure(fbW, fbH, static_cast<uint32_t>(prof.vncDensity)))
        {
            t.fbW = fbW;
            t.fbH = fbH;
            if (!d.full)
                t.session->RequestFullUpdate();   // a partial picture on a fresh buffer
        }
    }

    // placement: native when it fits, letterboxed and scaled down when not
    if (t.fbW > 0 && t.fbH > 0)
    {
        const float availW = static_cast<float>(m_device.Width());
        const float availH = static_cast<float>(m_device.Height()) - m_titleBarH;
        float scale = std::min(availW / static_cast<float>(t.fbW), availH / static_cast<float>(t.fbH));
        if (scale >= 1.0f)
            scale = 1.0f;
        t.scale = scale;
        t.dstX = std::floor((availW - static_cast<float>(t.fbW) * scale) * 0.5f);
        t.dstY = std::floor(m_titleBarH + (availH - static_cast<float>(t.fbH) * scale) * 0.5f);
        t.nativeScale = scale == 1.0f;
    }

    if (have && t.desk->Ready())
    {
        t.desk->Upload(cl, frame, d, static_cast<float>(prof.vncDisturbance) / 100.0f);
        t.damageRectsLastFrame = static_cast<uint32_t>(d.rects.size());
        t.damageBytesLastFrame = static_cast<uint64_t>(d.pixels.size()) * 4;
        t.lastDamageAt = m_time;
    }
    vnc::CursorShape cs;
    if (t.session->TakeCursor(cs) && t.desk->Ready())
        t.desk->SetCursor(cl, frame, cs);

    if (!t.desk->Ready())
        return;
    const ParticleTunables& tun = m_particles.tun;
    DesktopParticles::Params p;
    p.time = static_cast<float>(m_time);
    p.dt = static_cast<float>(m_dt * m_timeDial);
    p.solidity = static_cast<float>(prof.vncSolidity) / 100.0f;
    p.particleSize = static_cast<float>(prof.vncParticleSize);
    p.disturbance = static_cast<float>(prof.vncDisturbance) / 100.0f;
    p.animStyle = tun.animStyle;
    p.effectSpeed = tun.effectSpeed;
    p.dragAmt = tun.dragAmt;
    p.reducedMotion = tun.reducedMotion;
    p.lightMode = tun.lightMode > 0.5f;
    p.hdrBoost = m_device.HdrActive() ? 0.5f : 0.0f;
    p.audioWind = tun.audioWind;
    p.brightness = 1.0f;
    p.mouseX = tun.mouseX;
    p.mouseY = tun.mouseY;
    p.mouseRadius = tun.mouseRadius;
    p.mouseForce = tun.mouseForce;
    p.shockX = tun.shockX;
    p.shockY = tun.shockY;
    p.shockTime = tun.shockTime;
    p.dstX = t.dstX;
    p.dstY = t.dstY;
    p.scale = t.scale;
    p.showCursor = prof.vncCursorMode == 0 && t.pointerInside && m_focused;
    p.cursorX = static_cast<float>(m_lastMousePx);
    p.cursorY = static_cast<float>(m_lastMousePy);
    t.desk->Simulate(cl, frame, p, static_cast<float>(m_device.Width()), static_cast<float>(m_device.Height()));
}

void App::DrawVncScene(ID3D12GraphicsCommandList* cl, FrameContext& frame, D3D12_GPU_VIRTUAL_ADDRESS)
{
    if (VncTab* t = VncActive(); t && t->desk && t->desk->Ready())
        t->desk->Draw(cl);
    // The overlays draw against a FrameCB. The glyph field was not simulated
    // this frame, so the address the terminal path would hand over is last
    // frame's ring — or a ring re-created since; a fresh one is uploaded here.
    const D3D12_GPU_VIRTUAL_ADDRESS cb = m_particles.UploadFrameCbOnly(
        frame, m_gm, static_cast<float>(m_device.Width()), static_cast<float>(m_device.Height()),
        static_cast<float>(m_time), static_cast<float>(m_dt), m_device.HdrActive());
    m_prims.RecordOverBlend(cl, frame, cb);
    m_prims.RecordOver(cl, frame, cb);
}

// ---- input --------------------------------------------------------------------
bool App::VncKey(WPARAM vk, bool down)
{
    VncTab* t = VncActive();
    if (!t)
        return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    // the application's own chords stay with the application
    if ((ctrl && shift) || vk == VK_LWIN || vk == VK_RWIN)
        return false;
    if (!InputAllowed(*t))
        return true;   // consumed: a desktop tab never types into a terminal
    bool right = false;
    if (vk == VK_SHIFT)   right = (GetKeyState(VK_RSHIFT) & 0x8000) != 0;
    if (vk == VK_CONTROL) right = (GetKeyState(VK_RCONTROL) & 0x8000) != 0;
    if (vk == VK_MENU)    right = (GetKeyState(VK_RMENU) & 0x8000) != 0;
    const uint32_t ks = vnc::KeysymFromVirtualKey(static_cast<unsigned>(vk), right);
    if (ks == 0)
        return true;   // a text key: WM_CHAR carries it, press and release together
    auto& held = t->heldKeysyms;
    if (down)
    {
        t->session->SendKey(true, ks);
        if (std::find(held.begin(), held.end(), ks) == held.end())
            held.push_back(ks);
    }
    else
    {
        // release the keysym that was pressed, whatever the modifiers are now
        t->session->SendKey(false, ks);
        held.erase(std::remove(held.begin(), held.end(), ks), held.end());
    }
    t->lastInputSentAt = m_time;
    return true;
}

bool App::VncChar(wchar_t wc)
{
    VncTab* t = VncActive();
    if (!t)
        return false;
    if (!InputAllowed(*t))
        return true;
    char32_t cp = wc;
    if (wc >= 0xD800 && wc <= 0xDBFF)
    {
        t->pendingHighSurrogate = wc;
        return true;
    }
    if (wc >= 0xDC00 && wc <= 0xDFFF)
    {
        if (!t->pendingHighSurrogate)
            return true;
        cp = 0x10000 + ((static_cast<char32_t>(t->pendingHighSurrogate) - 0xD800) << 10) + (wc - 0xDC00);
        t->pendingHighSurrogate = 0;
    }
    // these arrive as characters too, but were already sent as keys
    if (cp == '\r' || cp == '\n' || cp == '\t' || cp == '\b' || cp == 0x1B || cp == 0x7F)
        return true;
    // Ctrl+letter arrives as a control character; the far side wants the
    // letter with Control held, which it already is
    if (cp < 0x20 && (GetKeyState(VK_CONTROL) & 0x8000))
        cp = cp + 0x60;
    const uint32_t ks = vnc::KeysymFromCodePoint(cp);
    t->session->SendKey(true, ks);
    t->session->SendKey(false, ks);
    t->lastInputSentAt = m_time;
    return true;
}

bool App::VncMouseButton(bool down, int px, int py, bool right, bool middle)
{
    VncTab* t = VncActive();
    if (!t)
        return false;
    if (py < static_cast<int>(m_titleBarH))
        return false;
    if (!InputAllowed(*t))
        return true;
    int fx, fy;
    const bool inside = ToFramebuffer(*t, px, py, fx, fy);
    const uint8_t bit = right ? vnc::kButtonRight : middle ? vnc::kButtonMiddle : vnc::kButtonLeft;
    if (down && !inside)
        return true;
    if (down)
        t->heldButtons |= bit;
    else
        t->heldButtons &= static_cast<uint8_t>(~bit);
    t->session->SendPointer(t->heldButtons, static_cast<uint16_t>(fx), static_cast<uint16_t>(fy));
    t->lastFbX = fx;
    t->lastFbY = fy;
    t->lastInputSentAt = m_time;
    return true;
}

bool App::VncMouseMove(int px, int py)
{
    VncTab* t = VncActive();
    if (!t)
        return false;
    int fx, fy;
    t->pointerInside = ToFramebuffer(*t, px, py, fx, fy) && py >= static_cast<int>(m_titleBarH);
    if (!InputAllowed(*t))
        return true;
    if (!t->pointerInside && t->heldButtons == 0)
        return true;
    if (fx == t->lastFbX && fy == t->lastFbY)
        return true;
    t->session->SendPointer(t->heldButtons, static_cast<uint16_t>(fx), static_cast<uint16_t>(fy));
    t->lastFbX = fx;
    t->lastFbY = fy;
    if (t->lastInputSentAt < 0.0)
        t->lastInputSentAt = m_time;
    return true;
}

bool App::VncWheel(int delta)
{
    VncTab* t = VncActive();
    if (!t)
        return false;
    if (!InputAllowed(*t) || !t->pointerInside || t->lastFbX < 0)
        return true;
    // one press+release of button 4 (up) or 5 (down) per notch
    const uint8_t bit = delta > 0 ? vnc::kButtonWheelUp : vnc::kButtonWheelDown;
    int notches = std::abs(delta) / WHEEL_DELTA;
    if (notches == 0)
        notches = 1;
    for (int i = 0; i < notches; ++i)
    {
        t->session->SendPointer(static_cast<uint8_t>(t->heldButtons | bit), static_cast<uint16_t>(t->lastFbX),
                                static_cast<uint16_t>(t->lastFbY));
        t->session->SendPointer(t->heldButtons, static_cast<uint16_t>(t->lastFbX), static_cast<uint16_t>(t->lastFbY));
    }
    t->lastInputSentAt = m_time;
    return true;
}

void App::VncReleaseAll()
{
    for (auto& sp : m_sessions)
    {
        if (!sp->vnc || !sp->vnc->session)
            continue;
        VncTab& t = *sp->vnc;
        for (uint32_t ks : t.heldKeysyms)
            t.session->SendKey(false, ks);
        t.heldKeysyms.clear();
        if (t.heldButtons && t.lastFbX >= 0)
            t.session->SendPointer(0, static_cast<uint16_t>(t.lastFbX), static_cast<uint16_t>(t.lastFbY));
        t.heldButtons = 0;
        t.pendingHighSurrogate = 0;
    }
}

// ---- diagnostics ----------------------------------------------------------------
void App::VncStatusLines(const Session& s, float y)
{
    if (!s.vnc || !s.vnc->session)
        return;
    const VncTab& t = *s.vnc;
    const vnc::VncStats st = t.session->GetStats();
    const DesktopParticles::Layout L = t.desk ? t.desk->GetLayout() : DesktopParticles::Layout{};
    char line[512];
    snprintf(line, sizeof line,
             "VNC %s  RFB 3.%d  %ux%u%s  particles %u  density %u%s  solidity %d%%  size %d px  %s  %s",
             vnc::VncStateName(t.session->State()), st.rfbMinor, t.fbW, t.fbH,
             t.nativeScale ? " native" : " scaled",
             L.particles, L.density, L.clamped ? " (clamped)" : "",
             s.profile.vncSolidity, s.profile.vncParticleSize,
             L.sampled ? (std::string("SAMPLED 1/") + std::to_string(L.stride) + " -> " +
                          std::to_string(L.sampledW) + "x" + std::to_string(L.sampledH)).c_str()
                       : "1:1",
             st.continuous ? "continuous" : "requested");
    m_prims.AddText(m_gm.originX, y, line, 0.6f, m_sampler);
    snprintf(line, sizeof line,
             "decode %.1f ms/s  gpu upload %.2f  sim %.2f  draw %.2f  frame %.2f ms  "
             "net %.1f KiB/s  damage %u rects %.0f KiB  pending %u rects  reconnects %d  "
             "input->picture ~%.0f ms (approx: next update after an input)",
             st.decodeMsPerSec, m_device.GpuDesktopUploadMs(), m_device.GpuDesktopSimMs(),
             m_device.GpuDesktopDrawMs(), m_device.GpuFrameMs(), t.bytesPerSec / 1024.0f,
             t.damageRectsLastFrame, static_cast<double>(t.damageBytesLastFrame) / 1024.0,
             st.pendingRects, st.reconnects, t.approxInputToPictureMs);
    m_prims.AddText(m_gm.originX, y + m_gm.cellH, line, 0.6f, m_sampler);
}

