#include "Guardian.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace amber
{

namespace
{

// Case-insensitive substring test; the transports write their reasons in
// lower case but a server-supplied message can arrive in any case.
bool Has(const std::string& hay, const char* needle)
{
    const size_t n = std::char_traits<char>::length(needle);
    if (n == 0 || hay.size() < n)
        return false;
    for (size_t i = 0; i + n <= hay.size(); ++i)
    {
        size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                            std::tolower(static_cast<unsigned char>(needle[k])))
            ++k;
        if (k == n)
            return true;
    }
    return false;
}

} // namespace

const char* DropClassName(DropClass c)
{
    switch (c)
    {
    case DropClass::Transient:        return "transient";
    case DropClass::RemoteClosed:     return "remote closed";
    case DropClass::AuthRequired:     return "authentication required";
    case DropClass::HostKeyAttention: return "host key needs attention";
    case DropClass::Fatal:            return "fatal";
    case DropClass::UserInitiated:    return "user initiated";
    }
    return "unknown";
}

DropClass ClassifyDrop(const std::string& reason)
{
    // Host key first: it is the one class that must never be mistaken for
    // anything else, and its messages contain words ("rejected", "mismatch")
    // that would otherwise be caught below.
    if (Has(reason, "host key"))
        return DropClass::HostKeyAttention;

    if (Has(reason, "authentication failed") ||
        Has(reason, "private key authentication") ||
        Has(reason, "no agent identity") ||
        Has(reason, "no ssh agent") ||
        Has(reason, "could not list agent identities") ||
        Has(reason, "permission denied") ||
        Has(reason, "passphrase") ||
        Has(reason, "password"))
        return DropClass::AuthRequired;

    // Configuration and capability problems. Retrying re-runs the same
    // configuration and fails the same way, so the guardian must not.
    if (Has(reason, "not yet supported") ||
        Has(reason, "could not resolve host") ||
        Has(reason, "pty request failed") ||
        Has(reason, "shell request failed") ||
        Has(reason, "remote command request failed") ||
        Has(reason, "could not open channel") ||
        Has(reason, "libssh2_session_init") ||
        Has(reason, "could not configure ") ||     // serial port options
        Has(reason, "could not open "))            // serial port / local exe
        return DropClass::Fatal;

    if (Has(reason, "cancelled") || Has(reason, "disconnected by user"))
        return DropClass::UserInitiated;

    // A clean end: the process on the far side finished. Reconnecting here
    // would fight the user's own "exit".
    //
    // Note what is NOT in this list: the stream transports' "remote closed
    // the connection". That is a socket EOF from a telnet/raw peer, which is
    // what a server restart and an idle timeout both look like, and it falls
    // through to Transient on purpose. "remote closed the session" is the
    // different case — an SSH shell that ended because the user ended it.
    if (Has(reason, "remote closed the session") ||
        Has(reason, "shell exited") ||
        Has(reason, "shell closed its input") ||
        Has(reason, "process exited") ||
        Has(reason, "port closed"))
        return DropClass::RemoteClosed;

    return DropClass::Transient;
}

bool RetryableClass(DropClass c)
{
    return c == DropClass::Transient;
}

// ----------------------------------------------------------------- backoff
int BackoffSeconds(int attempt)
{
    static const int kLadder[] = { 1, 2, 5, 10, 30 };
    const int n = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));
    if (attempt < 1)
        attempt = 1;
    return kLadder[std::min(attempt, n) - 1];
}

uint32_t SeedFromId(const std::string& id)
{
    // FNV-1a. Any stable hash would do; this one is short and has no
    // dependency, and the value only has to differ between profiles.
    uint32_t h = 2166136261u;
    for (unsigned char c : id)
    {
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

double BackoffDelay(int attempt, int jitterPercent, uint32_t seed)
{
    const double base = static_cast<double>(BackoffSeconds(attempt));
    jitterPercent = std::clamp(jitterPercent, 0, 50);
    if (jitterPercent == 0)
        return base;
    // Deterministic per (seed, attempt): the same tab retrying the same
    // attempt number always waits the same time, but two tabs differ.
    uint32_t x = seed ^ (static_cast<uint32_t>(attempt) * 2654435761u);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    const double unit = static_cast<double>(x % 2001u) / 1000.0 - 1.0;   // [-1, 1]
    const double delta = base * (static_cast<double>(jitterPercent) / 100.0) * unit;
    return std::max(0.25, base + delta);
}

// ------------------------------------------------------------------- state
const char* GuardianStateName(GuardianState s)
{
    switch (s)
    {
    case GuardianState::Idle:                   return "idle";
    case GuardianState::Connected:              return "connected";
    case GuardianState::ConnectionLost:         return "connection lost";
    case GuardianState::WaitingToReconnect:     return "waiting to reconnect";
    case GuardianState::Reconnecting:           return "reconnecting";
    case GuardianState::AuthenticationRequired: return "authentication required";
    case GuardianState::HostKeyAttention:       return "host key requires attention";
    case GuardianState::Reconnected:            return "reconnected";
    case GuardianState::GaveUp:                 return "gave up";
    case GuardianState::UserStopped:            return "user stopped reconnect";
    }
    return "unknown";
}

bool GuardianTerminal(GuardianState s)
{
    return s == GuardianState::AuthenticationRequired ||
           s == GuardianState::HostKeyAttention ||
           s == GuardianState::GaveUp ||
           s == GuardianState::UserStopped;
}

// ---------------------------------------------------------------- reattach
bool ValidReattachName(const std::string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (unsigned char c : name)
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}

std::string ReattachCommand(ReattachMode mode, const std::string& name,
                            const std::string& custom, std::string& err)
{
    err.clear();
    switch (mode)
    {
    case ReattachMode::None:
        return {};

    case ReattachMode::Tmux:
        if (!ValidReattachName(name))
        {
            err = "tmux session name must be 1-64 characters of letters, "
                  "digits, dot, underscore or hyphen";
            return {};
        }
        // Attach if it exists, otherwise create it. Never kill, never
        // detach anybody else: -d and kill-session are deliberately absent.
        return "tmux attach-session -t '" + name + "' || tmux new-session -s '" +
               name + "'";

    case ReattachMode::Screen:
        if (!ValidReattachName(name))
        {
            err = "screen session name must be 1-64 characters of letters, "
                  "digits, dot, underscore or hyphen";
            return {};
        }
        // -R resumes the named session if there is one and creates it
        // otherwise. -D (which detaches whoever is attached) is deliberately
        // not used: reconnecting must not throw somebody else off.
        return "screen -R '" + name + "'";

    case ReattachMode::Custom:
    {
        std::string c = custom;
        while (!c.empty() && (c.back() == ' ' || c.back() == '\t' ||
                              c.back() == '\r' || c.back() == '\n'))
            c.pop_back();
        size_t a = c.find_first_not_of(" \t");
        if (a != std::string::npos)
            c.erase(0, a);
        if (c.empty())
        {
            err = "custom reattach is selected but no command is configured";
            return {};
        }
        if (c.find('\n') != std::string::npos || c.find('\r') != std::string::npos)
        {
            err = "custom reattach command must be a single line";
            return {};
        }
        return c;
    }
    }
    return {};
}

// ----------------------------------------------------------- restore plan
std::string ShellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

bool SafeCwd(const std::string& cwd)
{
    if (cwd.empty() || cwd.size() > 4096)
        return false;
    for (unsigned char c : cwd)
        if (c < 0x20 || c == 0x7F)
            return false;
    return true;
}

std::vector<RestoreStep> BuildRestorePlan(const RestoreRequest& r, std::string& err)
{
    err.clear();
    std::vector<RestoreStep> plan;

    std::string rerr;
    const std::string reattach =
        ReattachCommand(r.reattach, r.reattachSession, r.reattachCustom, rerr);
    if (!rerr.empty())
        err = rerr;
    else if (!reattach.empty())
        plan.push_back({ RestoreStep::Kind::Reattach, reattach });

    // The working directory is restored only when there is no multiplexer.
    // tmux and screen bring their own panes back, each already in its own
    // directory; a cd on top of that would move the user somewhere they
    // never were.
    if (r.restoreCwd && r.reattach == ReattachMode::None)
    {
        if (!r.cwd.empty())
        {
            if (SafeCwd(r.cwd))
                plan.push_back({ RestoreStep::Kind::ChangeDirectory,
                                 "cd " + ShellQuote(r.cwd) });
            else if (err.empty())
                err = "the remembered directory contains control characters "
                      "and was not restored";
        }
    }
    return plan;
}

// ---------------------------------------------------------------- guardian
void Guardian::Reset()
{
    m_state = GuardianState::Idle;
    m_everConnected = false;
    m_userClosed = false;
    m_needConsent = false;
    m_episode = false;
    m_justReconnected = false;
    m_attempt = 0;
    m_nextAt = 0.0;
    m_lostAt = 0.0;
    m_reconnectedAt = 0.0;
    m_lastOutage = 0.0;
    m_lastClass = DropClass::Transient;
    m_lastReason.clear();
}

void Guardian::Stop(GuardianState s)
{
    m_state = s;
    m_needConsent = false;
    m_episode = false;
    m_nextAt = 0.0;
}

void Guardian::Schedule(double now)
{
    m_attempt++;
    if (m_policy.maxAttempts > 0 && m_attempt > m_policy.maxAttempts)
    {
        m_attempt = m_policy.maxAttempts;
        Stop(GuardianState::GaveUp);
        return;
    }
    m_nextAt = now + BackoffDelay(m_attempt, m_policy.jitterPercent, m_policy.seed);
    m_state = GuardianState::WaitingToReconnect;
}

void Guardian::OnConnected(double now)
{
    m_justReconnected = m_episode;
    if (m_episode)
    {
        m_lastOutage = std::max(0.0, now - m_lostAt);
        m_state = GuardianState::Reconnected;
        m_reconnectedAt = now;
    }
    else
    {
        m_state = GuardianState::Connected;
    }
    m_everConnected = true;
    m_episode = false;
    m_needConsent = false;
    m_attempt = 0;
    m_nextAt = 0.0;
}

void Guardian::OnDrop(const std::string& reason, double now)
{
    m_lastReason = reason;
    m_lastClass = ClassifyDrop(reason);

    if (m_userClosed)
    {
        m_state = GuardianState::Idle;
        return;
    }
    // Before the first successful connect there is nothing to reconnect TO:
    // a failure here is a configuration or credential problem and belongs to
    // the connection dialog, not to a retry loop.
    if (!m_everConnected)
    {
        m_state = GuardianState::Idle;
        return;
    }
    if (!m_episode)
    {
        m_lostAt = now;
        m_episode = true;
        m_attempt = 0;
    }

    // ---- security gates. Nothing below can be reached past these. -------
    if (m_lastClass == DropClass::HostKeyAttention)
    {
        Stop(GuardianState::HostKeyAttention);
        return;
    }
    if (m_lastClass == DropClass::AuthRequired)
    {
        Stop(GuardianState::AuthenticationRequired);
        return;
    }
    if (m_lastClass == DropClass::UserInitiated)
    {
        Stop(GuardianState::UserStopped);
        return;
    }
    if (!RetryableClass(m_lastClass))
    {
        // A clean remote exit or a fatal configuration error. The link is
        // down and stays down; the state says so without pretending a retry
        // is coming.
        Stop(GuardianState::ConnectionLost);
        return;
    }

    if (m_policy.mode == ReconnectMode::Off)
    {
        Stop(GuardianState::ConnectionLost);
        return;
    }
    if (m_policy.mode == ReconnectMode::Ask && m_attempt == 0)
    {
        // Consent is per episode, not per attempt: the user is asked once
        // when the link drops, not once every backoff step.
        m_state = GuardianState::ConnectionLost;
        m_needConsent = true;
        return;
    }
    Schedule(now);
}

void Guardian::OnUserDisconnect()
{
    m_userClosed = true;
    Stop(GuardianState::Idle);
}

void Guardian::GrantConsent(bool yes, double now)
{
    if (!m_needConsent)
        return;
    m_needConsent = false;
    if (yes)
        Schedule(now);
    else
        Stop(GuardianState::UserStopped);
}

void Guardian::StopByUser()
{
    if (m_state == GuardianState::Connected || m_state == GuardianState::Reconnected)
        return;
    Stop(GuardianState::UserStopped);
}

void Guardian::RetryNow(double now)
{
    // An explicit "reconnect now" supersedes an earlier manual disconnect:
    // the user is asking for this connection back, in as many words.
    m_userClosed = false;
    // Deliberately allowed out of GaveUp, UserStopped, AuthenticationRequired
    // and HostKeyAttention: this is an explicit human action, which is exactly
    // what those states were waiting for. The attempt itself still runs the
    // full authentication and host-key checks — nothing is bypassed, the
    // difference is only that a human asked for it.
    if (!m_episode)
    {
        m_episode = true;
        m_lostAt = now;
    }
    m_needConsent = false;
    m_attempt = 0;
    m_nextAt = now;
    m_state = GuardianState::WaitingToReconnect;
}

bool Guardian::Tick(double now)
{
    // Reconnected is a report, not a state to live in; it decays to Connected
    // once it has been on screen long enough to read.
    if (m_state == GuardianState::Reconnected && now - m_reconnectedAt > 6.0)
        m_state = GuardianState::Connected;

    if (m_state != GuardianState::WaitingToReconnect || m_needConsent)
        return false;
    if (now < m_nextAt)
        return false;
    m_state = GuardianState::Reconnecting;
    return true;
}

double Guardian::DownSeconds(double now) const
{
    if (m_episode)
        return std::max(0.0, now - m_lostAt);
    return m_lastOutage;
}

std::string Guardian::StatusText(double now) const
{
    char buf[256];
    switch (m_state)
    {
    case GuardianState::Idle:
    case GuardianState::Connected:
        return {};

    case GuardianState::ConnectionLost:
        if (m_needConsent)
            return "connection lost — reconnect?";
        return m_lastReason.empty() ? "connection lost" : m_lastReason;

    case GuardianState::WaitingToReconnect:
    {
        const double left = std::max(0.0, m_nextAt - now);
        if (m_policy.maxAttempts > 0)
            snprintf(buf, sizeof(buf), "reconnecting in %.0fs (attempt %d of %d)",
                     left, m_attempt, m_policy.maxAttempts);
        else
            snprintf(buf, sizeof(buf), "reconnecting in %.0fs (attempt %d)",
                     left, m_attempt);
        return buf;
    }

    case GuardianState::Reconnecting:
        if (m_policy.maxAttempts > 0)
            snprintf(buf, sizeof(buf), "reconnecting (attempt %d of %d)",
                     m_attempt, m_policy.maxAttempts);
        else
            snprintf(buf, sizeof(buf), "reconnecting (attempt %d)", m_attempt);
        return buf;

    case GuardianState::AuthenticationRequired:
        return "authentication required — reconnect stopped: " + m_lastReason;

    case GuardianState::HostKeyAttention:
        return "host key requires attention — reconnect stopped: " + m_lastReason;

    case GuardianState::Reconnected:
        snprintf(buf, sizeof(buf), "reconnected after %.1f s", m_lastOutage);
        return buf;

    case GuardianState::GaveUp:
        snprintf(buf, sizeof(buf), "gave up after %d attempt%s", m_attempt,
                 m_attempt == 1 ? "" : "s");
        return buf;

    case GuardianState::UserStopped:
        return "reconnect stopped";
    }
    (void)now;
    return {};
}

} // namespace amber
