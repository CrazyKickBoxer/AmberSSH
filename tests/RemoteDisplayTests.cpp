// RemoteDisplayTests — X server selection and the RemoteApp plan.
//
// The one rule worth a test suite of its own: neither end ever listens on
// anything but loopback. A VPS has a public IP by definition, and an RDP port
// on a public IP is among the most scanned surfaces on the internet.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "remote/RemoteDisplay.h"

using namespace amber;

namespace
{

bool Has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

XServerCandidate Cand(XServerKind k, const char* path)
{
    XServerCandidate c;
    c.kind = k;
    c.path = path;
    return c;
}

} // namespace

TEST_CASE("A running server outranks anything on disk", "[xsrv]")
{
    // What matters is what will actually answer, not what is installed.
    const std::vector<XServerCandidate> found = {
        Cand(XServerKind::Xming, "C:\\Program Files\\Xming\\Xming.exe"),
        Cand(XServerKind::VcXsrv, "C:\\Program Files\\VcXsrv\\vcxsrv.exe"),
    };
    const std::vector<XServerInfo> r = RankXServers(found, 0);
    REQUIRE(r.size() >= 2);
    CHECK(r.front().running);
}

TEST_CASE("With nothing running, the ranking is stable and sensible", "[xsrv]")
{
    const std::vector<XServerCandidate> found = {
        Cand(XServerKind::Xming, "x"),
        Cand(XServerKind::CygwinX, "c"),
        Cand(XServerKind::VcXsrv, "v"),
    };
    const std::vector<XServerInfo> r = RankXServers(found, -1);
    REQUIRE(r.size() == 3);
    CHECK(r[0].kind == XServerKind::VcXsrv);
    CHECK(r[2].kind == XServerKind::Xming);
    for (const XServerInfo& i : r)
        CHECK_FALSE(i.running);
    // Same input, same answer — the choice must not wander between launches.
    CHECK(RankXServers(found, -1)[0].kind == r[0].kind);
}

TEST_CASE("One entry per server, however many install roots matched", "[xsrv]")
{
    const std::vector<XServerCandidate> found = {
        Cand(XServerKind::VcXsrv, "C:\\Program Files\\VcXsrv\\vcxsrv.exe"),
        Cand(XServerKind::VcXsrv, "C:\\Program Files (x86)\\VcXsrv\\vcxsrv.exe"),
    };
    const std::vector<XServerInfo> r = RankXServers(found, -1);
    REQUIRE(r.size() == 1);
    CHECK(r[0].exePath == "C:\\Program Files\\VcXsrv\\vcxsrv.exe");
}

TEST_CASE("Something listening with nothing installed is still reported", "[xsrv]")
{
    // X410 is a Store app whose install path is not stable, so being already
    // running is the only way it can ever be found.
    const std::vector<XServerInfo> r = RankXServers({}, 0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].running);
    CHECK(r[0].display == 0);
}

TEST_CASE("Nothing found and nothing running is an empty list", "[xsrv]")
{
    CHECK(RankXServers({}, -1).empty());
}

TEST_CASE("Every server is named and licensed in the picker", "[xsrv]")
{
    const std::vector<XServerCandidate> found = {
        Cand(XServerKind::VcXsrv, "v"),   Cand(XServerKind::Xming, "x"),
        Cand(XServerKind::CygwinX, "c"),  Cand(XServerKind::MobaXterm, "m"),
    };
    for (const XServerInfo& i : RankXServers(found, -1))
    {
        INFO(i.name);
        CHECK_FALSE(i.name.empty());
        // The user is choosing what to install on their own machine. They are
        // entitled to know the terms, especially since AmberSSH bundles none
        // of these precisely because some of them could not be bundled.
        CHECK_FALSE(i.licence.empty());
    }
}

TEST_CASE("Launch arguments never disable access control", "[xsrv]")
{
    // "-ac" opens the display to every process on the machine and throws away
    // the entire reason cookie authentication exists.
    for (XServerKind k : { XServerKind::VcXsrv, XServerKind::Xming,
                           XServerKind::CygwinX })
    {
        const std::string a = XServerLaunchArgs(k, 0);
        INFO(XServerKindName(k));
        CHECK_FALSE(a.empty());
        CHECK_FALSE(Has(a, "-ac"));
        CHECK(Has(a, ":0"));
    }
    CHECK(Has(XServerLaunchArgs(XServerKind::VcXsrv, 3), ":3"));
    // A server we cannot launch reports nothing rather than a wrong command.
    CHECK(XServerLaunchArgs(XServerKind::X410, 0).empty());
    CHECK(XServerLaunchArgs(XServerKind::Unknown, 0).empty());
}

// ------------------------------------------------------------------ RemoteApp
TEST_CASE("The plan binds loopback and nothing else", "[remoteapp]")
{
    RemoteAppOptions o;
    const RemoteAppPlan p = PlanRemoteApp(o);
    REQUIRE(p.valid);
    CHECK(p.bindAddress == "127.0.0.1");

    // The forward spec carries the bind address explicitly. The syntax allows
    // omitting it, and omitting it is exactly how a tunnel ends up reachable
    // from the whole LAN.
    CHECK(Has(p.forwardSpec, "L127.0.0.1:"));
    CHECK_FALSE(Has(p.forwardSpec, "0.0.0.0"));
    CHECK_FALSE(Has(p.forwardSpec, "L*"));
    CHECK(Has(p.forwardSpec, ":127.0.0.1:3389"));

    // And the far end is told to listen on loopback too. Without this the RDP
    // backend binds every interface the moment the command runs — on a machine
    // with a public IP.
    CHECK(Has(p.remoteCommand, "--address=127.0.0.1"));
    CHECK_FALSE(Has(p.remoteCommand, "0.0.0.0"));
}

TEST_CASE("The client is pointed at the tunnel, never at the host", "[remoteapp]")
{
    RemoteAppOptions o;
    o.localPort = 13389;
    const RemoteAppPlan p = PlanRemoteApp(o);
    REQUIRE(p.valid);

    const std::string args = MstscArgs(p);
    CHECK(args == "/v:127.0.0.1:13389");

    const std::string rdp = RdpFile(p, false);
    CHECK(Has(rdp, "full address:s:127.0.0.1:13389"));
    // If the RDP hop ever addressed the remote host directly it would leave
    // the SSH tunnel, and the whole security model with it.
    CHECK_FALSE(Has(rdp, "example"));
    CHECK(Has(rdp, "use multimon:i:0"));
    CHECK(Has(RdpFile(p, true), "use multimon:i:1"));
}

TEST_CASE("Bad ports are refused with a reason", "[remoteapp]")
{
    for (int bad : { 0, 22, 80, 1023, 49152, 65536, -1 })
    {
        RemoteAppOptions o;
        o.localPort = bad;
        const RemoteAppPlan p = PlanRemoteApp(o);
        INFO(bad);
        CHECK_FALSE(p.valid);
        CHECK_FALSE(p.why.empty());
        // An invalid plan must produce no client arguments at all, so a
        // caller that forgets to check `valid` launches nothing rather than
        // launching something wrong.
        CHECK(MstscArgs(p).empty());
        CHECK(RdpFile(p, false).empty());
    }
    CHECK(PortUsable(13389));
    CHECK(PortUsable(1024));
    CHECK(PortUsable(49151));
    CHECK_FALSE(PortUsable(1023));
    CHECK_FALSE(PortUsable(49152));
}

TEST_CASE("The remote command reflects the options asked for", "[remoteapp]")
{
    RemoteAppOptions o;
    o.kiosk = true;
    o.xwayland = true;
    RemoteAppPlan p = PlanRemoteApp(o);
    REQUIRE(p.valid);
    CHECK(Has(p.remoteCommand, "kiosk-shell.so"));
    CHECK(Has(p.remoteCommand, "--xwayland"));
    CHECK(Has(p.remoteCommand, "rdp-backend.so"));
    CHECK(Has(p.remoteCommand, "--rdp-tls-cert="));

    o.kiosk = false;
    o.xwayland = false;
    p = PlanRemoteApp(o);
    CHECK_FALSE(Has(p.remoteCommand, "kiosk-shell.so"));
    CHECK_FALSE(Has(p.remoteCommand, "--xwayland"));

    // Someone running weston as a systemd user service wants the tunnel only.
    o.startWeston = false;
    p = PlanRemoteApp(o);
    REQUIRE(p.valid);
    CHECK(p.remoteCommand.empty());
    CHECK_FALSE(p.forwardSpec.empty());
}

TEST_CASE("A non-default remote port is carried through everywhere", "[remoteapp]")
{
    RemoteAppOptions o;
    o.localPort = 14000;
    o.remotePort = 3390;
    const RemoteAppPlan p = PlanRemoteApp(o);
    REQUIRE(p.valid);
    CHECK(Has(p.forwardSpec, "L127.0.0.1:14000:127.0.0.1:3390"));
    CHECK(Has(p.remoteCommand, "--port=3390"));
    CHECK(Has(MstscArgs(p), "14000"));
}
