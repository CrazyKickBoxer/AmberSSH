// RfbClient.h — the RFB client state machine, from the version string to a
// steady stream of framebuffer updates, over any byte source.
//
// The client is fed bytes and hands back bytes: Feed() consumes what has
// arrived and TakeOutput() returns what must be sent. It owns the decoded
// framebuffer, the dirty region, the cursor shape and the ZRLE stream, and it
// knows nothing about sockets, threads, SSH or Direct3D. That is what lets
// the whole protocol — three handshake dialects, two security types, four
// encodings, three pseudo-encodings — be driven by a test with a byte array,
// and it is where the version-specific rules that deadlock a naive client
// live in one place:
//
//   * the version reply may never exceed the server's offer;
//   * RFB 3.3 states one security type and takes no answer;
//   * a security type of None is followed by nothing before 3.8;
//   * a failed SecurityResult carries a reason only from 3.8;
//   * DesktopSize is acted on before the bounds check every other rectangle
//     gets, because a screen that grew is outside the framebuffer it replaces;
//   * the next update is requested as soon as an update's rectangle count is
//     read, not after it is decoded, with at most one request outstanding.
//
// Everything a server sends is validated before it is believed: dimensions,
// rectangle bounds, string lengths, compressed sizes, palette indices.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "RfbDecoders.h"
#include "RfbProtocol.h"

namespace amber::vnc
{

struct ClientOptions
{
    std::string password;              // for VNC Authentication; cleared after use
    bool shared = true;                // ClientInit shared-flag
    // Preference order. The server picks the first it supports.
    std::vector<int32_t> encodings = { EncZRLE, EncHextile, EncCopyRect, EncRaw };
    bool wantCursor = true;            // offer CursorShape
    bool wantDesktopSize = true;       // offer DesktopSize
    bool wantContinuousUpdates = true; // offer ContinuousUpdates
    bool allowNone = true;             // accept security type None
    bool allowVncAuth = true;          // accept VNC Authentication
    // VeNCrypt (RfbTls.h). With `tls` set the client takes security type
    // 19 or nothing — never a plaintext type the server also offers — and
    // only its X.509 subtypes. `username` is for X509Plain, which is taken
    // only when allowPlainOverTls says so.
    bool tls = false;
    std::string username;
    bool allowPlainOverTls = false;
};

enum class ClientState
{
    Version,          // waiting for the server's 12-byte version
    SecurityTypes,    // 3.3: one u32; 3.7+: a list
    SecurityReason,   // the server refused with a reason string
    VncAuthChallenge, // 16 bytes to encrypt
    SecurityResult,   // u32, and a reason from 3.8
    // VeNCrypt (security type 19): version, its ack, the subtype list, its
    // ack, then a pause while the caller runs the TLS handshake on the
    // socket (NeedsTls / TlsEstablished); the subtype's own authentication
    // follows inside TLS
    VeNCryptVersion,
    VeNCryptVersionAck,
    VeNCryptSubtypes,
    VeNCryptSubtypeAck,
    TlsHandshake,
    ServerInit,       // the desktop's size, format and name
    Ready,            // steady state: updates, cut text, bells
    Failed,           // the stream is dead; Error() says why
};

// Why a connection failed, so the caller can decide whether to retry:
// a rejected password must not be retried; a broken stream may be.
enum class FailureKind
{
    None,
    Protocol,         // malformed or unsupported stream
    Refused,          // the server said no (reason text, if any)
    Authentication,   // wrong password, or no acceptable security type
};

class RfbClient
{
public:
    explicit RfbClient(ClientOptions options);

    // Appends bytes and runs the state machine as far as they allow. Returns
    // false once the client has Failed; the remaining bytes are dropped.
    bool Feed(const uint8_t* data, size_t n);
    // Bytes to send, in order. Empty when there is nothing pending.
    std::vector<uint8_t> TakeOutput();

    ClientState State() const { return m_state; }
    FailureKind Failure() const { return m_failure; }
    const std::string& Error() const { return m_error; }
    int NegotiatedMinor() const { return m_minor; }
    const ServerInit& Init() const { return m_init; }
    bool ContinuousUpdates() const { return m_continuous; }

    // ---- VeNCrypt -----------------------------------------------------------
    // True while the state machine is parked at TlsHandshake: the caller
    // owns the socket, runs the TLS handshake on it, and — when it is up
    // and the certificate is accepted — calls TlsEstablished(), after which
    // every byte fed in must have come through TLS. Nothing is sent or
    // parsed in between.
    bool NeedsTls() const { return m_state == ClientState::TlsHandshake; }
    void TlsEstablished();
    uint32_t VeNCryptSubtype() const { return m_venSubtype; }   // 0 when not VeNCrypt
    bool Encrypted() const { return m_encrypted; }

    // ---- steady state, client → server ------------------------------------
    // A FramebufferUpdateRequest for the whole framebuffer. Bounded: with a
    // request already outstanding this is a no-op unless `force`.
    void RequestUpdate(bool incremental, bool force = false);
    void SendKey(bool down, uint32_t keysym);
    void SendPointer(uint8_t buttons, uint16_t x, uint16_t y);
    void SendCutText(std::string_view utf8);   // converted to Latin-1
    int OutstandingRequests() const { return m_outstanding; }

    // ---- steady state, server → client -------------------------------------
    Framebuffer& Fb() { return m_fb; }
    const Framebuffer& Fb() const { return m_fb; }
    DirtyRegion& Dirty() { return m_dirty; }
    // The framebuffer was resized by DesktopSize since the flag was last read.
    bool TakeResized();
    const CursorShape& Cursor() const { return m_cursor; }
    bool TakeCursorChanged();
    std::vector<std::string> TakeCutTexts();   // UTF-8, converted from Latin-1
    int TakeBells();
    // Completed FramebufferUpdate messages since last read.
    uint32_t TakeUpdatesCompleted();

private:
    void Fail(FailureKind kind, std::string why);
    void Send(const std::vector<uint8_t>& bytes) { m_out.insert(m_out.end(), bytes.begin(), bytes.end()); }
    // Each returns Ok when the state advanced (and may have consumed bytes),
    // NeedMore when it must wait, Bad when the stream is unusable.
    Parse StepVersion(Reader& r);
    Parse StepSecurityTypes(Reader& r);
    Parse StepSecurityReason(Reader& r);
    Parse StepVncAuthChallenge(Reader& r);
    Parse StepSecurityResult(Reader& r);
    Parse StepVeNCryptVersion(Reader& r);
    Parse StepVeNCryptVersionAck(Reader& r);
    Parse StepVeNCryptSubtypes(Reader& r);
    Parse StepVeNCryptSubtypeAck(Reader& r);
    uint32_t m_venSubtype = 0;
    bool m_encrypted = false;
    Parse StepServerInit(Reader& r);
    Parse StepReady(Reader& r);
    Parse StepRectangle(Reader& r);
    void AfterSecurity(uint8_t type);   // what follows a chosen security type
    void SendClientInit();
    void OfferEncodings();

    ClientOptions m_opt;
    ClientState m_state = ClientState::Version;
    FailureKind m_failure = FailureKind::None;
    std::string m_error;
    int m_minor = 0;
    uint8_t m_security = SecInvalid;

    std::vector<uint8_t> m_in;    // unconsumed input
    std::vector<uint8_t> m_out;   // pending output

    ServerInit m_init;
    Framebuffer m_fb;
    DirtyRegion m_dirty;
    bool m_resized = false;
    CursorShape m_cursor;
    bool m_cursorChanged = false;
    std::vector<std::string> m_cutTexts;
    int m_bells = 0;
    uint32_t m_updatesCompleted = 0;

    // the update being received
    bool m_inUpdate = false;
    uint16_t m_rectsLeft = 0;
    bool m_haveRect = false;      // header parsed, body pending
    RectHeader m_rect;
    ZrleDecoder m_zrle;
    HextileDecoder m_hextile;
    bool m_hextileActive = false;

    int m_outstanding = 0;        // FramebufferUpdateRequests not yet answered
    bool m_continuous = false;    // the server accepted EnableContinuousUpdates
    bool m_continuousAsked = false;
};

} // namespace amber::vnc
