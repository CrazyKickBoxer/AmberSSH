// RfbProtocol.h — the RFB wire format (RFC 6143) as pure functions: message
// builders, header parsers, and a reader that refuses to run off the end.
//
// Nothing in this file touches a socket, a thread or a framebuffer. That is
// what makes it testable against hand-assembled byte streams, and what keeps
// the client state machine (RfbClient) readable: it deals in messages, not
// in bytes. Every parser validates lengths and dimensions before anything is
// allocated or written, and reports NeedMore rather than guessing when the
// bytes are not all there yet — a partial read is the normal case on a
// socket, not an error.
//
// Byte order on the wire is big-endian throughout, except inside pixel data,
// whose order the client chooses through SetPixelFormat (§7.4).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace amber::vnc
{

// ---- limits, applied before any allocation ---------------------------------
// A server can claim any size it likes in a two-byte field; these are what
// the client is willing to believe.
constexpr uint32_t kMaxDimension = 16384;                 // per axis
constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;      // 8K x 8K
constexpr uint32_t kMaxCutText = 1u << 20;                // 1 MiB of clipboard
constexpr uint32_t kMaxReason = 64u * 1024;               // a refusal's text
constexpr uint32_t kMaxName = 64u * 1024;                 // the desktop's name
constexpr uint32_t kMaxCursorDim = 256;                   // cursor shape, per axis
constexpr uint32_t kMaxZrleRectBytes = 32u << 20;         // one rectangle's compressed body

// ---- pixel format (§7.4) --------------------------------------------------
struct PixelFormat
{
    uint8_t bitsPerPixel = 32;
    uint8_t depth = 24;
    uint8_t bigEndian = 0;
    uint8_t trueColour = 1;
    uint16_t redMax = 255, greenMax = 255, blueMax = 255;
    uint8_t redShift = 16, greenShift = 8, blueShift = 0;
};
// What every AmberSSH client asks for: 32 bits per pixel, depth 24,
// little-endian, red/green/blue at shifts 16/8/0 — which is BGRX in memory,
// exactly the byte order a BGRA texture wants with its alpha forced opaque.
inline constexpr PixelFormat kRequestedFormat{};
bool operator==(const PixelFormat& a, const PixelFormat& b);
inline bool operator!=(const PixelFormat& a, const PixelFormat& b) { return !(a == b); }
void EncodePixelFormat(const PixelFormat& f, uint8_t out[16]);
PixelFormat DecodePixelFormat(const uint8_t in[16]);

// ---- encodings (§7.7, §7.8) --------------------------------------------------
enum Encoding : int32_t
{
    EncRaw = 0,
    EncCopyRect = 1,
    EncRRE = 2,
    EncHextile = 5,
    EncTight = 7,
    EncZRLE = 16,
    EncPseudoCursor = -239,
    EncPseudoDesktopSize = -223,
    EncPseudoContinuousUpdates = -313,
    // ExtendedDesktopSize (community RFB extension, TigerVNC's): the server
    // announces its screen layout, and a client may ask for a new size with
    // SetDesktopSize. Offered before DesktopSize; a server that speaks it
    // sends the extended form instead.
    EncPseudoExtendedDesktopSize = -308,
};

// ---- security types (§7.1.2) -------------------------------------------------
enum SecurityType : uint8_t
{
    SecInvalid = 0,
    SecNone = 1,
    SecVncAuth = 2,
    SecVeNCrypt = 19,
};

// ---- message types (§7.5, §7.6) ----------------------------------------------
enum ClientMsg : uint8_t
{
    CSetPixelFormat = 0,
    CSetEncodings = 2,
    CFramebufferUpdateRequest = 3,
    CKeyEvent = 4,
    CPointerEvent = 5,
    CClientCutText = 6,
    CEnableContinuousUpdates = 150,   // ContinuousUpdates extension
    CSetDesktopSize = 251,            // ExtendedDesktopSize extension
};
enum ServerMsg : uint8_t
{
    SFramebufferUpdate = 0,
    SSetColourMapEntries = 1,
    SBell = 2,
    SServerCutText = 3,
    SEndOfContinuousUpdates = 150,    // ContinuousUpdates extension
};

// pointer button mask bits (§7.5.5); wheel motion is a press+release of 4/5
constexpr uint8_t kButtonLeft = 1;
constexpr uint8_t kButtonMiddle = 2;
constexpr uint8_t kButtonRight = 4;
constexpr uint8_t kButtonWheelUp = 8;
constexpr uint8_t kButtonWheelDown = 16;
constexpr uint8_t kButtonWheelLeft = 32;
constexpr uint8_t kButtonWheelRight = 64;

// ---- protocol version (§7.1.1) -----------------------------------------------
struct Version
{
    int major = 0, minor = 0;
};
// "RFB xxx.yyy\n". False when the twelve bytes are not that shape.
bool ParseVersion(const uint8_t in[12], Version& v);
// The minor version to answer with: 3 for a 3.3 server, 7 for 3.7, 8 for
// anything from 3.8 up. -1 when the major is not 3 or the minor is below 3.
// The reply may never exceed what the server offered (vncfree's note: a 3.3
// server answered with 3.8 reads the client's security choice as ClientInit).
int ChooseMinor(const Version& server);
std::array<uint8_t, 12> VersionReply(int minor);

// ---- reader ---------------------------------------------------------------
// Reads big-endian fields from a span. When the bytes are not all there, an
// accessor returns false, marks the reader Short, and consumes nothing — so
// the caller can keep the buffer, wait for more, and parse the same message
// again from its start. Bad() is for semantic failures the caller records
// after the fact; the reader itself only knows about length.
class Reader
{
public:
    Reader(const uint8_t* p, size_t n) : m_p(p), m_n(n) {}
    bool U8(uint8_t& v);
    bool U16(uint16_t& v);
    bool U32(uint32_t& v);
    bool I32(int32_t& v);
    bool Bytes(void* out, size_t n);
    bool Skip(size_t n);
    // The next n bytes without consuming them, or nullptr when short.
    const uint8_t* Peek(size_t n) const;
    const uint8_t* Ptr() const { return m_p + m_off; }
    size_t Remaining() const { return m_n - m_off; }
    size_t Consumed() const { return m_off; }
    bool Short() const { return m_short; }
    void Rewind(size_t to) { m_off = to; }

private:
    bool Need(size_t n);
    const uint8_t* m_p;
    size_t m_n;
    size_t m_off = 0;
    bool m_short = false;
};

enum class Parse
{
    Ok,
    NeedMore,   // keep the bytes, come back with more
    Bad,        // the stream is not RFB any more; close it
};

// ---- client → server messages ------------------------------------------------
std::vector<uint8_t> MsgClientInit(bool shared);
std::vector<uint8_t> MsgSetPixelFormat(const PixelFormat& f);
std::vector<uint8_t> MsgSetEncodings(const std::vector<int32_t>& encodings);
std::vector<uint8_t> MsgFramebufferUpdateRequest(bool incremental, uint16_t x, uint16_t y,
                                                 uint16_t w, uint16_t h);
std::vector<uint8_t> MsgKeyEvent(bool down, uint32_t keysym);
std::vector<uint8_t> MsgPointerEvent(uint8_t buttonMask, uint16_t x, uint16_t y);
// Latin-1 text with LF line endings, as §7.5.6 specifies. Convert first.
std::vector<uint8_t> MsgClientCutText(std::string_view latin1);
std::vector<uint8_t> MsgEnableContinuousUpdates(bool enable, uint16_t x, uint16_t y,
                                                uint16_t w, uint16_t h);

// ---- ExtendedDesktopSize -------------------------------------------------------
// One screen of the server's layout: id, its rectangle in the framebuffer,
// and flags the server defines. A SetDesktopSize request names the screens
// it wants; this client keeps one, the server's first, resized to fit.
struct Screen
{
    uint32_t id = 0;
    uint16_t x = 0, y = 0, w = 0, h = 0;
    uint32_t flags = 0;
};
// The rectangle body: u8 count, 3 padding, count x 16 bytes. Consumes
// nothing on a short read; Bad on a count of zero or a screen outside w x h.
Parse ParseScreenLayout(Reader& r, uint16_t w, uint16_t h, std::vector<Screen>& out);
// SetDesktopSize (type 251): the requested size and one screen covering it.
std::vector<uint8_t> MsgSetDesktopSize(uint16_t w, uint16_t h, const Screen& screen);
// ExtendedDesktopSize rectangle header semantics: x = why, y = how it went.
enum ResizeReason : uint16_t { ResizeByServer = 0, ResizeByThisClient = 1, ResizeByOtherClient = 2 };
enum ResizeStatus : uint16_t { ResizeOk = 0, ResizeProhibited = 1, ResizeOutOfResources = 2, ResizeInvalidLayout = 3 };
const char* ResizeStatusName(uint16_t status);

// ---- server → client messages ------------------------------------------------
struct ServerInit
{
    uint16_t width = 0, height = 0;
    PixelFormat format;
    std::string name;
};
// The whole ServerInit (§7.3.2). Dimensions are checked against the limits
// above and the name length against kMaxName before the name is read.
Parse ParseServerInit(Reader& r, ServerInit& out);

struct RectHeader
{
    uint16_t x = 0, y = 0, w = 0, h = 0;
    int32_t encoding = 0;
};
// Twelve bytes: position, size, encoding. Bounds are NOT checked here —
// DesktopSize carries the new size in a header that is, by definition,
// outside the old framebuffer, so the caller checks after it has decided
// which encoding it is looking at (see RectWithin).
Parse ParseRectHeader(Reader& r, RectHeader& out);
// x+w <= fbW, y+h <= fbH, w > 0, h > 0, with no 16-bit wrap.
bool RectWithin(const RectHeader& rect, uint32_t fbW, uint32_t fbH);

// FramebufferUpdate's own header (§7.6.1): type byte, padding, rectangle
// count. The rectangles follow one by one.
Parse ParseFramebufferUpdateHeader(Reader& r, uint16_t& numRects);
// ServerCutText (§7.6.4), from the type byte. Length is validated against
// kMaxCutText before any of the text is read.
Parse ParseServerCutText(Reader& r, std::string& latin1);

// ---- text (§7.5.6, §7.6.4: Latin-1, LF endings) ----------------------------
// UTF-8 to Latin-1: code points above U+00FF become '?', CRLF and CR become
// LF. This is the only text encoding the core protocol has; the Extended
// Clipboard pseudo-encoding that carries UTF-8 is not implemented, and
// docs/vnc.md says so.
std::string Latin1FromUtf8(std::string_view utf8);
std::string Utf8FromLatin1(std::string_view latin1);

} // namespace amber::vnc
