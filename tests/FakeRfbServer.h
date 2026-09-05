// FakeRfbServer.h — an in-process RFB server for the client tests: it
// speaks each of the three dialects byte for byte, and reads back what the
// client sends so a test can assert on exactly which bytes went out and in
// what order. No sockets: the client under test is fed the server's bytes
// directly and its TakeOutput() is parsed here.
//
// Layouts are RFC 6143's (§7.1 handshake, §7.3 init, §7.5 client messages,
// §7.6 server messages); the version-specific differences are the ones
// vncfree's design notes list as deadlocks for a client that gets them wrong.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "vnc/RfbProtocol.h"

namespace fakerfb
{

using Bytes = std::vector<uint8_t>;

inline void U16(Bytes& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
inline void U32(Bytes& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24)); v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));  v.push_back(static_cast<uint8_t>(x));
}
inline uint16_t R16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t R32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// ---- server → client ------------------------------------------------------------
struct Server
{
    int minor = 8;                 // 3, 7 or 8
    uint16_t width = 64, height = 48;
    std::string name = "fake desktop";
    uint8_t challenge[16] = { 0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

    Bytes Greeting() const
    {
        std::string s = "RFB 003.00" + std::to_string(minor) + "\n";
        return Bytes(s.begin(), s.end());
    }
    // 3.3 states one type in a u32; 3.7+ offers a list
    Bytes SecurityOffer(std::vector<uint8_t> types) const
    {
        Bytes v;
        if (minor == 3)
            U32(v, types.empty() ? 0u : types[0]);
        else
        {
            v.push_back(static_cast<uint8_t>(types.size()));
            v.insert(v.end(), types.begin(), types.end());
        }
        return v;
    }
    Bytes Refusal(const std::string& reason) const
    {
        Bytes v;
        if (minor == 3)
            U32(v, 0);
        else
            v.push_back(0);
        U32(v, static_cast<uint32_t>(reason.size()));
        v.insert(v.end(), reason.begin(), reason.end());
        return v;
    }
    Bytes Challenge() const { return Bytes(challenge, challenge + 16); }
    Bytes SecurityResult(bool ok, const std::string& reason = {}) const
    {
        Bytes v;
        U32(v, ok ? 0u : 1u);
        if (!ok && minor >= 8)
        {
            U32(v, static_cast<uint32_t>(reason.size()));
            v.insert(v.end(), reason.begin(), reason.end());
        }
        return v;
    }
    Bytes ServerInit() const
    {
        Bytes v;
        U16(v, width);
        U16(v, height);
        uint8_t pf[16];
        amber::vnc::EncodePixelFormat(amber::vnc::kRequestedFormat, pf);
        v.insert(v.end(), pf, pf + 16);
        U32(v, static_cast<uint32_t>(name.size()));
        v.insert(v.end(), name.begin(), name.end());
        return v;
    }
    // A FramebufferUpdate header for n rectangles; append rectangles after it.
    static Bytes UpdateHeader(uint16_t n)
    {
        Bytes v = { 0, 0 };
        U16(v, n);
        return v;
    }
    static Bytes RectHeader(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int32_t enc)
    {
        Bytes v;
        U16(v, x); U16(v, y); U16(v, w); U16(v, h);
        U32(v, static_cast<uint32_t>(enc));
        return v;
    }
    static Bytes CutText(const std::string& latin1)
    {
        Bytes v = { 3, 0, 0, 0 };
        U32(v, static_cast<uint32_t>(latin1.size()));
        v.insert(v.end(), latin1.begin(), latin1.end());
        return v;
    }
    static Bytes Bell() { return { 2 }; }
    static Bytes EndOfContinuousUpdates() { return { 150 }; }
    static Bytes ColourMap(uint16_t n)
    {
        Bytes v = { 1, 0 };
        U16(v, 0);
        U16(v, n);
        v.insert(v.end(), static_cast<size_t>(n) * 6, 0);
        return v;
    }
};

// ---- client → server ------------------------------------------------------------
// One parsed client message from the steady-state stream.
struct ClientMessage
{
    uint8_t type = 0;
    // FramebufferUpdateRequest
    bool incremental = false;
    uint16_t x = 0, y = 0, w = 0, h = 0;
    // SetEncodings
    std::vector<int32_t> encodings;
    // KeyEvent / PointerEvent
    bool down = false;
    uint32_t keysym = 0;
    uint8_t buttons = 0;
    // ClientCutText
    std::string text;
    // SetPixelFormat
    amber::vnc::PixelFormat format;
    // EnableContinuousUpdates
    bool enable = false;
};

// Parses steady-state client messages from `out` starting at `off`. Stops
// at the first message it does not understand and reports how far it got.
inline std::vector<ClientMessage> ParseClientMessages(const Bytes& out, size_t& off)
{
    std::vector<ClientMessage> msgs;
    while (off < out.size())
    {
        ClientMessage m;
        m.type = out[off];
        const uint8_t* p = out.data() + off;
        const size_t left = out.size() - off;
        size_t len = 0;
        switch (m.type)
        {
        case amber::vnc::CSetPixelFormat:
            len = 20;
            if (left < len) return msgs;
            m.format = amber::vnc::DecodePixelFormat(p + 4);
            break;
        case amber::vnc::CSetEncodings:
        {
            if (left < 4) return msgs;
            const uint16_t n = R16(p + 2);
            len = 4 + static_cast<size_t>(n) * 4;
            if (left < len) return msgs;
            for (uint16_t i = 0; i < n; ++i)
                m.encodings.push_back(static_cast<int32_t>(R32(p + 4 + i * 4)));
            break;
        }
        case amber::vnc::CFramebufferUpdateRequest:
            len = 10;
            if (left < len) return msgs;
            m.incremental = p[1] != 0;
            m.x = R16(p + 2); m.y = R16(p + 4); m.w = R16(p + 6); m.h = R16(p + 8);
            break;
        case amber::vnc::CKeyEvent:
            len = 8;
            if (left < len) return msgs;
            m.down = p[1] != 0;
            m.keysym = R32(p + 4);
            break;
        case amber::vnc::CPointerEvent:
            len = 6;
            if (left < len) return msgs;
            m.buttons = p[1];
            m.x = R16(p + 2); m.y = R16(p + 4);
            break;
        case amber::vnc::CClientCutText:
        {
            if (left < 8) return msgs;
            const uint32_t n = R32(p + 4);
            len = 8 + n;
            if (left < len) return msgs;
            m.text.assign(reinterpret_cast<const char*>(p + 8), n);
            break;
        }
        case amber::vnc::CEnableContinuousUpdates:
            len = 10;
            if (left < len) return msgs;
            m.enable = p[1] != 0;
            m.x = R16(p + 2); m.y = R16(p + 4); m.w = R16(p + 6); m.h = R16(p + 8);
            break;
        default:
            return msgs;
        }
        off += len;
        msgs.push_back(std::move(m));
    }
    return msgs;
}

} // namespace fakerfb
