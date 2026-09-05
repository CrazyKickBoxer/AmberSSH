// SelfCheckServer.h — an RFB server inside AmberSSH, for --vnc-selfcheck.
//
// The particle desktop's faithful contract is a claim about pixels: at
// solidity 1 and native scale, what the scene holds is what the server
// sent. No VNC server may be installed where the claim is checked, so this
// one serves a known picture on loopback — RFB 3.8, security None, one Raw
// update — and, on request, a second update that recolours one block. The
// app compares the scene target against PixelAt, and the desktop's energy
// texture against the block. It is a test fixture that happens to ship;
// it binds loopback only, on an ephemeral port, for the life of the check.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace amber::vnc
{

class SelfCheckServer
{
public:
    ~SelfCheckServer();
    // Binds 127.0.0.1:0 and starts serving `width` x `height` of the
    // pattern. False when the socket could not be bound.
    bool Start(uint16_t width, uint16_t height);
    void Stop();
    int Port() const { return m_port; }
    uint16_t Width() const { return m_w; }
    uint16_t Height() const { return m_h; }

    // The picture as the client should hold it, 0xFFRRGGBB.
    uint32_t PixelAt(uint32_t x, uint32_t y) const;
    // Recolour a block; the update goes out on the client's next request,
    // or at once if one is outstanding.
    void ChangeBlock(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t colour);

    int Accepted() const { return m_accepted.load(); }
    int UpdatesSent() const { return m_updates.load(); }
    int KeyEvents() const { return m_keys.load(); }
    int PointerEvents() const { return m_pointers.load(); }

private:
    void Serve(uintptr_t sock);
    bool SendUpdate(uintptr_t sock, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    uintptr_t m_listen = ~static_cast<uintptr_t>(0);
    int m_port = 0;
    uint16_t m_w = 0, m_h = 0;
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    mutable std::mutex m_mu;
    std::vector<uint32_t> m_px;
    struct Change { uint16_t x, y, w, h; bool pending = false; } m_change;
    std::atomic<int> m_accepted{ 0 }, m_updates{ 0 }, m_keys{ 0 }, m_pointers{ 0 };
};

} // namespace amber::vnc
