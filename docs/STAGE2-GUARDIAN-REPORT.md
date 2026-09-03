# Stage 2 — Session Guardian: Reconnect, Restore and Reattach

Implementation report. What was investigated, what was built, what is proven
by test, what was proven by running it, and what is not proven at all.

## 1. Investigation

The spec asked for this before any code. Findings, in its order.

**1. Connection state machine.** There wasn't one. `SshSession` had a boolean
(`m_running`) and posted four event types — `Status`, `HostKeyPrompt`,
`Connected`, `Closed`/`Error` — and `Session::state` was a seven-value enum
the UI set by hand from those events. The reconnect logic was eleven lines in
`PumpSshEvents`: a `1 << attempt` backoff up to 32 s, gated on
`profile.autoReconnect && everConnected && !userClosed`.

**2. Disconnect / error classification.** None. Every failure arrived as free
text in one `Error` event, and the old reconnect retried *all* of them — a
wrong password, a host-key mismatch and a dropped Wi-Fi link were the same
thing to it. This was the single biggest gap and is what most of Stage 2 is.

**3. Keepalive.** `libssh2_keepalive_config` + `keepalive_send` in the I/O
loop, driven by `Connection > keepaliveSeconds` (default 30, 0 = off); plus
optional `SO_KEEPALIVE`. Unchanged by this work.

**4. Port-forward ownership.** Listeners are opened inside `ThreadMain` after
authentication and closed in its teardown, so they are owned by the worker
thread and die with it. `SshSession::Start` calls `Disconnect()` first, which
**joins** the old thread — so the old listeners are always closed before the
new ones bind. That is what prevents duplication, and it was already correct.
See §6 for the thing that was *not* correct.

**5. Jump-host lifecycle.** There isn't one. `ThreadMain` refuses outright if
`jumpHost` is set, rather than silently connecting direct. So "jump host
reconnect" cannot be tested in this build — there is no jump host to reconnect
through.

**6. Authentication retry.** None, and deliberately so: secrets are zeroed
immediately after the handshake. Reconnects work because the *session* keeps
the password and passphrase in a `SecureString` for its own lifetime.

**7. Re-opening saved profiles.** `StartReconnect(Session&)` rebuilds an
`SshConfig` from the profile plus those retained secrets. Reused unchanged.

**8. Does the grid survive a disconnect?** Yes — the `Grid` belongs to the
`Session`, not the transport, and nothing clears it on close. But
`StartReconnect` **fed text into the parser** (`[amberssh: reconnecting...]`
in yellow), which means the annotation was in the grid: it appeared in
selections, in scrollback searches and in session logs, indistinguishable from
something the server had sent. Removed — see §3.

**9. Notification paths.** `TrayNotifier::Toast` (Shell_NotifyIcon NIF_INFO),
used in exactly one place before this (output triggers). Status bar and
transient `SetStatus` line for the foreground case.

## 2. What was built

**`src/sessions/Guardian.{h,cpp}` — the state machine, as a pure module.**
No Windows, no libssh2, no terminal. That is the point: classification,
backoff, the security gates, the reattach commands and the restore plan are
all deterministic functions, so the whole feature is unit-testable without a
network. The app drives it with three events and one `Tick()` per frame.

States, all nine the spec asked for plus `Idle`: `Connected`,
`ConnectionLost`, `WaitingToReconnect`, `Reconnecting`,
`AuthenticationRequired`, `HostKeyAttention`, `Reconnected`, `GaveUp`,
`UserStopped`.

**Classification.** `ClassifyDrop` maps a transport's message onto one of six
classes, and only `Transient` is retryable:

| Class | Examples | Retried |
|---|---|---|
| `HostKeyAttention` | mismatch, not listed, rejected | never |
| `AuthRequired` | auth failed, bad passphrase, no agent identity | never |
| `Fatal` | unresolvable host, PTY/shell request failed, jump host unsupported | never |
| `RemoteClosed` | the shell exited, the pty closed | never |
| `UserInitiated` | cancelled | never |
| `Transient` | timeouts, resets, read/write errors, socket EOF | yes |

The two security classes are matched **first and explicitly**, so an
unrecognised message can never fall into them. Unrecognised messages are
`Transient`, which is safe because reconnect only ever arms after a session
has connected once.

**Backoff.** 1 → 2 → 5 → 10 → 30 s, then held at 30, with optional jitter
(default ±20 %) derived deterministically from a per-profile seed. Two tabs
pointed at the same host therefore back off on different schedules, which is
what stops a fleet stampeding a server or a VPN the moment it returns.

**Retry limits.** A finite count (default 6) or 0 for "keep trying until
stopped". Never a tight loop: the minimum delay is 0.25 s even at maximum
jitter, and there is a test for it.

**Security gates.** `Guardian` has no code path from a gate state to an
attempt. `Tick()` returns false in `AuthenticationRequired`,
`HostKeyAttention`, `GaveUp` and `UserStopped`, and the tests walk an hour of
simulated time asserting it. The only way out is `RetryNow()`, which requires
a human action — and the attempt it starts is an ordinary connection that runs
the full host-key and authentication checks, because `Guardian` never touches
them.

**Ask mode** raises one modal per *outage*, not per attempt.

**Restore.** After a successful reconnect, and only when the profile asked:
the reattach command, then the working directory. That is the entire closed
set — `BuildRestorePlan` cannot produce anything else, so nothing the user
typed is ever replayed.

**Reattach.** tmux → `tmux attach-session -t 'NAME' || tmux new-session -s
'NAME'`. screen → `screen -R 'NAME'`. Both attach if the session exists and
create it otherwise; `-d`, `-D`, `kill-session` and `wipe` are deliberately
absent, and there are tests asserting they stay absent. Custom is run
verbatim, trimmed, and refused if empty or multi-line.

**Interrupted work.** If OSC 133 said a command was running when the link
died, it is filed with `interrupted = true` and `exitCode = -1`, and the
journal draws it with an amber pip rather than the red "failed" one. There is
no field in `InterruptedCommand` that could hold a fabricated status.

**UI.** A Connection → **Guardian** page (policy, limits, jitter,
notifications, annotation) and Guardian → **Reattach** (multiplexer, session
name, custom command, what to restore); File → Reconnect Now / Stop
Reconnecting, both also in the palette; a pulsing connection pip and a live
guardian read-out in the status bar. Every surface is drawn from the active
`ChromeSpec`, per the standing rule; captured on LCARS and Letterpress.

## 3. Design decisions worth stating

**The annotation is not terminal content.** `Session::Notice` is anchored to
an absolute row id and drawn over the grid by `DrawNotices`, exactly like the
tide marks and the error embers. Nothing reaches the parser, so "connection
lost" cannot appear in a copy, a scrollback search or a session log. The old
in-band banner was removed to make that true.

**Reconnect arms only after a first successful connect.** A wrong host or a
wrong password on the first attempt is a settings problem; retrying it on a
timer just replays the mistake. This is also what makes the "unknown reason →
transient" default safe.

**A socket EOF is not a shell exiting.** `remote closed the connection` (a
telnet/raw peer hanging up — what a server restart and an idle timeout both
look like) is retried; `remote closed the session` (an SSH shell ending,
because the user typed `exit`) is not.

**The working directory is restored only without a multiplexer.** tmux and
screen bring their own panes back, each already where it was; a `cd` on top of
that would move the user somewhere they never were.

**A session name is validated, not quoted-and-hoped.** `[A-Za-z0-9._-]`, 1-64
characters, because it is interpolated into a shell command line. An OSC 7
directory *is* quoted, POSIX-style, and refused outright if it contains
control characters.

## 4. Verified by automated test

`tests/GuardianTests.cpp` (17 cases) and two new cases in
`ProfileStoreTests.cpp`. Suite total: **3210 assertions in 180 test cases, all
passing.** Against the spec's own list:

| Spec test | Where |
|---|---|
| transient transport loss | "a transient drop is retried with backoff" |
| authentication failure | "automatic reconnect never drives past a security question" |
| host-key mismatch | same, first section — an hour of ticks, no attempt |
| user cancellation | "a manual disconnect is never reconnected", "the user can stop a reconnect" |
| backoff progression | "backoff climbs the ladder and then holds" + the ladder walked live |
| forward restoration | **partly** — the `restoreForwards` flag round-trips and clears `cfg.forwards`; no live forward was restored (§6) |
| jump host reconnect | **not testable** — this build has no jump host |
| tmux reattach command generation | "reattach commands are conservative and never destructive" |
| no duplicate reconnect worker | "Tick fires once per attempt, never twice" |
| shutdown during reconnect wait | "shutting down during the reconnect wait leaves nothing scheduled" |

Plus: jitter bands and determinism, the anti-stampede property, the retry
limit and revival from `GaveUp`, Ask-mode consent per outage, shell quoting of
a hostile OSC 7 payload, control-character refusal, the profile round trip,
and that a pre-Stage-2 file's `autoReconnect` still means what it meant.

## 5. Verified by running it

A real TCP peer on loopback that AmberSSH connects to, which is then killed
and later restarted, with the app driven end to end
(`scratchpad/guardian_e2e.ps1`, `guardian_modes.ps1`). Captures in the
scratchpad.

- **Automatic** — the link drops, the annotation appears, the status bar counts
  down, attempts climb the ladder (`attempt 3 of 6` at 12 s, which is 1+2+5),
  the peer returns and the tab reports `reconnected after 12.6 s`. The
  terminal contents from before the drop are still on screen.
- **Give up** — with a limit of 2, exactly two attempts are made and the tab
  settles on `gave up after 2 attempts`. Nothing further happens.
- **Off** — no reconnect at all; the tab closes with `read error`, which is
  precisely the pre-Stage-2 behaviour.
- **Ask** — the modal appears (`AmberSSH — connection lost`) and nothing is
  attempted until it is answered.
- **Annotation** — one per outage, not one per attempt; stacked when several
  land on one row; legible on both a dark skin and a light one.
- **Dialog** — the Guardian and Reattach pages, captured on LCARS and
  Letterpress: correct palette, fonts, casing, disabled-field sync, no
  clipping, no overflow.

## 6. Bugs found and fixed during the work

**`SO_REUSEADDR` defeated duplicate-listener detection.** Forward listeners
set it. On Windows that option does not mean what it means on POSIX: it lets a
*second* socket bind the same address and port while the first is still
listening, after which the kernel splits incoming connections between them.
So a forward that was already up bound a second time in silence instead of
reporting "port busy" — exactly the duplicate listener Stage 2 must not
create. Removed. A plain bind fails with `WSAEADDRINUSE` instead, and
rebinding after our own listener closes still works. `SO_EXCLUSIVEADDRUSE` was
considered and rejected: it can refuse a rebind while connections accepted on
the old listener are still in `TIME_WAIT`, which would break the reconnect
case this stage exists to fix.

**File → Disconnect did not disarm the reconnect.** It never set
`userClosed` — only the Ctrl+Shift+D path did. With a profile set to reconnect
automatically, the menu item would have appeared not to work: the session
would come straight back. Both paths now tell the guardian first.

**Four rendering defects in the annotation, all found by looking at it:**
the additive rule struck the label through (the rule now stops where the label
starts); several notices on one row overprinted each other (they stack);
stacking upwards pushed the newest — "reconnected" — off the top of the pane
(it stacks downwards); and the label's plate was painted with the *chrome*
background, which on the light skins put a near-white box under a dark accent
and swallowed the text whole (the plate is the terminal's ground, and a skin
accent too dark or too light for it is lifted or deepened until it carries).

**`restoreForwards` was a setting that did nothing.** Persisted, shown in the
dialog, never read. It now clears `cfg.forwards` on a reconnect — the first
connect always gets them.

**Two dialog labels overflowed the page** on uppercase skins, where the drawn
text is wider than the source string. Shortened, with the detail moved into
the adjacent note.

## 7. Not proven

**There is no SSH server on this machine** — no `sshd.exe`, no OpenSSH Server
service. Everything below needs one, and none of it was run:

| Item | Status |
|---|---|
| Host-key mismatch stopping a live reconnect | **unit-tested only.** No live mismatch was produced |
| Authentication failure stopping a live reconnect | **unit-tested only** |
| Windows Hello gate during a reconnect | not exercised |
| L / R / D forward restoration after a reconnect | **not run.** R forwards are not supported by this build at all |
| Duplicate listeners on reconnect | **not measured.** The `SO_REUSEADDR` fix is reasoned, not observed |
| Tunnels-only (`noShell`) profiles | not run |
| Jump host + final target | **not applicable** — this build refuses jump hosts |
| tmux / screen actually attaching | **not run.** The commands are asserted; no server ran them |
| OSC 7 directory restore on a real shell | not run |

**Network transitions were not tested.** Wi-Fi loss and recovery, an Ethernet
switch, VPN connect/disconnect, laptop sleep/wake, a real server restart and an
idle timeout are all listed in the spec and none of them was performed. What
was performed is a loopback peer being killed and restarted, which produces
the same `read error` / `connection failed (winsock 10061)` sequence but
proves nothing about how Windows behaves across a real link change.

**Notifications were not observed.** The toast fires only when the window is
in the background; the captures needed it in the foreground. The code path is
the same one the output triggers already use, but that is an argument, not a
measurement.

**Handle and thread leaks across repeated reconnects were not measured.** The
worker is joined before each new one starts, and the guardian will not tick
while `ssh.Running()`, but no counter was watched over many cycles.

## 8. Acceptance criteria

| Criterion | Status |
|---|---|
| Reconnect behavior is predictable and visible | yes — state, attempt number and countdown in the status bar; annotation on the grid; verified on screen |
| Security prompts are never bypassed | structurally: no path from a gate state to an attempt; unit-tested over an hour of simulated time. Not verified against a live server |
| Backoff prevents tight retry loops | yes — ladder and floor tested, and walked live |
| Port forwards restore without duplication | **not verified.** One real defect fixed; no live forward was restored |
| tmux/screen reattach works when explicitly enabled | command generation verified; execution not |
| Interrupted-command metadata is truthful | yes — no exit code is invented, and the type cannot hold one |
| Existing manual disconnect behaves as expected | yes, and better: the menu item now actually disconnects a profile set to reconnect |
| Implementation report | this document |
