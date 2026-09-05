// VncSession.h — one VNC tab's network and decode worker.
//
// Ownership is the whole design. The worker thread owns the socket, the
// RFB state machine (RfbClient) and with it the decoded framebuffer, the
// dirty region and the ZRLE stream. The render thread owns the texture.
// They meet in one place: a pending Damage — the rectangles that changed
// since the render thread last looked, with their pixels already copied out
// — that the worker publishes after each decode step under a lock held only
// for the copy, and that TakeDamage swaps out in O(1). Decoding a large
// update therefore never stalls a frame, and a frame never stalls decoding.
// Protocol order is kept because there is exactly one decoder and it runs
// in arrival order; a later CopyRect sees what the earlier rectangles left.
//
// The pending Damage is bounded: past twice the framebuffer's area it
// collapses to one copy of the whole framebuffer, so a burst of updates
// while the renderer is busy costs one full upload rather than unbounded
// memory or an unbounded frame.
//
// Transport is direct TCP, or a loopback connection to a local forward the
// tab asks its SSH session to raise (AddForward "L0:host:port"; the bound
// port comes back through SshEventType::ForwardUp, relayed here by the UI
// thread as OnForwardUp). libssh2 is only ever touched by the session's own
// thread; this side sees plain TCP. Transport failures reconnect with
// bounded exponential backoff and a fresh full picture; a rejected password
// or a refusal does not retry.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RfbClient.h"
#include "RfbTls.h"

namespace amber::vnc
{

struct VncConfig
{
    std::string host;
    int port = 5900;
    std::string password;          // kept for reconnects; zeroed on Disconnect
    bool viewOnly = false;         // no keys, pointer or clipboard go out
    int encodings = 0;             // 0 ZRLE first, 1 Hextile first, 2 Raw only
    bool wantCursor = true;        // offer the Cursor pseudo-encoding
    bool tls = false;              // VeNCrypt required; never downgraded
    std::string username;          // for VeNCrypt X509Plain, when allowed
    bool allowPlainOverTls = false;
    // Tunnel through a live SSH session: the function adds a local forward
    // to it (SshSession::AddForward, thread-safe), and the bound port comes
    // back through OnForwardUp. Empty = direct TCP. Whatever it captures
    // must outlive this VncSession's worker, or Disconnect() first.
    std::function<void(const std::string& spec)> addForward;
    int connectTimeoutMs = 10000;
    int handshakeTimeoutMs = 20000;
    int tunnelTimeoutMs = 15000;
    int maxReconnects = 5;         // consecutive attempts before giving up
};

enum class VncState
{
    Connecting,
    Authenticating,
    Connected,
    Reconnecting,
    Disconnected,
    Error,
};
const char* VncStateName(VncState s);

struct VncEvent
{
    enum class Type
    {
        Status,      // text = progress
        Connected,   // text = the desktop's name
        Resized,     // text = "WxH"
        CutText,     // text = UTF-8 from the server's clipboard
        Bell,
        Closed,      // text = reason; no further attempts
        Error,       // text = message; no further attempts
        AuthFailed,  // text = the server's reason; the password was rejected
        // text = the server certificate's fingerprint, subject and why it
        // did not verify, one per line; flag = a pinned certificate differs
        // (the alarm case). The worker waits for AnswerCertificate.
        CertPrompt,
    };
    Type type;
    std::string text;
    bool flag = false;
};

// What changed: rectangles in arrival order, pixels packed one rectangle
// after another (w*h each, 0xFFRRGGBB). `full` means the framebuffer was
// (re)created and the single rectangle is all of it.
struct Damage
{
    uint32_t width = 0, height = 0;
    bool full = false;
    std::vector<Rect> rects;
    std::vector<uint32_t> pixels;
};

struct VncStats
{
    uint64_t bytesIn = 0, bytesOut = 0;
    uint32_t updates = 0;           // FramebufferUpdate messages completed
    float decodeMsPerSec = 0.0f;    // CPU time spent decoding, per wall second
    uint32_t pendingRects = 0;      // damage the renderer has not taken yet
    uint64_t pendingPixels = 0;
    int reconnects = 0;
    uint32_t fbWidth = 0, fbHeight = 0;
    bool continuous = false;        // ContinuousUpdates in effect
    int rfbMinor = 0;
    bool encrypted = false;         // VeNCrypt: the stream is inside TLS
    uint32_t venSubtype = 0;        // VeNCryptSubtype, 0 when not VeNCrypt
    std::string tlsProtocol;        // e.g. "TLSv1.3", "" when plaintext
};

class VncSession
{
public:
    VncSession();
    ~VncSession();
    VncSession(const VncSession&) = delete;
    VncSession& operator=(const VncSession&) = delete;

    bool Start(const VncConfig& cfg);    // spawns the worker
    void Disconnect();                   // stops it and joins; safe to repeat
    bool Running() const { return m_running.load(); }
    VncState State() const { return m_state.load(); }
    bool PollEvent(VncEvent& ev);

    // Render thread. Swaps the pending damage out; false when nothing changed.
    bool TakeDamage(Damage& out);
    bool TakeCursor(CursorShape& out);

    // Input, queued to the worker. Dropped in view-only mode.
    void SendKey(bool down, uint32_t keysym);
    void SendPointer(uint8_t buttons, uint16_t x, uint16_t y);
    void SendCutText(std::string utf8);
    void RequestFullUpdate();
    bool ViewOnly() const { return m_viewOnly.load(); }
    void SetViewOnly(bool v) { m_viewOnly.store(v); }
    // The answer to a CertPrompt. Reject (or Disconnect) ends the attempt;
    // the worker does not retry a certificate the user refused.
    void AnswerCertificate(bool accept);

    // From the UI thread, when the SSH session this tab tunnels through
    // reports a forward up ("bound:host:port"). Ignored unless it is ours.
    void OnForwardUp(const std::string& text);
    // The forward spec this session asked for, or "" when direct.
    std::string TunnelSpec() const;

    VncStats GetStats() const;

private:
    struct Command
    {
        enum class Kind { Key, Pointer, CutText, Refresh } kind;
        bool down = false;
        uint32_t keysym = 0;
        uint8_t buttons = 0;
        uint16_t x = 0, y = 0;
        std::string text;
    };

    void Worker();
    // One attempt: transport up and the handshake to Ready. `retryable`
    // says whether a failure is the transport's (try again) or the server's
    // answer (do not).
    bool Connect(RfbClient& client, std::string& err, bool& retryable);
    // Steady state until stop or the stream dies; returns the reason.
    std::string Loop(RfbClient& client);
    bool OpenTransport(std::string& err);
    bool WaitForTunnel(std::string& err);
    bool Pump(RfbClient& client, int timeoutMs, bool& closed);   // one wait + read + write
    bool FlushOutput();
    // The client is parked at TlsHandshake: flush the subtype choice, run the
    // handshake, verify or ask about the certificate, then TlsEstablished.
    // A refusal or a rejected certificate is not retried (m_tlsFatal).
    bool StartTls(RfbClient& client, std::string& err);
    // recv/SSL_read, one call: > 0 bytes, 0 = closed, -1 = would block
    int ReadSome(uint8_t* buf, size_t n);
    void Publish(RfbClient& client);   // dirty rects → pending damage
    void Post(VncEvent::Type type, std::string text = {});
    void SetState(VncState s) { m_state.store(s); }

    VncConfig m_cfg;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stop{ false };
    std::atomic<VncState> m_state{ VncState::Disconnected };
    std::atomic<bool> m_viewOnly{ false };

    // transport
    uintptr_t m_sock = ~static_cast<uintptr_t>(0);   // INVALID_SOCKET without winsock2.h here
    void* m_sockEvent = nullptr;                     // WSAEVENT
    void* m_wake = nullptr;                          // input arrived / stop
    std::vector<uint8_t> m_outbound;                 // not yet written
    // VeNCrypt: the TLS layer over m_sock once the handshake is done, and
    // the certificate question in flight between the worker and the UI
    std::unique_ptr<TlsClient> m_tls;
    std::string m_tlsError;                          // why StartTls failed
    bool m_tlsFatal = false;                         // ... and that it must not retry
    std::mutex m_certMu;
    std::condition_variable m_certCv;
    int m_certAnswer = 0;                            // 0 pending, 1 accept, -1 reject
    // tunnel
    mutable std::mutex m_tunnelMu;
    std::condition_variable m_tunnelCv;
    int m_tunnelPort = 0;
    bool m_tunnelAsked = false;

    // worker → UI
    mutable std::mutex m_evMu;
    std::deque<VncEvent> m_events;
    mutable std::mutex m_damageMu;
    Damage m_pending;
    CursorShape m_cursor;
    bool m_cursorChanged = false;
    // UI → worker
    std::mutex m_cmdMu;
    std::deque<Command> m_cmds;

    // stats
    std::atomic<uint64_t> m_bytesIn{ 0 }, m_bytesOut{ 0 };
    std::atomic<uint32_t> m_updates{ 0 };
    std::atomic<float> m_decodeMsPerSec{ 0.0f };
    std::atomic<int> m_reconnects{ 0 };
    std::atomic<uint32_t> m_fbW{ 0 }, m_fbH{ 0 };
    std::atomic<bool> m_continuous{ false };
    std::atomic<int> m_minor{ 0 };
    std::atomic<bool> m_encrypted{ false };
    std::atomic<uint32_t> m_venSubtype{ 0 };
    std::string m_tlsProtocol;    // under m_damageMu, like the pending damage
    double m_decodeAccum = 0.0;   // worker thread only: decode CPU ms this window
};

} // namespace amber::vnc
