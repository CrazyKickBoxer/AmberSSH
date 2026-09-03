#include "RemoteDisplay.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <shlobj.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <sstream>

namespace amber
{

const char* XServerKindName(XServerKind k)
{
    switch (k)
    {
    case XServerKind::Unknown:   return "unknown";
    case XServerKind::VcXsrv:    return "VcXsrv";
    case XServerKind::X410:      return "X410";
    case XServerKind::Xming:     return "Xming";
    case XServerKind::CygwinX:   return "Cygwin/X";
    case XServerKind::MobaXterm: return "MobaXterm";
    case XServerKind::Wsl:       return "WSLg";
    }
    return "unknown";
}

namespace
{

// Shown next to each server in the picker. The user is choosing what to
// install on their own machine, which is use rather than distribution — but
// they are entitled to know, and AmberSSH bundles none of them precisely
// because some of these terms could not be bundled.
const char* LicenceOf(XServerKind k)
{
    switch (k)
    {
    case XServerKind::VcXsrv:    return "GPLv2 — installed separately, not bundled";
    case XServerKind::X410:      return "commercial (Microsoft Store)";
    case XServerKind::Xming:     return "current releases are not free software";
    case XServerKind::CygwinX:   return "MIT server on a GPLv3 runtime";
    case XServerKind::MobaXterm: return "freemium, ships its own server";
    case XServerKind::Wsl:       return "MIT (WSLg)";
    case XServerKind::Unknown:   return "";
    }
    return "";
}

// Preference when several are installed and none is running. VcXsrv first
// because it is the one most people have and it behaves predictably; X410 is
// better integrated but not everyone has paid for it.
int Preference(XServerKind k)
{
    switch (k)
    {
    case XServerKind::VcXsrv:    return 0;
    case XServerKind::X410:      return 1;
    case XServerKind::Wsl:       return 2;
    case XServerKind::MobaXterm: return 3;
    case XServerKind::CygwinX:   return 4;
    case XServerKind::Xming:     return 5;
    case XServerKind::Unknown:   return 99;
    }
    return 99;
}

} // namespace

const std::vector<XServerProbe>& XServerProbes()
{
    static const std::vector<XServerProbe> kProbes = {
        { XServerKind::VcXsrv,    "VcXsrv\\vcxsrv.exe",              0 },
        { XServerKind::VcXsrv,    "VcXsrv\\vcxsrv.exe",              1 },
        { XServerKind::Xming,     "Xming\\Xming.exe",                0 },
        { XServerKind::Xming,     "Xming\\Xming.exe",                1 },
        { XServerKind::CygwinX,   "cygwin64\\bin\\XWin.exe",         0 },
        { XServerKind::CygwinX,   "cygwin\\bin\\XWin.exe",           0 },
        { XServerKind::MobaXterm, "Mobatek\\MobaXterm\\MobaXterm.exe", 0 },
        { XServerKind::MobaXterm, "Mobatek\\MobaXterm\\MobaXterm.exe", 1 },
        // X410 is a Store app; its install path is not stable, so it is only
        // ever detected by being already running on the display port.
    };
    return kProbes;
}

std::vector<XServerInfo> RankXServers(const std::vector<XServerCandidate>& found,
                                      int runningDisplay)
{
    std::vector<XServerInfo> out;
    for (const XServerCandidate& c : found)
    {
        // One entry per kind: two install roots for the same server are the
        // same server, and offering it twice helps nobody.
        const bool seen = std::any_of(out.begin(), out.end(),
                                      [&](const XServerInfo& i)
                                      { return i.kind == c.kind; });
        if (seen)
            continue;
        XServerInfo i;
        i.kind = c.kind;
        i.name = XServerKindName(c.kind);
        i.exePath = c.path;
        i.licence = LicenceOf(c.kind);
        i.display = runningDisplay >= 0 ? runningDisplay : 0;
        i.running = false;
        out.push_back(std::move(i));
    }
    // Something is already listening: report it even when nothing was found on
    // disk, because a running server is what actually matters and X410 can
    // only ever be detected this way.
    if (runningDisplay >= 0)
    {
        auto it = std::find_if(out.begin(), out.end(),
                               [](const XServerInfo& i)
                               { return i.kind != XServerKind::Unknown; });
        if (it != out.end())
            it->running = true;
        else
        {
            XServerInfo i;
            i.kind = XServerKind::Unknown;
            i.name = "an X server";
            i.licence = "";
            i.running = true;
            i.display = runningDisplay;
            out.push_back(std::move(i));
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const XServerInfo& a, const XServerInfo& b)
                     {
                         if (a.running != b.running)
                             return a.running;        // running wins outright
                         return Preference(a.kind) < Preference(b.kind);
                     });
    return out;
}

std::string XServerLaunchArgs(XServerKind kind, int display)
{
    const std::string d = ":" + std::to_string(display);
    switch (kind)
    {
    case XServerKind::VcXsrv:
        // -multiwindow puts each remote window on the Windows desktop.
        // Deliberately NO "-ac": that disables access control for every
        // process on this machine, and cookie authentication is the whole
        // reason AmberSSH can forward X11 safely.
        return d + " -multiwindow -clipboard -wgl";
    case XServerKind::Xming:
        return d + " -multiwindow -clipboard";
    case XServerKind::CygwinX:
        return d + " -multiwindow -clipboard";
    case XServerKind::MobaXterm:
    case XServerKind::X410:
    case XServerKind::Wsl:
    case XServerKind::Unknown:
        break;
    }
    return {};
}

// -------------------------------------------------------------- RemoteApp
bool PortUsable(int port)
{
    // Unprivileged, and clear of the ephemeral range Windows hands out, so a
    // forward does not collide with an outbound socket the OS chose.
    if (port < 1024 || port > 65535)
        return false;
    if (port >= 49152)
        return false;
    return true;
}

RemoteAppPlan PlanRemoteApp(const RemoteAppOptions& o)
{
    RemoteAppPlan p;
    p.localPort = o.localPort;
    p.remotePort = o.remotePort;
    p.bindAddress = "127.0.0.1";

    if (!PortUsable(o.localPort))
    {
        p.why = "the local port must be between 1024 and 49151";
        return p;
    }
    if (o.remotePort < 1 || o.remotePort > 65535)
    {
        p.why = "the remote port is not a port number";
        return p;
    }

    // The forward is written with an explicit bind address. The profile's
    // forward syntax allows omitting it, and omitting it is how a tunnel ends
    // up reachable from the whole LAN — so it is never omitted here.
    std::ostringstream fs;
    fs << "L" << p.bindAddress << ":" << o.localPort << ":127.0.0.1:"
       << o.remotePort;
    p.forwardSpec = fs.str();

    if (o.startWeston)
    {
        // --address=127.0.0.1 is the load-bearing argument. A VPS has a public
        // IP by definition, and RDP on a public IP is among the most scanned
        // surfaces there is. Without this the backend would listen on every
        // interface the moment the command ran.
        std::ostringstream c;
        c << "weston --backend=rdp-backend.so"
          << " --address=127.0.0.1"
          << " --port=" << o.remotePort;
        if (o.kiosk)
            c << " --shell=kiosk-shell.so";
        if (o.xwayland)
            c << " --xwayland";
        if (!o.tlsCert.empty() && !o.tlsKey.empty())
            c << " --rdp-tls-cert=" << o.tlsCert
              << " --rdp-tls-key=" << o.tlsKey;
        p.remoteCommand = c.str();
    }

    p.valid = true;
    return p;
}

std::string MstscArgs(const RemoteAppPlan& p)
{
    if (!p.valid)
        return {};
    // /v: takes host:port. The address is this machine's loopback end of the
    // tunnel, never the remote host: the RDP hop must never leave the SSH
    // connection.
    return "/v:127.0.0.1:" + std::to_string(p.localPort);
}

std::string RdpFile(const RemoteAppPlan& p, bool multimon)
{
    if (!p.valid)
        return {};
    std::ostringstream f;
    f << "full address:s:127.0.0.1:" << p.localPort << "\r\n"
      << "screen mode id:i:1\r\n"
      << "use multimon:i:" << (multimon ? 1 : 0) << "\r\n"
      << "session bpp:i:32\r\n"
      << "compression:i:1\r\n"
      << "keyboardhook:i:2\r\n"
      << "audiocapturemode:i:0\r\n"
      << "audiomode:i:2\r\n"
      << "redirectclipboard:i:1\r\n"
      << "redirectprinters:i:0\r\n"
      << "redirectsmartcards:i:0\r\n"
      << "drivestoredirect:s:\r\n"
      // Weston's RDP backend uses a self-signed certificate. That is correct
      // here and not a weakened check: the hop is loopback to loopback inside
      // an SSH tunnel, and SSH is what authenticates the far end. Prompting
      // about it every time would train the user to click through warnings.
      << "authentication level:i:0\r\n"
      << "prompt for credentials:i:0\r\n"
      << "negotiate security layer:i:1\r\n";
    return f.str();
}

} // namespace amber

// ------------------------------------------------------------------- probes
namespace amber
{

std::vector<XServerCandidate> ScanForXServers()
{
    // The three roots the probe table indexes. Resolved once here rather than
    // hard-coded, so a machine with Program Files somewhere unusual works.
    std::wstring roots[3];
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)))
    {
        roots[0] = p;
        CoTaskMemFree(p);
    }
    p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &p)))
    {
        roots[1] = p;
        CoTaskMemFree(p);
    }
    p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
    {
        roots[2] = p;
        CoTaskMemFree(p);
    }
    // Cygwin installs to the drive root, not to Program Files.
    const std::wstring driveRoot = L"C:\\";

    std::vector<XServerCandidate> out;
    for (const XServerProbe& probe : XServerProbes())
    {
        const std::wstring& root = roots[probe.root];
        std::wstring rel(probe.relPath, probe.relPath + strlen(probe.relPath));
        auto tryPath = [&](const std::wstring& base)
        {
            if (base.empty())
                return;
            std::wstring full = base;
            if (full.back() != L'\\')
                full += L'\\';
            full += rel;
            const DWORD a = GetFileAttributesW(full.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY))
                return;
            XServerCandidate c;
            c.kind = probe.kind;
            // Back to UTF-8 for the pure layer, which deals in std::string.
            const int n = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1,
                                              nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, s.data(), n,
                                    nullptr, nullptr);
            c.path = s;
            out.push_back(std::move(c));
        };
        tryPath(root);
        if (probe.kind == XServerKind::CygwinX)
            tryPath(driveRoot);
    }
    return out;
}

int DetectRunningDisplay(int maxDisplay)
{
    // Loopback only, and only the first few displays. Anything wider would be
    // a port scan of the user's own machine for no benefit: X servers live on
    // :0, occasionally :1.
    WSADATA wsa;
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    int found = -1;
    for (int d = 0; d <= maxDisplay && found < 0; ++d)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            break;
        // Non-blocking with a short wait: a probe must never stall the UI
        // thread on a host that silently drops.
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<u_short>(6000 + d));
        connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        timeval tv = { 0, 120 * 1000 };
        if (select(0, nullptr, &w, nullptr, &tv) > 0)
        {
            int err = 0;
            int len = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&err), &len) == 0 && err == 0)
                found = d;
        }
        closesocket(s);
    }
    if (started)
        WSACleanup();
    return found;
}

std::wstring WriteTempRdpFile(const std::string& text)
{
    if (text.empty())
        return {};
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir))
        return {};
    std::wstring path = std::wstring(dir) + L"AmberSSH-RemoteApp.rdp";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, text.data(),
                              static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(h);
    if (!ok || wrote != text.size())
        return {};
    return path;
}

} // namespace amber
