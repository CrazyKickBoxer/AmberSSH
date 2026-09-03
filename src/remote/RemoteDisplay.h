// RemoteDisplay.h — finding a local X server, and planning the RemoteApp hop.
//
// AmberSSH ships neither an X server nor an RDP client. It finds what is
// already installed and hands off to it. That is a licensing decision as much
// as an engineering one: VcXsrv is GPLv2 and current Xming is not free, and
// neither licence can reach this codebase if nothing is redistributed. See
// docs/REMOTE-DISPLAY.md.
//
// Everything here is a decision, not an action: which server was found, what
// port to forward, what command to run on the far end, what arguments the
// client needs. The doing lives in app.cpp; keeping the choosing here is what
// makes the rules testable — above all the one rule that matters, which is
// that neither end ever listens on anything but loopback.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

// ------------------------------------------------------------- X servers
enum class XServerKind
{
    Unknown = 0,
    VcXsrv,
    X410,
    Xming,
    CygwinX,
    MobaXterm,
    Wsl,          // WSLg's server, reachable when WSL is installed
};

struct XServerInfo
{
    XServerKind kind = XServerKind::Unknown;
    std::string name;         // "VcXsrv"
    std::string exePath;      // may be empty for one already running
    std::string licence;      // shown in the UI, because the user is choosing
    bool running = false;     // something is listening on the display port
    int display = 0;
};

const char* XServerKindName(XServerKind k);

// Where each known server installs. Pure: takes the candidate paths that exist
// and turns them into a ranked list, so the search itself can be tested with a
// synthetic filesystem.
struct XServerCandidate
{
    XServerKind kind;
    std::string path;         // an executable that was found to exist
};

// Ranked best-first. Ranking prefers a server that is already running, then
// one that integrates best, and it is stable so the same install set always
// produces the same choice.
std::vector<XServerInfo> RankXServers(const std::vector<XServerCandidate>& found,
                                      int runningDisplay);

// The relative paths under Program Files / Program Files (x86) / LocalAppData
// that each known server installs to. Exposed so the scan is data, not code.
struct XServerProbe
{
    XServerKind kind;
    const char* relPath;      // "VcXsrv\\vcxsrv.exe"
    int root;                 // 0 = ProgramFiles, 1 = ProgramFilesX86, 2 = LocalAppData
};
const std::vector<XServerProbe>& XServerProbes();

// Command line for starting a server we found. Access control is deliberately
// left ON — never "-ac", which would open the display to every process on the
// machine and throw away the whole point of cookie authentication.
std::string XServerLaunchArgs(XServerKind kind, int display);

// -------------------------------------------------------------- RemoteApp
// What to ask the far end to run, and what to point the client at.
struct RemoteAppPlan
{
    // The loopback port on THIS machine that the SSH local forward listens on.
    int localPort = 0;
    // The port Weston's RDP backend listens on over there.
    int remotePort = 3389;
    // Always "127.0.0.1". Present as a field so the invariant is visible in a
    // test rather than buried in a format string.
    std::string bindAddress = "127.0.0.1";
    // The forward spec in the same syntax the profile's `forwards` field uses.
    std::string forwardSpec;
    // The command to run on the far end, or empty when the user runs their own
    // service and only wants the tunnel.
    std::string remoteCommand;
    bool valid = false;
    std::string why;          // why it is not valid, for the status bar
};

struct RemoteAppOptions
{
    int localPort = 13389;
    int remotePort = 3389;
    bool startWeston = true;       // false: assume a systemd user service
    bool xwayland = true;
    bool kiosk = true;             // one app per surface, rather than a desktop
    std::string tlsCert = "~/.config/weston/tls.crt";
    std::string tlsKey = "~/.config/weston/tls.key";
};

// Builds the plan, or explains why it cannot. Rejects any port outside the
// unprivileged range and any attempt to bind something other than loopback.
RemoteAppPlan PlanRemoteApp(const RemoteAppOptions& o);

// Arguments for mstsc.exe against a plan. mstsc ships with Windows, so this is
// the route with no third-party code and no licensing question at all.
std::string MstscArgs(const RemoteAppPlan& p);

// The contents of a .rdp file for the plan. mstsc takes settings far more
// reliably from a file than from the command line, and a file is also what a
// user would keep.
std::string RdpFile(const RemoteAppPlan& p, bool multimon);

// True when a port is safe to bind on this machine: unprivileged, not one of
// the ports something else is expected on.
bool PortUsable(int port);

// ------------------------------------------------------------------- probes
// The three functions here touch the filesystem and the network stack. They
// are kept apart from the decisions above so the decisions stay testable.

// Walks XServerProbes() against the real install roots and returns what
// exists. Nothing is launched and nothing is connected to.
std::vector<XServerCandidate> ScanForXServers();

// The lowest display number 6000+n that something is listening on, or -1.
// Probes loopback only, and only the first few displays: an X server is on
// :0 or thereabouts, and scanning further would be a port scan of the user's
// own machine for no benefit.
int DetectRunningDisplay(int maxDisplay = 4);

// Writes `text` to a temporary .rdp file and returns its path, or empty.
// Used because mstsc takes settings far more reliably from a file than from
// its command line.
std::wstring WriteTempRdpFile(const std::string& text);

} // namespace amber
