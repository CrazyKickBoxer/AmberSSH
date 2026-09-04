#include "Protocol.h"

#include <algorithm>
#include <cstring>

namespace amber::amberx
{

namespace
{

void PutU16(std::vector<uint8_t>& v, uint16_t n)
{
    v.push_back(static_cast<uint8_t>(n & 0xFF));
    v.push_back(static_cast<uint8_t>(n >> 8));
}

void PutU32(std::vector<uint8_t>& v, uint32_t n)
{
    v.push_back(static_cast<uint8_t>(n & 0xFF));
    v.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
}

uint16_t GetU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t GetU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

bool KnownType(uint16_t t)
{
    switch (static_cast<MsgType>(t))
    {
    case MsgType::Hello:
    case MsgType::HelloAck:
    case MsgType::AuthProof:
    case MsgType::SetCookie:
    case MsgType::Shutdown:
    case MsgType::ClipboardText:
    case MsgType::WindowAction:
    case MsgType::HostReport:
    case MsgType::ChannelOpen:
    case MsgType::ChannelData:
    case MsgType::ChannelClose:
    case MsgType::HostStatus:
    case MsgType::HostError:
        return true;
    }
    return false;
}

bool IsControlOnly(MsgType t)
{
    switch (t)
    {
    case MsgType::Hello:
    case MsgType::HelloAck:
    case MsgType::AuthProof:
    case MsgType::SetCookie:
    case MsgType::Shutdown:
    case MsgType::ClipboardText:
    case MsgType::WindowAction:
    case MsgType::HostReport:
    case MsgType::HostStatus:
    case MsgType::HostError:
        return true;
    default:
        return false;
    }
}

bool IsChannelOnly(MsgType t)
{
    switch (t)
    {
    case MsgType::ChannelOpen:
    case MsgType::ChannelData:
    case MsgType::ChannelClose:
        return true;
    default:
        return false;
    }
}

bool Encode(const Frame& f, std::vector<uint8_t>& out)
{
    if (f.payload.size() > kMaxPayload)
        return false;
    if (!KnownType(static_cast<uint16_t>(f.type)))
        return false;
    // A cookie install addressed to a data channel, or X11 bytes addressed to
    // the control channel, is a bug on this side. Refuse to put it on the wire
    // rather than making the far end decide.
    if (IsControlOnly(f.type) && f.channel != kControlChannel)
        return false;
    if (IsChannelOnly(f.type) && f.channel == kControlChannel)
        return false;

    out.clear();
    out.reserve(kHeaderBytes + f.payload.size());
    PutU32(out, kMagic);
    PutU16(out, f.version);
    PutU16(out, static_cast<uint16_t>(f.type));
    PutU32(out, f.channel);
    PutU32(out, static_cast<uint32_t>(f.payload.size()));
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    return true;
}

Decoded Decode(const std::vector<uint8_t>& in, Frame& frame, size_t& consumed)
{
    consumed = 0;
    if (in.size() < kHeaderBytes)
        return Decoded::NeedMore;

    const uint8_t* p = in.data();
    if (GetU32(p) != kMagic)
        return Decoded::Bad;

    const uint16_t version = GetU16(p + 4);
    const uint16_t type = GetU16(p + 6);
    const uint32_t channel = GetU32(p + 8);
    const uint32_t length = GetU32(p + 12);

    // Everything below is checked BEFORE the payload is touched or any space
    // is reserved for it. The length in particular: a frame claiming 4 GiB
    // must cost nothing to reject.
    if (version != kVersion)
        return Decoded::Bad;
    if (!KnownType(type))
        return Decoded::Bad;
    if (length > kMaxPayload)
        return Decoded::Bad;

    const MsgType mt = static_cast<MsgType>(type);
    if (IsControlOnly(mt) && channel != kControlChannel)
        return Decoded::Bad;
    if (IsChannelOnly(mt) && channel == kControlChannel)
        return Decoded::Bad;

    // Only now, with the length known sane, do we ask whether it has arrived.
    if (in.size() < kHeaderBytes + length)
        return Decoded::NeedMore;

    frame.version = version;
    frame.type = mt;
    frame.channel = channel;
    frame.payload.assign(in.begin() + static_cast<ptrdiff_t>(kHeaderBytes),
                         in.begin() + static_cast<ptrdiff_t>(kHeaderBytes + length));
    consumed = kHeaderBytes + length;
    return Decoded::Ok;
}

bool ConstantTimeEqual(const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b)
{
    // Length is not secret; contents are. Comparing every byte regardless of
    // an early mismatch keeps the time independent of where they differ. Two
    // empty values are not equal — a missing nonce must never authenticate.
    if (a.size() != b.size() || a.empty())
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

std::vector<uint8_t> MakeHello(const std::vector<uint8_t>& nonce)
{
    if (nonce.size() != kNonceBytes)
        return {};
    return nonce;
}

std::vector<uint8_t> MakeHelloAck(const std::vector<uint8_t>& echo,
                                  const std::vector<uint8_t>& own,
                                  const std::vector<uint8_t>& proof)
{
    if (echo.size() != kNonceBytes || own.size() != kNonceBytes ||
        proof.size() != kProofBytes)
        return {};
    std::vector<uint8_t> out;
    out.reserve(kNonceBytes * 2 + kProofBytes);
    out.insert(out.end(), echo.begin(), echo.end());
    out.insert(out.end(), own.begin(), own.end());
    out.insert(out.end(), proof.begin(), proof.end());
    return out;
}

std::vector<uint8_t> MakeAuthProof(const std::vector<uint8_t>& proof)
{
    if (proof.size() != kProofBytes)
        return {};
    return proof;
}

std::vector<uint8_t> MakeSetCookie(const std::vector<uint8_t>& cookie,
                                   uint32_t display)
{
    // 16 bytes is the only MIT-MAGIC-COOKIE-1 size AmberSSH generates or
    // accepts; anything else is a caller bug rather than a short cookie.
    if (cookie.size() != 16)
        return {};
    std::vector<uint8_t> out;
    PutU32(out, display);
    out.insert(out.end(), cookie.begin(), cookie.end());
    return out;
}

bool ParseHello(const std::vector<uint8_t>& p, std::vector<uint8_t>& nonce)
{
    if (p.size() != kNonceBytes)
        return false;      // exact length or nothing
    nonce = p;
    return true;
}

bool ParseHelloAck(const std::vector<uint8_t>& p, std::vector<uint8_t>& echo,
                   std::vector<uint8_t>& own, std::vector<uint8_t>& proof)
{
    if (p.size() != kNonceBytes * 2 + kProofBytes)
        return false;
    auto at = [&](size_t i) { return p.begin() + static_cast<ptrdiff_t>(i); };
    echo.assign(at(0), at(kNonceBytes));
    own.assign(at(kNonceBytes), at(kNonceBytes * 2));
    proof.assign(at(kNonceBytes * 2), p.end());
    return true;
}

bool ParseAuthProof(const std::vector<uint8_t>& p, std::vector<uint8_t>& proof)
{
    if (p.size() != kProofBytes)
        return false;
    proof = p;
    return true;
}

bool ParseSetCookie(const std::vector<uint8_t>& p, std::vector<uint8_t>& cookie,
                    uint32_t& display)
{
    if (p.size() != 4 + 16)
        return false;
    display = GetU32(p.data());
    cookie.assign(p.begin() + 4, p.end());
    return true;
}

std::vector<uint8_t> MakeHostStatus(uint32_t openChannels, uint64_t bytesIn,
                                    bool cookieSet)
{
    std::vector<uint8_t> out;
    PutU32(out, openChannels);
    PutU32(out, static_cast<uint32_t>(bytesIn & 0xFFFFFFFFu));
    PutU32(out, static_cast<uint32_t>(bytesIn >> 32));
    PutU32(out, cookieSet ? 1u : 0u);
    return out;
}

bool ParseHostStatus(const std::vector<uint8_t>& p, uint32_t& openChannels,
                     uint64_t& bytesIn, bool& cookieSet)
{
    if (p.size() != 16)
        return false;
    openChannels = GetU32(p.data());
    bytesIn = static_cast<uint64_t>(GetU32(p.data() + 4)) |
              (static_cast<uint64_t>(GetU32(p.data() + 8)) << 32);
    cookieSet = GetU32(p.data() + 12) != 0;
    return true;
}

std::vector<uint8_t> MakeHostReport(const HostReport& r)
{
    std::vector<uint8_t> out;
    PutU32(out, 1);                                     // layout version
    PutU32(out, r.clients);
    PutU32(out, r.windows);
    PutU32(out, static_cast<uint32_t>(r.pixmapBytes & 0xFFFFFFFFu));
    PutU32(out, static_cast<uint32_t>(r.pixmapBytes >> 32));
    PutU32(out, static_cast<uint32_t>(r.x11In & 0xFFFFFFFFu));
    PutU32(out, static_cast<uint32_t>(r.x11In >> 32));
    PutU32(out, static_cast<uint32_t>(r.x11Out & 0xFFFFFFFFu));
    PutU32(out, static_cast<uint32_t>(r.x11Out >> 32));
    PutU32(out, r.presents);
    PutU32(out, r.dirtyRects);
    PutU32(out, r.ipcHighWater);
    PutU32(out, r.rejected);
    const uint32_t n = static_cast<uint32_t>(
        r.windowList.size() > kMaxReportWindows ? kMaxReportWindows : r.windowList.size());
    PutU32(out, n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const ReportWindow& w = r.windowList[i];
        PutU32(out, w.xid);
        PutU32(out, w.flags);
        const uint32_t len = static_cast<uint32_t>(
            w.title.size() > kMaxWindowTitle ? kMaxWindowTitle : w.title.size());
        PutU32(out, len);
        out.insert(out.end(), w.title.begin(), w.title.begin() + static_cast<ptrdiff_t>(len));
    }
    return out;
}

bool ParseHostReport(const std::vector<uint8_t>& p, HostReport& r)
{
    // Fixed head first, then a counted list. Every length is checked against
    // what is actually left in the buffer before it is used, and the counts
    // are bounded before anything is reserved.
    constexpr size_t kHead = 14 * 4;
    if (p.size() < kHead)
        return false;
    if (GetU32(p.data()) != 1)
        return false;
    r = HostReport{};
    r.clients = GetU32(p.data() + 4);
    r.windows = GetU32(p.data() + 8);
    r.pixmapBytes = static_cast<uint64_t>(GetU32(p.data() + 12)) |
                    (static_cast<uint64_t>(GetU32(p.data() + 16)) << 32);
    r.x11In = static_cast<uint64_t>(GetU32(p.data() + 20)) |
              (static_cast<uint64_t>(GetU32(p.data() + 24)) << 32);
    r.x11Out = static_cast<uint64_t>(GetU32(p.data() + 28)) |
               (static_cast<uint64_t>(GetU32(p.data() + 32)) << 32);
    r.presents = GetU32(p.data() + 36);
    r.dirtyRects = GetU32(p.data() + 40);
    r.ipcHighWater = GetU32(p.data() + 44);
    r.rejected = GetU32(p.data() + 48);
    const uint32_t n = GetU32(p.data() + 52);
    if (n > kMaxReportWindows)
        return false;

    size_t at = kHead;
    r.windowList.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (p.size() - at < 12)
            return false;
        ReportWindow w;
        w.xid = GetU32(p.data() + at);
        w.flags = GetU32(p.data() + at + 4);
        const uint32_t len = GetU32(p.data() + at + 8);
        at += 12;
        if (len > kMaxWindowTitle || p.size() - at < len)
            return false;
        w.title.assign(reinterpret_cast<const char*>(p.data() + at), len);
        at += len;
        r.windowList.push_back(std::move(w));
    }
    return at == p.size();
}

std::vector<uint8_t> MakeWindowAction(uint32_t xid, WindowAct act)
{
    std::vector<uint8_t> out;
    PutU32(out, xid);
    PutU32(out, static_cast<uint32_t>(act));
    return out;
}

bool ParseWindowAction(const std::vector<uint8_t>& p, uint32_t& xid, WindowAct& act)
{
    if (p.size() != 8)
        return false;
    const uint32_t a = GetU32(p.data() + 4);
    if (a > static_cast<uint32_t>(WindowAct::Close))
        return false;
    xid = GetU32(p.data());
    act = static_cast<WindowAct>(a);
    return true;
}

std::vector<uint8_t> MakeHostError(const std::string& text)
{
    // Truncated rather than refused: an error the host could not report is
    // worse than a shortened one. Never contains a payload echo — the caller
    // passes a fixed description, not the bytes that caused it.
    const size_t n = text.size() > kMaxErrorBytes ? kMaxErrorBytes : text.size();
    return std::vector<uint8_t>(text.begin(), text.begin() + static_cast<ptrdiff_t>(n));
}

bool ParseHostError(const std::vector<uint8_t>& p, std::string& text)
{
    if (p.size() > kMaxErrorBytes)
        return false;
    text.assign(p.begin(), p.end());
    // Control characters are stripped: this string ends up in a status bar,
    // and a host must not be able to inject escape sequences into it.
    for (char& c : text)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            c = ' ';
    return true;
}

// ------------------------------------------------------------ channel state
bool ChannelTable::Open(uint32_t id)
{
    if (id == kControlChannel)
        return false;                       // 0 is never a data channel
    if (m_open.size() >= kMaxChannels)
        return false;                       // a cap, not a suggestion
    if (IsOpen(id))
        return false;                       // reopening is a protocol error
    m_open.push_back(id);
    return true;
}

bool ChannelTable::IsOpen(uint32_t id) const
{
    return std::find(m_open.begin(), m_open.end(), id) != m_open.end();
}

bool ChannelTable::Close(uint32_t id)
{
    auto it = std::find(m_open.begin(), m_open.end(), id);
    if (it == m_open.end())
        return false;
    m_open.erase(it);
    return true;
}

} // namespace amber::amberx
