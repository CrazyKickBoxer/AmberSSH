// ConnectionProfile.h — a saved session. Secrets are NEVER members of this
// struct; only booleans recording whether a secret exists in the Windows
// Credential Manager under this profile's id.
//
// The option set mirrors PuTTY's configuration tree (Session, Logging,
// Terminal, Keyboard, Bell, Features, Window, Appearance, Behaviour,
// Translation, Selection, Colours, Connection, Data, Proxy, SSH, Serial,
// Telnet, Rlogin). Every field here is honoured somewhere: the dialog edits
// it, the store persists it, and the app / transport / parser consume it.
#pragma once

#include <string>

namespace amber
{

enum class AuthMethod
{
    Password = 0,
    PublicKey = 1,
    KeyboardInteractive = 2,
    Agent = 3,                  // Windows OpenSSH agent / Pageant
};

inline const char* AuthMethodName(AuthMethod m)
{
    switch (m)
    {
    case AuthMethod::PublicKey:           return "publickey";
    case AuthMethod::KeyboardInteractive: return "keyboard-interactive";
    case AuthMethod::Agent:               return "agent";
    case AuthMethod::Password:            default: return "password";
    }
}

inline AuthMethod AuthMethodFromName(const std::string& name)
{
    if (name == "publickey")            return AuthMethod::PublicKey;
    if (name == "keyboard-interactive") return AuthMethod::KeyboardInteractive;
    if (name == "agent")                return AuthMethod::Agent;
    return AuthMethod::Password;
}

// Connection type (PuTTY: SSH / Serial / Other: Telnet, Rlogin, Raw), plus
// VNC — a remote desktop rather than a terminal: the tab carries a
// framebuffer instead of a grid (sessions/Session.h, `vnc`).
enum class Protocol { Ssh = 0, Telnet = 1, Rlogin = 2, Raw = 3, Serial = 4, Local = 5, Vnc = 6 };

inline const char* ProtocolName(Protocol p)
{
    switch (p)
    {
    case Protocol::Telnet: return "telnet";
    case Protocol::Rlogin: return "rlogin";
    case Protocol::Raw:    return "raw";
    case Protocol::Serial: return "serial";
    case Protocol::Local:  return "local";
    case Protocol::Vnc:    return "vnc";
    case Protocol::Ssh:    default: return "ssh";
    }
}

inline Protocol ProtocolFromName(const std::string& n)
{
    if (n == "telnet") return Protocol::Telnet;
    if (n == "rlogin") return Protocol::Rlogin;
    if (n == "raw")    return Protocol::Raw;
    if (n == "serial") return Protocol::Serial;
    if (n == "local")  return Protocol::Local;
    if (n == "vnc")    return Protocol::Vnc;
    return Protocol::Ssh;
}

inline int ProtocolDefaultPort(Protocol p)
{
    switch (p)
    {
    case Protocol::Telnet: return 23;
    case Protocol::Rlogin: return 513;
    case Protocol::Raw:    return 0;
    case Protocol::Serial: return 0;
    case Protocol::Local:  return 0;
    case Protocol::Vnc:    return 5900;
    case Protocol::Ssh:    default: return 22;
    }
}

enum class CloseOnExit  { Always = 0, Never = 1, CleanOnly = 2 };
enum class LogMode      { None = 0, Printable = 1, All = 2 };
enum class BellStyle    { None = 0, Visual = 1, Beep = 2, Both = 3 };
enum class TriState     { Auto = 0, Off = 1, On = 2 };     // local echo / line editing
enum class FnKeysMode   { Xterm = 0, Linux = 1, Vt100Plus = 2, Sco = 3 };
enum class HomeEndMode  { Xterm = 0, Standard = 1, Rxvt = 2 };
enum class Charset      { Utf8 = 0, Latin1 = 1, Cp1252 = 2, Cp437 = 3 };
enum class MouseButtons { Compromise = 0, Windows = 1, Xterm = 2 };
enum class ProxyType    { None = 0, Socks4 = 1, Socks5 = 2, Http = 3 };
enum class ResizeAction { Cells = 0, Font = 1, Forbid = 2 };
enum class CursorShape  { Block = 0, Underline = 1, Bar = 2 };
enum class BoldStyle    { Colour = 0, Font = 1, Both = 2 };
enum class SerialParity { None = 0, Odd = 1, Even = 2, Mark = 3, Space = 4 };
enum class SerialFlow   { None = 0, XonXoff = 1, RtsCts = 2, DsrDtr = 3 };

// Session Guardian. What happens when a connection that WAS up goes away.
// Off is the default: reconnecting is a thing the user opts into, per profile.
enum class ReconnectMode { Off = 0, Ask = 1, Automatic = 2 };

// How a reconnected session rejoins work that was already running. Only a
// persistent multiplexer can actually do that; None is honest about the fact
// that a plain shell's processes died with the old connection.
enum class ReattachMode  { None = 0, Tmux = 1, Screen = 2, Custom = 3 };

struct ConnectionProfile
{
    static constexpr int kSchemaVersion = 2;

    // ---- Session ----------------------------------------------------------
    std::string id;                 // UUID, also the credential-target key
    std::string name;               // display name in Saved Sessions
    std::string host;               // host name / IP (Serial: unused)
    int         port = 22;
    std::string username;
    Protocol    protocol = Protocol::Ssh;
    CloseOnExit closeOnExit = CloseOnExit::CleanOnly;

    // ---- Session > Logging ------------------------------------------------
    // Log file name accepts PuTTY's substitutions: &Y &M &D (date), &T (time),
    // &H (host), &P (port).
    LogMode     logMode = LogMode::None;
    std::string logFile = "amberssh-&H-&Y&M&D-&T.log";
    bool        logAppend = true;       // false = overwrite an existing file
    bool        logFlush = true;        // flush after every write

    // ---- Terminal ---------------------------------------------------------
    bool        autoWrap = true;        // initial DECAWM state
    bool        implicitCr = false;     // CR in every LF
    bool        implicitLf = false;     // LF in every CR
    TriState    localEcho = TriState::Auto;
    TriState    localLineEdit = TriState::Auto;
    std::string answerback = "AmberSSH";

    // ---- Terminal > Keyboard ---------------------------------------------
    bool        backspaceIsDel = true;  // Control-? (127) vs Control-H
    HomeEndMode homeEnd = HomeEndMode::Xterm;
    FnKeysMode  fnKeys = FnKeysMode::Xterm;
    bool        appCursorInitial = false;
    bool        appKeypadInitial = false;

    // ---- Terminal > Bell --------------------------------------------------
    BellStyle   bell = BellStyle::Visual;
    bool        bellTaskbar = true;     // flash the taskbar entry when unfocused
    bool        bellOverload = true;    // mute after a burst of bells

    // ---- Terminal > Features ---------------------------------------------
    bool        allowAppCursor = true;
    bool        allowAppKeypad = true;
    bool        allowMouse = true;
    bool        allowRemoteResize = true;
    bool        allowAltScreen = true;
    bool        allowRemoteTitle = true;
    bool        allowScrollbackClear = true;

    // ---- Window -----------------------------------------------------------
    int         cols = 80;
    int         rows = 25;
    ResizeAction resizeAction = ResizeAction::Cells;
    int         scrollbackLines = 5000;
    bool        scrollOnKey = true;
    bool        scrollOnOutput = false;

    // ---- Window > Appearance ---------------------------------------------
    CursorShape cursor = CursorShape::Block;
    bool        cursorBlink = true;
    std::string fontFamily;          // empty = follow global settings
    float       fontSize = 0.0f;     // px; 0 = follow global settings
    int         gapPx = 8;           // gap between text and window edge

    // ---- Window > Behaviour ----------------------------------------------
    std::string windowTitle;         // fixed title (overrides remote titles)
    bool        warnOnClose = true;
    bool        altF4Closes = true;
    bool        altSpaceMenu = true;
    bool        altEnterFullscreen = true;

    // ---- Window > Translation --------------------------------------------
    Charset     charset = Charset::Utf8;
    bool        poorMansLineDrawing = false;   // +, - and | instead of Unicode

    // ---- Window > Selection ----------------------------------------------
    MouseButtons mouseButtons = MouseButtons::Compromise;
    bool        shiftOverridesMouse = true;
    bool        rectSelectDefault = false;     // Alt+drag toggles the other mode
    bool        autoCopy = true;               // copy on mouse-up

    // ---- Window > Colours ------------------------------------------------
    int         palette = -1;        // -1 global, 0 Amber Miami, 1 classic xterm
    int         themeId = -1;        // -1 global, else a theme index
    bool        allowAnsiColours = true;
    bool        allow256Colours = true;
    BoldStyle   boldStyle = BoldStyle::Colour;

    // ---- Connection -------------------------------------------------------
    int         connectTimeoutSeconds = 15;
    int         keepaliveSeconds = 30;
    bool        tcpNoDelay = true;
    bool        tcpKeepalive = false;
    int         ipVersion = 0;       // 0 auto, 1 IPv4, 2 IPv6
    std::string logicalHost;         // known_hosts name override

    // ---- Connection > Guardian -------------------------------------------
    // Reconnect only ever arms after a session has connected once; a failure
    // on the FIRST connect is a configuration problem, not a dropped link,
    // and looping on it would only replay the same mistake.
    ReconnectMode reconnectMode = ReconnectMode::Off;
    int         reconnectMaxAttempts = 6;    // 0 = keep trying until stopped
    int         reconnectJitterPercent = 20; // spreads a fleet of tabs apart
    bool        reconnectNotify = true;      // toast on success / give-up
    bool        reconnectBanner = true;      // in-terminal annotation line
    // Rejoining remote work. tmux/screen attach if the session exists and
    // create it otherwise; neither form kills or detaches anything.
    ReattachMode reattachMode = ReattachMode::None;
    std::string reattachSession = "amberssh";   // tmux/screen session name
    std::string reattachCommand;                // Custom mode, run verbatim
    // AmberSSH-managed state to put back. The working directory is only
    // restored when there is no multiplexer — tmux and screen bring their
    // own panes back, each already where it was.
    bool        restoreCwd = false;
    bool        restoreForwards = true;

    // ---- Terminal > Command blocks (OSC 133) ------------------------------
    // Tell me when a long command finishes. -1 on either follows the global
    // setting, matching how palette / themeId defer. A command shorter than
    // the threshold never notifies — that is the whole point of it.
    int         notifyCommands = -1;      // -1 global, else amber::NotifyOn
    int         notifyAfterSeconds = -1;  // -1 global, else seconds

    // ---- Connection > Data -----------------------------------------------
    std::string termType = "xterm-256color";
    std::string termSpeed = "38400,38400";
    std::string envVars;             // one NAME=value per line

    // ---- Connection > Proxy ----------------------------------------------
    ProxyType   proxyType = ProxyType::None;
    std::string proxyHost;
    int         proxyPort = 1080;
    std::string proxyUser;
    bool        rememberProxyPassword = false;
    std::string proxyExclude;        // "*.local, 10.*, host" — connect directly
    bool        proxyLocalhost = false;
    bool        proxyDns = true;     // let the proxy resolve the host name

    // ---- Connection > SSH ------------------------------------------------
    AuthMethod  auth = AuthMethod::Password;
    std::string privateKeyPath;
    std::string publicKeyPath;
    bool        rememberPassword = false;   // secret lives in Credential Manager
    bool        rememberPassphrase = false;
    std::string remoteCommand;       // instead of a shell
    bool        noShell = false;     // tunnels only
    bool        compression = false;
    std::string cipherPref;          // comma list, empty = libssh2 default
    std::string kexPref;
    std::string hostKeyPref;
    bool        agentForward = false;
    bool        x11Forward = false;
    std::string x11Display = "localhost:0";
    // 0 = an external X server at x11Display (VcXsrv, X410, ...);
    // 1 = AmberX, the built-in host process (docs/amberx/). Experimental.
    int         x11Backend = 0;
    // 0 restricted (the SECURITY extension marks every forwarded client
    // untrusted); 1 trusted, saved with the profile after a warning;
    // 2 trusted for this session only — never written to disk.
    int         x11Trust = 0;
    // AmberX clipboard policy (AMBERWIN_CLIP_* in amberx/server/amberwin.h):
    // 0 disabled, 1 ask each transfer, 2 remote to local, 3 local to remote,
    // 4 both. Disabled by default, and text only in every mode.
    int         x11Clipboard = 0;

    // ---- Remote GUI (AmberX, Phase 7) -------------------------------------
    // The one control that decides whether this profile has a remote GUI and
    // what it is allowed to do: 0 off, 1 X11 restricted, 2 X11 trusted. It is
    // the authority; x11Forward, x11Backend and x11Trust above are kept in
    // step with it, and still stand alone for the external-X-server path.
    //
    // A profile written before this field existed has none, and the loader
    // derives it from those three rather than defaulting it to off — which
    // would silently turn a working remote GUI off on upgrade.
    int         remoteGui = 0;

    // ---- VNC (Protocol::Vnc) ---------------------------------------------
    // The password, when remembered, is the profile's ordinary Password
    // secret in the Credential Manager: nothing here holds it.
    std::string vncViaProfileId;     // empty = direct TCP; else a live SSH
                                     // session (by profile id) to tunnel through
    bool        vncViewOnly = false; // no keys, pointer or clipboard go out
    int         vncDensity = 1;      // particles per framebuffer pixel, 1..4
    int         vncSolidity = 100;   // 0 loose swarm .. 100 faithful desktop
    int         vncParticleSize = 1; // 1..3 px
    int         vncDisturbance = 100;// burst strength for changed pixels, 0..200 (%)
    int         vncEncodings = 0;    // 0 ZRLE first, 1 Hextile first, 2 Raw only
    int         vncCursorMode = 0;   // 0 local particle cursor (server shape),
                                     // 1 server-drawn (no cursor extension), 2 hidden
    int         vncTls = 0;          // 0 off, 1 VeNCrypt required — never downgraded
    // Same scale as x11Clipboard: 0 disabled, 1 ask each transfer,
    // 2 remote -> local, 3 local -> remote, 4 both. Latin-1 text only.
    int         vncClipboard = 1;
    // The desktop's effects (docs/vnc.md, "Effects"). Any of them on takes
    // the desktop out of the exact-pixel contract; all off = faithful.
    bool        vncFxShock = true;       // a click sends a shockwave through the particles
    bool        vncFxEdge = true;        // edges (window borders, text) glow
    bool        vncFxHeat = true;        // changed pixels run hot and lift until they cool
    bool        vncFxMaterialise = true; // connect and resize assemble the picture from a scatter
    int         vncVividness = 130;      // saturation + contrast %, 100 = as decoded; pinned to 100 in faithful mode
    int         vncMotion = 200;         // tempo % of the swarm and the effects
    int         vncGlow = 40;            // bloom % applied to the desktop (the terminal's is 100)
    // The desktop's size, asked of the server (ExtendedDesktopSize): 0 the
    // server's own, 1 fit the window (and follow it), 2.. a preset from
    // kVncDesktopSizes, the last = custom below.
    int         vncDesktopSize = 1;
    int         vncCustomW = 1600, vncCustomH = 900;
    // Where remote windows appear: 0 native Windows windows, 1 AmberSSH tabs,
    // 2 AmberSSH panes, 3 ask per application. Only 0 is implemented; the
    // others are accepted, reported, and fall back to native (see PHASE-7-GATE).
    int         windowMode = 0;
    // The X screen the session gets: 0 all monitors (the virtual desktop),
    // 1 the monitor AmberSSH is on, 2 a fixed size.
    int         displayMode = 0;
    int         displayW = 1920;     // displayMode 2 only
    int         displayH = 1080;
    // How hard AmberX works to keep windows current: 0 auto, 1 quality,
    // 2 balanced, 3 low bandwidth. This caps how often forwarded windows
    // repaint locally; it does not compress the X11 stream, and the page
    // says so.
    int         perfMode = 0;
    std::string manualHostKeys;      // accepted fingerprints, one per line
    // Tunnels: semicolon-separated forward specs —
    //   L<listenPort>:<host>:<port>   local forward
    //   R<listenPort>:<host>:<port>   remote forward
    //   D<listenPort>                 dynamic SOCKS5 proxy
    std::string forwards;
    // Jump host ("user@host[:port]", empty user = same user).
    std::string jumpHost;

    // ---- Connection > Serial ---------------------------------------------
    std::string serialPort = "COM1";
    int         serialBaud = 9600;
    int         serialDataBits = 8;
    int         serialStopBits = 1;  // 1, 2 (15 = 1.5)
    SerialParity serialParity = SerialParity::None;
    SerialFlow  serialFlow = SerialFlow::None;


    // ---- Connection > Local (ConPTY) --------------------------------------
    // A local console session. `localShellKey` names a discovered shell
    // ("pwsh", "powershell", "cmd", "gitbash", "wsl:Ubuntu"); it is resolved
    // at launch so a profile keeps working when a shell is upgraded or moved.
    // An empty key means the executable below is the whole answer.
    std::string localShellKey;
    std::string localExe;            // console executable, bare name allowed
    std::string localArgs;
    std::string localCwd;            // empty = the user's profile directory
    std::string localEnv;            // NAME=value per line
    bool        localShellIntegration = true;   // per-session OSC 7 / OSC 133
    // Elevation is deliberately NOT a profile field. Launching elevated has
    // to be an explicit, visible action, never a property a saved session can
    // carry silently.
    // ---- Connection > Telnet ---------------------------------------------
    bool        telnetPassive = false;   // negotiation: respond only
    bool        telnetKeyboard = false;  // Ctrl+C/Z/D send IP/SUSP/EOF
    bool        telnetNewline = false;   // Return sends CR LF instead of CR NUL

    // ---- Connection > Rlogin ---------------------------------------------
    std::string rloginLocalUser;

    // ---- Effects ----------------------------------------------------------
    std::string effectPreset;        // empty = follow global settings
    int         densityPpc = 0;      // 0 = follow global settings

    bool Valid() const
    {
        if (id.empty())
            return false;
        if (protocol == Protocol::Local)
            return !localExe.empty() || !localShellKey.empty();
        if (protocol == Protocol::Serial)
            return !serialPort.empty();
        return !host.empty() && port > 0 && port <= 65535;
    }
};

// RFC 4122 version 4 UUID, lowercase, hyphenated. Uses the OS CSPRNG.
std::string MakeUuid();

} // namespace amber
