// RfbProtocol.cpp — RFC 6143 on the wire. See RfbProtocol.h.
#include "RfbProtocol.h"

#include <cstring>

namespace amber::vnc
{
namespace
{

void Put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

void Put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

} // namespace

// ---- pixel format -------------------------------------------------------------
bool operator==(const PixelFormat& a, const PixelFormat& b)
{
    return a.bitsPerPixel == b.bitsPerPixel && a.depth == b.depth && a.bigEndian == b.bigEndian &&
           a.trueColour == b.trueColour && a.redMax == b.redMax && a.greenMax == b.greenMax &&
           a.blueMax == b.blueMax && a.redShift == b.redShift && a.greenShift == b.greenShift &&
           a.blueShift == b.blueShift;
}

void EncodePixelFormat(const PixelFormat& f, uint8_t out[16])
{
    out[0] = f.bitsPerPixel;
    out[1] = f.depth;
    out[2] = f.bigEndian ? 1 : 0;
    out[3] = f.trueColour ? 1 : 0;
    out[4] = static_cast<uint8_t>(f.redMax >> 8);
    out[5] = static_cast<uint8_t>(f.redMax & 0xFF);
    out[6] = static_cast<uint8_t>(f.greenMax >> 8);
    out[7] = static_cast<uint8_t>(f.greenMax & 0xFF);
    out[8] = static_cast<uint8_t>(f.blueMax >> 8);
    out[9] = static_cast<uint8_t>(f.blueMax & 0xFF);
    out[10] = f.redShift;
    out[11] = f.greenShift;
    out[12] = f.blueShift;
    out[13] = out[14] = out[15] = 0;   // padding
}

PixelFormat DecodePixelFormat(const uint8_t in[16])
{
    PixelFormat f;
    f.bitsPerPixel = in[0];
    f.depth = in[1];
    f.bigEndian = in[2] ? 1 : 0;
    f.trueColour = in[3] ? 1 : 0;
    f.redMax = static_cast<uint16_t>((in[4] << 8) | in[5]);
    f.greenMax = static_cast<uint16_t>((in[6] << 8) | in[7]);
    f.blueMax = static_cast<uint16_t>((in[8] << 8) | in[9]);
    f.redShift = in[10];
    f.greenShift = in[11];
    f.blueShift = in[12];
    return f;
}

// ---- version -------------------------------------------------------------------
bool ParseVersion(const uint8_t in[12], Version& v)
{
    // "RFB xxx.yyy\n", digits only
    if (std::memcmp(in, "RFB ", 4) != 0 || in[7] != '.' || in[11] != '\n')
        return false;
    int major = 0, minor = 0;
    for (int i = 4; i < 7; ++i)
    {
        if (in[i] < '0' || in[i] > '9')
            return false;
        major = major * 10 + (in[i] - '0');
    }
    for (int i = 8; i < 11; ++i)
    {
        if (in[i] < '0' || in[i] > '9')
            return false;
        minor = minor * 10 + (in[i] - '0');
    }
    v.major = major;
    v.minor = minor;
    return true;
}

int ChooseMinor(const Version& server)
{
    if (server.major != 3 || server.minor < 3)
        return -1;
    if (server.minor >= 8)
        return 8;
    if (server.minor >= 7)
        return 7;
    return 3;   // 3.3 .. 3.6: everything below 3.7 is answered as 3.3 (§7.1.1)
}

std::array<uint8_t, 12> VersionReply(int minor)
{
    std::array<uint8_t, 12> out = { 'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '0', '\n' };
    out[10] = static_cast<uint8_t>('0' + (minor % 10));
    return out;
}

// ---- reader --------------------------------------------------------------------
bool Reader::Need(size_t n)
{
    if (m_n - m_off < n)
    {
        m_short = true;
        return false;
    }
    return true;
}

bool Reader::U8(uint8_t& v)
{
    if (!Need(1))
        return false;
    v = m_p[m_off++];
    return true;
}

bool Reader::U16(uint16_t& v)
{
    if (!Need(2))
        return false;
    v = static_cast<uint16_t>((m_p[m_off] << 8) | m_p[m_off + 1]);
    m_off += 2;
    return true;
}

bool Reader::U32(uint32_t& v)
{
    if (!Need(4))
        return false;
    v = (static_cast<uint32_t>(m_p[m_off]) << 24) | (static_cast<uint32_t>(m_p[m_off + 1]) << 16) |
        (static_cast<uint32_t>(m_p[m_off + 2]) << 8) | static_cast<uint32_t>(m_p[m_off + 3]);
    m_off += 4;
    return true;
}

bool Reader::I32(int32_t& v)
{
    uint32_t u;
    if (!U32(u))
        return false;
    v = static_cast<int32_t>(u);
    return true;
}

bool Reader::Bytes(void* out, size_t n)
{
    if (!Need(n))
        return false;
    std::memcpy(out, m_p + m_off, n);
    m_off += n;
    return true;
}

bool Reader::Skip(size_t n)
{
    if (!Need(n))
        return false;
    m_off += n;
    return true;
}

const uint8_t* Reader::Peek(size_t n) const
{
    return (m_n - m_off >= n) ? m_p + m_off : nullptr;
}

// ---- client messages --------------------------------------------------------
std::vector<uint8_t> MsgClientInit(bool shared)
{
    return { static_cast<uint8_t>(shared ? 1 : 0) };
}

std::vector<uint8_t> MsgSetPixelFormat(const PixelFormat& f)
{
    std::vector<uint8_t> m = { CSetPixelFormat, 0, 0, 0 };
    uint8_t pf[16];
    EncodePixelFormat(f, pf);
    m.insert(m.end(), pf, pf + 16);
    return m;
}

std::vector<uint8_t> MsgSetEncodings(const std::vector<int32_t>& encodings)
{
    std::vector<uint8_t> m = { CSetEncodings, 0 };
    Put16(m, static_cast<uint16_t>(encodings.size()));
    for (int32_t e : encodings)
        Put32(m, static_cast<uint32_t>(e));
    return m;
}

std::vector<uint8_t> MsgFramebufferUpdateRequest(bool incremental, uint16_t x, uint16_t y,
                                                 uint16_t w, uint16_t h)
{
    std::vector<uint8_t> m = { CFramebufferUpdateRequest, static_cast<uint8_t>(incremental ? 1 : 0) };
    Put16(m, x);
    Put16(m, y);
    Put16(m, w);
    Put16(m, h);
    return m;
}

std::vector<uint8_t> MsgKeyEvent(bool down, uint32_t keysym)
{
    std::vector<uint8_t> m = { CKeyEvent, static_cast<uint8_t>(down ? 1 : 0), 0, 0 };
    Put32(m, keysym);
    return m;
}

std::vector<uint8_t> MsgPointerEvent(uint8_t buttonMask, uint16_t x, uint16_t y)
{
    std::vector<uint8_t> m = { CPointerEvent, buttonMask };
    Put16(m, x);
    Put16(m, y);
    return m;
}

std::vector<uint8_t> MsgClientCutText(std::string_view latin1)
{
    std::vector<uint8_t> m = { CClientCutText, 0, 0, 0 };
    const size_t n = latin1.size() > kMaxCutText ? kMaxCutText : latin1.size();
    Put32(m, static_cast<uint32_t>(n));
    m.insert(m.end(), latin1.begin(), latin1.begin() + static_cast<std::ptrdiff_t>(n));
    return m;
}

std::vector<uint8_t> MsgEnableContinuousUpdates(bool enable, uint16_t x, uint16_t y,
                                                uint16_t w, uint16_t h)
{
    std::vector<uint8_t> m = { CEnableContinuousUpdates, static_cast<uint8_t>(enable ? 1 : 0) };
    Put16(m, x);
    Put16(m, y);
    Put16(m, w);
    Put16(m, h);
    return m;
}

// ---- server messages --------------------------------------------------------
Parse ParseServerInit(Reader& r, ServerInit& out)
{
    const size_t start = r.Consumed();
    uint16_t w, h;
    uint8_t pf[16];
    uint32_t nameLen;
    if (!r.U16(w) || !r.U16(h) || !r.Bytes(pf, 16) || !r.U32(nameLen))
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    if (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension ||
        static_cast<uint64_t>(w) * h > kMaxPixels || nameLen > kMaxName)
        return Parse::Bad;
    const uint8_t* name = r.Peek(nameLen);
    if (!name)
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    out.width = w;
    out.height = h;
    out.format = DecodePixelFormat(pf);
    out.name.assign(reinterpret_cast<const char*>(name), nameLen);
    r.Skip(nameLen);
    return Parse::Ok;
}

Parse ParseRectHeader(Reader& r, RectHeader& out)
{
    const size_t start = r.Consumed();
    if (!r.U16(out.x) || !r.U16(out.y) || !r.U16(out.w) || !r.U16(out.h) || !r.I32(out.encoding))
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    return Parse::Ok;
}

bool RectWithin(const RectHeader& rect, uint32_t fbW, uint32_t fbH)
{
    if (rect.w == 0 || rect.h == 0)
        return false;
    // 32-bit arithmetic: no 16-bit wrap can hide an overrun
    return static_cast<uint32_t>(rect.x) + rect.w <= fbW && static_cast<uint32_t>(rect.y) + rect.h <= fbH;
}

Parse ParseFramebufferUpdateHeader(Reader& r, uint16_t& numRects)
{
    const size_t start = r.Consumed();
    uint8_t type, pad;
    if (!r.U8(type) || !r.U8(pad) || !r.U16(numRects))
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    return type == SFramebufferUpdate ? Parse::Ok : Parse::Bad;
}

Parse ParseServerCutText(Reader& r, std::string& latin1)
{
    const size_t start = r.Consumed();
    uint8_t type;
    uint32_t len;
    if (!r.U8(type) || !r.Skip(3) || !r.U32(len))
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    if (type != SServerCutText)
        return Parse::Bad;
    // The Extended Clipboard pseudo-encoding negotiates a negative length
    // here; it is not offered, so a length past the limit is refused rather
    // than interpreted. The connection stays sane: the caller can still not
    // skip an unbounded body, so this is fatal for the stream.
    if (len > kMaxCutText)
        return Parse::Bad;
    const uint8_t* p = r.Peek(len);
    if (!p)
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    latin1.assign(reinterpret_cast<const char*>(p), len);
    r.Skip(len);
    return Parse::Ok;
}

// ---- text ----------------------------------------------------------------
std::string Latin1FromUtf8(std::string_view utf8)
{
    std::string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size())
    {
        const uint8_t c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out.push_back('?'); ++i; continue; }
        if (i + len > utf8.size())
        {
            out.push_back('?');
            break;
        }
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
        {
            const uint8_t cc = static_cast<uint8_t>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok)
        {
            out.push_back('?');
            ++i;
            continue;
        }
        i += len;
        if (cp == '\r')
        {
            // CRLF and a lone CR both become LF
            if (i < utf8.size() && utf8[i] == '\n')
                ++i;
            out.push_back('\n');
        }
        else if (cp <= 0xFF)
            out.push_back(static_cast<char>(cp));
        else
            out.push_back('?');
    }
    return out;
}

std::string Utf8FromLatin1(std::string_view latin1)
{
    std::string out;
    out.reserve(latin1.size() + latin1.size() / 4);
    for (char ch : latin1)
    {
        const uint8_t c = static_cast<uint8_t>(ch);
        if (c < 0x80)
            out.push_back(static_cast<char>(c));
        else
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

} // namespace amber::vnc
