// SelfCheckServer.cpp — see the header. The wire layouts are RFC 6143's,
// written out here rather than through RfbProtocol so that the server side
// of the check does not share code with the client it checks.
#include "SelfCheckServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>

namespace amber::vnc
{
namespace
{

using Bytes = std::vector<uint8_t>;

void U16(Bytes& v, uint16_t x) { v.push_back(static_cast<uint8_t>(x >> 8)); v.push_back(static_cast<uint8_t>(x)); }
void U32(Bytes& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24)); v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));  v.push_back(static_cast<uint8_t>(x));
}
uint16_t R16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t R32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool WriteAll(SOCKET s, const Bytes& b)
{
    size_t off = 0;
    while (off < b.size())
    {
        const int k = send(s, reinterpret_cast<const char*>(b.data() + off), static_cast<int>(b.size() - off), 0);
        if (k <= 0)
            return false;
        off += static_cast<size_t>(k);
    }
    return true;
}

// Exactly n bytes, or false; `stop` is polled between waits.
bool ReadExact(SOCKET s, uint8_t* out, size_t n, const std::atomic<bool>& stop)
{
    size_t got = 0;
    while (got < n)
    {
        if (stop.load())
            return false;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{ 0, 100000 };
        const int sel = select(0, &r, nullptr, nullptr, &tv);
        if (sel < 0)
            return false;
        if (sel == 0)
            continue;
        const int k = recv(s, reinterpret_cast<char*>(out + got), static_cast<int>(n - got), 0);
        if (k <= 0)
            return false;
        got += static_cast<size_t>(k);
    }
    return true;
}

// The pattern: a red/green gradient with a blue checker, and a white
// two-pixel border so the edges are checked too. Every colour distinct
// enough that a one-pixel offset is a visible mismatch.
uint32_t Pattern(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (x < 2 || y < 2 || x + 2 >= w || y + 2 >= h)
        return 0xFFFFFFFFu;
    const uint32_t r = w > 1 ? x * 255u / (w - 1) : 0;
    const uint32_t g = h > 1 ? y * 255u / (h - 1) : 0;
    const uint32_t b = (((x / 16u) + (y / 16u)) & 1u) ? 200u : 40u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

} // namespace

SelfCheckServer::~SelfCheckServer()
{
    Stop();
}

bool SelfCheckServer::Start(uint16_t width, uint16_t height)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    m_w = width;
    m_h = height;
    m_px.resize(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            m_px[static_cast<size_t>(y) * width + x] = Pattern(x, y, width, height);

    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET)
        return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(l, 2) != 0)
    {
        closesocket(l);
        return false;
    }
    int len = sizeof a;
    getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
    m_port = ntohs(a.sin_port);
    m_listen = static_cast<uintptr_t>(l);
    m_stop.store(false);
    m_thread = std::thread([this] {
        for (;;)
        {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(static_cast<SOCKET>(m_listen), &r);
            timeval tv{ 0, 100000 };
            const int sel = select(0, &r, nullptr, nullptr, &tv);
            if (m_stop.load())
                return;
            if (sel <= 0)
                continue;
            SOCKET c = accept(static_cast<SOCKET>(m_listen), nullptr, nullptr);
            if (c == INVALID_SOCKET)
                return;
            m_accepted.fetch_add(1);
            Serve(static_cast<uintptr_t>(c));
            closesocket(c);
        }
    });
    return true;
}

void SelfCheckServer::Stop()
{
    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();
    if (m_listen != ~static_cast<uintptr_t>(0))
    {
        closesocket(static_cast<SOCKET>(m_listen));
        m_listen = ~static_cast<uintptr_t>(0);
    }
}

uint32_t SelfCheckServer::PixelAt(uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (x >= m_w || y >= m_h)
        return 0;
    return m_px[static_cast<size_t>(y) * m_w + x];
}

void SelfCheckServer::ChangeBlock(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t colour)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (uint32_t yy = y; yy < static_cast<uint32_t>(y) + h && yy < m_h; ++yy)
        for (uint32_t xx = x; xx < static_cast<uint32_t>(x) + w && xx < m_w; ++xx)
            m_px[static_cast<size_t>(yy) * m_w + xx] = 0xFF000000u | (colour & 0x00FFFFFFu);
    m_change = { x, y, w, h, true };
}

bool SelfCheckServer::SendUpdate(uintptr_t sock, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    Bytes u = { 0, 0 };
    U16(u, 1);
    U16(u, x); U16(u, y); U16(u, w); U16(u, h);
    U32(u, 0);   // Raw
    {
        std::lock_guard<std::mutex> lk(m_mu);
        for (uint32_t yy = y; yy < static_cast<uint32_t>(y) + h; ++yy)
            for (uint32_t xx = x; xx < static_cast<uint32_t>(x) + w; ++xx)
            {
                const uint32_t p = m_px[static_cast<size_t>(yy) * m_w + xx];
                u.push_back(static_cast<uint8_t>(p));         // B
                u.push_back(static_cast<uint8_t>(p >> 8));    // G
                u.push_back(static_cast<uint8_t>(p >> 16));   // R
                u.push_back(0);
            }
    }
    m_updates.fetch_add(1);
    return WriteAll(static_cast<SOCKET>(sock), u);
}

void SelfCheckServer::Serve(uintptr_t sockv)
{
    const SOCKET s = static_cast<SOCKET>(sockv);
    uint8_t buf[64];
    // version
    WriteAll(s, Bytes{ 'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '8', '\n' });
    if (!ReadExact(s, buf, 12, m_stop))
        return;
    // security: None only
    WriteAll(s, Bytes{ 1, 1 });
    if (!ReadExact(s, buf, 1, m_stop) || buf[0] != 1)
        return;
    WriteAll(s, Bytes{ 0, 0, 0, 0 });
    if (!ReadExact(s, buf, 1, m_stop))   // ClientInit
        return;
    Bytes init;
    U16(init, m_w);
    U16(init, m_h);
    const uint8_t pf[16] = { 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0 };
    init.insert(init.end(), pf, pf + 16);
    const char* name = "AmberSSH self-check";
    U32(init, static_cast<uint32_t>(strlen(name)));
    init.insert(init.end(), name, name + strlen(name));
    WriteAll(s, init);

    int outstanding = 0;
    bool sentFull = false;
    for (;;)
    {
        // a pending change goes out the moment a request is outstanding
        bool pending;
        Change ch;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            pending = m_change.pending;
            ch = m_change;
        }
        if (pending && outstanding > 0)
        {
            {
                std::lock_guard<std::mutex> lk(m_mu);
                m_change.pending = false;
            }
            if (!SendUpdate(sockv, ch.x, ch.y, ch.w, ch.h))
                return;
            outstanding = 0;
        }

        // one client message
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{ 0, 50000 };
        const int sel = select(0, &r, nullptr, nullptr, &tv);
        if (m_stop.load())
            return;
        if (sel < 0)
            return;
        if (sel == 0)
            continue;
        if (!ReadExact(s, buf, 1, m_stop))
            return;
        switch (buf[0])
        {
        case 0:   // SetPixelFormat
            if (!ReadExact(s, buf, 19, m_stop)) return;
            break;
        case 2:   // SetEncodings
        {
            if (!ReadExact(s, buf, 3, m_stop)) return;
            const uint16_t n = R16(buf + 1);
            std::vector<uint8_t> enc(static_cast<size_t>(n) * 4);
            if (n && !ReadExact(s, enc.data(), enc.size(), m_stop)) return;
            break;
        }
        case 3:   // FramebufferUpdateRequest
        {
            if (!ReadExact(s, buf, 9, m_stop)) return;
            const bool incremental = buf[0] != 0;
            if (!incremental || !sentFull)
            {
                if (!SendUpdate(sockv, 0, 0, m_w, m_h))
                    return;
                sentFull = true;
                outstanding = 0;
            }
            else
                ++outstanding;
            break;
        }
        case 4:   // KeyEvent
            if (!ReadExact(s, buf, 7, m_stop)) return;
            m_keys.fetch_add(1);
            break;
        case 5:   // PointerEvent
            if (!ReadExact(s, buf, 5, m_stop)) return;
            m_pointers.fetch_add(1);
            break;
        case 6:   // ClientCutText
        {
            if (!ReadExact(s, buf, 7, m_stop)) return;
            const uint32_t n = R32(buf + 3);
            std::vector<uint8_t> t(n);
            if (n && !ReadExact(s, t.data(), n, m_stop)) return;
            break;
        }
        case 150: // EnableContinuousUpdates: never offered by this server; consume
            if (!ReadExact(s, buf, 9, m_stop)) return;
            break;
        default:
            return;
        }
    }
}

} // namespace amber::vnc
