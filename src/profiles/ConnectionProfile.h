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

// Connection type (PuTTY: SSH / Serial / Other: Telnet, Rlogin, Raw).
enum class Protocol { Ssh = 0, Telnet = 1, Rlogin = 2, Raw = 3, Serial = 4 };

inline const char* ProtocolName(Protocol p)
{
    switch (p)
    {
    case Protocol::Telnet: return "telnet";
    case Protocol::Rlogin: return "rlogin";
    case Protocol::Raw:    return "raw";
    case Protocol::Serial: return "serial";
    case Protocol::Ssh:    default: return "ssh";
    }
}

inline Protocol ProtocolFromName(const std::string& n)
{
    if (n == "telnet") return Protocol::Telnet;
    if (n == "rlogin") return Protocol::Rlogin;
    if (n == "raw")    return Protocol::Raw;
    if (n == "serial") return Protocol::Serial;
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
    bool        autoReconnect = false;
    bool        tcpNoDelay = true;
    bool        tcpKeepalive = false;
    int         ipVersion = 0;       // 0 auto, 1 IPv4, 2 IPv6
    std::string logicalHost;         // known_hosts name override

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
        if (protocol == Protocol::Serial)
            return !serialPort.empty();
        return !host.empty() && port > 0 && port <= 65535;
    }
};

// RFC 4122 version 4 UUID, lowercase, hyphenated. Uses the OS CSPRNG.
std::string MakeUuid();

} // namespace amber
