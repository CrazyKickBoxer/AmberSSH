#include "ProfileStore.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include "../platform/Paths.h"

using nlohmann::json;

namespace amber
{

std::string MakeUuid()
{
    unsigned char bytes[16] = {};
    NTSTATUS st = BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0)
    {
        // Deterministic fallback keeps profile creation working if the CSPRNG
        // is unavailable; mixes tick count with a counter for uniqueness.
        static unsigned counter = 0;
        unsigned long long t = GetTickCount64();
        ++counter;
        std::memcpy(bytes, &t, sizeof(t));
        std::memcpy(bytes + 8, &counter, sizeof(counter));
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // variant

    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out.push_back('-');
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

namespace
{

// Tolerant field readers: a wrong type yields the fallback instead of throwing,
// so one bad field cannot take the whole file down.
template <typename T>
T Get(const json& j, const char* key, T fallback)
{
    auto it = j.find(key);
    if (it == j.end())
        return fallback;
    try
    {
        return it->template get<T>();
    }
    catch (const json::exception&)
    {
        return fallback;
    }
}

// Enum fields are stored as small integers; readers clamp to the valid range
// so a hand-edited file can never produce an out-of-range enum.
template <typename E>
int EnumInt(E e) { return static_cast<int>(e); }

template <typename E>
E EnumFrom(const json& j, const char* key, E fallback, int maxValue)
{
    int v = Get<int>(j, key, static_cast<int>(fallback));
    if (v < 0 || v > maxValue)
        return fallback;
    return static_cast<E>(v);
}

json ToJson(const ConnectionProfile& p)
{
    return json{
        // Session
        {"id", p.id},
        {"name", p.name},
        {"host", p.host},
        {"port", p.port},
        {"username", p.username},
        {"protocol", ProtocolName(p.protocol)},
        {"closeOnExit", EnumInt(p.closeOnExit)},
        // Logging
        {"logMode", EnumInt(p.logMode)},
        {"logFile", p.logFile},
        {"logAppend", p.logAppend},
        {"logFlush", p.logFlush},
        // Terminal
        {"autoWrap", p.autoWrap},
        {"implicitCr", p.implicitCr},
        {"implicitLf", p.implicitLf},
        {"localEcho", EnumInt(p.localEcho)},
        {"localLineEdit", EnumInt(p.localLineEdit)},
        {"answerback", p.answerback},
        // Keyboard
        {"backspaceIsDel", p.backspaceIsDel},
        {"homeEnd", EnumInt(p.homeEnd)},
        {"fnKeys", EnumInt(p.fnKeys)},
        {"appCursorInitial", p.appCursorInitial},
        {"appKeypadInitial", p.appKeypadInitial},
        // Bell
        {"bell", EnumInt(p.bell)},
        {"bellTaskbar", p.bellTaskbar},
        {"bellOverload", p.bellOverload},
        // Features
        {"allowAppCursor", p.allowAppCursor},
        {"allowAppKeypad", p.allowAppKeypad},
        {"allowMouse", p.allowMouse},
        {"allowRemoteResize", p.allowRemoteResize},
        {"allowAltScreen", p.allowAltScreen},
        {"allowRemoteTitle", p.allowRemoteTitle},
        {"allowScrollbackClear", p.allowScrollbackClear},
        // Window
        {"cols", p.cols},
        {"rows", p.rows},
        {"resizeAction", EnumInt(p.resizeAction)},
        {"scrollbackLines", p.scrollbackLines},
        {"scrollOnKey", p.scrollOnKey},
        {"scrollOnOutput", p.scrollOnOutput},
        // Appearance
        {"cursor", EnumInt(p.cursor)},
        {"cursorBlink", p.cursorBlink},
        {"fontFamily", p.fontFamily},
        {"fontSize", p.fontSize},
        {"gapPx", p.gapPx},
        // Behaviour
        {"windowTitle", p.windowTitle},
        {"warnOnClose", p.warnOnClose},
        {"altF4Closes", p.altF4Closes},
        {"altSpaceMenu", p.altSpaceMenu},
        {"altEnterFullscreen", p.altEnterFullscreen},
        // Translation
        {"charset", EnumInt(p.charset)},
        {"poorMansLineDrawing", p.poorMansLineDrawing},
        // Selection
        {"mouseButtons", EnumInt(p.mouseButtons)},
        {"shiftOverridesMouse", p.shiftOverridesMouse},
        {"rectSelectDefault", p.rectSelectDefault},
        {"autoCopy", p.autoCopy},
        // Colours
        {"palette", p.palette},
        {"themeId", p.themeId},
        {"allowAnsiColours", p.allowAnsiColours},
        {"allow256Colours", p.allow256Colours},
        {"boldStyle", EnumInt(p.boldStyle)},
        // Connection
        {"connectTimeoutSeconds", p.connectTimeoutSeconds},
        {"keepaliveSeconds", p.keepaliveSeconds},
        // Guardian. "autoReconnect" is the Stage 1 key and is still written
        // so an older build reading this file behaves the way it used to;
        // "reconnectMode" is the truth and wins on load when present.
        {"autoReconnect", p.reconnectMode != ReconnectMode::Off},
        {"reconnectMode", EnumInt(p.reconnectMode)},
        {"reconnectMaxAttempts", p.reconnectMaxAttempts},
        {"reconnectJitterPercent", p.reconnectJitterPercent},
        {"reconnectNotify", p.reconnectNotify},
        {"reconnectBanner", p.reconnectBanner},
        {"reattachMode", EnumInt(p.reattachMode)},
        {"reattachSession", p.reattachSession},
        {"reattachCommand", p.reattachCommand},
        {"restoreCwd", p.restoreCwd},
        {"restoreForwards", p.restoreForwards},
        {"notifyCommands", p.notifyCommands},
        {"notifyAfterSeconds", p.notifyAfterSeconds},
        {"tcpNoDelay", p.tcpNoDelay},
        {"tcpKeepalive", p.tcpKeepalive},
        {"ipVersion", p.ipVersion},
        {"logicalHost", p.logicalHost},
        // Data
        {"termType", p.termType},
        {"termSpeed", p.termSpeed},
        {"envVars", p.envVars},
        // Proxy
        {"proxyType", EnumInt(p.proxyType)},
        {"proxyHost", p.proxyHost},
        {"proxyPort", p.proxyPort},
        {"proxyUser", p.proxyUser},
        {"rememberProxyPassword", p.rememberProxyPassword},
        {"proxyExclude", p.proxyExclude},
        {"proxyLocalhost", p.proxyLocalhost},
        {"proxyDns", p.proxyDns},
        // SSH
        {"auth", AuthMethodName(p.auth)},
        {"privateKeyPath", p.privateKeyPath},
        {"publicKeyPath", p.publicKeyPath},
        {"rememberPassword", p.rememberPassword},
        {"rememberPassphrase", p.rememberPassphrase},
        {"remoteCommand", p.remoteCommand},
        {"noShell", p.noShell},
        {"compression", p.compression},
        {"cipherPref", p.cipherPref},
        {"kexPref", p.kexPref},
        {"hostKeyPref", p.hostKeyPref},
        {"agentForward", p.agentForward},
        {"x11Forward", p.x11Forward},
        {"x11Display", p.x11Display},
        {"x11Backend", p.x11Backend},
        {"x11Trust", p.x11Trust == 1 ? 1 : 0},   // session-only trust is not saved
        {"x11Clipboard", p.x11Clipboard},
        {"remoteGui", p.remoteGui},
        {"windowMode", p.windowMode},
        {"displayMode", p.displayMode},
        {"displayW", p.displayW},
        {"displayH", p.displayH},
        {"perfMode", p.perfMode},
        {"manualHostKeys", p.manualHostKeys},
        {"forwards", p.forwards},
        {"jumpHost", p.jumpHost},
        // Serial
        {"localShellKey", p.localShellKey},
        {"localExe", p.localExe},
        {"localArgs", p.localArgs},
        {"localCwd", p.localCwd},
        {"localEnv", p.localEnv},
        {"localShellIntegration", p.localShellIntegration},
        {"serialPort", p.serialPort},
        {"serialBaud", p.serialBaud},
        {"serialDataBits", p.serialDataBits},
        {"serialStopBits", p.serialStopBits},
        {"serialParity", EnumInt(p.serialParity)},
        {"serialFlow", EnumInt(p.serialFlow)},
        // Telnet
        {"telnetPassive", p.telnetPassive},
        {"telnetKeyboard", p.telnetKeyboard},
        {"telnetNewline", p.telnetNewline},
        // Rlogin
        {"rloginLocalUser", p.rloginLocalUser},
        // Effects
        {"effectPreset", p.effectPreset},
        {"densityPpc", p.densityPpc},
    };
}

bool FromJson(const json& j, ConnectionProfile& out)
{
    if (!j.is_object())
        return false;

    const ConnectionProfile d;   // defaults

    // Session
    out.id = Get<std::string>(j, "id", std::string());
    out.name = Get<std::string>(j, "name", std::string());
    out.host = Get<std::string>(j, "host", std::string());
    out.port = Get<int>(j, "port", 22);
    out.username = Get<std::string>(j, "username", std::string());
    out.protocol = ProtocolFromName(Get<std::string>(j, "protocol", std::string("ssh")));
    out.closeOnExit = EnumFrom(j, "closeOnExit", d.closeOnExit, 2);
    // Logging
    out.logMode = EnumFrom(j, "logMode", d.logMode, 2);
    out.logFile = Get<std::string>(j, "logFile", d.logFile);
    out.logAppend = Get<bool>(j, "logAppend", d.logAppend);
    out.logFlush = Get<bool>(j, "logFlush", d.logFlush);
    // Terminal
    out.autoWrap = Get<bool>(j, "autoWrap", d.autoWrap);
    out.implicitCr = Get<bool>(j, "implicitCr", d.implicitCr);
    out.implicitLf = Get<bool>(j, "implicitLf", d.implicitLf);
    out.localEcho = EnumFrom(j, "localEcho", d.localEcho, 2);
    out.localLineEdit = EnumFrom(j, "localLineEdit", d.localLineEdit, 2);
    out.answerback = Get<std::string>(j, "answerback", d.answerback);
    // Keyboard
    out.backspaceIsDel = Get<bool>(j, "backspaceIsDel", d.backspaceIsDel);
    out.homeEnd = EnumFrom(j, "homeEnd", d.homeEnd, 2);
    out.fnKeys = EnumFrom(j, "fnKeys", d.fnKeys, 3);
    out.appCursorInitial = Get<bool>(j, "appCursorInitial", d.appCursorInitial);
    out.appKeypadInitial = Get<bool>(j, "appKeypadInitial", d.appKeypadInitial);
    // Bell
    out.bell = EnumFrom(j, "bell", d.bell, 3);
    out.bellTaskbar = Get<bool>(j, "bellTaskbar", d.bellTaskbar);
    out.bellOverload = Get<bool>(j, "bellOverload", d.bellOverload);
    // Features
    out.allowAppCursor = Get<bool>(j, "allowAppCursor", true);
    out.allowAppKeypad = Get<bool>(j, "allowAppKeypad", true);
    out.allowMouse = Get<bool>(j, "allowMouse", true);
    out.allowRemoteResize = Get<bool>(j, "allowRemoteResize", true);
    out.allowAltScreen = Get<bool>(j, "allowAltScreen", true);
    out.allowRemoteTitle = Get<bool>(j, "allowRemoteTitle", true);
    out.allowScrollbackClear = Get<bool>(j, "allowScrollbackClear", true);
    // Window
    out.cols = Get<int>(j, "cols", 80);
    out.rows = Get<int>(j, "rows", 25);
    out.resizeAction = EnumFrom(j, "resizeAction", d.resizeAction, 2);
    out.scrollbackLines = Get<int>(j, "scrollbackLines", d.scrollbackLines);
    out.scrollOnKey = Get<bool>(j, "scrollOnKey", d.scrollOnKey);
    out.scrollOnOutput = Get<bool>(j, "scrollOnOutput", d.scrollOnOutput);
    // Appearance
    out.cursor = EnumFrom(j, "cursor", d.cursor, 2);
    out.cursorBlink = Get<bool>(j, "cursorBlink", d.cursorBlink);
    out.fontFamily = Get<std::string>(j, "fontFamily", std::string());
    out.fontSize = Get<float>(j, "fontSize", 0.0f);
    out.gapPx = Get<int>(j, "gapPx", d.gapPx);
    // Behaviour
    out.windowTitle = Get<std::string>(j, "windowTitle", std::string());
    out.warnOnClose = Get<bool>(j, "warnOnClose", d.warnOnClose);
    out.altF4Closes = Get<bool>(j, "altF4Closes", d.altF4Closes);
    out.altSpaceMenu = Get<bool>(j, "altSpaceMenu", d.altSpaceMenu);
    out.altEnterFullscreen = Get<bool>(j, "altEnterFullscreen", d.altEnterFullscreen);
    // Translation
    out.charset = EnumFrom(j, "charset", d.charset, 3);
    out.poorMansLineDrawing = Get<bool>(j, "poorMansLineDrawing", false);
    // Selection
    out.mouseButtons = EnumFrom(j, "mouseButtons", d.mouseButtons, 2);
    out.shiftOverridesMouse = Get<bool>(j, "shiftOverridesMouse", true);
    out.rectSelectDefault = Get<bool>(j, "rectSelectDefault", false);
    out.autoCopy = Get<bool>(j, "autoCopy", true);
    // Colours
    out.palette = Get<int>(j, "palette", -1);
    out.themeId = Get<int>(j, "themeId", -1);
    out.allowAnsiColours = Get<bool>(j, "allowAnsiColours", true);
    out.allow256Colours = Get<bool>(j, "allow256Colours", true);
    out.boldStyle = EnumFrom(j, "boldStyle", d.boldStyle, 2);
    // Connection
    out.connectTimeoutSeconds = Get<int>(j, "connectTimeoutSeconds", 15);
    out.keepaliveSeconds = Get<int>(j, "keepaliveSeconds", 30);
    // Guardian. A file written before Stage 2 has only "autoReconnect", so
    // that is what the mode defaults to; once "reconnectMode" is present it
    // is authoritative and the old key is ignored.
    {
        const bool legacy = Get<bool>(j, "autoReconnect", false);
        const ReconnectMode fallback =
            legacy ? ReconnectMode::Automatic : ReconnectMode::Off;
        out.reconnectMode = EnumFrom(j, "reconnectMode", fallback, 2);
    }
    out.reconnectMaxAttempts = std::clamp(Get<int>(j, "reconnectMaxAttempts", 6), 0, 1000);
    out.reconnectJitterPercent = std::clamp(Get<int>(j, "reconnectJitterPercent", 20), 0, 50);
    out.reconnectNotify = Get<bool>(j, "reconnectNotify", true);
    out.reconnectBanner = Get<bool>(j, "reconnectBanner", true);
    out.reattachMode = EnumFrom(j, "reattachMode", d.reattachMode, 3);
    out.reattachSession = Get<std::string>(j, "reattachSession", d.reattachSession);
    out.reattachCommand = Get<std::string>(j, "reattachCommand", std::string());
    out.restoreCwd = Get<bool>(j, "restoreCwd", false);
    out.restoreForwards = Get<bool>(j, "restoreForwards", true);
    out.notifyCommands = std::clamp(Get<int>(j, "notifyCommands", -1), -1, 3);
    out.notifyAfterSeconds = std::clamp(Get<int>(j, "notifyAfterSeconds", -1), -1, 86400);
    out.tcpNoDelay = Get<bool>(j, "tcpNoDelay", true);
    out.tcpKeepalive = Get<bool>(j, "tcpKeepalive", false);
    out.ipVersion = Get<int>(j, "ipVersion", 0);
    out.logicalHost = Get<std::string>(j, "logicalHost", std::string());
    // Data
    out.termType = Get<std::string>(j, "termType", d.termType);
    out.termSpeed = Get<std::string>(j, "termSpeed", d.termSpeed);
    out.envVars = Get<std::string>(j, "envVars", std::string());
    // Proxy
    out.proxyType = EnumFrom(j, "proxyType", d.proxyType, 3);
    out.proxyHost = Get<std::string>(j, "proxyHost", std::string());
    out.proxyPort = Get<int>(j, "proxyPort", d.proxyPort);
    out.proxyUser = Get<std::string>(j, "proxyUser", std::string());
    out.rememberProxyPassword = Get<bool>(j, "rememberProxyPassword", false);
    out.proxyExclude = Get<std::string>(j, "proxyExclude", std::string());
    out.proxyLocalhost = Get<bool>(j, "proxyLocalhost", false);
    out.proxyDns = Get<bool>(j, "proxyDns", true);
    // SSH
    out.auth = AuthMethodFromName(Get<std::string>(j, "auth", std::string("password")));
    out.privateKeyPath = Get<std::string>(j, "privateKeyPath", std::string());
    out.publicKeyPath = Get<std::string>(j, "publicKeyPath", std::string());
    out.rememberPassword = Get<bool>(j, "rememberPassword", false);
    out.rememberPassphrase = Get<bool>(j, "rememberPassphrase", false);
    out.remoteCommand = Get<std::string>(j, "remoteCommand", std::string());
    out.noShell = Get<bool>(j, "noShell", false);
    out.compression = Get<bool>(j, "compression", false);
    out.cipherPref = Get<std::string>(j, "cipherPref", std::string());
    out.kexPref = Get<std::string>(j, "kexPref", std::string());
    out.hostKeyPref = Get<std::string>(j, "hostKeyPref", std::string());
    out.agentForward = Get<bool>(j, "agentForward", false);
    out.x11Forward = Get<bool>(j, "x11Forward", false);
    out.x11Display = Get<std::string>(j, "x11Display", d.x11Display);
    out.x11Backend = std::clamp(Get<int>(j, "x11Backend", 0), 0, 1);
    out.x11Trust = std::clamp(Get<int>(j, "x11Trust", 0), 0, 1);
    out.x11Clipboard = std::clamp(Get<int>(j, "x11Clipboard", 0), 0, 4);
    // Remote GUI. A profile saved before this field existed does not have the
    // key, and defaulting it to "off" would turn a working remote GUI off on
    // upgrade — so its absence is answered from the three fields that used to
    // carry the same meaning. Present-but-zero still means off.
    if (j.contains("remoteGui"))
        out.remoteGui = std::clamp(Get<int>(j, "remoteGui", 0), 0, 2);
    else
        out.remoteGui = (out.x11Forward && out.x11Backend == 1)
                            ? (out.x11Trust != 0 ? 2 : 1)
                            : 0;
    out.windowMode = std::clamp(Get<int>(j, "windowMode", 0), 0, 3);
    out.displayMode = std::clamp(Get<int>(j, "displayMode", 0), 0, 2);
    out.displayW = std::clamp(Get<int>(j, "displayW", 1920), 320, 16384);
    out.displayH = std::clamp(Get<int>(j, "displayH", 1080), 240, 16384);
    out.perfMode = std::clamp(Get<int>(j, "perfMode", 0), 0, 3);
    out.manualHostKeys = Get<std::string>(j, "manualHostKeys", std::string());
    out.forwards = Get<std::string>(j, "forwards", std::string());
    out.jumpHost = Get<std::string>(j, "jumpHost", std::string());
    // Serial
    out.localShellKey = Get<std::string>(j, "localShellKey", d.localShellKey);
    out.localExe = Get<std::string>(j, "localExe", d.localExe);
    out.localArgs = Get<std::string>(j, "localArgs", d.localArgs);
    out.localCwd = Get<std::string>(j, "localCwd", d.localCwd);
    out.localEnv = Get<std::string>(j, "localEnv", d.localEnv);
    out.localShellIntegration = Get<bool>(j, "localShellIntegration", d.localShellIntegration);
    out.serialPort = Get<std::string>(j, "serialPort", d.serialPort);
    out.serialBaud = Get<int>(j, "serialBaud", d.serialBaud);
    out.serialDataBits = Get<int>(j, "serialDataBits", d.serialDataBits);
    out.serialStopBits = Get<int>(j, "serialStopBits", d.serialStopBits);
    out.serialParity = EnumFrom(j, "serialParity", d.serialParity, 4);
    out.serialFlow = EnumFrom(j, "serialFlow", d.serialFlow, 3);
    // Telnet
    out.telnetPassive = Get<bool>(j, "telnetPassive", false);
    out.telnetKeyboard = Get<bool>(j, "telnetKeyboard", false);
    out.telnetNewline = Get<bool>(j, "telnetNewline", false);
    // Rlogin
    out.rloginLocalUser = Get<std::string>(j, "rloginLocalUser", std::string());
    // Effects
    out.effectPreset = Get<std::string>(j, "effectPreset", std::string());
    out.densityPpc = Get<int>(j, "densityPpc", 0);

    // Repair rather than reject: a profile missing only its id is still useful.
    if (out.id.empty())
        out.id = MakeUuid();
    if (out.port < 0 || out.port > 65535)
        out.port = ProtocolDefaultPort(out.protocol);
    if (out.port == 0 && out.protocol != Protocol::Serial && out.protocol != Protocol::Raw &&
        out.protocol != Protocol::Local)
        out.port = ProtocolDefaultPort(out.protocol);
    if (out.cols < 20 || out.cols > 1000)
        out.cols = 80;
    if (out.rows < 5 || out.rows > 500)
        out.rows = 25;
    if (out.connectTimeoutSeconds < 1 || out.connectTimeoutSeconds > 600)
        out.connectTimeoutSeconds = 15;
    if (out.keepaliveSeconds < 0 || out.keepaliveSeconds > 3600)
        out.keepaliveSeconds = 30;
    if (out.scrollbackLines < 0 || out.scrollbackLines > 200000)
        out.scrollbackLines = 5000;
    if (out.gapPx < 0 || out.gapPx > 64)
        out.gapPx = 8;
    if (out.ipVersion < 0 || out.ipVersion > 2)
        out.ipVersion = 0;
    if (out.proxyPort <= 0 || out.proxyPort > 65535)
        out.proxyPort = 1080;
    if (out.serialBaud < 50 || out.serialBaud > 4000000)
        out.serialBaud = 9600;
    if (out.serialDataBits < 5 || out.serialDataBits > 8)
        out.serialDataBits = 8;
    if (out.serialStopBits != 1 && out.serialStopBits != 2 && out.serialStopBits != 15)
        out.serialStopBits = 1;
    if (out.palette < -1 || out.palette > 1)
        out.palette = -1;
    if (out.densityPpc < 0 || out.densityPpc > 256)
        out.densityPpc = 0;

    // A destination is the one field we cannot invent.
    if (out.protocol == Protocol::Local)
        return !out.localExe.empty() || !out.localShellKey.empty();
    return out.protocol == Protocol::Serial ? !out.serialPort.empty()
                                            : !out.host.empty();
}

} // namespace

bool ProfileStore::LoadFrom(const std::filesystem::path& file, LoadReport* reportOut)
{
    m_profiles.clear();
    LoadReport report;

    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        // An absent file is a valid empty store, not an error.
        if (reportOut)
            *reportOut = report;
        return true;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();

    json root;
    try
    {
        root = json::parse(buffer.str(), nullptr, true, /*ignore_comments*/ true);
    }
    catch (const json::exception& e)
    {
        report.fileError = e.what();
        if (reportOut)
            *reportOut = report;
        return false;
    }

    if (!root.is_object())
    {
        report.fileError = "profiles.json root is not an object";
        if (reportOut)
            *reportOut = report;
        return false;
    }

    report.schemaVersion =
        Get<int>(root, "schemaVersion", ConnectionProfile::kSchemaVersion);

    auto it = root.find("profiles");
    if (it != root.end() && it->is_array())
    {
        for (const auto& entry : *it)
        {
            ConnectionProfile p;
            if (FromJson(entry, p))
                m_profiles.push_back(std::move(p));
            else
                ++report.skipped;   // malformed entry: drop it, keep the rest
        }
    }

    if (reportOut)
        *reportOut = report;
    return true;
}

bool ProfileStore::SaveTo(const std::filesystem::path& file, std::string* errorOut) const
{
    json root;
    root["schemaVersion"] = ConnectionProfile::kSchemaVersion;
    json array = json::array();
    for (const auto& p : m_profiles)
        array.push_back(ToJson(p));
    root["profiles"] = std::move(array);

    // Atomic replace: write a sibling temp file, flush it, then rename over the
    // target so an interrupted write cannot truncate the existing profiles.
    std::filesystem::path temp = file;
    temp += L".tmp";

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            if (errorOut)
                *errorOut = "Unable to open " + temp.string() + " for writing.";
            return false;
        }
        out << root.dump(2) << '\n';
        out.flush();
        if (!out)
        {
            if (errorOut)
                *errorOut = "Failed while writing " + temp.string() + ".";
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec)
    {
        // std::filesystem::rename over an existing file is not guaranteed on
        // every filesystem; fall back to remove-then-rename.
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        ec.clear();
        std::filesystem::rename(temp, file, ec);
        if (ec)
        {
            if (errorOut)
                *errorOut =
                    "Unable to replace " + file.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool ProfileStore::Load(LoadReport* reportOut)
{
    return LoadFrom(ProfilesFile(), reportOut);
}

bool ProfileStore::Save(std::string* errorOut) const
{
    return SaveTo(ProfilesFile(), errorOut);
}

const ConnectionProfile* ProfileStore::Find(std::string_view id) const
{
    for (const auto& p : m_profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}

ConnectionProfile* ProfileStore::Find(std::string_view id)
{
    for (auto& p : m_profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}

const ConnectionProfile* ProfileStore::FindByName(std::string_view name) const
{
    for (const auto& p : m_profiles)
        if (p.name == name)
            return &p;
    return nullptr;
}

std::string ProfileStore::Upsert(ConnectionProfile profile)
{
    if (profile.id.empty())
        profile.id = MakeUuid();
    if (auto* existing = Find(profile.id))
    {
        *existing = std::move(profile);
        return existing->id;
    }
    std::string id = profile.id;
    m_profiles.push_back(std::move(profile));
    return id;
}

bool ProfileStore::Remove(std::string_view id)
{
    for (auto it = m_profiles.begin(); it != m_profiles.end(); ++it)
    {
        if (it->id == id)
        {
            m_profiles.erase(it);
            return true;
        }
    }
    return false;
}

} // namespace amber
