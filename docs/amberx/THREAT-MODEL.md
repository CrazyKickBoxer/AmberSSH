# AmberX — threat model

What can go wrong when a remote machine draws windows on this desktop, and
what AmberX does about each of it. Written against the code as it is after
Phase 6; every "mitigation" names the place it lives, and every "gap" is a
gap. The remote side of an SSH session is assumed hostile: it may be
compromised, or it may simply be a machine the user does not fully trust.

## Trust boundaries

```
 remote X client ──SSH channel──▶ AmberSSH (SshSession worker)
                                     │ AmberXControl frames over a DACL'd,
                                     │ handshaken named pipe
                                     ▼
                              AmberXHost.exe  (Job Object; restricted token,
                                               low integrity; mitigations)
                                     │ native windows
                                     ▼
                              the user's desktop
```

Three boundaries: the SSH channel (bytes from the remote are untrusted),
the pipe (AmberSSH ↔ host; each side proves the other), and the process
boundary (the host runs at low integrity and cannot reach up).

## Assets

The session cookie; the pipe secret and nonces; the user's clipboard;
keystrokes into forwarded windows; the contents of every X window; the
identity strip; AmberSSH's own process and windows; the user's files.

## Threats and mitigations

| # | threat | mitigation | where | status |
|---|---|---|---|---|
| 1 | **Malformed X11 requests** | Request lengths validated before use; oversized requests skipped and answered `BadLength`; the X.Org dispatch validates every request body (`REQUEST_SIZE_MATCH`). Requests arrive from a channel that the pipe layer has already length-checked. | `os_connection.c` (from `os/io.c`), dix | mitigated; not fuzzed (Phase 8) |
| 2 | **Integer overflow / signedness** | `ReadRequestFromClient` checks `needed > MAXINT >> 2`; limits refuse pixmap dimensions before `w*h*4` is computed on a large value; icon lengths checked against the property size; LLP64 warnings read and listed (PHASE-1-GATE §3). | `os_connection.c`, `ddx_limits.c`, `ddx_wm.c` | mitigated; 20 LLP64 casts listed, not individually proven |
| 3 | **Oversized pixmaps / images / icons / properties** | Central limits: pixmap 8192² / 64 MiB / 512 MiB total; property 1 MiB; icon 256²; request 4 MiB. Refused with `BadAlloc`; the server continues. | `amberlimits.h`, `ddx_limits.c` | mitigated, gate-tested |
| 4 | **Use-after-free across window/client teardown** | Upstream dix owns resource lifetimes; AmberX's per-client record is freed only in `CloseDownConnection`, its frame records only through the rootless `DestroyFrame`; the close timer is freed with the managed record. | dix, `os_connection.c`, `ddx_wm.c` | relies on upstream; no ASan run yet |
| 5 | **Clipboard theft / clipboard bombs** | Disabled by default; five explicit per-profile modes; text only (no images, file lists or markup, each of which would need its own bounded parser). The policy is enforced in AmberSSH, in the session worker and in the server, and the host — which is the part running at low integrity — cannot reach the Windows clipboard at all, only relay. One transfer is capped at 1 MiB and one selection transfer at 5 s, INCR included. In "ask" mode the confirmation names the direction, the host and the size, and never shows the text. | `app.cpp`, `session.cpp`, `ddx_clipboard.c`, `SafetyDialog.cpp` | mitigated, gate-tested in three modes |
| 6 | **Keystroke monitoring in trusted mode** | Trusted mode is what makes it possible, by design (X11 trusted = one client may see another's input). It is an explicit per-profile opt-in behind a typed-hostname warning, shown as `X11 TRUSTED` on every frame, and cannot be entered from restricted mode without restarting the host. | `ConnectionDialog`, `SafetyDialog`, strip | mitigated by friction and display |
| 7 | **Input injection** | XTEST is compiled; in restricted mode the SECURITY device policy denies untrusted clients keyboard access beyond the core mask. Synthetic input never leaves the host: it has no way to send messages to other processes (low integrity + UIPI). | `Xext/security.c`, token | mitigated |
| 8 | **Focus stealing and fake security dialogs** | A remote window can only be a frame with the identity strip; it cannot draw outside its view, cannot raise itself above AmberSSH's dialogs by `SetForegroundWindow` (foreground rules; low integrity), and the strip text is not the remote's. AmberSSH's own safety dialogs are skinned and owned by AmberSSH's medium-integrity process. | `WinBackend.cpp` | mitigated |
| 9 | **Host-identity spoofing** | The strip is a separate `AmberXStrip` window painted by the host from arguments AmberSSH passed; X pixels are confined to the view's DIB and window. Titles are sanitised and bounded and affect only the caption. | `WinBackend.cpp`, `ddx_wm.c` | mitigated, gate-tested (strip outside the view) |
| 10 | **Named-pipe squatting** | Random name; `FILE_FLAG_FIRST_PIPE_INSTANCE`; DACL of one SID read back and verified; mandatory label Low so only the intended low-integrity client needs it; mutual HMAC handshake over nonces before any frame is honoured. | `Pipe.cpp`, `Handshake.cpp` | mitigated, tested |
| 11 | **IPC frame confusion** | Fixed 16-byte header with magic and version; max payload 1 MiB checked before allocation; types validated; control-only and channel-only types cannot cross; unknown channel ids refused; `Bad` is fatal to the connection. No object graphs are ever deserialised. | `Protocol.cpp` | mitigated, tested |
| 12 | **X11 authorization leakage** | One CSPRNG cookie per session; delivered to the host over the handshaken pipe; stored only in mitauth's table; never logged (`os_log.c` formats every line; the gate greps the host log). Restricted mode registers it UNTRUSTED with a timeout: an unused cookie expires. The remote never holds a cookie that opens any display but its own session's host. | `os_auth.c`, `os_log.c` | mitigated, gate-tested |
| 13 | **Cross-session channel mix-up** | One host process per session; channel ids are owned per controller and validated on both ends; a frame naming an unknown channel is refused. | `ChannelTable`, `WinBackend.cpp` | mitigated by construction; two live sessions not yet run together |
| 14 | **Remote title / path / control-character injection** | Titles: control characters removed, cut at 256 bytes on a UTF-8 boundary, shown only in the caption. Nothing from the remote reaches a log line or the strip. | `ddx_wm.c` | mitigated |
| 15 | **GPU device removal** | No GPU: frames are system-memory DIBs. | — | not applicable yet |
| 16 | **AmberXHost crash loops** | The controller reports a closed pipe as one status line and closes the session's X11 channels; the session does not restart the host on its own. A crash is one crash. | `session.cpp` `PumpAmberX` | mitigated; no automatic restart, which is the point |
| 17 | **SSH reconnect races** | The controller is owned by the session thread and stopped when that thread ends; a reconnect is a new thread with a new host; the Job Object kills a host whose controller is gone. | `session.cpp` | mitigated |
| 18 | **Host escaping its box** | Restricted token (all privileges removed), integrity Low, mitigation policies (no dynamic code, no extension points, no remote/low-label images, strict handles, ASLR), child-process creation forbidden, Job Object limits (1 process, 1 GiB, 50 % CPU, no `ExitWindows`/system-parameter/display changes). The confinement is read back from the token after launch. | `AmberXController.cpp` | mitigated, gate-tested (token read-back) |
| 19 | **Resource exhaustion of AmberSSH through the host** | The pipe is synchronous with backpressure; AmberSSH's per-channel queues have 1 MiB watermarks; the host's memory and CPU are capped by the job. | `session.cpp`, job | mitigated |
| 20 | **A misleading badge** | The mode word on the strip is the enforced mode: `X11 RESTRICTED` only when the cookie is registered untrusted, `X11 TRUSTED` only when the user opted in. Before Phase 5 the strip said `X11 FORWARDED` for exactly this reason. | `session.cpp`, `os_auth.c` | mitigated |

| 21 | **One forwarded client attacking another** | Nothing stops it. Upstream's `SecurityDoCheck` permits any access whose *object* is untrusted, and every forwarded client in a session shares the untrusted level, so two forwarded programs reach each other exactly as on a normal X server. Restricted mode is a boundary between the session and the server, not within the session. | `Xext/security.c` | **not mitigated, by upstream design** |
| 22 | **Selection theft between trust levels** | The SECURITY extension does not register `XACE_SELECTION_ACCESS` in 21.1.24 — only the SELinux hooks do, and they are not built — so selection ownership is not restricted by trust level. | — | **gap; policy belongs with the Phase 6 clipboard** |

| 23 | **An extension as an attack surface** | Restricted clients may use ten extensions by name (`ddx_policy.c`), each with its reason recorded there. XTEST (input injection), SECURITY (authorization management), DAMAGE and Composite (both screen scraping) and MIT-SHM are excluded although compiled. Upstream's own list of two is not usable — it denies RENDER and XKEYBOARD, so nothing runs — and a default mode nothing runs in pushes users to trusted mode, which is worse. | `ddx_policy.c` | mitigated by an allowlist that is wider than upstream's and narrower than trusted |
| 24 | **Input from one forwarded client seen by another** | XI2 raw events can be selected on the root, so a forwarded client can see input directed at another forwarded window. The AmberX root only ever carries input the user aimed at a forwarded window — the server never sees the rest of the desktop's input — so this is not a keylogger for the machine. Within the session it is the same boundary as 21. | `ddx_policy.c`, `WinBackend.cpp` | **the same gap as 21, named separately because it is easy to miss** |

## Gaps, plainly

- Forwarded clients are not isolated from each other (21), and selections
  are not restricted by trust level (22). These are the two things
  "restricted" does not mean, and both are upstream semantics rather than
  AmberX shortcuts. Isolating them would break copy-and-paste between two
  forwarded applications, so it is a deliberate open item, not an oversight.
- No fuzzing has been run against the request parser (1); Phase 8.
- Two sessions have not been run at once (13).
- AppContainer was evaluated and not adopted for now: it would need the
  pipe and the log handle granted to a per-host capability SID and changes
  to how frames are created; the restricted low-integrity token gives most
  of the same isolation with none of that. Revisit when the clipboard and
  GPU presentation exist, which is when the trade-off changes.
- "Closing AmberSSH kills the tree" rests on `KILL_ON_JOB_CLOSE` plus the
  host's own parent-watch thread. It is exercised every time the preview
  runs (no stray host after a stop) and, since Phase 5, by
  `tools\amberx-kill-test.ps1`, which kills AmberSSH with
  `TerminateProcess` — no cleanup path at all — and finds the host gone
  within 100 ms.
