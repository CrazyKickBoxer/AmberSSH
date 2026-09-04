// Protocol.h — AmberXControl, the framed control protocol between AmberSSH and
// an isolated AmberXHost process.
//
// This carries two things that must never be confused: control messages that
// AmberSSH originates, and X11 bytes that a REMOTE HOST originates. The second
// kind is hostile input by definition — it arrives over an SSH channel from a
// machine AmberSSH does not control — so every length, type and channel id
// here is validated before anything is allocated or indexed.
//
// The module is deliberately pure. No Windows types, no pipes, no sockets: it
// turns bytes into decisions and decisions into bytes, which is what lets the
// dangerous half be tested exhaustively without a process on the other end.
// The pipe itself lives in the host and the controller.
//
// Wire format, little-endian, 16-byte header:
//
//     magic    u32   'AmbX'
//     version  u16
//     type     u16
//     channel  u32   0 = control, otherwise an X11 channel id
//     length   u32   payload bytes that follow
//
// See docs/amberx/PROTOCOL.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber::amberx
{

inline constexpr uint32_t kMagic = 0x58626D41u;   // 'A','m','b','X'
inline constexpr uint16_t kVersion = 1;
inline constexpr size_t kHeaderBytes = 16;

// The largest payload any message may carry. This is the single most important
// constant in the file: the length field arrives from a buffer that a remote
// X client can influence, and a parser that allocates before checking it is a
// one-packet denial of service. 1 MiB comfortably holds an X11 burst and is
// far below anything that would strain the host.
inline constexpr uint32_t kMaxPayload = 1u << 20;

// Channel 0 is reserved for control messages. An X11 channel id is never 0, so
// a data frame that claims channel 0 is malformed rather than ambiguous.
inline constexpr uint32_t kControlChannel = 0;

enum class MsgType : uint16_t
{
    // --- control channel only -------------------------------------------
    Hello = 1,          // controller → host: version + nonce
    HelloAck = 2,       // host → controller: version + echoed nonce + own nonce
    AuthProof = 3,      // controller → host: proof over the host's nonce
    SetCookie = 4,      // controller → host: the MIT-MAGIC-COOKIE-1 to accept
    Shutdown = 5,       // controller → host: close cleanly
    ClipboardText = 6,  // either way: UTF-8 clipboard text, already policy-checked
    // --- per-channel ------------------------------------------------------
    ChannelOpen = 16,   // a forwarded X11 connection began
    ChannelData = 17,   // X11 bytes in either direction
    ChannelClose = 18,  // that connection ended
    // --- host → controller ------------------------------------------------
    HostStatus = 32,    // counts and health, for the diagnostics overlay
    HostError = 33,     // a bounded, non-sensitive error string
};

// True for a type this build knows. An unrecognised type is refused rather
// than skipped: silently ignoring a message you do not understand is how two
// versions of a protocol quietly disagree about state.
bool KnownType(uint16_t t);

// True when the type is only ever legal on the control channel, and vice
// versa. Enforced so a data frame cannot impersonate a cookie install.
bool IsControlOnly(MsgType t);
bool IsChannelOnly(MsgType t);

struct Frame
{
    uint16_t version = kVersion;
    MsgType type = MsgType::Hello;
    uint32_t channel = kControlChannel;
    std::vector<uint8_t> payload;
};

// Serialises a frame. Returns false without touching `out` when the payload is
// over the cap or the type/channel pairing is illegal — a frame that could not
// be parsed by the far end is never emitted.
bool Encode(const Frame& f, std::vector<uint8_t>& out);

enum class Decoded
{
    NeedMore,    // a complete frame is not yet in the buffer; keep reading
    Ok,          // `frame` is filled and `consumed` bytes may be dropped
    Bad,         // the stream is unusable; close the pipe
};

// Reads one frame from the front of `in`.
//
// `Bad` is returned for a wrong magic, an unknown version, an unknown type, a
// length over the cap, or a type on the wrong kind of channel. The caller must
// treat it as fatal for the connection: there is no way to resynchronise a
// framed stream whose framing is wrong, and trying is how a parser gets walked
// into somebody else's buffer.
Decoded Decode(const std::vector<uint8_t>& in, Frame& frame, size_t& consumed);

// --------------------------------------------------------------- handshake
// A 32-byte random challenge. Both sides prove they can read a private pipe
// that only this logon session can open; the nonce exchange is what stops a
// squatter who won the pipe name from being mistaken for the real host.
inline constexpr size_t kNonceBytes = 32;

// Constant-time equality. Used for nonces and cookies, where leaking the
// position of the first difference through timing is a real weakness.
bool ConstantTimeEqual(const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b);

// An HMAC-SHA256 proof over the nonce transcript (see Handshake.h).
inline constexpr size_t kProofBytes = 32;

// Builds the payloads. Kept as functions rather than structs with a memcpy so
// there is exactly one definition of each layout, and no C++ object graph is
// ever deserialised from the wire.
//
//   Hello      nonceA
//   HelloAck   nonceA (echoed) | nonceB | host proof
//   AuthProof  controller proof
//   SetCookie  display (u32) | 16-byte cookie
std::vector<uint8_t> MakeHello(const std::vector<uint8_t>& nonce);
std::vector<uint8_t> MakeHelloAck(const std::vector<uint8_t>& echo,
                                  const std::vector<uint8_t>& own,
                                  const std::vector<uint8_t>& proof);
std::vector<uint8_t> MakeAuthProof(const std::vector<uint8_t>& proof);
std::vector<uint8_t> MakeSetCookie(const std::vector<uint8_t>& cookie,
                                   uint32_t display);

// Parsers. Each returns false on any length that does not match exactly —
// short, long, or absent — rather than reading what it can.
bool ParseHello(const std::vector<uint8_t>& p, std::vector<uint8_t>& nonce);
bool ParseHelloAck(const std::vector<uint8_t>& p, std::vector<uint8_t>& echo,
                   std::vector<uint8_t>& own, std::vector<uint8_t>& proof);
bool ParseAuthProof(const std::vector<uint8_t>& p, std::vector<uint8_t>& proof);
bool ParseSetCookie(const std::vector<uint8_t>& p, std::vector<uint8_t>& cookie,
                    uint32_t& display);

// Host → controller. Counts only: nothing in a status message can be a
// secret, a window's contents, or a byte of X11 data.
//   HostStatus  openChannels (u32) | bytesIn (u64) | cookieSet (u32)
//   HostError   UTF-8 text, at most kMaxErrorBytes, never a payload echo
inline constexpr size_t kMaxErrorBytes = 256;
std::vector<uint8_t> MakeHostStatus(uint32_t openChannels, uint64_t bytesIn,
                                    bool cookieSet);
bool ParseHostStatus(const std::vector<uint8_t>& p, uint32_t& openChannels,
                     uint64_t& bytesIn, bool& cookieSet);
std::vector<uint8_t> MakeHostError(const std::string& text);   // truncates
bool ParseHostError(const std::vector<uint8_t>& p, std::string& text);

// ------------------------------------------------------------ channel state
// Tracks which channel ids this side has opened, so a frame naming a channel
// that was never opened — or was already closed — is rejected instead of
// creating one. The prompt calls this channel-id ownership validation; it is
// what stops a confused or hostile peer from addressing another session's
// stream.
class ChannelTable
{
public:
    // The most channels one session may have open at once. X clients are
    // one-channel-per-connection, so this is also a cap on forwarded clients.
    static constexpr size_t kMaxChannels = 64;

    bool Open(uint32_t id);       // false if 0, already open, or at the cap
    bool IsOpen(uint32_t id) const;
    bool Close(uint32_t id);      // false if it was not open
    size_t Count() const { return m_open.size(); }
    void Clear() { m_open.clear(); }

private:
    std::vector<uint32_t> m_open;
};

} // namespace amber::amberx
