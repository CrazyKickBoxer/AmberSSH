# AmberX — Phase 5 gate

Phase 5 is the security phase: restricted versus trusted X11, a real box
around AmberXHost, central resource limits, and a written threat model.
The rule the phase is judged by is the prompt's own: *restricted mode must
be real, not a label*. This is the report, including what restricted mode
does **not** do.

Everything below was measured by `AmberSSH.exe --preview-amberx` from the
`build-amberx` tree unless it says otherwise. A transcript of the run this
report describes is `docs/amberx/phase5-preview-run.txt` (75 checks, 0
failures, repeated twice).

## The prompt's five gate criteria

```
1. restricted and trusted differ in enforced behaviour ... PASS
     ChangeProperty on the trusted root window:
       restricted → BadAccess (10)      trusted → success
     Same client code, same request, same server build; the only
     difference is whether the cookie was registered untrusted.
2. authorization and clipboard secrets absent from logs ... PASS (auth)
     The preview runs a host with the cookie set, then greps the whole
     host log for the 32 hex digits: absent. Clipboard: nothing to leak,
     there is no clipboard bridge until Phase 6.
3. resource-exhaustion tests fail safely ................. PASS
     20000x20000 pixmap        → BadAlloc, server continues
     2 MiB property            → BadAlloc, server continues
     2100 InternAtoms in a row → 102 refused by the rate limit, and the
                                 server answers a GetGeometry afterwards
4. closing AmberSSH kills the whole host tree ............ PASS
     tools\amberx-kill-test.ps1: TerminateProcess on AmberSSH; the host
     is gone within 100 ms. Not a clean shutdown path — the job object's
     KILL_ON_JOB_CLOSE is what does it.
5. a crashed host cannot crash the terminal process ...... PASS
     A host launched with a deliberate crash: the controller sees a
     closed pipe, reports it, starts a fresh host, and AmberSSH lives.
```

## What restricted mode actually enforces

The cookie is registered as a `SecurityAuthorizationRec` with
`trustLevel = XSecurityClientUntrusted` (`os_auth.c`), so every client that
connects with it is untrusted to upstream's SECURITY extension, which is
compiled in and driven. In trusted mode the authorization is simply not
registered, and clients are trusted. There is no third state and no
promotion: the mode is fixed when the host is launched, from the profile,
and changing it means a new host.

What that buys, precisely — from `Xext/security.c`, `SecurityDoCheck`:

- an untrusted client may **not** write, add to, or configure resources
  owned by a trusted client, including the root window and everything the
  in-server window manager owns (the WM runs as the server client);
- it may **not** read input belonging to trusted clients, which is the
  keystroke-monitoring case;
- it **may** grab the core keyboard for its own windows: `DixGrabAccess`
  is in `SecurityDeviceMask` by design, because a forwarded application
  with a menu open needs it. Grabbing is not monitoring.

And the boundary, stated plainly because it is easy to assume otherwise:

> **Untrusted clients are not isolated from each other.** `SecurityDoCheck`
> returns `Success` whenever the *object* is untrusted, so two forwarded
> applications in the same session can reach each other's windows and
> properties exactly as they could on a normal X server. Restricted mode
> is a boundary between the forwarded session and the server, not between
> two forwarded programs.

That is upstream X11 semantics, not an AmberX shortcut, and it is why the
one thing the strip promises is the mode word. Isolating forwarded clients
from one another would need a per-client policy that upstream's SECURITY
model does not have, and it would break copy-and-paste between two
forwarded applications. It is listed in the threat model, not fixed here.

Selections are the related gap: in xserver 21.1.24 only the SELinux hooks
register `XACE_SELECTION_ACCESS`, and SELinux is not in this build, so the
SECURITY extension does not restrict selection ownership by trust level.
The clipboard bridge in Phase 6 is where that policy belongs, on the
AmberSSH side.

**The authorization timeout** is `--auth-timeout` seconds (default 1200, 0
disables), implemented with the server's own `TimerSet` in `os_auth.c`. On
expiry the authorization is removed: a client that connects afterwards is
refused with *"No authorization is installed for this display"*. Clients
already connected keep their connections — the "existing policy" the
prompt leaves to the implementation, chosen so an expiry cannot kill an
editor with unsaved work. The preview proves both halves with a 2 s
timeout.

## Trusted mode: how a user gets there

| requirement | how |
|---|---|
| explicit per-profile opt-in | `ConnectionProfile::x11Trust` (0 off, 1 saved, 2 session-only); radio row in the connection dialog |
| high-friction warning | `ShowTrustedX11Dialog` — a painted, skinned modal that states what trusted X11 means and requires the **hostname typed back**; declining reverts the profile to restricted |
| persistent `X11 TRUSTED` on every window | the identity strip's mode word, on every frame, painted by the host from `--mode` |
| no silent promotion | the mode is a launch argument; a restricted host cannot become trusted, and the cookie's registration is decided once at cookie time |
| optional session-only | trust level 2 is honoured for the session and written back as 0 by `ProfileStore` |

## Local sandbox — the prompt's evaluation order

1. **AppContainer** — evaluated, not adopted yet. The host would need the
   pipe and its log handle granted to a per-host capability SID, and the
   frame windows re-examined; the restricted low-integrity token below
   gives most of the same isolation without that work. Revisit when the
   clipboard and GPU presentation exist, because that is when the
   trade-off changes. Recorded in `THREAT-MODEL.md`.
2. **Restricted token / low integrity** — adopted.
   `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)` plus a Low integrity
   label, launched with `CreateProcessAsUserW`. Read back from the running
   host's own token: `integrity=Low privileges=1`.
3. **Job Object** — `KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS = 1` (the host
   cannot spawn anything), `DIE_ON_UNHANDLED_EXCEPTION`, 1 GiB process and
   job memory, a **hard** 50 % CPU cap, and UI restrictions denying
   `ExitWindows`, system parameters, display settings, global atoms and
   other desktops.
4. **Process mitigation policies** — DEP, SEHOP, bottom-up and high-entropy
   ASLR, no dynamic code, extension points disabled, strict handle checks,
   heap terminate, no remote or low-label image loads.
5. **Narrow ACLs** — the pipe is `D:P(A;;GA;;;<user SID>)S:(ML;;NW;;;LW)`:
   one access-allowed ACE for the user, plus a Low mandatory label so the
   low-integrity host may write to it. The DACL is read back and verified
   at launch. The host is given no other handles but its log.

`PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY` was tried and **removed**: with
it the host fails to start (`0xC0000142`, a DLL initialisation failure
under the restricted token). The job's `ActiveProcessLimit` of 1 gives the
same "no children" guarantee, so nothing was lost. `AMBERX_SANDBOX_DIAG`
(`notoken|nomitig|nojoball|nojobui|nojobmem|nojobcpu`) turns individual
pieces off for diagnosis; it is a developer switch, and the preview never
sets it.

## Resource limits

Central defaults live in `src/amberx/server/amberlimits.h`; enforcement is
`ddx_limits.c`, `os_connection.c` and the Windows side.

| the prompt asks for | value | enforced where | status |
|---|---|---|---|
| X clients per session | 64 + server client | `LimitClients`, `os_connection.c` | in place |
| windows per client / per session | 4096 / 16384 | XACE resource hook + `DestroyWindow` wrapper | in place |
| pixmap dimensions | 8192 either side | `CreatePixmap` wrapper | **gate-tested** |
| aggregate pixmap memory | 64 MiB one, 512 MiB all | `CreatePixmap`/`DestroyPixmap` wrappers | in place |
| shared GPU texture use | — | no GPU path yet; frames are system-memory DIBs | n/a until Phase 8 |
| property size | 1 MiB | `ProcVector[X_ChangeProperty]` wrapper | **gate-tested** |
| icon size / decoded pixels | 256 per side (65536 px) | `ddx_wm.c` `_NET_WM_ICON` | in place |
| clipboard payload size | — | no clipboard bridge yet | **Phase 6** |
| selection transfer duration | — | same | **Phase 6** |
| request size incl. BIG-REQUESTS | 4 MiB (`maxBigRequestSize`) | `amber_limits_os_init` | **gate-tested** (BIG-REQUESTS enabled and bounded) |
| atom creation rate | 2000 per 10 s per client | `ProcVector[X_InternAtom]` wrapper | **gate-tested** |
| pending events | bounded by construction, not counted: 16 KiB in the server's output buffer, then backpressure through a 1 MiB per-channel watermark in AmberSSH | `os_connection.c`, `session.cpp` | in place, no separate counter |
| IPC queue bytes | 1 MiB max frame payload | `Protocol.h` | in place |
| per-channel queue bytes | 1 MiB watermark | `kTunnelHighWater`, `session.cpp` | in place |
| CPU percentage | 50 %, hard cap | Job Object | in place |
| CPU time | not capped separately | — | see limitations |

Every violation is a `BadAlloc` to the offending client. No limit path can
end the server, and none of them can reach AmberSSH, which is a different
process.

### Where the limits are enforced, and why not XACE

The first implementation put property size and atom rate in an XACE
`XACE_CORE_DISPATCH` callback. It never fired. In this xserver
`XaceHookDispatch` runs `XaceHooks[XACE_EXT_DISPATCH]` only — core opcodes
never reach the core-dispatch hook — so the limits were a label, exactly
what the phase forbids. They are now wrappers on `ProcVector` and
`SwappedProcVector` for `X_ChangeProperty` and `X_InternAtom`, saving the
originals and calling through, which is the mechanism upstream extensions
use for the same reason. The swapped wrapper reads the request fields
byte-swapped, because dix's swapped procedure has not run yet when the
wrapper sees the bytes. Both are exercised by the gate.

## A correctness bug this phase found

The exhaustion tests stalled, and the cause was not the limits: the port's
`ReadRequestFromClient` cleared the client's readiness whenever a request
consumed the whole input buffer. Upstream sets `AvailableInput` there
instead and leaves readiness alone — readiness is level-triggered on the
socket. With it cleared, any request that made dix leave its inner loop
early (an error reply does) left already-buffered requests unread until
the client happened to send more, which showed up as one-second stalls
under load and as a refused request "hanging" the ones behind it.

Readiness is now dropped in exactly one place, `YieldControlNoInput`, when
a channel read comes up empty or short. `WriteToClient` also no longer
flushes on every reply when the buffer is empty; it buffers as upstream
does and lets `FlushAllOutput` decide. This is a latency and correctness
fix that has nothing to do with security, but it was found here and it is
in this phase's diff.

`AMBERX_TRACE_IO=1` is the instrument that found it: flushes, clients
running out of input, deferred flushes and slow waits, sizes and counts
only, never request or reply content. With it set, the controller appends
the host log instead of truncating it per launch.

## What changed in this pass

| file | what |
|---|---|
| `src/amberx/server/os_auth.c` | cookie registered as an untrusted `SecurityAuthorizationRec` with a `TimerSet` expiry; trusted mode registers nothing |
| `src/amberx/server/amberlimits.h` | every limit in one place with its default |
| `src/amberx/server/ddx_limits.c` | new: `ProcVector` wrappers, XACE resource hook, `DestroyWindow`/`CreatePixmap`/`DestroyPixmap` wrappers, per-client counters |
| `src/amberx/server/os_connection.c` | readiness fix, upstream output buffering, `AMBERX_TRACE_IO` |
| `src/amberx/server/os_misc.c` | `amber_limits_os_init()` from `OsInit` |
| `src/amberx/server/ddx_screen.c` | limits installed last on the screen |
| `src/amberx/AmberXController.{h,cpp}` | restricted token + integrity label + mitigations + job; confinement read back; `authTimeoutSeconds`; trusted flag; host log appends under the trace |
| `src/amberx/control/Pipe.cpp` | mandatory Low label in the pipe SDDL |
| `src/amberx/PreviewClient.{h,cpp}` | Phase 5 checks: restricted vs trusted, limits, timeout, crash survival, cookie-absent-from-log |
| `src/ui/SafetyDialog.{h,cpp}` | `Kind::TrustedX11` — the typed-hostname trusted-X11 warning, skinned like the rest |
| `src/ui/ConnectionDialog.cpp` | the X11 trust radio row and the dialog it raises |
| `src/profiles/{ConnectionProfile.h,ProfileStore.cpp}` | `x11Trust`; session-only never persists |
| `src/ssh/session.{h,cpp}` | `x11Trusted` through to `Launch`; mode word is now `X11 RESTRICTED` / `X11 TRUSTED` |
| `tools/amberx-kill-test.ps1` | new: the kill-the-parent test the preview cannot run on itself |
| `docs/amberx/THREAT-MODEL.md` | new: 20 threats, mitigation, where it lives, and the gaps |

The trusted-X11 dialog is painted, not composed from system controls, so
it takes every interface skin like the host-key and blast-radius dialogs
it sits beside.

## Decisions

- **The strip's mode word is the enforced mode.** `X11 RESTRICTED` appears
  only when the cookie was registered untrusted; `X11 TRUSTED` only after
  the typed opt-in. Phase 4 deliberately said `X11 FORWARDED` because
  neither was true yet.
- **An expiring authorization does not disconnect existing clients.** The
  prompt allows either; killing live windows on a timer would lose work.
- **`ProcVector` wrappers over an XACE hook**, because the XACE hook does
  not run for core requests in this server. Measured, not assumed.
- **No AppContainer yet**, with the reason and the revisit condition
  written down rather than left as a silence.
- **Limits refuse, never terminate.** The prompt allows terminating the
  offending client; refusing with `BadAlloc` is what a well-written client
  already handles, and a flood that is merely refused stops being
  interesting.

## Limitations

- Untrusted clients are not isolated from each other (above). This is the
  single largest thing "restricted" does not mean.
- Selections are not restricted by trust level; that policy belongs with
  the Phase 6 clipboard bridge.
- No separate CPU-time cap (`JOB_OBJECT_LIMIT_JOB_TIME`); the hard 50 %
  rate cap is what bounds a spinning host today.
- "Pending events" is bounded by backpressure rather than a configurable
  count, so it has no entry in `amberlimits.h`.
- The limits have been exercised by the preview's specific cases, not
  fuzzed; the request parser has had no fuzzing at all (Phase 8).
- Two sessions have not been run at once, so cross-session channel
  isolation is by construction only.
- Still no real X application: the live gate from Phase 4 is still owed,
  and Phase 5 changes nothing about how to run it.

## How to re-run this gate

```
cmake --build build-amberx --config Release --target AmberSSH
build-amberx\Release\AmberSSH.exe --preview-amberx      # writes %TEMP%\amberx-preview.txt
powershell -ExecutionPolicy Bypass -File tools\amberx-kill-test.ps1
```

The preview exits non-zero if any check fails and prints `PREVIEW FAILED`.
`AMBERX_TRACE_IO=1` adds the I/O trace to `%TEMP%\amberx-host.log`.
The regression suite for the rest of AmberSSH is `build\tests\Release\AmberTests.exe`
(375 test cases, 67,961 assertions, passing with these changes).

## Gate verdict

**Phase 5: pass.** All five criteria are met and measured, restricted mode
is an enforced difference rather than a label, and the two things it does
not do — isolate forwarded clients from each other, restrict selections —
are written down here and in the threat model instead of being implied
away. The live application gate inherited from Phase 4 remains open.
