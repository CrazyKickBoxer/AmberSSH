// session.h — one remote session on a dedicated network thread. The SSH
// transport (libssh2: password / key / agent auth, known_hosts with explicit
// accept, PTY + shell or command, forwards, X11, agent forwarding) is the
// main path; Telnet, Rlogin, Raw TCP and Serial (COM) ride the same object so
// the application sees one interface (Send / Output / events / resize).
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ring.h"

namespace amber::amberx { class AmberXController; }

struct SshConfig
{
    // 0 SSH, 1 Telnet, 2 Rlogin, 3 Raw, 4 Serial, 5 Local (amber::Protocol).
    int protocol = 0;

    std::string host;
    int port = 22;
    std::string user;
    std::string password;      // zeroed after auth
    std::string keyPath;       // OpenSSH PEM private key
    std::string passphrase;    // zeroed after auth
    bool useKey = false;
    bool useAgent = false;     // authenticate via the running SSH agent
    // Tunnels: ';'-separated specs — L<port>:<host>:<port> local forward,
    // R<port>:<host>:<port> remote forward, D<port> dynamic SOCKS5.
    std::string forwards;
    // "user@host[:port]" — connect through this jump host (direct-tcpip
    // bridge; empty user inherits `user`, authenticated with the same
    // credentials).
    std::string jumpHost;
    int cols = 80;
    int rows = 24;
    // Terminal capability advertisement. COLORTERM=truecolor is requested via
    // channel setenv; servers that refuse env requests are not an error.
    std::string termType = "xterm-256color";

    // ---- Connection --------------------------------------------------------
    int connectTimeoutSeconds = 15;
    int keepaliveSeconds = 0;      // SSH-level keepalive (0 = off)
    bool tcpNoDelay = true;
    bool tcpKeepalive = false;
    int ipVersion = 0;             // 0 auto, 1 IPv4, 2 IPv6
    std::string logicalHost;       // known_hosts name (empty = host)

    // ---- Data --------------------------------------------------------------
    std::string termSpeed = "38400,38400";   // ispeed,ospeed
    std::string envVars;                     // NAME=value per line

    // ---- Proxy -------------------------------------------------------------
    int proxyType = 0;             // 0 none, 1 SOCKS4, 2 SOCKS5, 3 HTTP CONNECT
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyUser;
    std::string proxyPass;         // zeroed after the proxy handshake
    std::string proxyExclude;      // wildcard list connecting directly
    bool proxyLocalhost = false;
    bool proxyDns = true;          // proxy resolves the host name

    // ---- SSH extras --------------------------------------------------------
    std::string remoteCommand;     // exec instead of a shell
    bool noShell = false;          // tunnels only, no channel
    bool compression = false;
    std::string cipherPref, kexPref, hostKeyPref;   // comma lists
    std::vector<std::string> manualHostKeys;        // fingerprints to accept
    bool agentForward = false;
    bool x11Forward = false;
    std::string x11Display = "localhost:0";
    // Untrusted X11 forwarding (see src/remote/XAuth.h). The FAKE cookie is
    // what the remote host is told; the REAL one is substituted into each X11
    // connection here, so a compromised remote host never learns the
    // credential that opens the display. Both are lowercase hex.
    //
    // An empty real cookie means none was found in .Xauthority: the fake is
    // still verified, but the packet is forwarded unchanged and the local X
    // server's own access control decides. Weaker, and reported to the user.
    std::string x11FakeCookieHex;
    std::string x11RealCookieHex;
    // 0 = connect each X11 channel to the X server at x11Display over TCP;
    // 1 = hand each channel to an AmberXHost process (src/amberx/) owned by
    // this session. With AmberX the host IS the display, so the real cookie
    // is the fake one and the substitution is an identity — the check still
    // runs.
    int x11Backend = 0;
    // What AmberX paints on every frame's identity strip: the session as
    // AmberSSH names it (never anything the remote sent) and the chrome
    // style to paint it in. The host-key sigil is added by the session
    // itself once the key has been verified.
    std::string amberxIdentity;
    int amberxSkin = 0;
    // Trusted X11: an explicit, warned opt-in. Fixed for the host's lifetime.
    bool x11Trusted = false;
    // Clipboard policy for AmberX, one of the AMBERWIN_CLIP_* values in
    // src/amberx/server/amberwin.h. Disabled by default; every other value
    // is something the user chose in the profile.
    int x11Clipboard = 0;
    // Phase 7: the X screen rectangle in Windows desktop coordinates, chosen
    // by the profile's Display setting. Width 0 means the whole virtual
    // desktop. And the repaint ceiling for forwarded windows, 0 = none.
    int x11DesktopX = 0, x11DesktopY = 0, x11DesktopW = 0, x11DesktopH = 0;
    int x11PresentCapHz = 0;
    // 0 native windows, 1 tabs, 2 panes, 3 ask. Only 0 is implemented; the
    // session says so and uses native windows.
    int x11WindowMode = 0;

    // ---- Telnet / Rlogin ---------------------------------------------------
    bool telnetPassive = false;
    bool telnetNewline = false;    // Return = CR LF (else CR NUL)
    std::string rloginLocalUser;

    // ---- Serial ------------------------------------------------------------
    std::string serialPort;        // "COM3"
    int serialBaud = 9600;
    int serialDataBits = 8;
    int serialStopBits = 1;        // 1, 2, 15 (= 1.5)
    int serialParity = 0;          // 0 none, 1 odd, 2 even, 3 mark, 4 space
    int serialFlow = 0;            // 0 none, 1 XON/XOFF, 2 RTS/CTS, 3 DSR/DTR

    // ---- Local (ConPTY) ----------------------------------------------------
    std::string localShellKey;     // "pwsh", "cmd", "wsl:Ubuntu", "" = custom
    std::string localExe;          // console executable (may be a bare name)
    std::string localArgs;
    std::string localCwd;          // empty = the user's profile directory
    std::string localEnv;          // NAME=value per line, overrides ours
    bool localShellIntegration = true;   // per-session OSC 7 / OSC 133 bootstrap
};

// What the Remote Apps shelf and the AmberX diagnostics are drawn from
// (Phase 7). Filled from the host's periodic report, which carries counts and
// window titles and nothing else. Mirrored here rather than shared so that
// session.h does not pull in the control protocol.
struct RemoteAppWindow
{
    uint32_t xid = 0;
    // 1 minimised, 2 maximised, 4 active, 8 override-redirect (a menu or a
    // tooltip — a window, but not an application the shelf should list)
    uint32_t flags = 0;
    std::string title;
};

struct RemoteAppReport
{
    bool valid = false;          // false until the first report arrives
    uint32_t clients = 0;
    uint32_t windows = 0;
    uint64_t pixmapBytes = 0;
    uint64_t x11In = 0, x11Out = 0;
    uint32_t presents = 0, dirtyRects = 0, ipcHighWater = 0, rejected = 0;
    // Measured here, not reported by the host: how long host frames sat in
    // the controller's queue before the session loop picked them up, over
    // the last report interval. Arrival is enqueue, so this is the loop's
    // own pickup latency — the number that was 0-30 ms while the loop found
    // host frames on a tick.
    uint32_t hostWaitMaxUs = 0, hostWaitAvgUs = 0, hostWaitFrames = 0;
    std::string lastError;       // the host's last reported error, if any
    std::vector<RemoteAppWindow> list;
};

enum class SshEventType
{
    Status,          // text = progress message
    HostKeyPrompt,   // text = "keytype SHA256:fingerprint" — call AnswerHostKey
    Connected,
    Closed,          // text = reason
    Error,           // text = message
    // Phase 6: UTF-8 text an X client put on the session's clipboard. The
    // worker never touches the Windows clipboard itself — opening it can
    // block on whichever application currently holds it — so the text is
    // handed to the UI thread as an ordinary event and applied there.
    ClipboardText,
};

struct SshEvent
{
    SshEventType type;
    std::string text;
};

class SshSession
{
public:
    // Both defined in session.cpp, where AmberXController is a complete type.
    // Left implicit, the constructor's unwind path would delete the
    // unique_ptr in every TU that constructs a session and fail to compile
    // against the forward declaration above.
    SshSession();
    ~SshSession();

    bool Start(const SshConfig& cfg);     // spawns the network thread
    void AnswerHostKey(bool accept);
    void Send(const char* data, size_t len);
    // Telnet only: queue an IAC <command> (244 IP, 237 SUSP, 236 EOF, 247 EC,
    // 243 BRK, 246 AYT ...) ahead of the next data write.
    void SendTelnetCommand(uint8_t cmd);
    // Kernel-smoothed TCP round-trip time of the link, microseconds (0 until
    // the first sample). Drives the latency-ghosting effect.
    uint32_t RttUs() const { return m_rttUs.load(); }
    // Bytes the kernel had to retransmit on this link (monotonic); a rise
    // means packets are being lost right now.
    uint32_t RetransBytes() const { return m_retrans.load(); }
    void RequestResize(int cols, int rows);
    // Adds a local forward to an already-running session, in the same syntax
    // the profile's `forwards` field uses. Thread-safe; the worker picks it up
    // on its next pass and reports success or "port busy" as a Status event.
    //
    // Exists so RemoteApp can raise its tunnel without making the user
    // reconnect. It only ever ADDS: nothing here can remove a forward the user
    // configured, and a listener still binds loopback only.
    void AddForward(const std::string& spec);
    // Offers UTF-8 text from the Windows clipboard to this session's X
    // clients. Thread-safe and non-blocking: the worker sends it on its next
    // pass, and drops it if the session has no AmberX host or the policy
    // does not allow this direction.
    void OfferClipboard(std::string utf8);
    // The latest AmberX report, or a report with valid == false when this
    // session has no host or none has arrived yet. Copies under a lock: the
    // caller is the UI thread and must not hold anything the worker needs.
    RemoteAppReport AmberXReport() const;
    // Show, minimise or close one forwarded window (0, 1, 2). Queued for the
    // worker; closing asks the application, it does not kill it.
    void RemoteAppAction(uint32_t xid, int action);
    void Disconnect();
    bool Running() const { return m_running.load(); }
    // True when the far end echoes what we type (SSH/Rlogin always; Telnet
    // once the server negotiated ECHO; Raw/Serial never). Drives the
    // "local echo: auto" and "local line editing: auto" settings.
    bool RemoteEcho() const { return m_remoteEcho.load(); }
    // Whether the last close was clean (remote EOF / user) rather than an
    // error — "close window on exit: only on clean exit".
    bool CleanClose() const { return m_cleanClose.load(); }

    bool PollEvent(SshEvent& ev);
    SpscRing& Output() { return m_output; }

private:
    void ThreadMain(SshConfig cfg);          // SSH
    void ThreadMainStream(SshConfig cfg);    // Telnet / Rlogin / Raw
    void ThreadMainSerial(SshConfig cfg);    // COM port
    void ThreadMainLocal(SshConfig cfg);     // local console via ConPTY
    void PostEvent(SshEventType type, std::string text = {});
    // Forward listener / tunnel bookkeeping lives in the .cpp anonymous
    // namespace (FwdListener / FwdTunnel).

    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<uint32_t> m_rttUs{ 0 };
    std::atomic<uint32_t> m_retrans{ 0 };
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_remoteEcho{ true };
    std::atomic<bool> m_cleanClose{ false };

    SpscRing m_output;

    std::mutex m_outMutex;                 // keystrokes → server
    std::vector<uint8_t> m_outQueue;
    std::vector<uint8_t> m_cmdQueue;       // telnet commands (IAC x)

    std::mutex m_evMutex;
    std::deque<SshEvent> m_events;

    std::mutex m_hkMutex;                  // host-key prompt handshake
    std::condition_variable m_hkCv;
    bool m_hkDecided = false;
    bool m_hkAccepted = false;

    std::atomic<int> m_pendingCols{ 0 };
    std::atomic<int> m_pendingRows{ 0 };
    std::atomic<bool> m_resizePending{ false };
    // Forwards added while the session is up (RemoteApp). Drained by the
    // worker; the mutex is held only long enough to move the strings out.
    std::mutex m_fwdMutex;
    std::vector<std::string> m_pendingForwards;
    // Clipboard text waiting to go to the AmberX host. One value: a
    // clipboard has one current content, and an older copy is not worth
    // delivering late.
    std::mutex m_clipMutex;
    std::string m_pendingClipboard;
    bool m_clipPending = false;
    // The AmberX report, written by the worker and read by the UI thread,
    // and the window actions going the other way.
    mutable std::mutex m_amberxMutex;
    RemoteAppReport m_amberxReport;
    std::vector<std::pair<uint32_t, int>> m_pendingWindowActions;
    // The AmberX host for this session, when x11Backend == 1. Owned by the
    // worker thread: created after the x11-req succeeds, stopped as the
    // thread exits. Its Job Object ends the host if this process dies first.
    std::unique_ptr<amber::amberx::AmberXController> m_amberx;

    std::atomic<uintptr_t> m_socket{ ~0ull };   // for abortive close on cancel
    std::atomic<uintptr_t> m_serial{ 0 };       // COM handle for abort
    // Local: the pseudoconsole read end. Disconnect only CANCELS a pending
    // read on it to wake the reader thread — the ConPty owns and closes it.
    std::atomic<uintptr_t> m_localRead{ 0 };
};
