// Guardian.h — the reconnect state machine for one session.
//
// Stage 2's rule, and the reason this file has no Windows, no libssh2 and no
// terminal in it: AmberSSH may restore the CONNECTION and the state AmberSSH
// itself owns. It may not restore remote process continuity unless the user
// asked for a persistent multiplexer (tmux / screen), because nothing can
// bring back an interactive process whose pty died with the TCP connection.
//
// Everything here is pure and deterministic — classification, backoff (with
// seeded jitter), the reattach command, the restore plan — so all of it is
// unit-testable without a network. The app drives it from three events
// (connected, dropped, the user asked) and one Tick() per frame.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../profiles/ConnectionProfile.h"

namespace amber
{

// ---------------------------------------------------------------- classify
// Why the link went away. The two security classes exist so that automatic
// reconnect can be made structurally incapable of driving past them.
enum class DropClass
{
    Transient,          // network went away — a retry is meaningful
    RemoteClosed,       // the shell exited / server closed: not a fault
    AuthRequired,       // credentials missing or rejected — the user decides
    HostKeyAttention,   // mismatch, not listed, or rejected — never retried
    Fatal,              // configuration / unsupported — retrying cannot help
    UserInitiated,      // we asked for it
};

const char* DropClassName(DropClass c);

// Maps a transport's free-text reason onto a class. Deliberately explicit
// about the security classes and the fatal configuration errors; every other
// reason on a session that HAD connected is treated as transient, because a
// link that worked and then stopped working almost always stopped for a
// network reason. Guardian only ever arms after a successful connect, which
// is what makes that default safe.
DropClass ClassifyDrop(const std::string& reason);

// Whether automatic reconnect is even allowed to consider this class. The
// security gates return false here and nothing in Guardian can override it.
bool RetryableClass(DropClass c);

// ----------------------------------------------------------------- backoff
// 1 -> 2 -> 5 -> 10 -> 30 s, then held at 30. `attempt` is 1-based.
int BackoffSeconds(int attempt);

// The ladder with +/- jitterPercent applied, derived deterministically from
// (seed, attempt). Two tabs pointed at the same host get different seeds and
// so different delays, which is what stops a reconnect storm when a shared
// server or a VPN comes back.
double BackoffDelay(int attempt, int jitterPercent, uint32_t seed);

// A stable seed for a profile, so a tab's backoff pattern is reproducible
// across runs but differs from its neighbour's.
uint32_t SeedFromId(const std::string& id);

// ------------------------------------------------------------------- state
enum class GuardianState
{
    Idle,                    // no episode: never connected, or guardian off
    Connected,
    ConnectionLost,          // dropped; deciding, or waiting for consent
    WaitingToReconnect,      // backoff timer running
    Reconnecting,            // an attempt is in flight
    AuthenticationRequired,  // stopped: the user must supply credentials
    HostKeyAttention,        // stopped: the host key needs a human
    Reconnected,             // succeeded; held briefly so it can be reported
    GaveUp,                  // retry limit reached
    UserStopped,             // the user stopped it, or declined in Ask mode
};

const char* GuardianStateName(GuardianState s);

// True for the states in which nothing further will happen on its own.
bool GuardianTerminal(GuardianState s);

struct GuardianPolicy
{
    ReconnectMode mode = ReconnectMode::Off;
    int  maxAttempts = 6;        // 0 = keep trying until the user stops it
    int  jitterPercent = 20;     // 0..50
    uint32_t seed = 0;
};

// ---------------------------------------------------------------- reattach
// The command run once, after authentication, to rejoin a persistent remote
// session. Nothing here is destructive: neither form kills or detaches an
// existing session.
//
// A session name is restricted to [A-Za-z0-9._-] because it is interpolated
// into a shell command line. An invalid name is refused with an error rather
// than quoted and hoped for.
bool ValidReattachName(const std::string& name);

// Returns the command, or an empty string with `err` set. Mode None yields an
// empty command and no error.
std::string ReattachCommand(ReattachMode mode, const std::string& name,
                            const std::string& custom, std::string& err);

// ----------------------------------------------------------- restore plan
// What AmberSSH sends to the far end after a reconnect. Only ever these two
// things, in this order, and only when the profile asked for them — arbitrary
// terminal input is never replayed.
struct RestoreStep
{
    enum class Kind { Reattach, ChangeDirectory };
    Kind kind;
    std::string text;      // the line to send, without its newline
};

struct RestoreRequest
{
    ReattachMode reattach = ReattachMode::None;
    std::string reattachSession;
    std::string reattachCustom;
    bool restoreCwd = false;
    std::string cwd;           // as learned from OSC 7, may be empty
};

// Builds the plan. `err` describes anything that was refused (an unusable
// session name, an unsafe directory) — a refused step is omitted, never
// silently mangled.
std::vector<RestoreStep> BuildRestorePlan(const RestoreRequest& r, std::string& err);

// POSIX single-quoting for a path going onto a shell command line.
std::string ShellQuote(const std::string& s);

// True when a path is safe to put in a command at all: no control characters
// and no embedded newline. A path that fails this is dropped from the plan.
bool SafeCwd(const std::string& cwd);

// -------------------------------------------------------- interrupted work
// What was running when the link died. Deliberately carries no exit code:
// AmberSSH does not know how the command ended and will not invent one.
struct InterruptedCommand
{
    std::string command;
    std::string cwd;
    int64_t startedAt = 0;      // Unix seconds
    double ranForSec = 0.0;     // as far as AmberSSH saw
};

// ---------------------------------------------------------------- guardian
class Guardian
{
public:
    void Configure(const GuardianPolicy& p) { m_policy = p; }
    const GuardianPolicy& Policy() const { return m_policy; }

    // Back to a never-connected session.
    void Reset();

    // The transport reported a successful connect.
    void OnConnected(double now);

    // The transport reported Closed or Error. Decides the next state. Does
    // nothing at all before the first successful connect: a session that
    // never came up has a configuration problem, not a dropped link.
    void OnDrop(const std::string& reason, double now);

    // The user asked for the disconnect. Disarms permanently for this session.
    void OnUserDisconnect();

    // Ask mode: the user answered the "reconnect?" question.
    void GrantConsent(bool yes, double now);

    // "Stop reconnecting" — from the menu, the palette or the banner.
    void StopByUser();

    // "Reconnect now" — collapses the remaining wait. Also revives a session
    // that gave up, which is the only way back out of GaveUp.
    void RetryNow(double now);

    // Called once per frame. Returns true EXACTLY once per attempt, when the
    // caller should open a connection; the attempt counter has already been
    // advanced when it does.
    bool Tick(double now);

    GuardianState State() const { return m_state; }
    bool NeedsConsent() const { return m_needConsent; }
    // True while a reconnect is pending or running — the tab is not idle.
    bool Armed() const
    {
        return m_state == GuardianState::WaitingToReconnect ||
               m_state == GuardianState::Reconnecting;
    }
    int  Attempt() const { return m_attempt; }
    double NextAt() const { return m_nextAt; }
    double LostAt() const { return m_lostAt; }
    // Seconds the link has been (or was) down; 0 when it never dropped.
    double DownSeconds(double now) const;
    DropClass LastClass() const { return m_lastClass; }
    const std::string& LastReason() const { return m_lastReason; }
    // True when the most recent OnConnected ended a reconnect episode; the
    // caller uses it to decide whether to run the restore plan and report.
    bool JustReconnected() const { return m_justReconnected; }
    void ClearJustReconnected() { m_justReconnected = false; }
    // How long the last completed outage lasted, in seconds.
    double LastOutageSec() const { return m_lastOutage; }
    bool EverConnected() const { return m_everConnected; }

    // One line for the status bar and the banner. Never speculative.
    std::string StatusText(double now) const;

private:
    void Stop(GuardianState s);
    void Schedule(double now);

    GuardianPolicy m_policy;
    GuardianState m_state = GuardianState::Idle;
    bool m_everConnected = false;
    bool m_userClosed = false;
    bool m_needConsent = false;
    bool m_episode = false;          // inside a loss episode
    bool m_justReconnected = false;
    int  m_attempt = 0;
    double m_nextAt = 0.0;
    double m_lostAt = 0.0;
    double m_reconnectedAt = 0.0;
    double m_lastOutage = 0.0;
    DropClass m_lastClass = DropClass::Transient;
    std::string m_lastReason;
};

} // namespace amber
