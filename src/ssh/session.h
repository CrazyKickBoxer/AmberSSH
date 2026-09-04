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

enum class SshEventType
{
    Status,          // text = progress message
    HostKeyPrompt,   // text = "keytype SHA256:fingerprint" — call AnswerHostKey
    Connected,
    Closed,          // text = reason
    Error,           // text = message
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
