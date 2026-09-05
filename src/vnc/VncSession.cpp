// VncSession.cpp — the VNC worker: transport, handshake, steady state,
// reconnect, and the damage hand-off to the render thread. See the header.
#include "VncSession.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace amber::vnc
{
namespace
{

constexpr int kReadChunk = 64 * 1024;

uint64_t NowMs()
{
    return static_cast<uint64_t>(GetTickCount64());
}

double CpuMs()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

std::string WsaText(int e)
{
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(e), 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string s = msg ? msg : "";
    if (msg)
        LocalFree(msg);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    return s.empty() ? "winsock " + std::to_string(e) : s + " (winsock " + std::to_string(e) + ")";
}

} // namespace

const char* VncStateName(VncState s)
{
    switch (s)
    {
    case VncState::Connecting:     return "connecting";
    case VncState::Authenticating: return "authenticating";
    case VncState::Connected:      return "connected";
    case VncState::Reconnecting:   return "reconnecting";
    case VncState::Disconnected:   return "disconnected";
    case VncState::Error:          return "error";
    }
    return "unknown";
}

VncSession::VncSession() = default;

VncSession::~VncSession()
{
    Disconnect();
}

bool VncSession::Start(const VncConfig& cfg)
{
    if (m_running.load())
        return false;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   // reference-counted; the app has done this already
    m_cfg = cfg;
    m_stop.store(false);
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running.store(true);
    SetState(VncState::Connecting);
    m_thread = std::thread([this] { Worker(); });
    return true;
}

void VncSession::Disconnect()
{
    m_stop.store(true);
    if (m_wake)
        SetEvent(m_wake);
    {
        std::lock_guard<std::mutex> lk(m_tunnelMu);
        m_tunnelCv.notify_all();
    }
    if (m_thread.joinable())
        m_thread.join();
    if (m_wake)
    {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
    std::fill(m_cfg.password.begin(), m_cfg.password.end(), '\0');
    m_cfg.password.clear();
    m_running.store(false);
    if (m_state.load() != VncState::Error)
        SetState(VncState::Disconnected);
}

bool VncSession::PollEvent(VncEvent& ev)
{
    std::lock_guard<std::mutex> lk(m_evMu);
    if (m_events.empty())
        return false;
    ev = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

void VncSession::Post(VncEvent::Type type, std::string text)
{
    std::lock_guard<std::mutex> lk(m_evMu);
    m_events.push_back({ type, std::move(text) });
}

bool VncSession::TakeDamage(Damage& out)
{
    std::lock_guard<std::mutex> lk(m_damageMu);
    if (m_pending.rects.empty())
        return false;
    out = std::move(m_pending);
    m_pending = Damage{};
    m_pending.width = out.width;
    m_pending.height = out.height;
    return true;
}

bool VncSession::TakeCursor(CursorShape& out)
{
    std::lock_guard<std::mutex> lk(m_damageMu);
    if (!m_cursorChanged)
        return false;
    out = m_cursor;
    m_cursorChanged = false;
    return true;
}

void VncSession::SendKey(bool down, uint32_t keysym)
{
    if (m_cfg.viewOnly)
        return;
    Command c{ Command::Kind::Key };
    c.down = down;
    c.keysym = keysym;
    {
        std::lock_guard<std::mutex> lk(m_cmdMu);
        m_cmds.push_back(std::move(c));
    }
    if (m_wake)
        SetEvent(m_wake);
}

void VncSession::SendPointer(uint8_t buttons, uint16_t x, uint16_t y)
{
    if (m_cfg.viewOnly)
        return;
    Command c{ Command::Kind::Pointer };
    c.buttons = buttons;
    c.x = x;
    c.y = y;
    {
        std::lock_guard<std::mutex> lk(m_cmdMu);
        // pointer motion coalesces: only the newest position matters, so a
        // burst of moves while the link is slow becomes one message
        if (!m_cmds.empty() && m_cmds.back().kind == Command::Kind::Pointer &&
            m_cmds.back().buttons == buttons)
            m_cmds.back() = std::move(c);
        else
            m_cmds.push_back(std::move(c));
    }
    if (m_wake)
        SetEvent(m_wake);
}

void VncSession::SendCutText(std::string utf8)
{
    if (m_cfg.viewOnly)
        return;
    Command c{ Command::Kind::CutText };
    c.text = std::move(utf8);
    {
        std::lock_guard<std::mutex> lk(m_cmdMu);
        m_cmds.push_back(std::move(c));
    }
    if (m_wake)
        SetEvent(m_wake);
}

void VncSession::RequestFullUpdate()
{
    {
        std::lock_guard<std::mutex> lk(m_cmdMu);
        m_cmds.push_back(Command{ Command::Kind::Refresh });
    }
    if (m_wake)
        SetEvent(m_wake);
}

std::string VncSession::TunnelSpec() const
{
    if (!m_cfg.addForward)
        return {};
    return "L0:" + m_cfg.host + ":" + std::to_string(m_cfg.port);
}

void VncSession::OnForwardUp(const std::string& text)
{
    // "bound:host:port"
    const size_t c1 = text.find(':');
    const size_t c2 = text.rfind(':');
    if (c1 == std::string::npos || c2 == c1)
        return;
    const int bound = atoi(text.substr(0, c1).c_str());
    const std::string host = text.substr(c1 + 1, c2 - c1 - 1);
    const int port = atoi(text.substr(c2 + 1).c_str());
    if (host != m_cfg.host || port != m_cfg.port || bound <= 0)
        return;
    {
        std::lock_guard<std::mutex> lk(m_tunnelMu);
        m_tunnelPort = bound;
    }
    m_tunnelCv.notify_all();
}

VncStats VncSession::GetStats() const
{
    VncStats s;
    s.bytesIn = m_bytesIn.load();
    s.bytesOut = m_bytesOut.load();
    s.updates = m_updates.load();
    s.decodeMsPerSec = m_decodeMsPerSec.load();
    s.reconnects = m_reconnects.load();
    s.fbWidth = m_fbW.load();
    s.fbHeight = m_fbH.load();
    s.continuous = m_continuous.load();
    s.rfbMinor = m_minor.load();
    {
        std::lock_guard<std::mutex> lk(m_damageMu);
        s.pendingRects = static_cast<uint32_t>(m_pending.rects.size());
        s.pendingPixels = m_pending.pixels.size();
    }
    return s;
}

// ---- worker --------------------------------------------------------------------
void VncSession::Worker()
{
    int attempt = 0;
    uint64_t backoffMs = 1000;
    for (;;)
    {
        if (m_stop.load())
            break;
        SetState(attempt == 0 ? VncState::Connecting : VncState::Reconnecting);

        ClientOptions o;
        o.password = m_cfg.password;
        o.wantCursor = m_cfg.wantCursor;
        switch (m_cfg.encodings)
        {
        case 1:  o.encodings = { EncHextile, EncZRLE, EncCopyRect, EncRaw }; break;
        case 2:  o.encodings = { EncRaw }; break;
        default: o.encodings = { EncZRLE, EncHextile, EncCopyRect, EncRaw }; break;
        }
        RfbClient client(o);
        std::fill(o.password.begin(), o.password.end(), '\0');

        std::string err;
        bool retryable = true;
        const bool up = Connect(client, err, retryable);
        if (!up)
        {
            if (m_sock != INVALID_SOCKET)
            {
                closesocket(static_cast<SOCKET>(m_sock));
                m_sock = INVALID_SOCKET;
            }
            if (m_stop.load())
                break;
            if (!retryable)
            {
                SetState(VncState::Error);
                Post(client.Failure() == FailureKind::Authentication ? VncEvent::Type::AuthFailed
                                                                     : VncEvent::Type::Error,
                     err);
                break;
            }
            if (attempt >= m_cfg.maxReconnects)
            {
                SetState(VncState::Error);
                Post(VncEvent::Type::Error, err + " — giving up after " + std::to_string(attempt) + " retries");
                break;
            }
            ++attempt;
            m_reconnects.fetch_add(1);
            SetState(VncState::Reconnecting);
            Post(VncEvent::Type::Status, err + " — retrying in " + std::to_string(backoffMs / 1000) + " s");
            WaitForSingleObject(m_wake, static_cast<DWORD>(backoffMs));
            backoffMs = std::min<uint64_t>(backoffMs * 2, 30000);
            continue;
        }

        attempt = 0;
        backoffMs = 1000;
        SetState(VncState::Connected);
        Post(VncEvent::Type::Connected, client.Init().name);
        const std::string why = Loop(client);
        if (m_sock != INVALID_SOCKET)
        {
            closesocket(static_cast<SOCKET>(m_sock));
            m_sock = INVALID_SOCKET;
        }
        if (m_stop.load())
            break;
        // the stream died: a fresh connection asks for a fresh full picture
        ++attempt;
        m_reconnects.fetch_add(1);
        SetState(VncState::Reconnecting);
        Post(VncEvent::Type::Status, why + " — reconnecting");
        WaitForSingleObject(m_wake, static_cast<DWORD>(backoffMs));
        backoffMs = std::min<uint64_t>(backoffMs * 2, 30000);
    }
    if (m_sockEvent)
    {
        WSACloseEvent(static_cast<WSAEVENT>(m_sockEvent));
        m_sockEvent = nullptr;
    }
    if (m_state.load() != VncState::Error)
    {
        SetState(VncState::Disconnected);
        Post(VncEvent::Type::Closed, "disconnected");
    }
    m_running.store(false);
}

bool VncSession::WaitForTunnel(std::string& err)
{
    std::unique_lock<std::mutex> lk(m_tunnelMu);
    if (m_tunnelPort > 0)
        return true;
    if (!m_tunnelAsked)
    {
        m_tunnelAsked = true;
        lk.unlock();
        m_cfg.addForward(TunnelSpec());
        lk.lock();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_cfg.tunnelTimeoutMs);
    while (m_tunnelPort == 0 && !m_stop.load())
    {
        if (m_tunnelCv.wait_until(lk, deadline) == std::cv_status::timeout)
            break;
    }
    if (m_tunnelPort > 0)
        return true;
    err = m_stop.load() ? "cancelled" : "the SSH session did not open the tunnel";
    return false;
}

bool VncSession::OpenTransport(std::string& err)
{
    std::string host = m_cfg.host;
    int port = m_cfg.port;
    if (m_cfg.addForward)
    {
        if (!WaitForTunnel(err))
            return false;
        host = "127.0.0.1";
        std::lock_guard<std::mutex> lk(m_tunnelMu);
        port = m_tunnelPort;
    }

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res)
    {
        err = "cannot resolve " + host + ": " + WsaText(rc ? rc : WSAGetLastError());
        return false;
    }
    SOCKET s = INVALID_SOCKET;
    std::string last;
    for (addrinfo* ai = res; ai && !m_stop.load(); ai = ai->ai_next)
    {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        const BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
            break;
        if (WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set wfd, efd;
            FD_ZERO(&wfd);
            FD_ZERO(&efd);
            FD_SET(s, &wfd);
            FD_SET(s, &efd);
            timeval tv;
            tv.tv_sec = m_cfg.connectTimeoutMs / 1000;
            tv.tv_usec = (m_cfg.connectTimeoutMs % 1000) * 1000;
            const int n = select(0, nullptr, &wfd, &efd, &tv);
            if (n > 0 && FD_ISSET(s, &wfd))
            {
                int soerr = 0;
                int len = sizeof soerr;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
                if (soerr == 0)
                    break;
                last = WsaText(soerr);
            }
            else if (n == 0)
                last = "connection timed out";
            else
            {
                int soerr = 0;
                int len = sizeof soerr;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
                last = WsaText(soerr ? soerr : WSAGetLastError());
            }
        }
        else
            last = WsaText(WSAGetLastError());
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET)
    {
        err = "connection failed: " + (last.empty() ? std::string("no address to try") : last);
        return false;
    }
    m_sock = static_cast<uintptr_t>(s);
    if (!m_sockEvent)
        m_sockEvent = WSACreateEvent();
    WSAEventSelect(s, static_cast<WSAEVENT>(m_sockEvent), FD_READ | FD_WRITE | FD_CLOSE);
    m_outbound.clear();
    return true;
}

bool VncSession::FlushOutput()
{
    while (!m_outbound.empty())
    {
        const int n = send(static_cast<SOCKET>(m_sock), reinterpret_cast<const char*>(m_outbound.data()),
                           static_cast<int>(std::min<size_t>(m_outbound.size(), 1u << 20)), 0);
        if (n > 0)
        {
            m_bytesOut.fetch_add(static_cast<uint64_t>(n));
            m_outbound.erase(m_outbound.begin(), m_outbound.begin() + n);
            continue;
        }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            return true;   // FD_WRITE will say when to continue
        return false;
    }
    return true;
}

// One wait, then whatever can be read and written. `closed` reports the
// peer going away or a failed read/write.
bool VncSession::Pump(RfbClient& client, int timeoutMs, bool& closed)
{
    closed = false;
    HANDLE hs[2] = { static_cast<HANDLE>(m_sockEvent), m_wake };
    const DWORD w = WaitForMultipleObjects(2, hs, FALSE, static_cast<DWORD>(timeoutMs));
    if (w == WAIT_OBJECT_0)
    {
        WSANETWORKEVENTS ne;
        if (WSAEnumNetworkEvents(static_cast<SOCKET>(m_sock), static_cast<WSAEVENT>(m_sockEvent), &ne) == 0)
        {
            if (ne.lNetworkEvents & FD_READ)
            {
                uint8_t buf[kReadChunk];
                for (;;)
                {
                    const int n = recv(static_cast<SOCKET>(m_sock), reinterpret_cast<char*>(buf), sizeof buf, 0);
                    if (n > 0)
                    {
                        m_bytesIn.fetch_add(static_cast<uint64_t>(n));
                        const double t0 = CpuMs();
                        const bool ok = client.Feed(buf, static_cast<size_t>(n));
                        m_decodeAccum += CpuMs() - t0;
                        if (!ok)
                        {
                            closed = true;
                            return false;
                        }
                        if (n < static_cast<int>(sizeof buf))
                            break;
                        continue;
                    }
                    if (n == 0)
                    {
                        closed = true;
                        return false;
                    }
                    if (WSAGetLastError() != WSAEWOULDBLOCK)
                    {
                        closed = true;
                        return false;
                    }
                    break;
                }
            }
            if (ne.lNetworkEvents & FD_CLOSE)
            {
                // read what is left first: the last bytes often ride with the FIN
                uint8_t buf[kReadChunk];
                int n;
                while ((n = recv(static_cast<SOCKET>(m_sock), reinterpret_cast<char*>(buf), sizeof buf, 0)) > 0)
                {
                    m_bytesIn.fetch_add(static_cast<uint64_t>(n));
                    if (!client.Feed(buf, static_cast<size_t>(n)))
                        break;
                }
                closed = true;
                return false;
            }
        }
    }
    // commands from the UI thread, in order
    {
        std::deque<Command> cmds;
        {
            std::lock_guard<std::mutex> lk(m_cmdMu);
            cmds.swap(m_cmds);
        }
        for (const Command& c : cmds)
        {
            switch (c.kind)
            {
            case Command::Kind::Key:     client.SendKey(c.down, c.keysym); break;
            case Command::Kind::Pointer: client.SendPointer(c.buttons, c.x, c.y); break;
            case Command::Kind::CutText: client.SendCutText(c.text); break;
            case Command::Kind::Refresh: client.RequestUpdate(false, true); break;
            }
        }
    }
    const std::vector<uint8_t> out = client.TakeOutput();
    m_outbound.insert(m_outbound.end(), out.begin(), out.end());
    if (!FlushOutput())
    {
        closed = true;
        return false;
    }
    return true;
}

bool VncSession::Connect(RfbClient& client, std::string& err, bool& retryable)
{
    retryable = true;
    if (!OpenTransport(err))
        return false;
    const uint64_t deadline = NowMs() + static_cast<uint64_t>(m_cfg.handshakeTimeoutMs);
    while (client.State() != ClientState::Ready)
    {
        if (m_stop.load())
        {
            err = "cancelled";
            return false;
        }
        if (client.State() == ClientState::Failed)
            break;
        if (client.State() == ClientState::VncAuthChallenge || client.State() == ClientState::SecurityResult)
            SetState(VncState::Authenticating);
        const uint64_t now = NowMs();
        if (now >= deadline)
        {
            err = "the server did not complete the handshake in time";
            return false;
        }
        bool closed = false;
        if (!Pump(client, static_cast<int>(std::min<uint64_t>(deadline - now, 1000)), closed) && closed &&
            client.State() != ClientState::Failed)
        {
            err = "the server closed the connection during the handshake";
            return false;
        }
    }
    if (client.State() == ClientState::Failed)
    {
        err = client.Error();
        retryable = client.Failure() == FailureKind::Protocol;
        return false;
    }
    m_minor.store(client.NegotiatedMinor());
    m_fbW.store(client.Fb().width);
    m_fbH.store(client.Fb().height);
    {
        // The render side learns the size from the pending damage's header;
        // the framebuffer's initial black is not a picture worth uploading,
        // so the dirty rect ServerInit leaves is dropped rather than published.
        // The first real picture — the full update just requested — is what
        // arrives next, and it arrives as `full`.
        std::lock_guard<std::mutex> lk(m_damageMu);
        m_pending = Damage{};
        m_pending.width = client.Fb().width;
        m_pending.height = client.Fb().height;
    }
    client.Dirty().Clear();
    Publish(client);
    return true;
}

void VncSession::Publish(RfbClient& client)
{
    if (client.TakeResized())
    {
        m_fbW.store(client.Fb().width);
        m_fbH.store(client.Fb().height);
        Post(VncEvent::Type::Resized, std::to_string(client.Fb().width) + "x" + std::to_string(client.Fb().height));
        std::lock_guard<std::mutex> lk(m_damageMu);
        m_pending = Damage{};
        m_pending.width = client.Fb().width;
        m_pending.height = client.Fb().height;
    }
    DirtyRegion& dirty = client.Dirty();
    if (!dirty.Empty())
    {
        const Framebuffer& fb = client.Fb();
        std::lock_guard<std::mutex> lk(m_damageMu);
        m_pending.width = fb.width;
        m_pending.height = fb.height;
        const size_t area = static_cast<size_t>(fb.width) * fb.height;
        for (const Rect& r : dirty.rects)
        {
            const size_t px = static_cast<size_t>(r.w) * r.h;
            if (m_pending.pixels.size() + px > area * 2)
            {
                // too much unclaimed: one copy of everything is the bound
                m_pending.rects.clear();
                m_pending.pixels.assign(fb.px.begin(), fb.px.end());
                m_pending.rects.push_back({ 0, 0, static_cast<uint16_t>(fb.width), static_cast<uint16_t>(fb.height) });
                m_pending.full = true;
                break;
            }
            m_pending.rects.push_back(r);
            const size_t base = m_pending.pixels.size();
            m_pending.pixels.resize(base + px);
            for (uint32_t row = 0; row < r.h; ++row)
                std::memcpy(m_pending.pixels.data() + base + static_cast<size_t>(row) * r.w,
                            fb.Row(r.y + row) + r.x, static_cast<size_t>(r.w) * 4);
        }
        if (dirty.rects.size() == 1 && dirty.rects[0].x == 0 && dirty.rects[0].y == 0 &&
            dirty.rects[0].w == fb.width && dirty.rects[0].h == fb.height)
            m_pending.full = true;
        dirty.Clear();
    }
    if (client.TakeCursorChanged())
    {
        std::lock_guard<std::mutex> lk(m_damageMu);
        m_cursor = client.Cursor();
        m_cursorChanged = true;
    }
    for (std::string& t : client.TakeCutTexts())
        Post(VncEvent::Type::CutText, std::move(t));
    for (int b = client.TakeBells(); b > 0; --b)
        Post(VncEvent::Type::Bell);
    m_updates.fetch_add(client.TakeUpdatesCompleted());
    m_continuous.store(client.ContinuousUpdates());
}

std::string VncSession::Loop(RfbClient& client)
{
    uint64_t statWindow = NowMs();
    m_decodeAccum = 0.0;
    for (;;)
    {
        if (m_stop.load())
            return "closed";
        bool closed = false;
        const bool ok = Pump(client, 1000, closed);
        Publish(client);
        if (!ok)
        {
            if (client.State() == ClientState::Failed)
                return client.Error();
            return closed ? "the server closed the connection" : "connection lost";
        }
        const uint64_t now = NowMs();
        if (now - statWindow >= 1000)
        {
            m_decodeMsPerSec.store(static_cast<float>(m_decodeAccum * 1000.0 / static_cast<double>(now - statWindow)));
            m_decodeAccum = 0.0;
            statWindow = now;
        }
    }
}

} // namespace amber::vnc
