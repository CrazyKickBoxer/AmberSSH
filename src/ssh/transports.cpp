// transports.cpp — shared TCP connect (IP version, timeout, TCP options,
// SOCKS4/4a/5 and HTTP CONNECT proxies) plus the non-SSH session threads:
// Telnet (RFC 854 negotiation: ECHO, SGA, TTYPE, TSPEED, NAWS), Rlogin
// (RFC 1282 handshake and window-size messages), Raw TCP, and Serial COM
// ports (DCB: baud / data bits / stop bits / parity / flow control).
#include "session.h"
#include "transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace
{

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Base64Pad(const std::string& in)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size())
    {
        uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                     (static_cast<uint8_t>(in[i + 1]) << 8) |
                     static_cast<uint8_t>(in[i + 2]);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i < in.size())
    {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        if (i + 1 < in.size())
            v |= static_cast<uint8_t>(in[i + 1]) << 8;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// Send everything (blocking socket), honouring stop.
bool SendAll(SOCKET s, const void* data, size_t len, std::atomic<bool>& stop)
{
    const char* p = static_cast<const char*>(data);
    while (len > 0 && !stop.load())
    {
        int n = send(s, p, static_cast<int>(std::min<size_t>(len, 1 << 20)), 0);
        if (n <= 0)
            return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return len == 0;
}

// Receive exactly len bytes with a per-call timeout (blocking socket).
bool RecvExact(SOCKET s, void* buf, size_t len, std::atomic<bool>& stop, int timeoutMs)
{
    char* p = static_cast<char*>(buf);
    while (len > 0 && !stop.load())
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        int sel = select(0, &r, nullptr, nullptr, &tv);
        if (sel <= 0)
            return false;
        int n = recv(s, p, static_cast<int>(len), 0);
        if (n <= 0)
            return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return len == 0;
}

// Non-blocking connect with a timeout; the socket is returned blocking.
SOCKET ConnectAddr(const addrinfo* ai, int timeoutMs, std::atomic<uintptr_t>& abortSlot,
                   std::atomic<bool>& stop, int& err)
{
    SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET)
    {
        err = WSAGetLastError();
        return INVALID_SOCKET;
    }
    abortSlot.store(static_cast<uintptr_t>(s));
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        err = WSAGetLastError();
        abortSlot.exchange(~0ull);
        closesocket(s);
        return INVALID_SOCKET;
    }
    // Wait in slices so a cancel is noticed promptly.
    int waited = 0;
    bool ok = false;
    while (waited < timeoutMs && !stop.load())
    {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv = { 0, 200 * 1000 };
        int sel = select(0, nullptr, &w, &e, &tv);
        if (sel > 0)
        {
            if (FD_ISSET(s, &e))
            {
                int soerr = 0, sl = sizeof(soerr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl);
                err = soerr ? soerr : WSAECONNREFUSED;
                break;
            }
            if (FD_ISSET(s, &w))
            {
                ok = true;
                break;
            }
        }
        waited += 200;
    }
    if (!ok)
    {
        if (err == 0)
            err = stop.load() ? WSAEINTR : WSAETIMEDOUT;
        abortSlot.exchange(~0ull);
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

bool ResolveIPv4(const std::string& host, uint8_t out[4])
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return false;
    auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    memcpy(out, &sin->sin_addr, 4);
    freeaddrinfo(res);
    return true;
}

// ---- proxy handshakes -------------------------------------------------------

bool Socks5Handshake(SOCKET s, const SshConfig& cfg, std::atomic<bool>& stop,
                     std::string& err, int timeoutMs)
{
    const bool haveAuth = !cfg.proxyUser.empty();
    uint8_t greet[4] = { 0x05, haveAuth ? uint8_t(2) : uint8_t(1), 0x00, 0x02 };
    if (!SendAll(s, greet, haveAuth ? 4 : 3, stop))
    {
        err = "proxy: send failed";
        return false;
    }
    uint8_t rep[2];
    if (!RecvExact(s, rep, 2, stop, timeoutMs) || rep[0] != 0x05)
    {
        err = "proxy: no SOCKS5 greeting reply";
        return false;
    }
    if (rep[1] == 0x02)
    {
        std::string a;
        a.push_back(0x01);
        a.push_back(static_cast<char>(std::min<size_t>(cfg.proxyUser.size(), 255)));
        a += cfg.proxyUser.substr(0, 255);
        a.push_back(static_cast<char>(std::min<size_t>(cfg.proxyPass.size(), 255)));
        a += cfg.proxyPass.substr(0, 255);
        if (!SendAll(s, a.data(), a.size(), stop))
        {
            err = "proxy: auth send failed";
            return false;
        }
        uint8_t ar[2];
        if (!RecvExact(s, ar, 2, stop, timeoutMs) || ar[1] != 0x00)
        {
            err = "proxy: SOCKS5 username/password rejected";
            return false;
        }
    }
    else if (rep[1] != 0x00)
    {
        err = "proxy: SOCKS5 offered no acceptable auth method";
        return false;
    }

    std::string req;
    req.push_back(0x05);
    req.push_back(0x01);   // CONNECT
    req.push_back(0x00);
    uint8_t ip[4];
    if (!cfg.proxyDns && ResolveIPv4(cfg.host, ip))
    {
        req.push_back(0x01);
        req.append(reinterpret_cast<char*>(ip), 4);
    }
    else
    {
        req.push_back(0x03);
        req.push_back(static_cast<char>(std::min<size_t>(cfg.host.size(), 255)));
        req += cfg.host.substr(0, 255);
    }
    req.push_back(static_cast<char>((cfg.port >> 8) & 0xFF));
    req.push_back(static_cast<char>(cfg.port & 0xFF));
    if (!SendAll(s, req.data(), req.size(), stop))
    {
        err = "proxy: CONNECT send failed";
        return false;
    }
    uint8_t hdr[4];
    if (!RecvExact(s, hdr, 4, stop, timeoutMs))
    {
        err = "proxy: no CONNECT reply";
        return false;
    }
    if (hdr[1] != 0x00)
    {
        static const char* reasons[] = {
            "succeeded", "general failure", "connection not allowed",
            "network unreachable", "host unreachable", "connection refused",
            "TTL expired", "command not supported", "address type not supported"
        };
        err = std::string("proxy refused the connection: ") +
              (hdr[1] < 9 ? reasons[hdr[1]] : "unknown error");
        return false;
    }
    size_t rest = 0;
    if (hdr[3] == 0x01) rest = 4 + 2;
    else if (hdr[3] == 0x04) rest = 16 + 2;
    else if (hdr[3] == 0x03)
    {
        uint8_t l;
        if (!RecvExact(s, &l, 1, stop, timeoutMs))
            return false;
        rest = l + 2u;
    }
    std::string skip(rest, '\0');
    if (rest && !RecvExact(s, skip.data(), rest, stop, timeoutMs))
    {
        err = "proxy: truncated CONNECT reply";
        return false;
    }
    return true;
}

bool Socks4Handshake(SOCKET s, const SshConfig& cfg, std::atomic<bool>& stop,
                     std::string& err, int timeoutMs)
{
    std::string req;
    req.push_back(0x04);
    req.push_back(0x01);
    req.push_back(static_cast<char>((cfg.port >> 8) & 0xFF));
    req.push_back(static_cast<char>(cfg.port & 0xFF));
    uint8_t ip[4];
    bool socks4a = false;
    if (!cfg.proxyDns && ResolveIPv4(cfg.host, ip))
        req.append(reinterpret_cast<char*>(ip), 4);
    else
    {
        // SOCKS4a: 0.0.0.x address, host name after the user id.
        socks4a = true;
        req.append("\0\0\0\1", 4);
    }
    req += cfg.proxyUser;
    req.push_back('\0');
    if (socks4a)
    {
        req += cfg.host;
        req.push_back('\0');
    }
    if (!SendAll(s, req.data(), req.size(), stop))
    {
        err = "proxy: SOCKS4 send failed";
        return false;
    }
    uint8_t rep[8];
    if (!RecvExact(s, rep, 8, stop, timeoutMs))
    {
        err = "proxy: no SOCKS4 reply";
        return false;
    }
    if (rep[1] != 90)
    {
        err = "proxy refused the connection (SOCKS4 code " +
              std::to_string(rep[1]) + ")";
        return false;
    }
    return true;
}

bool HttpConnectHandshake(SOCKET s, const SshConfig& cfg, std::atomic<bool>& stop,
                          std::string& err, int timeoutMs)
{
    std::string hostPort = cfg.host + ":" + std::to_string(cfg.port);
    std::string req = "CONNECT " + hostPort + " HTTP/1.1\r\nHost: " + hostPort +
                      "\r\nProxy-Connection: keep-alive\r\n";
    if (!cfg.proxyUser.empty())
        req += "Proxy-Authorization: Basic " +
               Base64Pad(cfg.proxyUser + ":" + cfg.proxyPass) + "\r\n";
    req += "\r\n";
    if (!SendAll(s, req.data(), req.size(), stop))
    {
        err = "proxy: CONNECT send failed";
        return false;
    }
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos && resp.size() < 16384)
    {
        char b;
        if (!RecvExact(s, &b, 1, stop, timeoutMs))
        {
            err = "proxy: no HTTP CONNECT reply";
            return false;
        }
        resp.push_back(b);
    }
    // "HTTP/1.x 200 ..."
    size_t sp = resp.find(' ');
    int code = (sp != std::string::npos) ? atoi(resp.c_str() + sp + 1) : 0;
    if (code != 200)
    {
        size_t eol = resp.find("\r\n");
        err = "proxy refused the connection: " + resp.substr(0, eol);
        return false;
    }
    return true;
}

// ---- telnet -----------------------------------------------------------------

enum : uint8_t
{
    TN_IAC = 255, TN_DONT = 254, TN_DO = 253, TN_WONT = 252, TN_WILL = 251,
    TN_SB = 250, TN_SE = 240,
    TN_OPT_BINARY = 0, TN_OPT_ECHO = 1, TN_OPT_SGA = 3, TN_OPT_TTYPE = 24,
    TN_OPT_NAWS = 31, TN_OPT_TSPEED = 32,
};

struct TelnetState
{
    enum { Data, Iac, Will, Wont, Do, Dont, Sb, SbIac } st = Data;
    std::vector<uint8_t> sb;
    bool usEnabled[256] = {};      // options we have WILL-ed
    bool themEnabled[256] = {};    // options the server has WILL-ed
    bool lastCr = false;
    std::string termType;
    std::string speed;
    int cols = 80, rows = 24;

    bool WeSupport(uint8_t o) const
    {
        return o == TN_OPT_TTYPE || o == TN_OPT_NAWS || o == TN_OPT_TSPEED ||
               o == TN_OPT_SGA || o == TN_OPT_BINARY;
    }
    bool TheyMay(uint8_t o) const
    {
        return o == TN_OPT_ECHO || o == TN_OPT_SGA || o == TN_OPT_BINARY;
    }
    static void Cmd(std::vector<uint8_t>& out, uint8_t c, uint8_t o)
    {
        out.push_back(TN_IAC);
        out.push_back(c);
        out.push_back(o);
    }
    void Naws(std::vector<uint8_t>& out) const
    {
        out.push_back(TN_IAC);
        out.push_back(TN_SB);
        out.push_back(TN_OPT_NAWS);
        auto put = [&](int v) {
            uint8_t hi = static_cast<uint8_t>((v >> 8) & 0xFF), lo = static_cast<uint8_t>(v & 0xFF);
            out.push_back(hi); if (hi == TN_IAC) out.push_back(TN_IAC);
            out.push_back(lo); if (lo == TN_IAC) out.push_back(TN_IAC);
        };
        put(cols);
        put(rows);
        out.push_back(TN_IAC);
        out.push_back(TN_SE);
    }
    void SubIs(std::vector<uint8_t>& out, uint8_t opt, const std::string& v) const
    {
        out.push_back(TN_IAC);
        out.push_back(TN_SB);
        out.push_back(opt);
        out.push_back(0);   // IS
        for (char c : v)
            out.push_back(static_cast<uint8_t>(c));
        out.push_back(TN_IAC);
        out.push_back(TN_SE);
    }

    // Splits incoming bytes into terminal data and protocol replies.
    void Feed(const uint8_t* d, size_t n, std::vector<uint8_t>& data,
              std::vector<uint8_t>& reply, bool& remoteEcho)
    {
        for (size_t i = 0; i < n; ++i)
        {
            uint8_t b = d[i];
            switch (st)
            {
            case Data:
                if (b == TN_IAC) { st = Iac; break; }
                if (lastCr && b == 0)
                {
                    lastCr = false;   // CR NUL = bare CR
                    break;
                }
                lastCr = (b == '\r');
                data.push_back(b);
                break;
            case Iac:
                switch (b)
                {
                case TN_IAC:  data.push_back(TN_IAC); st = Data; break;
                case TN_WILL: st = Will; break;
                case TN_WONT: st = Wont; break;
                case TN_DO:   st = Do; break;
                case TN_DONT: st = Dont; break;
                case TN_SB:   sb.clear(); st = Sb; break;
                default:      st = Data; break;   // NOP, GA, DM, ...
                }
                break;
            case Will:
                if (TheyMay(b))
                {
                    if (!themEnabled[b]) { themEnabled[b] = true; Cmd(reply, TN_DO, b); }
                    if (b == TN_OPT_ECHO) remoteEcho = true;
                }
                else
                    Cmd(reply, TN_DONT, b);
                st = Data;
                break;
            case Wont:
                if (themEnabled[b]) { themEnabled[b] = false; Cmd(reply, TN_DONT, b); }
                if (b == TN_OPT_ECHO) remoteEcho = false;
                st = Data;
                break;
            case Do:
                if (WeSupport(b))
                {
                    if (!usEnabled[b])
                    {
                        usEnabled[b] = true;
                        Cmd(reply, TN_WILL, b);
                        if (b == TN_OPT_NAWS)
                            Naws(reply);
                    }
                }
                else
                    Cmd(reply, TN_WONT, b);
                st = Data;
                break;
            case Dont:
                if (usEnabled[b]) { usEnabled[b] = false; Cmd(reply, TN_WONT, b); }
                st = Data;
                break;
            case Sb:
                if (b == TN_IAC) st = SbIac;
                else sb.push_back(b);
                break;
            case SbIac:
                if (b == TN_IAC) { sb.push_back(TN_IAC); st = Sb; break; }
                if (b == TN_SE)
                {
                    if (sb.size() >= 2 && sb[1] == 1)   // SEND
                    {
                        if (sb[0] == TN_OPT_TTYPE)
                            SubIs(reply, TN_OPT_TTYPE, termType);
                        else if (sb[0] == TN_OPT_TSPEED)
                            SubIs(reply, TN_OPT_TSPEED, speed);
                    }
                }
                st = Data;
                break;
            }
        }
    }
};

void SetNonblocking(SOCKET s, bool nb)
{
    u_long v = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

std::wstring WidenA(const std::string& s)
{
    std::wstring w;
    for (unsigned char c : s)
        w.push_back(static_cast<wchar_t>(c));
    return w;
}

} // namespace

// ---------------------------------------------------------------- public API

bool WildcardMatch(const std::string& pattern, const std::string& text)
{
    std::string p = Lower(pattern), t = Lower(text);
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size())
    {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) { ++pi; ++ti; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
        else if (star != std::string::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*')
        ++pi;
    return pi == p.size();
}

bool ProxyExcluded(const SshConfig& cfg)
{
    std::string h = Lower(cfg.host);
    if (!cfg.proxyLocalhost)
    {
        if (h == "localhost" || h == "::1" || h.rfind("127.", 0) == 0)
            return true;
    }
    size_t pos = 0;
    const std::string& list = cfg.proxyExclude;
    while (pos < list.size())
    {
        size_t end = list.find_first_of(",; \t\r\n", pos);
        std::string one = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? list.size() : end + 1;
        if (one.empty())
            continue;
        if (WildcardMatch(one, h))
            return true;
    }
    return false;
}

SOCKET ConnectTransport(const SshConfig& cfg, std::atomic<uintptr_t>& abortSlot,
                        std::atomic<bool>& stop, std::string& err, std::string& status)
{
    const bool viaProxy = cfg.proxyType != 0 && !cfg.proxyHost.empty() &&
                          cfg.proxyPort > 0 && !ProxyExcluded(cfg);
    const std::string connHost = viaProxy ? cfg.proxyHost : cfg.host;
    const int connPort = viaProxy ? cfg.proxyPort : cfg.port;
    const int timeoutMs = std::max(1, cfg.connectTimeoutSeconds) * 1000;

    status = "resolving " + connHost + "...";
    addrinfo hints = {};
    hints.ai_family = cfg.ipVersion == 1 ? AF_INET : cfg.ipVersion == 2 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", connPort);
    addrinfo* res = nullptr;
    if (getaddrinfo(connHost.c_str(), portStr, &hints, &res) != 0 || !res)
    {
        err = "could not resolve host \"" + connHost + "\"";
        return INVALID_SOCKET;
    }

    SOCKET sock = INVALID_SOCKET;
    int lastErr = 0;
    for (addrinfo* ai = res; ai && !stop.load(); ai = ai->ai_next)
    {
        sock = ConnectAddr(ai, timeoutMs, abortSlot, stop, lastErr);
        if (sock != INVALID_SOCKET)
            break;
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCKET || stop.load())
    {
        if (stop.load())
            err = "cancelled";
        else if (lastErr == WSAETIMEDOUT)
            err = "connection timed out after " + std::to_string(cfg.connectTimeoutSeconds) + " s";
        else
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "connection failed (winsock %d)", lastErr);
            err = buf;
        }
        if (sock != INVALID_SOCKET)
        {
            abortSlot.exchange(~0ull);
            closesocket(sock);
        }
        return INVALID_SOCKET;
    }

    BOOL nd = cfg.tcpNoDelay ? TRUE : FALSE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
    BOOL ka = cfg.tcpKeepalive ? TRUE : FALSE;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&ka), sizeof(ka));

    if (viaProxy)
    {
        status = "proxy handshake...";
        bool ok = false;
        switch (cfg.proxyType)
        {
        case 1: ok = Socks4Handshake(sock, cfg, stop, err, timeoutMs); break;
        case 2: ok = Socks5Handshake(sock, cfg, stop, err, timeoutMs); break;
        case 3: ok = HttpConnectHandshake(sock, cfg, stop, err, timeoutMs); break;
        default: ok = true; break;
        }
        if (!ok)
        {
            abortSlot.exchange(~0ull);
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }
    return sock;
}

// ---------------------------------------------------------- stream sessions

void SshSession::ThreadMainStream(SshConfig cfg)
{
    const bool telnet = cfg.protocol == 1;
    const bool rlogin = cfg.protocol == 2;
    m_remoteEcho.store(rlogin);   // telnet: until the server WILL ECHO; raw: never
    m_cleanClose.store(false);

    auto fail = [&](const std::string& msg)
    {
        uintptr_t s = m_socket.exchange(~0ull);
        if (s != ~0ull)
            closesocket(static_cast<SOCKET>(s));
        PostEvent(SshEventType::Error, msg);
        m_running.store(false);
    };

    std::string err, status;
    PostEvent(SshEventType::Status, "resolving " + cfg.host + "...");
    SOCKET sock = ConnectTransport(cfg, m_socket, m_stop, err, status);
    if (!cfg.proxyPass.empty())
        SecureZeroMemory(cfg.proxyPass.data(), cfg.proxyPass.size());
    if (sock == INVALID_SOCKET)
    {
        fail(err);
        return;
    }

    const std::string term = cfg.termType.empty() ? "xterm-256color" : cfg.termType;
    std::string speed = cfg.termSpeed.empty() ? "38400,38400" : cfg.termSpeed;

    if (rlogin)
    {
        // RFC 1282: NUL, client user, NUL, server user, NUL, "term/speed", NUL.
        PostEvent(SshEventType::Status, "rlogin handshake...");
        std::string hs;
        hs.push_back('\0');
        hs += cfg.rloginLocalUser.empty() ? cfg.user : cfg.rloginLocalUser;
        hs.push_back('\0');
        hs += cfg.user;
        hs.push_back('\0');
        hs += term + "/" + speed.substr(0, speed.find(','));
        hs.push_back('\0');
        uint8_t ack = 1;
        if (!SendAll(sock, hs.data(), hs.size(), m_stop) ||
            !RecvExact(sock, &ack, 1, m_stop, std::max(1, cfg.connectTimeoutSeconds) * 1000) ||
            ack != 0)
        {
            fail(m_stop.load() ? "cancelled" : "rlogin server rejected the connection");
            return;
        }
    }

    TelnetState tn;
    tn.termType = term;
    for (char& c : tn.termType)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    tn.speed = speed;
    tn.cols = cfg.cols;
    tn.rows = cfg.rows;
    std::vector<uint8_t> toSend;
    if (telnet && !cfg.telnetPassive)
    {
        // Active negotiation: offer what we do, ask for what we want.
        TelnetState::Cmd(toSend, TN_WILL, TN_OPT_TTYPE);  tn.usEnabled[TN_OPT_TTYPE] = true;
        TelnetState::Cmd(toSend, TN_WILL, TN_OPT_NAWS);   tn.usEnabled[TN_OPT_NAWS] = true;
        TelnetState::Cmd(toSend, TN_WILL, TN_OPT_TSPEED); tn.usEnabled[TN_OPT_TSPEED] = true;
        TelnetState::Cmd(toSend, TN_DO, TN_OPT_SGA);      tn.themEnabled[TN_OPT_SGA] = true;
        TelnetState::Cmd(toSend, TN_DO, TN_OPT_ECHO);     tn.themEnabled[TN_OPT_ECHO] = true;
    }

    PostEvent(SshEventType::Connected);
    SetNonblocking(sock, true);

    std::vector<uint8_t> readBuf(64 * 1024), data, reply;
    std::vector<uint8_t> writeBuf;
    size_t writeOff = 0;
    std::string closeReason = "connection closed";
    bool remoteEcho = m_remoteEcho.load();

    while (!m_stop.load())
    {
        bool activity = false;

        if (m_resizePending.exchange(false))
        {
            tn.cols = m_pendingCols.load();
            tn.rows = m_pendingRows.load();
            if (telnet && tn.usEnabled[TN_OPT_NAWS])
                tn.Naws(toSend);
            else if (rlogin)
            {
                // Window-size message: FF FF 's' 's' rows cols xpix ypix.
                uint8_t m[12] = { 0xFF, 0xFF, 's', 's',
                                  static_cast<uint8_t>(tn.rows >> 8), static_cast<uint8_t>(tn.rows & 0xFF),
                                  static_cast<uint8_t>(tn.cols >> 8), static_cast<uint8_t>(tn.cols & 0xFF),
                                  0, 0, 0, 0 };
                toSend.insert(toSend.end(), m, m + 12);
            }
        }

        // Server → ring.
        int n = recv(sock, reinterpret_cast<char*>(readBuf.data()),
                     static_cast<int>(readBuf.size()), 0);
        if (n > 0)
        {
            activity = true;
            data.clear();
            reply.clear();
            if (telnet)
            {
                tn.Feed(readBuf.data(), static_cast<size_t>(n), data, reply, remoteEcho);
                m_remoteEcho.store(remoteEcho);
                toSend.insert(toSend.end(), reply.begin(), reply.end());
            }
            else
                data.assign(readBuf.begin(), readBuf.begin() + n);
            size_t off = 0;
            while (off < data.size() && !m_stop.load())
            {
                size_t pushed = m_output.Push(data.data() + off, data.size() - off);
                off += pushed;
                if (off < data.size())
                    Sleep(1);
            }
        }
        else if (n == 0)
        {
            closeReason = "remote closed the connection";
            m_cleanClose.store(true);
            break;
        }
        else if (WSAGetLastError() != WSAEWOULDBLOCK)
        {
            closeReason = "read error";
            break;
        }

        // Keystrokes → server.
        if (writeOff >= writeBuf.size())
        {
            writeBuf.clear();
            writeOff = 0;
            std::vector<uint8_t> raw, cmds;
            {
                std::lock_guard<std::mutex> lk(m_outMutex);
                raw.swap(m_outQueue);
                cmds.swap(m_cmdQueue);
            }
            if (telnet)
            {
                for (uint8_t c : cmds)
                {
                    toSend.push_back(TN_IAC);
                    toSend.push_back(c);
                }
                for (uint8_t b : raw)
                {
                    if (b == TN_IAC) { toSend.push_back(TN_IAC); toSend.push_back(TN_IAC); }
                    else if (b == '\r')
                    {
                        toSend.push_back('\r');
                        toSend.push_back(cfg.telnetNewline ? '\n' : '\0');
                    }
                    else
                        toSend.push_back(b);
                }
            }
            else
                toSend.insert(toSend.end(), raw.begin(), raw.end());
            writeBuf.swap(toSend);
            toSend.clear();
        }
        while (writeOff < writeBuf.size())
        {
            int w = send(sock, reinterpret_cast<const char*>(writeBuf.data()) + writeOff,
                         static_cast<int>(writeBuf.size() - writeOff), 0);
            if (w > 0)
            {
                writeOff += static_cast<size_t>(w);
                activity = true;
            }
            else if (WSAGetLastError() == WSAEWOULDBLOCK)
                break;
            else
            {
                closeReason = "write error";
                goto closed;
            }
        }

        if (!activity)
        {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(sock, &r);
            timeval tv = { 0, 30 * 1000 };
            select(0, &r, nullptr, nullptr, &tv);
        }
    }
    if (m_stop.load())
    {
        closeReason = "disconnected";
        m_cleanClose.store(true);
    }
closed:
    {
        uintptr_t s = m_socket.exchange(~0ull);
        if (s != ~0ull)
            closesocket(static_cast<SOCKET>(s));
    }
    PostEvent(SshEventType::Closed, closeReason);
    m_running.store(false);
}

// ------------------------------------------------------------ serial session

void SshSession::ThreadMainSerial(SshConfig cfg)
{
    m_remoteEcho.store(false);
    m_cleanClose.store(false);
    std::wstring path = L"\\\\.\\" + WidenA(cfg.serialPort);
    PostEvent(SshEventType::Status, "opening " + cfg.serialPort + "...");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD e = GetLastError();
        std::string msg = "could not open " + cfg.serialPort + " (";
        msg += e == ERROR_FILE_NOT_FOUND ? "no such port" :
               e == ERROR_ACCESS_DENIED ? "port in use" : ("error " + std::to_string(e));
        msg += ")";
        PostEvent(SshEventType::Error, msg);
        m_running.store(false);
        return;
    }
    m_serial.store(reinterpret_cast<uintptr_t>(h));

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = static_cast<DWORD>(cfg.serialBaud);
    dcb.ByteSize = static_cast<BYTE>(cfg.serialDataBits);
    dcb.StopBits = cfg.serialStopBits == 2 ? TWOSTOPBITS
                 : cfg.serialStopBits == 15 ? ONE5STOPBITS : ONESTOPBIT;
    dcb.fParity = cfg.serialParity != 0;
    dcb.Parity = cfg.serialParity == 1 ? ODDPARITY : cfg.serialParity == 2 ? EVENPARITY
               : cfg.serialParity == 3 ? MARKPARITY : cfg.serialParity == 4 ? SPACEPARITY
               : NOPARITY;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    switch (cfg.serialFlow)
    {
    case 1: dcb.fOutX = TRUE; dcb.fInX = TRUE; dcb.XonChar = 0x11; dcb.XoffChar = 0x13; break;
    case 2: dcb.fOutxCtsFlow = TRUE; dcb.fRtsControl = RTS_CONTROL_HANDSHAKE; break;
    case 3: dcb.fOutxDsrFlow = TRUE; dcb.fDtrControl = DTR_CONTROL_HANDSHAKE; break;
    default: break;
    }
    if (!SetCommState(h, &dcb))
    {
        DWORD e = GetLastError();
        m_serial.exchange(0);
        CloseHandle(h);
        PostEvent(SshEventType::Error, "could not configure " + cfg.serialPort +
                                       " (error " + std::to_string(e) + ") - check baud/parity");
        m_running.store(false);
        return;
    }
    // Reads return immediately with whatever is buffered; we poll.
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 0;
    to.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(h, &to);
    SetupComm(h, 65536, 65536);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    PostEvent(SshEventType::Status, cfg.serialPort + " " + std::to_string(cfg.serialBaud) +
                                    " baud");
    PostEvent(SshEventType::Connected);

    std::vector<uint8_t> readBuf(16384), writeBuf;
    std::string closeReason = "port closed";
    while (!m_stop.load())
    {
        bool activity = false;
        DWORD n = 0;
        if (!ReadFile(h, readBuf.data(), static_cast<DWORD>(readBuf.size()), &n, nullptr))
        {
            if (m_stop.load())
                break;
            closeReason = "serial port error (device removed?)";
            goto closed;
        }
        if (n > 0)
        {
            activity = true;
            size_t off = 0;
            while (off < n && !m_stop.load())
            {
                size_t pushed = m_output.Push(readBuf.data() + off, n - off);
                off += pushed;
                if (off < n)
                    Sleep(1);
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_outMutex);
            if (!m_outQueue.empty())
            {
                writeBuf.swap(m_outQueue);
                m_outQueue.clear();
            }
            m_cmdQueue.clear();
        }
        if (!writeBuf.empty())
        {
            DWORD w = 0;
            if (!WriteFile(h, writeBuf.data(), static_cast<DWORD>(writeBuf.size()), &w, nullptr))
            {
                closeReason = "serial write error";
                goto closed;
            }
            writeBuf.clear();
            activity = true;
        }
        if (!activity)
            Sleep(8);
    }
    closeReason = "disconnected";
    m_cleanClose.store(true);
closed:
    {
        uintptr_t hs = m_serial.exchange(0);
        if (hs)
            CloseHandle(reinterpret_cast<HANDLE>(hs));
    }
    PostEvent(SshEventType::Closed, closeReason);
    m_running.store(false);
}
