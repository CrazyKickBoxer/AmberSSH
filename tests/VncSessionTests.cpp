// VncSessionTests.cpp — the VNC worker against a real TCP server on
// loopback: the transport, the hand-off to the render side, input going the
// other way, and what happens when the server drops the connection or
// rejects the password.
//
// The fake server here is a thread with a blocking socket and a script per
// connection; it records every client message it reads so the tests can
// assert on what actually crossed the socket. Timeouts are generous so the
// suite is stable on a loaded machine, and short where a wait IS the thing
// under test (backoff).
#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FakeRfbServer.h"
#include "vnc/RfbDes.h"
#include "vnc/VncSession.h"

using namespace amber::vnc;
using fakerfb::Bytes;
using fakerfb::Server;

namespace
{

struct Wsa
{
    Wsa() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
    ~Wsa() { WSACleanup(); }
};

bool ReadExact(SOCKET s, uint8_t* out, size_t n, int timeoutMs = 5000)
{
    size_t got = 0;
    while (got < n)
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        if (select(0, &r, nullptr, nullptr, &tv) <= 0)
            return false;
        const int k = recv(s, reinterpret_cast<char*>(out + got), static_cast<int>(n - got), 0);
        if (k <= 0)
            return false;
        got += static_cast<size_t>(k);
    }
    return true;
}

Bytes Read(SOCKET s, size_t n)
{
    Bytes b(n);
    if (!ReadExact(s, b.data(), n))
        b.clear();
    return b;
}

void Write(SOCKET s, const Bytes& b)
{
    size_t off = 0;
    while (off < b.size())
    {
        const int k = send(s, reinterpret_cast<const char*>(b.data() + off), static_cast<int>(b.size() - off), 0);
        if (k <= 0)
            return;
        off += static_cast<size_t>(k);
    }
}

// Reads one steady-state client message and returns it (type byte first).
Bytes ReadClientMessage(SOCKET s, int timeoutMs = 5000)
{
    uint8_t t;
    if (!ReadExact(s, &t, 1, timeoutMs))
        return {};
    Bytes m = { t };
    auto more = [&](size_t n) {
        Bytes b(n);
        if (!ReadExact(s, b.data(), n, timeoutMs))
            return false;
        m.insert(m.end(), b.begin(), b.end());
        return true;
    };
    switch (t)
    {
    case CSetPixelFormat: if (!more(19)) return {}; break;
    case CSetEncodings:
    {
        if (!more(3)) return {};
        const uint16_t n = fakerfb::R16(&m[2]);
        if (!more(static_cast<size_t>(n) * 4)) return {};
        break;
    }
    case CFramebufferUpdateRequest: if (!more(9)) return {}; break;
    case CKeyEvent: if (!more(7)) return {}; break;
    case CPointerEvent: if (!more(5)) return {}; break;
    case CClientCutText:
    {
        if (!more(7)) return {};
        const uint32_t n = fakerfb::R32(&m[4]);
        if (!more(n)) return {};
        break;
    }
    case CEnableContinuousUpdates: if (!more(9)) return {}; break;
    default: return {};
    }
    return m;
}

// A loopback server: one handler per accepted connection, on its own thread.
struct TcpServer
{
    SOCKET lst = INVALID_SOCKET;
    int port = 0;
    std::thread th;
    std::atomic<int> accepted{ 0 };
    std::function<void(SOCKET, int)> handler;   // socket, connection index

    void Start(std::function<void(SOCKET, int)> h)
    {
        handler = std::move(h);
        lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        REQUIRE(bind(lst, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        REQUIRE(listen(lst, 4) == 0);
        int len = sizeof a;
        getsockname(lst, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] {
            for (;;)
            {
                SOCKET c = accept(lst, nullptr, nullptr);
                if (c == INVALID_SOCKET)
                    return;
                const int idx = accepted.fetch_add(1);
                handler(c, idx);
                closesocket(c);
            }
        });
    }
    void Stop()
    {
        if (lst != INVALID_SOCKET)
        {
            closesocket(lst);
            lst = INVALID_SOCKET;
        }
        if (th.joinable())
            th.join();
    }
    ~TcpServer() { Stop(); }
};

// The 3.8 / None handshake through ServerInit and the client's setup
// messages, then one full Raw update of the given colour. Returns false if
// the client did not follow the script.
bool ServeToFirstPicture(SOCKET c, const Server& s, uint8_t r, uint8_t g, uint8_t b)
{
    Write(c, s.Greeting());
    if (Read(c, 12).empty()) return false;
    Write(c, s.SecurityOffer({ SecNone }));
    Bytes choice = Read(c, 1);
    if (choice != Bytes{ SecNone }) return false;
    Write(c, s.SecurityResult(true));
    if (Read(c, 1).empty()) return false;   // ClientInit
    Write(c, s.ServerInit());
    // SetPixelFormat, SetEncodings, the first FramebufferUpdateRequest
    for (int i = 0; i < 3; ++i)
        if (ReadClientMessage(c).empty()) return false;
    Bytes upd = Server::UpdateHeader(1);
    Bytes rect = Server::RectHeader(0, 0, s.width, s.height, EncRaw);
    for (uint32_t i = 0; i < static_cast<uint32_t>(s.width) * s.height; ++i)
    {
        rect.push_back(b); rect.push_back(g); rect.push_back(r); rect.push_back(0);
    }
    upd.insert(upd.end(), rect.begin(), rect.end());
    Write(c, upd);
    return true;
}

template <typename F>
bool WaitFor(F&& f, int ms = 5000)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until)
    {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

bool WaitForState(VncSession& v, VncState s, int ms = 5000)
{
    return WaitFor([&] { return v.State() == s; }, ms);
}

} // namespace

TEST_CASE("the worker connects, hands over the first picture, and input reaches the server", "[vnc][session]")
{
    Wsa wsa;
    Server s;
    std::mutex mu;
    std::vector<Bytes> received;
    TcpServer srv;
    srv.Start([&](SOCKET c, int) {
        if (!ServeToFirstPicture(c, s, 200, 100, 50))
            return;
        for (;;)
        {
            Bytes m = ReadClientMessage(c, 3000);
            if (m.empty())
                return;
            std::lock_guard<std::mutex> lk(mu);
            received.push_back(std::move(m));
        }
    });

    VncSession v;
    VncConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port;
    REQUIRE(v.Start(cfg));
    REQUIRE(WaitForState(v, VncState::Connected));

    Damage d;
    REQUIRE(WaitFor([&] { return v.TakeDamage(d); }));
    CHECK(d.width == 64);
    CHECK(d.height == 48);
    CHECK(d.full);
    REQUIRE(d.rects.size() == 1);
    REQUIRE(d.pixels.size() == 64u * 48u);
    CHECK(d.pixels[0] == 0xFFC86432u);
    CHECK(d.pixels.back() == 0xFFC86432u);

    v.SendKey(true, 'q');
    v.SendPointer(kButtonLeft, 10, 20);
    v.SendCutText("clip");
    REQUIRE(WaitFor([&] { std::lock_guard<std::mutex> lk(mu); return received.size() >= 3; }));
    {
        std::lock_guard<std::mutex> lk(mu);
        // the incremental request pipelined at the update's start comes first
        size_t i = 0;
        while (i < received.size() && received[i][0] == CFramebufferUpdateRequest)
            ++i;
        REQUIRE(received.size() >= i + 3);
        CHECK(received[i][0] == CKeyEvent);
        CHECK(fakerfb::R32(&received[i][4]) == 'q');
        CHECK(received[i + 1][0] == CPointerEvent);
        CHECK(received[i + 1][1] == kButtonLeft);
        CHECK(received[i + 2][0] == CClientCutText);
        CHECK(std::string(received[i + 2].begin() + 8, received[i + 2].end()) == "clip");
    }

    const VncStats st = v.GetStats();
    CHECK(st.bytesIn > 0);
    CHECK(st.bytesOut > 0);
    CHECK(st.fbWidth == 64);
    CHECK(st.rfbMinor == 8);
    CHECK(st.updates >= 1);

    VncEvent ev;
    bool connected = false;
    while (v.PollEvent(ev))
        if (ev.type == VncEvent::Type::Connected && ev.text == "fake desktop")
            connected = true;
    CHECK(connected);

    v.Disconnect();
    CHECK_FALSE(v.Running());
    CHECK(v.State() == VncState::Disconnected);
}

TEST_CASE("view-only sends nothing", "[vnc][session]")
{
    Wsa wsa;
    Server s;
    std::atomic<int> inputs{ 0 };
    TcpServer srv;
    srv.Start([&](SOCKET c, int) {
        if (!ServeToFirstPicture(c, s, 1, 2, 3))
            return;
        for (;;)
        {
            Bytes m = ReadClientMessage(c, 1500);
            if (m.empty())
                return;
            if (m[0] == CKeyEvent || m[0] == CPointerEvent || m[0] == CClientCutText)
                inputs.fetch_add(1);
        }
    });
    VncSession v;
    VncConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port;
    cfg.viewOnly = true;
    REQUIRE(v.Start(cfg));
    REQUIRE(WaitForState(v, VncState::Connected));
    v.SendKey(true, 'x');
    v.SendPointer(kButtonLeft, 1, 1);
    v.SendCutText("no");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(inputs.load() == 0);
    CHECK(v.ViewOnly());
    v.Disconnect();
}

TEST_CASE("a dropped connection reconnects with a fresh full picture", "[vnc][session]")
{
    Wsa wsa;
    Server s;
    TcpServer srv;
    srv.Start([&](SOCKET c, int idx) {
        // first connection: one picture, then hang up; second: stay up
        if (!ServeToFirstPicture(c, s, idx == 0 ? 10 : 20, 0, 0))
            return;
        if (idx == 0)
            return;
        // the second connection stays up until the test disconnects: an
        // idle timeout here would manufacture a third connection
        for (;;)
        {
            if (ReadClientMessage(c, 30000).empty())
                return;
        }
    });
    VncSession v;
    VncConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port;
    REQUIRE(v.Start(cfg));
    // The first connection lives for milliseconds — picture, then FIN — so
    // its Connected state is not something a poll can be relied on to see.
    // The picture is what it left behind; wait for that.
    Damage d;
    REQUIRE(WaitFor([&] { return v.TakeDamage(d); }));
    CHECK(d.full);
    CHECK(d.pixels[0] == 0xFF0A0000u);

    // the server hung up: reconnecting, then connected again with a new picture
    REQUIRE(WaitFor([&] { return srv.accepted.load() >= 2; }, 8000));
    Damage d2;
    REQUIRE(WaitFor([&] { return v.TakeDamage(d2); }, 8000));
    CHECK(d2.full);
    CHECK(d2.pixels[0] == 0xFF140000u);
    CHECK(WaitForState(v, VncState::Connected, 2000));
    CHECK(v.GetStats().reconnects == 1);
    v.Disconnect();
}

TEST_CASE("a rejected password is reported once and never retried", "[vnc][session]")
{
    Wsa wsa;
    Server s;
    TcpServer srv;
    srv.Start([&](SOCKET c, int) {
        Write(c, s.Greeting());
        Read(c, 12);
        Write(c, s.SecurityOffer({ SecVncAuth }));
        Read(c, 1);
        Write(c, s.Challenge());
        Read(c, 16);
        Write(c, s.SecurityResult(false, "Authentication failure"));
    });
    VncSession v;
    VncConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port;
    cfg.password = "nope";
    cfg.maxReconnects = 3;
    REQUIRE(v.Start(cfg));
    REQUIRE(WaitForState(v, VncState::Error));
    VncEvent ev;
    bool authFailed = false;
    while (v.PollEvent(ev))
        if (ev.type == VncEvent::Type::AuthFailed && ev.text == "Authentication failure")
            authFailed = true;
    CHECK(authFailed);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(srv.accepted.load() == 1);   // no second attempt with the same password
    CHECK_FALSE(v.Running());
}

TEST_CASE("a refused connection retries a bounded number of times, then stops", "[vnc][session]")
{
    Wsa wsa;
    // a port nothing listens on: bind one, then close it
    SOCKET tmp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(tmp, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    int len = sizeof a;
    getsockname(tmp, reinterpret_cast<sockaddr*>(&a), &len);
    const int deadPort = ntohs(a.sin_port);
    closesocket(tmp);

    VncSession v;
    VncConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = deadPort;
    cfg.maxReconnects = 1;
    cfg.connectTimeoutMs = 1000;
    REQUIRE(v.Start(cfg));
    REQUIRE(WaitForState(v, VncState::Error, 10000));
    CHECK(v.GetStats().reconnects == 1);
    VncEvent ev;
    bool gaveUp = false;
    while (v.PollEvent(ev))
        if (ev.type == VncEvent::Type::Error && ev.text.find("giving up") != std::string::npos)
            gaveUp = true;
    CHECK(gaveUp);
}

TEST_CASE("the tunnel path asks for a forward and connects to the port it is told", "[vnc][session]")
{
    Wsa wsa;
    Server s;
    TcpServer srv;   // stands in for the SSH session's loopback listener
    srv.Start([&](SOCKET c, int) {
        if (!ServeToFirstPicture(c, s, 5, 5, 5))
            return;
        for (;;)
            if (ReadClientMessage(c, 3000).empty())
                return;
    });
    VncSession v;
    VncConfig cfg;
    cfg.host = "desktop.internal";   // never resolved: the tunnel is the transport
    cfg.port = 5900;
    std::string askedSpec;
    cfg.addForward = [&](const std::string& spec) {
        askedSpec = spec;
        // what the SSH session would post as ForwardUp once bound
        v.OnForwardUp(std::to_string(srv.port) + ":desktop.internal:5900");
    };
    REQUIRE(v.Start(cfg));
    REQUIRE(WaitForState(v, VncState::Connected));
    CHECK(askedSpec == "L0:desktop.internal:5900");
    CHECK(v.TunnelSpec() == askedSpec);
    v.Disconnect();
}

TEST_CASE("a forward-up for some other tunnel is ignored", "[vnc][session]")
{
    Wsa wsa;
    VncSession v;
    VncConfig cfg;
    cfg.host = "a";
    cfg.port = 1;
    cfg.tunnelTimeoutMs = 300;
    cfg.maxReconnects = 0;
    cfg.addForward = [&](const std::string&) { v.OnForwardUp("4444:b:2"); };
    REQUIRE(v.Start(cfg));
    REQUIRE(WaitForState(v, VncState::Error, 5000));
    VncEvent ev;
    bool noTunnel = false;
    while (v.PollEvent(ev))
        if (ev.type == VncEvent::Type::Error && ev.text.find("tunnel") != std::string::npos)
            noTunnel = true;
    CHECK(noTunnel);
}
