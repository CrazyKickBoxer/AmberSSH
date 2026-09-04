# AmberX — Phase 2 specification, as written by the linker

> **Addendum, 2026-09-04 — executed.** The plan below was carried out and the
> gate passed; PHASE-2-GATE.md is the report. Where reality differed: §A lost
> `xsha1.c` (its CryptoAPI path pulls `<windows.h>` into an X unit) and
> `strcasecmp.c` (BSD types), both trivially re-provided; `os/log.c` and the
> timer half of `WaitFor.c` were rewritten rather than retained, the timer
> list carried over with attribution; §C gained a keymap layer because
> `xkb/ddxLoad.c` spawns `xkbcomp` and was excluded; §E gained zlib, because
> libXfont2's built-in fonts are gzipped. The final unresolved count was zero.

Phase 2 is "a standalone minimal AmberX server". Rather than describe it from
the prompt, this document describes it from the only source that cannot be
wrong about what is missing: the linker. `run-link-probe.sh` archives the 228
compiled core objects into a library, links a stub `main` against it with
`/WHOLEARCHIVE`, and records every unresolved external.

```
objects archived: 231      distinct unresolved symbols: 194
  pixman_*                71   → build pixman        (library, not porting)
  libXfont2               13   → build libXfont2     (library, not porting)
  not a library          110   → THE PORT

  of which, by responsibility (name-pattern buckets, may overlap):
    connections & auth   37     logging   7     time & timers  11
    memory & strings      5     DDX hooks 9     extension flags 9
    POSIX shim leftovers  3
```

The first run reported 199 and 115; adding `Xext/xace.c` to the allowlist
resolved its seven `Xace*` symbols and introduced two of its own, so the
honest number moved by five. So the Windows port of the X.Org core is
**110 symbols**, and this is the map of them. Names are exact; every one is
in `probe-out/unresolved.txt`.

## A. Retain from upstream `os/` — pure C, MIT, just compile it (~35 symbols)

These have no platform dependency worth the name. They are in `os/` only
because that is where X.Org keeps utilities. Approve the files, compile them
in, done.

| symbols | upstream file |
|---|---|
| `XNFalloc XNFrealloc XNFreallocarray XNFstrdup Xstrdup xreallocarray Xasprintf` (and the `XNF*`/`Xv*` printf family) | `os/utils.c`, `os/xprintf.c` |
| `strlcpy strndup xstrcasecmp reallocarray timingsafe_memcmp` | `os/strlcpy.c`, `os/strndup.c`, `os/strcasecmp.c`, `os/reallocarray.c`, `os/timingsafe_memcmp.c` |
| `x_sha1_init x_sha1_update x_sha1_final` | `os/xsha1.c` with `HAVE_SHA1_IN_CRYPTOAPI` — upstream already has the Windows backend |
| `OsLookupColor` | `os/oscolor.c` |
| `TimerSet TimerCancel TimerFree AdjustWaitForDelay` (the timer heap) | the timer half of `os/WaitFor.c` — worth extracting, because the other half is `select()` |
| `GenerateAuthorization RemoveAuthorization AuthorizationFromID AuthorizationIDOfClient CheckUserAuthorization` | `os/auth.c`, `os/mitauth.c` — the MIT-MAGIC-COOKIE-1 table is pure C |
| `LogMessage LogMessageVerb VAuditF auditTrailLevel FreeAuditTimer ErrorF ErrorFSigSafe VErrorF FatalError` | `os/log.c` — retain, but see rule 9 below |

**Rule 9 applies here**: `os/log.c` writes wherever it is pointed. AmberX
points it at a bounded, non-sensitive sink, and nothing that logs a request
body, a cookie or window content is ever wired to it.

## B. Replace: the connection layer (~40 symbols) — **the substantive port**

This is `os/connection.c`, `os/io.c`, `os/access.c`, `os/client.c` and the
`select()` half of `os/WaitFor.c`. Upstream's versions listen on TCP and Unix
sockets, manage host access lists, and multiplex file descriptors. **AmberX
has none of that.** Clients arrive over the AmberXControl pipe as
already-authenticated channels. The replacement is therefore *smaller* than
what it replaces, and that is the single most important fact in this
document.

| responsibility | symbols | AmberX implementation |
|---|---|---|
| lifecycle | `CreateWellKnownSockets ResetWellKnownSockets CloseWellKnownConnections ListenToAllClients OnlyListenToOneClient LimitClients` | **No sockets.** "Well-known sockets" is the control pipe; listen/limit map onto `ChannelTable` (max 64). |
| per-client I/O | `ReadRequestFromClient WriteToClient WriteFdToClient FlushAllOutput FlushIfCriticalOutputPending SetCriticalOutputPending ResetOsBuffers InsertFakeRequest ResetCurrentRequest` | Per-channel input/output byte buffers fed by `ChannelData` frames. `ReadRequestFromClient` is the one non-trivial function: it must present whole X requests from a byte stream, honouring BIG-REQUESTS. Upstream's `io.c` logic for that is portable and can be lifted almost verbatim. |
| the wait loop | `WaitForSomething SetNotifyFd ClientSleepUntil` | Wait on: the control pipe, the timer heap, and "a client has data". No fds, no `select`. |
| client identity | `AttendClient IgnoreClient MakeClientGrabImpervious MakeClientGrabPervious ReserveClientIds ReleaseClientIds GetClientPid GetClientCmdName GetClientCmdArgs GetLocalClientCreds FreeLocalClientCreds ClientAuthorized CheckUserParameters` | Mostly trivial: pid/cmd are the *remote* client's and are unknowable — return "unknown", never fabricate. `ClientAuthorized` is answered by the cookie check that already happened in `X11Intake`. |
| host access | `AddHost RemoveHost GetHosts ChangeAccessControl` | **No hosts.** Access is decided by the pipe's DACL and the SSH channel. Return `BadAccess` to every request: there is no list to edit. |
| misc | `set_font_authorizations Popen Pclose` | fonts: none needed. `Popen/Pclose` exist for `xkbcomp`; AmberX cannot spawn (Job Object limit), so these fail cleanly and keymap delivery is designed separately (REJECTED-COMPONENTS.md). |

## C. The DDX hooks (~14 symbols) — AmberWinDDX proper

| symbols | meaning |
|---|---|
| `InitOutput` | create the one screen; install `fb` as the rendering backend on a system-memory framebuffer |
| `InitInput CloseInput ProcessInputEvents` | one keyboard, one pointer; events arrive from the controller, not from Win32 directly |
| `InputThreadInit InputThreadFini in_input_thread input_lock input_unlock` | no input thread (`INPUTTHREAD` off); these become no-ops and a plain mutex |
| `DDXRingBell ddxGiveUp OsInit OsCleanup ProcessCommandLine UseMsg NotifyParentProcess GiveUp xorg_backtrace` | trivial; `NotifyParentProcess` becomes a `HostStatus` frame |
| `getuid geteuid ffs` | shim leftovers: return a fixed id; `ffs` → `_BitScanForward` |

## D. Globals and stubs (~12 symbols) — one file

```
noCompositeExtension noDamageExtension noGEExtension noMITShmExtension
noRRExtension noRenderExtension noSecurityExtension noTestExtensions
noXFixesExtension                              → Bool globals, all FALSE except MITShm
ShmExtensionInit ShmRegisterFbFuncs            → empty: MIT-SHM is off
XaceHook XaceHookDispatch XaceHookIsSet
XaceHookPropertyAccess XaceHookSelectionAccess
XaceHooks XaceCensorImage                      → NOT stubs: Xext/xace.c, now on the allowlist
```

The `Xace*` line is the allowlist gap the probe found. XACE is the hook
mechanism the SECURITY extension enforces restricted clients through; without
`xace.c`, Phase 5's "restricted mode must be real, not a label" cannot be
built. It is MIT and is now approved.

## E. Libraries (84 symbols) — not porting

pixman (71) and libXfont2 (13) both build with MSVC upstream. Adding CMake
targets for them against the pinned trees is Phase 2 work of the ordinary
kind, and their symbols leave the list when it is done.

## What "Phase 2 done" looks like, in these terms

`AmberXLinkProbe.exe` links with **zero** unresolved symbols, using: the
228 core objects, the retained `os/` files from §A, an `AmberWinOS` module
for §B, an `AmberWinDDX` module for §C, one file for §D, and pixman and
libXfont2 built from the pinned trees. Then — and only then — the Phase 2
gate proper begins: setup packet, cookie check, a window, a pixmap, a
rectangle, input in, pixels out to a native window.

Estimate, with the usual honesty: §A and §D are a day. §C is a few days for
a stub screen. §B is the port — a week or two for someone who has read
`io.c`, because `ReadRequestFromClient`'s request framing is the only part
with real subtlety and BIG-REQUESTS makes it subtle. §E is build plumbing.
Nothing here is a person-year; the person-years are in Phases 3, 5 and 6.
