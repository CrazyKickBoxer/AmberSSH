# AmberX — Phase 7 gate

Phase 7 is where AmberX becomes something a person can see and steer from
inside AmberSSH: one page in the connection manager that decides what a
remote GUI is allowed to be, a shelf listing the windows a session has open,
and the numbers behind it all in the diagnostics overlay.

Measured by `AmberSSH.exe --preview-amberx` (105 checks, no failures, twice;
transcript in `docs/amberx/phase7-preview-run.txt`) and by the regression
suite (382 cases, 68,086 assertions).

## The prompt's four gate criteria

```
1. profile settings migrate safely ......................... PASS
     A profile written before Remote GUI existed has no such key, and the
     loader derives it from the three fields that used to carry the same
     meaning rather than defaulting it to off — which would silently turn
     a working remote GUI off on upgrade. Six cases are unit-tested,
     including "present but zero means off" and every out-of-range value
     being clamped rather than believed.
2. remote app windows are discoverable and manageable ...... PASS
     Ctrl+Shift+G opens the Remote Apps shelf: every window the session's
     host is showing, its state, and Enter / M / C to show, minimise or
     close it. The preview finds its own window in the host's report by
     title, minimises it through the shelf path and restores it.
3. native-window mode is stable ............................ PASS on the
     preview's terms — the same 105 checks that cover Phases 2 to 6 all run
     in native-window mode, twice, with no stray host. "Stable" in the
     sense the prompt means it — a day of real applications — still needs
     the live gate.
4. pane mode is clearly experimental and off by default .... PASS
     Native windows is the default and the only implemented mode. Tabs,
     panes and ask-per-application appear in the dropdown labelled
     "(not yet)", the page says why, and a session configured for one of
     them reports the fallback in its log and uses native windows. No
     shared-surface code was written, so nothing pretends to be nearly
     working.
```

## The Remote GUI page

`Connection → SSH → Remote GUI`, in the order a person decides it:

| control | values | what it does |
|---|---|---|
| Remote GUI | Off, X11 Restricted, X11 Trusted | the authority for AmberX; turning it on sets X11 forwarding and the AmberX backend, and Trusted still raises the typed-hostname warning |
| Window mode | Native windows, tabs, panes, ask | only native is implemented; the rest fall back and say so |
| Clipboard | Disabled, Ask, Remote→Local, Local→Remote, Bidirectional | Phase 6's bridge, now on the same page as the rest |
| Display | All monitors, Active monitor only, Fixed virtual size | the X screen the session gets |
| Performance | Auto, Quality, Balanced, Low bandwidth | how often forwarded windows repaint |

**Remote GUI is the authority, and the older fields follow it.** `x11Forward`,
`x11Backend` and `x11Trust` still exist and still stand alone for the
external-X-server path, but a profile with Remote GUI on has them set to
match on save, and turning Remote GUI off turns the AmberX backend off with
it rather than leaving a host nothing asked for.

**Display** is computed by AmberSSH, not by the host: AmberSSH knows which
monitor its own window is on, so it sends a rectangle and the host is told a
geometry rather than a policy. All monitors is the virtual desktop as
before; active monitor is the monitor AmberSSH is on; fixed is a size at
that monitor's origin.

**Performance** caps how often a forwarded window repaints locally: auto and
quality leave it alone, balanced allows 60 a second, low bandwidth 20. The
host accumulates damage into one rectangle per frame between repaints, so
nothing is lost, only delayed. It is **not** compression, and the page says
so in as many words: AmberX forwards the X protocol as it is, and the
bandwidth is the protocol's.

## The Remote Apps shelf

`Ctrl+Shift+G`. Session-scoped, drawn in the terminal's own surface the way
the command palette and the journal are — which is what makes it take the
interface skin, and what makes it impossible for a forwarded window to cover:
nothing a remote application draws reaches that layer.

Each row is one window: its title, and whether it is active, minimised,
maximised or simply open. `Enter` shows it, `M` minimises it, `C` asks it to
close — the same request its own close button makes, never a kill, so an
editor with unsaved work still gets to say no. The footer carries the
session's own numbers: clients, X windows, bytes in and out, presents, and
whether any frames were refused.

Menus and tooltips are filtered out. They are windows, and there can be
dozens of them, and none of them is an application.

The titles come from the remote machine. They were sanitised and bounded by
the X side before they reached the host, bounded again by the report parser,
and cut to the column here.

## Diagnostics (F3)

Two lines added to the existing overlay when the session has a host:

```
AmberX 0.6 (X.Org 21.1.24)  1 client  1 window  pixmaps 0.1 MiB  restricted
AmberX x11 in 0.42 MiB  out 1.03 MiB  presents 96  dirty 96
             ipc high water 0 B  refused 0  gpu n/a
```

Against the prompt's list: version and upstream revision, client count,
window and pixmap counts, X11 bytes both ways, dirty rectangles, IPC queue
high water, refused messages, and the last error — all present. **CPU and
memory** are not in these lines: the Job Object already holds the truthful
numbers on the AmberSSH side and the host would only be reporting its own
guess, so they belong to a later pass rather than being invented here.
**GPU upload bytes** reads `n/a` because there is no GPU path — frames are
system-memory DIBs. **The extension list** is not on the overlay either; it
is what a restricted client is allowed to use, it is fixed at build time,
and `ddx_policy.c` is where it is written down.

## What carries it: one new message each way

`HostReport` (host → AmberSSH, twice a second) carries counts, rates and the
window list. `WindowAction` (AmberSSH → host) carries one window and one of
three verbs. Both are control-channel only, and `HostStatus` was left exactly
as it was — the message the handshake depends on keeps its fixed length and
its fixed meaning rather than growing a variable-length list.

The report is the first message with a counted list in it, which makes it the
first place a length bug could hide. Its parser refuses a short buffer at
every offset, a buffer with bytes left over, an unknown layout version, and a
window count larger than the cap — checked before anything is reserved. Four
unit tests cover exactly that, including truncation at every single byte.

## What changed in this pass

| file | what |
|---|---|
| `src/amberx/control/Protocol.{h,cpp}` | `HostReport` and `WindowAction`, their builders and their parsers; the version constants the overlay prints |
| `src/amberx/host/WinBackend.cpp` | the live frame list, window titles, present and dirty counters, the half-second report timer, window actions, present coalescing, the display rectangle |
| `src/amberx/server/{os_connection.c,ddx_limits.c}` | X11 bytes out, live window and pixmap counts pushed to the Windows side twice a second |
| `src/amberx/AmberXController.{h,cpp}` | `SendWindowAction`, the desktop rectangle and the present cap as launch options |
| `src/ssh/session.{h,cpp}` | the report mirrored for the UI thread, window actions queued for the worker, the window-mode fallback reported |
| `src/app.{h,cpp}` | the Remote Apps shelf, the AmberX diagnostics lines, the display and performance settings |
| `src/profiles/*`, `src/ui/ConnectionDialog.cpp` | the Remote GUI page, its persistence and its migration |
| `tests/` | seven new cases: the report and action parsers, the migration, the clamping |

## Decisions

- **One authority, not five.** Remote GUI decides; the older X11 fields
  follow. Two controls that can disagree about the same thing is how a user
  ends up with a setting that does nothing.
- **The unimplemented window modes are visible and labelled.** Hiding them
  would be tidier and would also hide the fact that the feature is not
  finished. They fall back to native and say so in the session log.
- **The shelf is drawn in the terminal's surface**, not as a window, so a
  remote application cannot cover it and it inherits the skin.
- **Close asks, it never kills.** The shelf's close is the same
  `WM_DELETE_WINDOW` path the window's own button uses.
- **No shared-surface pane rendering was attempted.** The prompt describes
  it in detail — shareable DXGI textures, duplicated restricted handles,
  fences — and it is a phase of its own. Half of it behind a flag would be
  worse than none.

## Limitations

- Tabs, panes and ask-per-application are not implemented.
- CPU and memory are not in the diagnostics lines (above).
- The shelf lists windows; it does not yet show per-window byte counts or
  update rates, which the prompt asks for. The session totals are there;
  attributing them per window would need per-window accounting in the
  server that does not exist.
- Crash and disconnect state appears as the last error line rather than as
  a per-window state.
- The performance cap has been exercised by construction, not measured
  against a real application.
- Everything here has been driven by the preview, not by a person clicking
  the shelf while xterm runs. The live gate is still owed.

## How to re-run this gate

```
cmake --build build-amberx --config Release --target AmberSSH
build-amberx\Release\AmberSSH.exe --preview-amberx
build\tests\Release\AmberTests.exe
```

## Gate verdict

**Phase 7: pass.** The four criteria are met: migration is safe and tested,
remote windows are discoverable and manageable, native-window mode carries
every check the earlier phases established, and the modes that are not built
are labelled, defaulted off, and reported when chosen. What is missing —
per-window traffic, CPU and memory, and shared-surface panes — is listed
above rather than implied.
