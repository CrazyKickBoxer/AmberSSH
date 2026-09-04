#include "session.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <libssh2.h>

#include <algorithm>

#include "transport.h"
#include "../remote/XAuth.h"
#include "../amberx/AmberXController.h"
#include "../security/HostSigil.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------- global init
static bool EnsureGlobalInit()
{
    static bool ok = []
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        if (libssh2_init(0) != 0)
            return false;
        return true;
    }();
    return ok;
}

static std::string Base64(const uint8_t* data, size_t len)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? tbl[v & 63] : '=');
    }
    // OpenSSH-style fingerprints strip padding.
    while (!out.empty() && out.back() == '=')
        out.pop_back();
    return out;
}

// Wait for socket readiness in the direction(s) libssh2 asks for.
#include <mstcpip.h>   // SIO_TCP_INFO / TCP_INFO_v0 (link RTT sampling)

static void WaitSocket(SOCKET sock, LIBSSH2_SESSION* session, int timeoutMs)
{
    fd_set rfd, wfd;
    FD_ZERO(&rfd);
    FD_ZERO(&wfd);
    int dir = session ? libssh2_session_block_directions(session)
                      : LIBSSH2_SESSION_BLOCK_INBOUND;
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) FD_SET(sock, &rfd);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(sock, &wfd);
    if (!(dir & (LIBSSH2_SESSION_BLOCK_INBOUND | LIBSSH2_SESSION_BLOCK_OUTBOUND)))
        FD_SET(sock, &rfd);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    select(0, &rfd, &wfd, nullptr, &tv);
}

// keyboard-interactive: answer every prompt with the password.
static thread_local const std::string* tlKbdPassword = nullptr;

static void KbdCallback(const char*, int, const char*, int, int numPrompts,
                        const LIBSSH2_USERAUTH_KBDINT_PROMPT* /*prompts*/,
                        LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses,
                        void** /*abstract*/)
{
    for (int i = 0; i < numPrompts; ++i)
    {
        if (tlKbdPassword && !tlKbdPassword->empty())
        {
            responses[i].text = static_cast<char*>(malloc(tlKbdPassword->size()));
            memcpy(responses[i].text, tlKbdPassword->data(), tlKbdPassword->size());
            responses[i].length = static_cast<unsigned>(tlKbdPassword->size());
        }
        else
        {
            responses[i].text = nullptr;
            responses[i].length = 0;
        }
    }
}

// ----------------------------------------------------------- port forwarding
namespace
{

struct FwdListener
{
    SOCKET sock = INVALID_SOCKET;
    char type = 'L';                 // 'L' local forward, 'D' dynamic SOCKS5
    std::string destHost;            // 'L' only
    int destPort = 0;
};

struct FwdRemote
{
    LIBSSH2_LISTENER* lst = nullptr;
    std::string destHost;
    int destPort = 0;
};

// Per-direction backlog cap for a forwarded channel. Reaching it stops the
// READ on the producing side rather than dropping bytes, so the flow control
// is the transport's own — TCP's receive window one way, libssh2's channel
// window the other. Sized to hold a large X11 image burst without stalling
// ordinary traffic, and small enough that a runaway client cannot exhaust
// memory: with many channels this is the per-channel bound, not the total.
constexpr size_t kTunnelHighWater = 1u << 20;   // 1 MiB

struct FwdTunnel
{
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_CHANNEL* ch = nullptr;   // null while the channel is opening
    std::string pendingHost;         // direct-tcpip target while opening
    int pendingPort = 0;
    int socksState = 0;              // 'D': 0 greeting, 1 request, 2 open
    std::vector<uint8_t> socksBuf;
    std::vector<uint8_t> toChannel;  // socket → channel backlog
    std::vector<uint8_t> toSock;     // channel → socket backlog
    bool sockEof = false;
    bool chanEof = false;

    // X11 only: the first bytes from the channel are the X11 setup packet,
    // which carries the cookie the remote host was given. It is verified and
    // the real local cookie substituted before anything reaches the display —
    // see src/remote/XAuth.h for why that substitution is the point.
    bool x11 = false;
    bool x11Ready = false;           // the setup packet has been dealt with
    std::vector<uint8_t> x11Setup;   // buffered until the whole packet is in
    // Non-zero when this X11 channel goes to the session's AmberXHost over
    // the control pipe instead of to a TCP socket. `sock` is then unused and
    // the tunnel is pumped by PumpAmberX, not PumpTunnels.
    uint32_t amberxChannel = 0;
};

// Feeds bytes read from an X11 channel toward the display. Until the setup
// packet has been verified and rewritten, bytes are buffered; after that they
// pass straight through to `toSock`. Returns false when the tunnel must be
// dropped.
//
// Shared by the TCP path and the AmberX path so the cookie check cannot
// differ between them: the display being a process we own changes nothing
// about what is verified before a byte reaches it.
bool X11Intake(FwdTunnel& t, const uint8_t* buf, size_t n,
               const std::vector<uint8_t>& x11Fake,
               const std::vector<uint8_t>& x11Real, std::string* x11Error)
{
    if (!t.x11 || t.x11Ready)
    {
        t.toSock.insert(t.toSock.end(), buf, buf + n);
        return true;
    }
    // The setup packet carries the cookie the remote host was handed. It is
    // checked here, and the real local cookie put in its place, before one
    // byte reaches the display. A channel that fails the check is closed —
    // never forwarded unauthenticated.
    t.x11Setup.insert(t.x11Setup.end(), buf, buf + n);
    if (t.x11Setup.size() > amber::kMaxSetupBytes)
    {
        if (x11Error)
            *x11Error = "X11: oversized setup packet refused";
        return false;
    }
    std::vector<uint8_t> rewritten;
    switch (amber::RewriteSetup(t.x11Setup, x11Fake, x11Real, rewritten))
    {
    case amber::XAuthVerdict::NeedMore:
        return true;                // keep buffering
    case amber::XAuthVerdict::Rejected:
        if (x11Error)
            *x11Error = "X11: connection refused — the cookie did not match";
        return false;
    case amber::XAuthVerdict::Rewritten:
        t.toSock.insert(t.toSock.end(), rewritten.begin(), rewritten.end());
        t.x11Setup.clear();
        t.x11Setup.shrink_to_fit();
        t.x11Ready = true;
        return true;
    }
    return false;
}

void SetNonblock(SOCKET s)
{
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

SOCKET OpenLoopbackListener(int port, int* boundPort = nullptr)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    // Deliberately NO SO_REUSEADDR. On Windows that option does not mean what
    // it means on POSIX: it lets a second socket bind the SAME address and
    // port while the first is still listening, and the two then split
    // incoming connections between them at the kernel's discretion. With it
    // set, a forward that was already up bound a second time in silence
    // instead of reporting "port busy" — which is exactly the duplicate
    // listener a reconnect must not create. A plain bind fails with
    // WSAEADDRINUSE instead, and rebinding after our own listener closes
    // still works, so nothing is lost.
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(s, 8) != 0)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (boundPort)
    {
        sockaddr_in got = {};
        int gl = sizeof(got);
        getsockname(s, reinterpret_cast<sockaddr*>(&got), &gl);
        *boundPort = ntohs(got.sin_port);
    }
    SetNonblock(s);
    return s;
}

// Parse "L8080:host:80;R9000:host:9000;D1080" into listener/remote specs.
void ParseForwards(const std::string& spec, std::vector<FwdListener>& local,
                   std::vector<std::pair<int, std::pair<std::string, int>>>& remote,
                   std::string& errors)
{
    size_t pos = 0;
    while (pos < spec.size())
    {
        size_t end = spec.find(';', pos);
        std::string one = spec.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? spec.size() : end + 1;
        while (!one.empty() && (one.front() == ' ' || one.front() == '\t'))
            one.erase(one.begin());
        while (!one.empty() && (one.back() == ' ' || one.back() == '\r' ||
                                one.back() == '\n'))
            one.pop_back();
        if (one.empty())
            continue;
        char kind = static_cast<char>(toupper(one[0]));
        std::string rest = one.substr(1);
        if (kind == 'D')
        {
            int p = atoi(rest.c_str());
            if (p > 0 && p < 65536)
            {
                FwdListener l;
                l.type = 'D';
                l.sock = OpenLoopbackListener(p);
                if (l.sock != INVALID_SOCKET)
                    local.push_back(l);
                else
                    errors += " D" + std::to_string(p) + " (port busy)";
            }
            continue;
        }
        size_t c1 = rest.find(':');
        size_t c2 = rest.rfind(':');
        if (c1 == std::string::npos || c2 == c1)
        {
            errors += " " + one + " (bad spec)";
            continue;
        }
        int lp = atoi(rest.substr(0, c1).c_str());
        std::string host = rest.substr(c1 + 1, c2 - c1 - 1);
        int dp = atoi(rest.substr(c2 + 1).c_str());
        if (lp <= 0 || lp > 65535 || dp <= 0 || dp > 65535 || host.empty())
        {
            errors += " " + one + " (bad spec)";
            continue;
        }
        if (kind == 'L')
        {
            FwdListener l;
            l.type = 'L';
            l.destHost = host;
            l.destPort = dp;
            l.sock = OpenLoopbackListener(lp);
            if (l.sock != INVALID_SOCKET)
                local.push_back(l);
            else
                errors += " L" + std::to_string(lp) + " (port busy)";
        }
        else if (kind == 'R')
        {
            remote.push_back({ lp, { host, dp } });
        }
        else
        {
            errors += " " + one + " (unknown kind)";
        }
    }
}

// Accept new connections on each listener and start a tunnel per client.
void AcceptForwards(std::vector<FwdListener>& listeners,
                    std::vector<FwdTunnel>& tunnels)
{
    for (FwdListener& l : listeners)
    {
        for (;;)
        {
            SOCKET c = accept(l.sock, nullptr, nullptr);
            if (c == INVALID_SOCKET)
                break;
            SetNonblock(c);
            BOOL nd = TRUE;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&nd), sizeof(nd));
            FwdTunnel t;
            t.sock = c;
            if (l.type == 'L')
            {
                t.pendingHost = l.destHost;
                t.pendingPort = l.destPort;
            }
            else
            {
                t.socksState = 0;   // SOCKS5: read greeting first
            }
            tunnels.push_back(std::move(t));
        }
    }
}

// Advance a SOCKS5 handshake using bytes already in t.socksBuf. Returns false
// if the client must be dropped; sets t.pendingHost/Port when a CONNECT is
// parsed (state advances to 2, channel open handled by the caller).
bool SocksStep(FwdTunnel& t)
{
    if (t.socksState == 0)   // greeting: VER NMETHODS METHODS...
    {
        if (t.socksBuf.size() < 2)
            return true;
        uint8_t n = t.socksBuf[1];
        if (t.socksBuf[0] != 0x05)
            return false;
        if (t.socksBuf.size() < 2u + n)
            return true;
        t.socksBuf.erase(t.socksBuf.begin(), t.socksBuf.begin() + 2 + n);
        uint8_t reply[2] = { 0x05, 0x00 };   // no auth
        send(static_cast<SOCKET>(t.sock),
             reinterpret_cast<const char*>(reply), 2, 0);
        t.socksState = 1;
    }
    if (t.socksState == 1)   // request: VER CMD RSV ATYP DST.ADDR DST.PORT
    {
        if (t.socksBuf.size() < 4)
            return true;
        uint8_t atyp = t.socksBuf[3];
        std::string host;
        size_t need = 0, portOff = 0;
        if (atyp == 0x01) { need = 4 + 4 + 2; portOff = 8; }
        else if (atyp == 0x03)
        {
            if (t.socksBuf.size() < 5)
                return true;
            uint8_t dl = t.socksBuf[4];
            need = 5u + dl + 2; portOff = 5u + dl;
        }
        else if (atyp == 0x04) { need = 4 + 16 + 2; portOff = 20; }
        else return false;
        if (t.socksBuf.size() < need)
            return true;
        if (atyp == 0x01)
        {
            char b[16];
            snprintf(b, sizeof(b), "%u.%u.%u.%u", t.socksBuf[4], t.socksBuf[5],
                     t.socksBuf[6], t.socksBuf[7]);
            host = b;
        }
        else if (atyp == 0x03)
        {
            uint8_t dl = t.socksBuf[4];
            host.assign(reinterpret_cast<char*>(&t.socksBuf[5]), dl);
        }
        else return false;   // IPv6 direct-tcpip not attempted
        int port = (t.socksBuf[portOff] << 8) | t.socksBuf[portOff + 1];
        t.socksBuf.erase(t.socksBuf.begin(), t.socksBuf.begin() + need);
        t.pendingHost = host;
        t.pendingPort = port;
        t.socksState = 2;    // caller opens the channel, then replies success
    }
    return true;
}

// One pump step for every live tunnel; opens pending channels, moves bytes in
// both directions, and erases finished ones.
void PumpTunnels(std::vector<FwdTunnel>& tunnels, LIBSSH2_SESSION* session,
                 bool& activity, const std::vector<uint8_t>& x11Fake,
                 const std::vector<uint8_t>& x11Real, std::string* x11Error)
{
    uint8_t buf[32768];
    for (auto it = tunnels.begin(); it != tunnels.end();)
    {
        FwdTunnel& t = *it;
        SOCKET s = static_cast<SOCKET>(t.sock);
        bool drop = false;
        // AmberX tunnels have no socket; PumpAmberX owns them.
        if (t.amberxChannel)
        {
            ++it;
            continue;
        }

        // SOCKS5 handshake bytes from the client.
        if (t.socksState < 2 && !t.chanEof)
        {
            int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n > 0)
            {
                t.socksBuf.insert(t.socksBuf.end(), buf, buf + n);
                if (!SocksStep(t))
                    drop = true;
                activity = true;
            }
            else if (n == 0)
                drop = true;
        }

        // Open the direct-tcpip channel once the target is known.
        if (!drop && !t.ch && !t.pendingHost.empty())
        {
            LIBSSH2_CHANNEL* ch = libssh2_channel_direct_tcpip_ex(
                session, t.pendingHost.c_str(), t.pendingPort, "127.0.0.1", 0);
            if (ch)
            {
                t.ch = ch;
                libssh2_channel_set_blocking(ch, 0);
                if (t.socksState == 2)   // SOCKS: send success reply
                {
                    uint8_t ok[10] = { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0,
                                       0, 0 };
                    send(s, reinterpret_cast<const char*>(ok), 10, 0);
                    t.socksState = 3;
                }
            }
            else if (libssh2_session_last_errno(session) !=
                     LIBSSH2_ERROR_EAGAIN)
            {
                drop = true;
            }
        }

        // socket → channel
        //
        // Only read when the backlog we would add to is below the high-water
        // mark. Stopping the read IS the backpressure: TCP closes its receive
        // window and the peer stops sending. Without it a producer that
        // outruns the consumer grows this vector without limit — which never
        // mattered at terminal scale and very much does for a forwarded X11
        // client rendering faster than the display drains.
        if (!drop && t.ch && !t.sockEof && t.toChannel.size() < kTunnelHighWater)
        {
            int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n > 0)
            {
                t.toChannel.insert(t.toChannel.end(), buf, buf + n);
                activity = true;
            }
            else if (n == 0)
                t.sockEof = true;
        }
        if (t.ch && !t.toChannel.empty())
        {
            ssize_t w = libssh2_channel_write(
                t.ch, reinterpret_cast<char*>(t.toChannel.data()),
                t.toChannel.size());
            if (w > 0)
            {
                t.toChannel.erase(t.toChannel.begin(),
                                  t.toChannel.begin() + w);
                activity = true;
            }
            else if (w < 0 && w != LIBSSH2_ERROR_EAGAIN)
                drop = true;
        }
        if (t.ch && t.sockEof && t.toChannel.empty())
        {
            libssh2_channel_send_eof(t.ch);
            t.sockEof = false;   // sent once
        }

        // channel → socket
        //
        // Same rule in the other direction. Declining to read leaves the bytes
        // in libssh2's channel window, which stops the remote end rather than
        // buffering its output here without limit.
        if (!drop && t.ch && t.toSock.size() < kTunnelHighWater)
        {
            ssize_t n = libssh2_channel_read(
                t.ch, reinterpret_cast<char*>(buf), sizeof(buf));
            if (n > 0)
            {
                if (!X11Intake(t, buf, static_cast<size_t>(n), x11Fake, x11Real,
                               x11Error))
                    drop = true;
                activity = true;
            }
            else if (n == 0 && libssh2_channel_eof(t.ch))
                t.chanEof = true;
            else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN)
                drop = true;
        }
        if (!t.toSock.empty())
        {
            int w = send(s, reinterpret_cast<const char*>(t.toSock.data()),
                         static_cast<int>(t.toSock.size()), 0);
            if (w > 0)
            {
                t.toSock.erase(t.toSock.begin(), t.toSock.begin() + w);
                activity = true;
            }
            else if (w == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
                drop = true;
        }

        if (drop || (t.chanEof && t.toSock.empty()))
        {
            if (t.ch)
            {
                libssh2_channel_close(t.ch);
                libssh2_channel_free(t.ch);
            }
            closesocket(s);
            it = tunnels.erase(it);
        }
        else
            ++it;
    }
}

// The AmberX counterpart of PumpTunnels. Every tunnel with an amberxChannel is
// pumped between its libssh2 channel and the ONE pipe to the host, which
// multiplexes them by channel id. Same high-water rule in both directions,
// same X11Intake cookie check, same drop semantics.
//
// Frames from the host are routed by channel id to a tunnel this side
// already opened. A frame for a channel that is not open is a protocol error
// from the host: it is reported and dropped, never used to create a tunnel.
void PumpAmberX(std::vector<FwdTunnel>& tunnels, amber::amberx::AmberXController& host,
                bool& activity, const std::vector<uint8_t>& x11Fake,
                const std::vector<uint8_t>& x11Real, std::string* x11Error,
                std::string* clipboardIn, amber::amberx::HostReport* report)
{
    using namespace amber::amberx;
    uint8_t buf[32768];
    bool any = false;
    bool backlogged = false;
    for (auto it = tunnels.begin(); it != tunnels.end();)
    {
        FwdTunnel& t = *it;
        if (!t.amberxChannel)
        {
            ++it;
            continue;
        }
        any = true;
        bool drop = false;

        // channel → host, gated on the outbound backlog exactly as the TCP
        // path is: declining to read is the backpressure.
        if (t.ch && !t.chanEof && t.toSock.size() < kTunnelHighWater)
        {
            ssize_t n = libssh2_channel_read(t.ch, reinterpret_cast<char*>(buf), sizeof(buf));
            if (n > 0)
            {
                if (!X11Intake(t, buf, static_cast<size_t>(n), x11Fake, x11Real, x11Error))
                    drop = true;
                activity = true;
            }
            else if (n == 0 && libssh2_channel_eof(t.ch))
                t.chanEof = true;
            else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN)
                drop = true;
        }
        // Verified bytes go to the host one frame at a time.
        while (!drop && !t.toSock.empty())
        {
            const size_t n = std::min(t.toSock.size(), static_cast<size_t>(kMaxPayload));
            if (!host.SendData(t.amberxChannel, t.toSock.data(), n))
            {
                drop = true;
                break;
            }
            t.toSock.erase(t.toSock.begin(), t.toSock.begin() + static_cast<ptrdiff_t>(n));
            activity = true;
        }
        // host → channel
        if (!drop && t.ch && !t.toChannel.empty())
        {
            ssize_t w = libssh2_channel_write(t.ch, reinterpret_cast<char*>(t.toChannel.data()),
                                              t.toChannel.size());
            if (w > 0)
            {
                t.toChannel.erase(t.toChannel.begin(), t.toChannel.begin() + w);
                activity = true;
            }
            else if (w < 0 && w != LIBSSH2_ERROR_EAGAIN)
                drop = true;
        }
        if (t.toChannel.size() >= kTunnelHighWater)
            backlogged = true;

        if (drop || (t.chanEof && t.toSock.empty()))
        {
            host.CloseChannel(t.amberxChannel);
            if (t.ch)
            {
                libssh2_channel_close(t.ch);
                libssh2_channel_free(t.ch);
            }
            it = tunnels.erase(it);
        }
        else
            ++it;
    }
    // Nothing to route to, or nowhere to put it: frames stay in the pipe and
    // the host feels the backpressure.
    if (!any || backlogged)
        return;

    for (int budget = 0; budget < 64; ++budget)
    {
        Frame f;
        const PipeRead r = host.Poll(f, 0);
        if (r == PipeRead::Timeout)
            break;
        if (r != PipeRead::Ok)
        {
            // The host went away. Every AmberX channel drains and drops on
            // the next pass; the remote clients see EOF, not a hang.
            if (x11Error)
                *x11Error = "AmberX: host connection lost — X11 channels closed";
            for (FwdTunnel& t : tunnels)
                if (t.amberxChannel)
                {
                    t.toSock.clear();
                    t.chanEof = true;
                }
            break;
        }
        switch (f.type)
        {
        case MsgType::ChannelData:
        case MsgType::ChannelClose:
        {
            auto tt = std::find_if(tunnels.begin(), tunnels.end(),
                                   [&](const FwdTunnel& t) { return t.amberxChannel == f.channel; });
            if (tt == tunnels.end())
            {
                if (x11Error)
                    *x11Error = "AmberX: frame for a channel that is not open";
                break;
            }
            if (f.type == MsgType::ChannelData)
            {
                tt->toChannel.insert(tt->toChannel.end(), f.payload.begin(), f.payload.end());
                activity = true;
            }
            else
            {
                tt->toSock.clear();
                tt->chanEof = true;
            }
            break;
        }
        case MsgType::HostReport:
            // Counts and window titles for the shelf and the diagnostics
            // overlay. A report that does not parse is dropped: it is the
            // only message whose loss costs nothing.
            if (report)
                ParseHostReport(f.payload, *report);
            break;
        case MsgType::ClipboardText:
            // An X client's selection text. The server has already bounded
            // it and checked the session's policy; the caller checks it
            // again before any of it reaches the Windows clipboard.
            if (clipboardIn && !f.payload.empty())
                clipboardIn->assign(f.payload.begin(), f.payload.end());
            break;
        case MsgType::HostError:
        {
            std::string text;
            if (ParseHostError(f.payload, text) && x11Error)
                *x11Error = "AmberX: " + text;
            break;
        }
        case MsgType::HostStatus:
            break;                      // diagnostics overlay, later
        default:
            if (x11Error)
                *x11Error = "AmberX: unexpected frame from the host";
            break;
        }
    }
}

// X11 forwarding: the server opens an "x11" channel per client; each one is
// connected to the local X display (VcXsrv / Xming / WSLg) and pumped like a
// tunnel. The callback runs on the network thread inside libssh2 calls.
thread_local std::vector<LIBSSH2_CHANNEL*>* tlX11Pending = nullptr;

void X11Callback(LIBSSH2_SESSION*, LIBSSH2_CHANNEL* ch, char*, int, void**)
{
    if (tlX11Pending)
        tlX11Pending->push_back(ch);
}

// "host:display[.screen]" -> TCP 6000 + display on that host (localhost /
// unix / empty = 127.0.0.1). Returns a connected, non-blocking socket.
SOCKET ConnectX11Display(const std::string& display)
{
    std::string host = "127.0.0.1";
    int num = 0;
    size_t colon = display.rfind(':');
    if (colon != std::string::npos)
    {
        if (colon > 0)
            host = display.substr(0, colon);
        num = atoi(display.c_str() + colon + 1);
    }
    if (host.empty() || host == "localhost" || host == "unix")
        host = "127.0.0.1";
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char port[16];
    snprintf(port, sizeof(port), "%d", 6000 + num);
    if (getaddrinfo(host.c_str(), port, &hints, &res) != 0 || !res)
        return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next)
    {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET)
        SetNonblock(s);
    return s;
}

// The host-key algorithms this machine already has a key for, in the order to
// ask for them; empty when known_hosts says nothing about this host.
//
// Why this exists: known_hosts stores one key per algorithm, and a server
// usually offers several. If the stored key is ed25519 and the handshake
// settles on ecdsa, the key that arrives is a perfectly genuine key that does
// not match the stored one — and a naive comparison calls that a
// man-in-the-middle. OpenSSH avoids it by ordering its HostKeyAlgorithms by
// what it already trusts; this does the same, and it is the difference
// between a warning that means something and a warning users learn to click
// through.
//
// Hashed known_hosts entries have no readable name, so they cannot be matched
// here. That is safe: no preference is expressed, the handshake picks its
// default, and the check afterwards behaves as it always did.
std::string HostKeyPrefFromKnownHosts(LIBSSH2_KNOWNHOSTS* kh,
                                      const std::string& name, int port)
{
    if (!kh || name.empty())
        return {};
    const std::string bracketed = "[" + name + "]:" + std::to_string(port);
    std::vector<int> have;
    struct libssh2_knownhost* store = nullptr;
    struct libssh2_knownhost* prev = nullptr;
    while (libssh2_knownhost_get(kh, &store, prev) == 0 && store)
    {
        prev = store;
        if (!store->name)
            continue;                      // hashed: not matchable by name
        if (name != store->name && bracketed != store->name)
            continue;
        const int type = store->typemask & LIBSSH2_KNOWNHOST_KEY_MASK;
        if (std::find(have.begin(), have.end(), type) == have.end())
            have.push_back(type);
    }
    if (have.empty())
        return {};

    // Strongest first among what is on file. Everything not on file is left
    // out entirely: asking for an algorithm we could not check against would
    // put us back where we started.
    static const struct { int type; const char* names; } kOrder[] = {
        { LIBSSH2_KNOWNHOST_KEY_ED25519,   "ssh-ed25519" },
        { LIBSSH2_KNOWNHOST_KEY_ECDSA_521, "ecdsa-sha2-nistp521" },
        { LIBSSH2_KNOWNHOST_KEY_ECDSA_384, "ecdsa-sha2-nistp384" },
        { LIBSSH2_KNOWNHOST_KEY_ECDSA_256, "ecdsa-sha2-nistp256" },
        { LIBSSH2_KNOWNHOST_KEY_SSHRSA,    "rsa-sha2-512,rsa-sha2-256,ssh-rsa" },
        { LIBSSH2_KNOWNHOST_KEY_SSHDSS,    "ssh-dss" },
    };
    std::string pref;
    for (const auto& o : kOrder)
        if (std::find(have.begin(), have.end(), o.type) != have.end())
        {
            if (!pref.empty())
                pref += ",";
            pref += o.names;
        }
    return pref;
}

// The name a known_hosts key type goes by, for messages.
const char* KnownHostKeyTypeName(int typemask)
{
    switch (typemask & LIBSSH2_KNOWNHOST_KEY_MASK)
    {
    case LIBSSH2_KNOWNHOST_KEY_ED25519:   return "ssh-ed25519";
    case LIBSSH2_KNOWNHOST_KEY_ECDSA_521: return "ecdsa-sha2-nistp521";
    case LIBSSH2_KNOWNHOST_KEY_ECDSA_384: return "ecdsa-sha2-nistp384";
    case LIBSSH2_KNOWNHOST_KEY_ECDSA_256: return "ecdsa-sha2-nistp256";
    case LIBSSH2_KNOWNHOST_KEY_SSHRSA:    return "ssh-rsa";
    case LIBSSH2_KNOWNHOST_KEY_SSHDSS:    return "ssh-dss";
    default:                              return "an unrecognised type";
    }
}

} // anonymous namespace

SshSession::SshSession() = default;

SshSession::~SshSession()
{
    Disconnect();
}

bool SshSession::Start(const SshConfig& cfg)
{
    if (m_running.load())
        return false;
    if (!EnsureGlobalInit())
        return false;

    Disconnect();   // join any previous thread
    m_stop.store(false);
    m_output.Clear();
    {
        std::lock_guard<std::mutex> lk(m_outMutex);
        m_outQueue.clear();
        m_cmdQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_evMutex);
        m_events.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_hkMutex);
        m_hkDecided = false;
        m_hkAccepted = false;
    }
    m_resizePending.store(false);
    m_running.store(true);
    m_remoteEcho.store(true);
    m_cleanClose.store(false);
    if (cfg.protocol == 5)
        m_thread = std::thread(&SshSession::ThreadMainLocal, this, cfg);
    else if (cfg.protocol == 4)
        m_thread = std::thread(&SshSession::ThreadMainSerial, this, cfg);
    else if (cfg.protocol != 0)
        m_thread = std::thread(&SshSession::ThreadMainStream, this, cfg);
    else
        m_thread = std::thread(&SshSession::ThreadMain, this, cfg);
    return true;
}

void SshSession::AnswerHostKey(bool accept)
{
    std::lock_guard<std::mutex> lk(m_hkMutex);
    m_hkDecided = true;
    m_hkAccepted = accept;
    m_hkCv.notify_all();
}

void SshSession::Send(const char* data, size_t len)
{
    if (!m_running.load() || len == 0)
        return;
    std::lock_guard<std::mutex> lk(m_outMutex);
    m_outQueue.insert(m_outQueue.end(), data, data + len);
}

void SshSession::SendTelnetCommand(uint8_t cmd)
{
    if (!m_running.load())
        return;
    std::lock_guard<std::mutex> lk(m_outMutex);
    m_cmdQueue.push_back(cmd);
}

void SshSession::RequestResize(int cols, int rows)
{
    m_pendingCols.store(cols);
    m_pendingRows.store(rows);
    m_resizePending.store(true);
}

void SshSession::AddForward(const std::string& spec)
{
    if (spec.empty())
        return;
    std::lock_guard<std::mutex> lock(m_fwdMutex);
    m_pendingForwards.push_back(spec);
}

RemoteAppReport SshSession::AmberXReport() const
{
    std::lock_guard<std::mutex> lock(m_amberxMutex);
    return m_amberxReport;
}

void SshSession::RemoteAppAction(uint32_t xid, int action)
{
    if (xid == 0 || action < 0 || action > 2)
        return;
    std::lock_guard<std::mutex> lock(m_amberxMutex);
    if (m_pendingWindowActions.size() < 64)
        m_pendingWindowActions.emplace_back(xid, action);
}

void SshSession::OfferClipboard(std::string utf8)
{
    if (utf8.empty())
        return;
    std::lock_guard<std::mutex> lock(m_clipMutex);
    // A clipboard has one current value: an offer that has not been sent yet
    // is replaced rather than queued behind, so what reaches the session is
    // what the user last copied and never a backlog of older copies.
    m_pendingClipboard = std::move(utf8);
    m_clipPending = true;
}

void SshSession::Disconnect()
{
    m_stop.store(true);
    {
        // Unblock a pending host-key prompt.
        std::lock_guard<std::mutex> lk(m_hkMutex);
        if (!m_hkDecided)
        {
            m_hkDecided = true;
            m_hkAccepted = false;
        }
        m_hkCv.notify_all();
    }
    // Abort a blocking connect/handshake.
    uintptr_t s = m_socket.exchange(~0ull);
    if (s != ~0ull)
        closesocket(static_cast<SOCKET>(s));
    // Local: wake the reader thread out of a blocking pipe read. The handle
    // belongs to the ConPty, which closes it on its own thread — cancelling
    // here and closing there is what keeps the two from racing.
    uintptr_t lr = m_localRead.exchange(0);
    if (lr)
        CancelIoEx(reinterpret_cast<HANDLE>(lr), nullptr);
    // Serial: cancel a blocking read and close the port so the thread exits.
    uintptr_t hs = m_serial.exchange(0);
    if (hs)
    {
        CancelIoEx(reinterpret_cast<HANDLE>(hs), nullptr);
        CloseHandle(reinterpret_cast<HANDLE>(hs));
    }
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
}

bool SshSession::PollEvent(SshEvent& ev)
{
    std::lock_guard<std::mutex> lk(m_evMutex);
    if (m_events.empty())
        return false;
    ev = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

void SshSession::PostEvent(SshEventType type, std::string text)
{
    std::lock_guard<std::mutex> lk(m_evMutex);
    m_events.push_back({ type, std::move(text) });
}

// ------------------------------------------------------------- network thread
void SshSession::ThreadMain(SshConfig cfg)
{
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_CHANNEL* channel = nullptr;
    LIBSSH2_KNOWNHOSTS* kh = nullptr;
    std::string closeReason = "connection closed";
    std::vector<LIBSSH2_CHANNEL*> x11Pending;   // opened by the server (X11)
    uint32_t nextAmberxId = 1;                  // channel ids given to the host
    // Decoded once, so no hex parsing happens per forwarded connection.
    const std::vector<uint8_t> x11FakeCookie =
        amber::HexToBytes(cfg.x11FakeCookieHex);
    const std::vector<uint8_t> x11RealCookie =
        amber::HexToBytes(cfg.x11RealCookieHex);
    m_remoteEcho.store(true);
    m_cleanClose.store(false);

    auto zeroSecrets = [&]()
    {
        if (!cfg.password.empty())
            SecureZeroMemory(cfg.password.data(), cfg.password.size());
        if (!cfg.passphrase.empty())
            SecureZeroMemory(cfg.passphrase.data(), cfg.passphrase.size());
    };

    auto fail = [&](const std::string& msg)
    {
        zeroSecrets();
        if (channel)
        {
            libssh2_channel_free(channel);
            channel = nullptr;
        }
        if (kh)
        {
            libssh2_knownhost_free(kh);
            kh = nullptr;
        }
        if (session)
        {
            libssh2_session_disconnect(session, "error");
            libssh2_session_free(session);
            session = nullptr;
        }
        uintptr_t s = m_socket.exchange(~0ull);
        if (s != ~0ull)
            closesocket(static_cast<SOCKET>(s));
        PostEvent(SshEventType::Error, msg);
        m_running.store(false);
    };

    // Jump host (ProxyJump) is not yet tunnelled. Refuse rather than silently
    // connecting directly — bypassing the jump would be a security surprise.
    if (!cfg.jumpHost.empty())
    {
        fail("jump host (" + cfg.jumpHost +
             ") is configured but ProxyJump tunnelling is not yet supported "
             "in this build — clear the jump host to connect directly");
        return;
    }

    // ---- connect: direct or via proxy, IP version, timeout, TCP options --
    {
        std::string cerr, cstatus;
        PostEvent(SshEventType::Status, "resolving " + cfg.host + "...");
        sock = ConnectTransport(cfg, m_socket, m_stop, cerr, cstatus);
        if (!cfg.proxyPass.empty())
            SecureZeroMemory(cfg.proxyPass.data(), cfg.proxyPass.size());
        if (sock == INVALID_SOCKET)
        {
            fail(cerr);
            return;
        }
    }

    // ---- handshake ------------------------------------------------------
    session = libssh2_session_init();
    if (!session)
    {
        fail("libssh2_session_init failed");
        return;
    }
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, std::max(5, cfg.connectTimeoutSeconds) * 1000);
    if (cfg.compression)
        libssh2_session_flag(session, LIBSSH2_FLAG_COMPRESS, 1);
    // Algorithm preferences (comma lists, PuTTY-style). A list libssh2
    // cannot honour leaves its default order in place.
    if (!cfg.cipherPref.empty())
    {
        libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS, cfg.cipherPref.c_str());
        libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC, cfg.cipherPref.c_str());
    }
    if (!cfg.kexPref.empty())
        libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX, cfg.kexPref.c_str());
    // ---- known_hosts, loaded BEFORE the handshake ------------------------
    // The file has to be read first so the handshake can ask for the host-key
    // algorithm this machine already trusts. Reading it afterwards, as this
    // did, is how a genuine server ends up accused of being an impostor: the
    // stored key is ed25519, the handshake settles on ecdsa, and the two do
    // not match because they were never the same key.
    wchar_t profileW[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"USERPROFILE", profileW, MAX_PATH);
    std::string sshDir;
    {
        char profileA[MAX_PATH * 3];
        int n = WideCharToMultiByte(CP_UTF8, 0, profileW, -1, profileA,
                                    sizeof(profileA), nullptr, nullptr);
        sshDir = (n > 0) ? std::string(profileA) + "\\.ssh" : ".ssh";
    }
    const std::string khPath = sshDir + "\\known_hosts";
    // "Logical name of remote host" (Connection page): the known_hosts entry
    // is looked up and stored under this name instead of the address.
    const std::string khName = cfg.logicalHost.empty() ? cfg.host : cfg.logicalHost;
    kh = libssh2_knownhost_init(session);
    if (kh)
        libssh2_knownhost_readfile(kh, khPath.c_str(),
                                   LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    if (!cfg.hostKeyPref.empty())
    {
        // An explicit preference is the user's decision and is not overridden.
        libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, cfg.hostKeyPref.c_str());
    }
    else
    {
        const std::string pref = HostKeyPrefFromKnownHosts(kh, khName, cfg.port);
        if (!pref.empty())
            libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, pref.c_str());
    }

    PostEvent(SshEventType::Status, "ssh handshake...");
    if (libssh2_session_handshake(session, sock) != 0)
    {
        char* msg = nullptr;
        libssh2_session_last_error(session, &msg, nullptr, 0);
        fail(std::string("SSH handshake failed: ") + (msg ? msg : "unknown"));
        return;
    }
    // Protocol-level keepalive (Connection page): libssh2 sends
    // keepalive@openssh.com requests from keepalive_send() in the loop.
    if (cfg.keepaliveSeconds > 0)
        libssh2_keepalive_config(session, 1, static_cast<unsigned>(cfg.keepaliveSeconds));

    // ---- known_hosts ----------------------------------------------------
    size_t keyLen = 0;
    int keyType = 0;
    const char* hostKey = libssh2_session_hostkey(session, &keyLen, &keyType);
    if (!hostKey)
    {
        fail("server sent no host key");
        return;
    }

    const char* typeName = "unknown";
    int khKeyBit = LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    switch (keyType)
    {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        typeName = "ssh-rsa";
        khKeyBit = LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        break;
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        typeName = "ssh-dss";
        khKeyBit = LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        break;
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        typeName = "ecdsa-sha2-nistp256";
        khKeyBit = LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        typeName = "ecdsa-sha2-nistp384";
        khKeyBit = LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        typeName = "ecdsa-sha2-nistp521";
        khKeyBit = LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        break;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        typeName = "ssh-ed25519";
        khKeyBit = LIBSSH2_KNOWNHOST_KEY_ED25519;
        break;
#endif
    default:
        break;
    }

    std::string fingerprint;
    if (const char* sha256 =
            libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256))
        fingerprint = "SHA256:" +
            Base64(reinterpret_cast<const uint8_t*>(sha256), 32);

    int checkResult = LIBSSH2_KNOWNHOST_CHECK_NOTFOUND;
    struct libssh2_knownhost* found = nullptr;
    if (kh)
        checkResult = libssh2_knownhost_checkp(
            kh, khName.c_str(), cfg.port, hostKey, keyLen,
            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW,
            &found);

    // Manually configured host keys (SSH > Host keys): when any are listed
    // they are the whole trust store for this session - a listed
    // fingerprint is accepted without a prompt, anything else is refused.
    if (!cfg.manualHostKeys.empty())
    {
        std::string fpBare = fingerprint.rfind("SHA256:", 0) == 0 ? fingerprint.substr(7)
                                                                    : fingerprint;
        bool listed = false;
        for (std::string k : cfg.manualHostKeys)
        {
            size_t a = k.find_first_not_of(" \t\r\n");
            size_t b = k.find_last_not_of(" \t\r\n");
            if (a == std::string::npos)
                continue;
            k = k.substr(a, b - a + 1);
            if (k.rfind("SHA256:", 0) == 0)
                k = k.substr(7);
            if (_stricmp(k.c_str(), fpBare.c_str()) == 0)
            {
                listed = true;
                break;
            }
        }
        if (!listed)
        {
            fail("host key is not in the configured list. Fingerprint: " +
                 std::string(typeName) + " " + fingerprint);
            return;
        }
        checkResult = LIBSSH2_KNOWNHOST_CHECK_MATCH;
        PostEvent(SshEventType::Status, "host key matched the configured fingerprint");
    }

    if (checkResult == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
    {
        // Name the stored key's type as well as the one that arrived. A
        // mismatch between two keys of the SAME type is the alarming case;
        // between different types it is almost always a stale entry, and
        // saying which is which is the difference between a warning that can
        // be acted on and one that can only be clicked through.
        const char* stored = found ? KnownHostKeyTypeName(found->typemask)
                                   : "an unknown type";
        fail("HOST KEY MISMATCH for " + khName +
             " — possible man-in-the-middle. known_hosts holds a " + stored +
             " key for this host; the server offered " + typeName + " " +
             fingerprint + ". Remove the old entry (ssh-keygen -R " + khName +
             ") only if you are sure this is the same machine.");
        return;
    }

    if (checkResult != LIBSSH2_KNOWNHOST_CHECK_MATCH)
    {
        // Unknown host: surface the fingerprint and wait for explicit accept.
        PostEvent(SshEventType::HostKeyPrompt,
                  std::string(typeName) + " " + fingerprint);
        std::unique_lock<std::mutex> lk(m_hkMutex);
        m_hkCv.wait(lk, [&] { return m_hkDecided; });
        if (!m_hkAccepted || m_stop.load())
        {
            lk.unlock();
            fail("host key rejected");
            return;
        }
        lk.unlock();

        if (kh && khKeyBit != LIBSSH2_KNOWNHOST_KEY_UNKNOWN)
        {
            libssh2_knownhost_addc(
                kh, khName.c_str(), nullptr, hostKey, keyLen,
                "added by AmberSSH", 18,
                LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                    khKeyBit,
                nullptr);
            CreateDirectoryW((std::wstring(profileW) + L"\\.ssh").c_str(), nullptr);
            libssh2_knownhost_writefile(kh, khPath.c_str(),
                                        LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
    }

    // ---- auth -----------------------------------------------------------
    PostEvent(SshEventType::Status, "authenticating...");
    char* authList = libssh2_userauth_list(session, cfg.user.c_str(),
                                           static_cast<unsigned>(cfg.user.size()));
    bool authed = libssh2_userauth_authenticated(session) != 0;

    if (!authed && cfg.useAgent)
    {
        // Windows OpenSSH agent / Pageant. Try every identity the agent holds
        // until one authenticates.
        LIBSSH2_AGENT* agent = libssh2_agent_init(session);
        if (!agent || libssh2_agent_connect(agent) != 0)
        {
            if (agent)
                libssh2_agent_free(agent);
            fail("no SSH agent is running (start ssh-agent or Pageant)");
            return;
        }
        if (libssh2_agent_list_identities(agent) != 0)
        {
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
            fail("could not list agent identities");
            return;
        }
        struct libssh2_agent_publickey* id = nullptr;
        struct libssh2_agent_publickey* prev = nullptr;
        while (!authed)
        {
            int rc = libssh2_agent_get_identity(agent, &id, prev);
            if (rc != 0)
                break;           // 1 = no more, <0 = error
            if (libssh2_agent_userauth(agent, cfg.user.c_str(), id) == 0)
                authed = true;
            prev = id;
        }
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        if (!authed)
        {
            fail("no agent identity was accepted by the server");
            return;
        }
    }
    else if (!authed && cfg.useKey)
    {
        int rc = libssh2_userauth_publickey_fromfile_ex(
            session, cfg.user.c_str(), static_cast<unsigned>(cfg.user.size()),
            nullptr, cfg.keyPath.c_str(),
            cfg.passphrase.empty() ? nullptr : cfg.passphrase.c_str());
        authed = (rc == 0);
        if (!authed)
        {
            char* msg = nullptr;
            libssh2_session_last_error(session, &msg, nullptr, 0);
            fail(std::string("private key authentication failed: ") +
                 (msg ? msg : "unknown"));
            return;
        }
    }
    else if (!authed)
    {
        if (authList && strstr(authList, "password"))
            authed = libssh2_userauth_password(session, cfg.user.c_str(),
                                               cfg.password.c_str()) == 0;
        if (!authed && authList && strstr(authList, "keyboard-interactive"))
        {
            tlKbdPassword = &cfg.password;
            authed = libssh2_userauth_keyboard_interactive(
                         session, cfg.user.c_str(), &KbdCallback) == 0;
            tlKbdPassword = nullptr;
        }
        if (!authed)
        {
            fail("authentication failed (check user/password)");
            return;
        }
    }
    zeroSecrets();   // never keep credentials in memory post-auth

    // ---- pty + shell / command (or no channel at all: tunnels only) ----
    if (!cfg.noShell)
    {
        channel = libssh2_channel_open_session(session);
        if (!channel)
        {
            fail("could not open channel");
            return;
        }
        const std::string& term =
            cfg.termType.empty() ? std::string("xterm-256color") : cfg.termType;
        // Terminal modes: TTY_OP_ISPEED (128) / TTY_OP_OSPEED (129) from the
        // Data page's "terminal speeds", then TTY_OP_END.
        unsigned ispeed = 38400, ospeed = 38400;
        if (sscanf(cfg.termSpeed.c_str(), "%u,%u", &ispeed, &ospeed) == 1)
            ospeed = ispeed;
        unsigned char modes[11];
        auto put32 = [&](int at, unsigned v)
        {
            modes[at] = static_cast<unsigned char>(v >> 24);
            modes[at + 1] = static_cast<unsigned char>(v >> 16);
            modes[at + 2] = static_cast<unsigned char>(v >> 8);
            modes[at + 3] = static_cast<unsigned char>(v);
        };
        modes[0] = 128; put32(1, ispeed);
        modes[5] = 129; put32(6, ospeed);
        modes[10] = 0;
        if (libssh2_channel_request_pty_ex(channel, term.c_str(),
                                           static_cast<unsigned>(term.size()),
                                           reinterpret_cast<const char*>(modes),
                                           static_cast<unsigned>(sizeof(modes)),
                                           cfg.cols, cfg.rows, 0, 0) != 0)
        {
            fail("PTY request failed");
            return;
        }
        // Environment variables (Data page), one NAME=value per line. sshd
        // silently refuses names outside its AcceptEnv list.
        {
            size_t pos = 0;
            while (pos < cfg.envVars.size())
            {
                size_t nl = cfg.envVars.find('\n', pos);
                std::string line = cfg.envVars.substr(
                    pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? cfg.envVars.size() : nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                size_t eq = line.find('=');
                if (eq == std::string::npos || eq == 0)
                    continue;
                libssh2_channel_setenv(channel, line.substr(0, eq).c_str(),
                                       line.substr(eq + 1).c_str());
            }
        }
        if (cfg.agentForward && libssh2_channel_request_auth_agent(channel) != 0)
            PostEvent(SshEventType::Status, "agent forwarding refused by the server");
        if (cfg.x11Forward)
        {
            tlX11Pending = &x11Pending;
            libssh2_session_callback_set2(
                session, LIBSSH2_CALLBACK_X11,
                reinterpret_cast<libssh2_cb_generic*>(&X11Callback));
            // The remote host is given the FAKE cookie and never the real one.
            // Without a fake cookie there is nothing to authenticate a
            // forwarded connection with, so forwarding is refused rather than
            // requested unauthenticated — see src/remote/XAuth.h.
            if (x11FakeCookie.empty())
                PostEvent(SshEventType::Status,
                          "X11 forwarding not requested: no cookie could be "
                          "generated");
            else if (libssh2_channel_x11_req_ex(
                         channel, 0, amber::kMitMagicCookie,
                         cfg.x11FakeCookieHex.c_str(), 0) != 0)
                PostEvent(SshEventType::Status,
                          "X11 forwarding refused by the server");
            else if (cfg.x11Backend == 1)
            {
                // AmberX: the display is a host process this session owns.
                // Started here, in the blocking phase, so its synchronous
                // handshake costs nothing the connection was not already
                // paying for. If it cannot start, forwarding has already
                // been requested from the server, so every channel that
                // arrives is refused — never silently redirected to an
                // external display the user did not choose.
                m_amberx = std::make_unique<amber::amberx::AmberXController>();
                {
                    // The identity strip: AmberSSH's name for the session, the
                    // verified host key's sigil, and the user's skin. Nothing
                    // in it came from the remote side.
                    amber::amberx::AmberXController::Launch launch;
                    launch.identity = cfg.amberxIdentity.empty()
                                          ? cfg.host + " \xc2\xb7 " + cfg.user
                                          : cfg.amberxIdentity;
                    launch.trusted = cfg.x11Trusted;
                    launch.modeLabel = cfg.x11Trusted ? "X11 TRUSTED" : "X11 RESTRICTED";
                    launch.sigil = fingerprint.empty()
                                       ? std::string()
                                       : amber::SigilMnemonic(amber::MakeSigil(fingerprint));
                    launch.skin = cfg.amberxSkin;
                    launch.clipboardMode = cfg.x11Clipboard;
                    launch.desktopX = cfg.x11DesktopX;
                    launch.desktopY = cfg.x11DesktopY;
                    launch.desktopW = cfg.x11DesktopW;
                    launch.desktopH = cfg.x11DesktopH;
                    launch.presentCapHz = cfg.x11PresentCapHz;
                    if (cfg.x11WindowMode != 0)
                        PostEvent(SshEventType::Status,
                                  "Remote GUI: only native windows are implemented  14 "
                                  "this session uses native windows");
                    m_amberx->Configure(launch);
                }
                std::string aerr;
                if (!m_amberx->Start(aerr))
                {
                    PostEvent(SshEventType::Status,
                              "AmberX could not start: " + aerr +
                                  " — X11 channels will be refused");
                    m_amberx.reset();
                }
                else if (!m_amberx->SetCookie(x11FakeCookie, 0))
                {
                    PostEvent(SshEventType::Status,
                              "AmberX: could not install the cookie — X11 "
                              "channels will be refused");
                    m_amberx.reset();
                }
                else
                    PostEvent(SshEventType::Status,
                              "AmberX host ready (" + m_amberx->Security() + ")");
            }
            else if (x11RealCookie.empty())
                PostEvent(SshEventType::Status,
                          "X11: no local cookie found — the X server's own "
                          "access control decides who may connect");
        }
    }
    // Advertise 24-bit color. Many sshd configs refuse SetEnv for anything but
    // LC_*/LANG — a refusal is expected and must never fail the connection.
    if (channel)
        libssh2_channel_setenv(channel, "COLORTERM", "truecolor");
    // UTF-8 charset hint (sshd's stock AcceptEnv allows LC_*). Without it a
    // session that lands in the C/POSIX locale makes zsh/readline/vim echo
    // pasted emoji as numeric byte escapes instead of the glyphs. LC_CTYPE
    // (not LANG) so a server-configured language/messages locale is left
    // alone; if C.UTF-8 doesn't exist remotely the shell falls back harmlessly.
    if (channel)
        libssh2_channel_setenv(channel, "LC_CTYPE", "C.UTF-8");
    if (channel && !cfg.remoteCommand.empty())
    {
        // SSH page: run this command instead of a login shell.
        if (libssh2_channel_exec(channel, cfg.remoteCommand.c_str()) != 0)
        {
            fail("remote command request failed");
            return;
        }
    }
    else if (channel && libssh2_channel_shell(channel) != 0)
    {
        fail("shell request failed");
        return;
    }

    libssh2_session_set_blocking(session, 0);

    // ---- port forwarding ------------------------------------------------
    std::vector<FwdListener> listeners;
    std::vector<FwdTunnel> tunnels;
    if (!cfg.forwards.empty())
    {
        std::vector<std::pair<int, std::pair<std::string, int>>> remote;
        std::string ferr;
        ParseForwards(cfg.forwards, listeners, remote, ferr);
        std::string msg = "tunnels: " + std::to_string(listeners.size()) +
                          " local/SOCKS active";
        if (!remote.empty())
            msg += ", " + std::to_string(remote.size()) +
                   " remote (R) skipped — not yet supported";
        if (!ferr.empty())
            msg += " (skipped" + ferr + ")";
        PostEvent(SshEventType::Status, msg);
    }

    PostEvent(SshEventType::Connected);

    // ---- I/O loop -------------------------------------------------------
    std::vector<uint8_t> readBuf(64 * 1024);
    std::vector<uint8_t> writeBuf;
    size_t writeOff = 0;

    while (!m_stop.load())
    {
        bool activity = false;

        // Forwards raised while the session is up (RemoteApp). Drained here so
        // the listener is created on this thread like every other one, and a
        // busy port is reported rather than silently ignored.
        {
            std::vector<std::string> add;
            {
                std::lock_guard<std::mutex> lock(m_fwdMutex);
                add.swap(m_pendingForwards);
            }
            for (const std::string& spec : add)
            {
                std::vector<FwdListener> more;
                std::vector<std::pair<int, std::pair<std::string, int>>> ignored;
                std::string err;
                ParseForwards(spec, more, ignored, err);
                if (!err.empty())
                    PostEvent(SshEventType::Status, "forward failed:" + err);
                for (FwdListener& l : more)
                    listeners.push_back(l);
                if (more.empty() && err.empty())
                    PostEvent(SshEventType::Status,
                              "forward not understood: " + spec);
                else if (!more.empty())
                    PostEvent(SshEventType::Status, "tunnel up: " + spec);
            }
        }

        // Tunnels: accept new local/SOCKS clients, pump existing ones.
        if (!listeners.empty() || !tunnels.empty())
        {
            AcceptForwards(listeners, tunnels);
            std::string x11Err;
            PumpTunnels(tunnels, session, activity, x11FakeCookie, x11RealCookie,
                        &x11Err);
            std::string clipIn;
            amber::amberx::HostReport report;
            bool haveReport = false;
            if (m_amberx)
            {
                const uint32_t before = report.presents;
                PumpAmberX(tunnels, *m_amberx, activity, x11FakeCookie, x11RealCookie,
                           &x11Err, &clipIn, &report);
                haveReport = report.clients || report.windows || report.presents != before ||
                             !report.windowList.empty() || report.x11In;
                // Window actions the shelf asked for go down on the same pass.
                std::vector<std::pair<uint32_t, int>> acts;
                {
                    std::lock_guard<std::mutex> lock(m_amberxMutex);
                    acts.swap(m_pendingWindowActions);
                }
                for (const auto& a : acts)
                    m_amberx->SendWindowAction(a.first,
                                               static_cast<amber::amberx::WindowAct>(a.second));
            }
            if (haveReport)
            {
                RemoteAppReport out;
                out.valid = true;
                out.clients = report.clients;
                out.windows = report.windows;
                out.pixmapBytes = report.pixmapBytes;
                out.x11In = report.x11In;
                out.x11Out = report.x11Out;
                out.presents = report.presents;
                out.dirtyRects = report.dirtyRects;
                out.ipcHighWater = report.ipcHighWater;
                out.rejected = report.rejected;
                out.list.reserve(report.windowList.size());
                for (const auto& w : report.windowList)
                    out.list.push_back({ w.xid, w.flags, w.title });
                std::lock_guard<std::mutex> lock(m_amberxMutex);
                out.lastError = m_amberxReport.lastError;   // errors are sticky
                m_amberxReport = std::move(out);
            }
            if (!x11Err.empty())
            {
                std::lock_guard<std::mutex> lock(m_amberxMutex);
                m_amberxReport.lastError = x11Err;
            }
            if (!x11Err.empty())
                PostEvent(SshEventType::Status, x11Err);
            // Clipboard, both directions. The policy is checked here as well
            // as in the host and the server: three copies of one rule, none
            // of them able to move text on their own.
            if (!clipIn.empty() &&
                (cfg.x11Clipboard == 1 || cfg.x11Clipboard == 2 || cfg.x11Clipboard == 4))
                PostEvent(SshEventType::ClipboardText, std::move(clipIn));
            if (m_amberx &&
                (cfg.x11Clipboard == 1 || cfg.x11Clipboard == 3 || cfg.x11Clipboard == 4))
            {
                std::string out;
                {
                    std::lock_guard<std::mutex> lock(m_clipMutex);
                    if (m_clipPending)
                    {
                        out = std::move(m_pendingClipboard);
                        m_pendingClipboard.clear();
                        m_clipPending = false;
                    }
                }
                if (!out.empty())
                    m_amberx->SendClipboard(out);
            }
        }

        // X11 channels the server opened: connect each to the local display.
        while (!x11Pending.empty())
        {
            LIBSSH2_CHANNEL* xc = x11Pending.back();
            x11Pending.pop_back();
            if (cfg.x11Backend == 1)
            {
                // AmberX: the channel is handed to the host under a fresh
                // id. No host, or a host at its channel cap, means the
                // channel is refused — the remote client sees a closed
                // connection, not a redirect.
                if (!m_amberx || !m_amberx->OpenChannel(nextAmberxId))
                {
                    libssh2_channel_close(xc);
                    libssh2_channel_free(xc);
                    PostEvent(SshEventType::Status,
                              m_amberx ? "AmberX: channel refused (limit reached)"
                                       : "AmberX: no host — X11 channel refused");
                    continue;
                }
                FwdTunnel t;
                t.ch = xc;
                libssh2_channel_set_blocking(xc, 0);
                t.socksState = 3;
                t.x11 = true;
                t.amberxChannel = nextAmberxId++;
                tunnels.push_back(std::move(t));
                continue;
            }
            SOCKET xs = ConnectX11Display(cfg.x11Display);
            if (xs == INVALID_SOCKET)
            {
                libssh2_channel_close(xc);
                libssh2_channel_free(xc);
                PostEvent(SshEventType::Status, "X11: no X server at " + cfg.x11Display);
                continue;
            }
            FwdTunnel t;
            t.sock = xs;
            t.ch = xc;
            libssh2_channel_set_blocking(xc, 0);
            t.socksState = 3;
            // The setup packet on this channel carries the cookie the remote
            // host was given, and is checked before anything reaches the
            // display.
            t.x11 = true;
            tunnels.push_back(std::move(t));
        }

        // Protocol keepalive (no-op unless configured).
        if (cfg.keepaliveSeconds > 0)
        {
            int secs = 0;
            libssh2_keepalive_send(session, &secs);
        }

        // Window-size change.
        if (m_resizePending.exchange(false) && channel)
        {
            int rc;
            do
            {
                rc = libssh2_channel_request_pty_size(
                    channel, m_pendingCols.load(), m_pendingRows.load());
                if (rc == LIBSSH2_ERROR_EAGAIN)
                    WaitSocket(sock, session, 50);
            } while (rc == LIBSSH2_ERROR_EAGAIN && !m_stop.load());
        }

        // Server → ring (stdout + stderr merged).
        for (int streamId = 0; channel && streamId < 2; ++streamId)
        {
            for (;;)
            {
                ssize_t n = libssh2_channel_read_ex(channel, streamId,
                                                    reinterpret_cast<char*>(readBuf.data()),
                                                    readBuf.size());
                if (n > 0)
                {
                    activity = true;
                    size_t off = 0;
                    while (off < static_cast<size_t>(n) && !m_stop.load())
                    {
                        size_t pushed = m_output.Push(readBuf.data() + off,
                                                      static_cast<size_t>(n) - off);
                        off += pushed;
                        if (off < static_cast<size_t>(n))
                            Sleep(1);   // consumer catching up
                    }
                }
                else if (n == LIBSSH2_ERROR_EAGAIN || n == 0)
                    break;
                else
                {
                    closeReason = "read error";
                    goto closed;
                }
            }
        }

        // Keystrokes → server.
        if (writeOff >= writeBuf.size())
        {
            writeBuf.clear();
            writeOff = 0;
            std::lock_guard<std::mutex> lk(m_outMutex);
            writeBuf.swap(m_outQueue);
        }
        while (channel && writeOff < writeBuf.size())
        {
            ssize_t n = libssh2_channel_write(
                channel, reinterpret_cast<char*>(writeBuf.data()) + writeOff,
                writeBuf.size() - writeOff);
            if (n > 0)
            {
                writeOff += static_cast<size_t>(n);
                activity = true;
            }
            else if (n == LIBSSH2_ERROR_EAGAIN)
                break;
            else
            {
                closeReason = "write error";
                goto closed;
            }
        }

        if (channel && libssh2_channel_eof(channel))
        {
            closeReason = "remote closed the session";
            m_cleanClose.store(true);
            goto closed;
        }

        // Link latency for the renderer's ghosting effect: the kernel's own
        // smoothed TCP RTT for this socket, sampled about once a second.
        {
            static thread_local ULONGLONG lastRtt = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastRtt >= 1000)
            {
                lastRtt = now;
                TCP_INFO_v0 info = {};
                DWORD ver = 0, got = 0;
                if (WSAIoctl(sock, SIO_TCP_INFO, &ver, sizeof(ver), &info,
                             sizeof(info), &got, nullptr, nullptr) == 0)
                {
                    m_rttUs.store(info.RttUs);
                    m_retrans.store(static_cast<uint32_t>(
                        info.BytesRetrans > 0xFFFFFFFFull ? 0xFFFFFFFFull : info.BytesRetrans));
                }
            }
        }

        if (!activity)
            WaitSocket(sock, session, 30);
    }
    closeReason = "disconnected";
    m_cleanClose.store(true);

closed:
    for (FwdTunnel& t : tunnels)
    {
        if (t.ch)
        {
            libssh2_channel_close(t.ch);
            libssh2_channel_free(t.ch);
        }
        if (t.sock != INVALID_SOCKET)
            closesocket(static_cast<SOCKET>(t.sock));
    }
    tunnels.clear();
    for (FwdListener& l : listeners)
        if (l.sock != INVALID_SOCKET)
            closesocket(l.sock);
    listeners.clear();
    if (channel)
    {
        libssh2_session_set_blocking(session, 0);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = nullptr;
    }
    if (kh)
    {
        libssh2_knownhost_free(kh);
        kh = nullptr;
    }
    // The host goes before the transport: a Shutdown frame, a moment for a
    // clean exit, then the Job Object takes whatever is left.
    if (m_amberx)
    {
        m_amberx->Stop();
        m_amberx.reset();
    }
    if (session)
    {
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session);
        session = nullptr;
    }
    uintptr_t s = m_socket.exchange(~0ull);
    if (s != ~0ull)
        closesocket(static_cast<SOCKET>(s));

    PostEvent(SshEventType::Closed, closeReason);
    m_running.store(false);
}
