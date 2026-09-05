# AmberX — compatibility matrix

Which applications have been run against AmberX, and what happened.

**First results: 2026-09-04.** `xeyes` ran, over SSH to a Fedora 44 Server
guest, in restricted mode, drawn as a native window — the first real X
application AmberX has ever served. `xterm` did not, and the reason is a real
gap rather than a misconfiguration (Finding 1 below). Everything else still
says NOT RUN, and a blank row is not a pass.

This file was published as an empty form before any of it was run, so that
the result could not be quietly narrowed to whatever happened to work.

## How to run the suite

1. Build with the X server enabled — it is opt-in:
   ```
   cmake -S . -B build -DAMBERX_CORE=ON
   cmake --build build --config Release
   ```
   A build without it produces a 52 KB `AmberXHost.exe` that handshakes and
   draws nothing. If windows never appear, check that size first: it cost an
   hour the first time.
2. Run `build\Release\AmberSSH.exe`.
3. Open the profile for the Linux host. Under **SSH → Remote GUI** set
   *Remote GUI* to **X11 Restricted**, leave *Window mode* on native windows
   and *Clipboard* on **Ask each transfer**.
4. Connect. The session log should report the AmberX host starting, and F3
   should show an `AmberX` line with one client once something connects.
5. Run each application below from that session's shell.
6. For each, record: does it appear, does it draw correctly, does input work,
   does it close cleanly. Fill in the row. **A blank row is not a pass.**
7. Repeat the ones marked *both modes* with *Remote GUI* set to X11 Trusted,
   because the restricted extension allowlist is the most likely thing to
   change an application's behaviour.

Record the exact distribution and package version for each: "Fedora 42,
xterm-390-1.fc42". A result without a version is a result nobody can
reproduce.

## Basic X applications

| application | distro + version | result | visual | input | workaround |
|---|---|---|---|---|---|
| `xterm` *(both modes)* | Fedora 44 (Server) | **FAIL** | never opens a window | — | `xterm -fa Monospace -fs 12` — Xft instead of core fonts |
| `xeyes` | Fedora 44 (Server) | **PASS** | window and drawing correct | pointer tracks only while the cursor is over a forwarded window | none |
| `xclock` | | NOT RUN | | | |
| `xmessage` | | NOT RUN | | | |
| `xcalc` | | NOT RUN | | | |

First run: 2026-09-04, over SSH to a Fedora 44 Server guest, restricted mode,
native windows. Package versions still to be recorded.

### Finding 1 — no core font path (xterm, and anything else asking for one)

xterm exits with `cannot load font
"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1"`. AmberX
has **no font path at all**: it serves only the `fixed` and `cursor` fonts
built into libXfont2, so every core-font request for anything else fails and
the application never opens a window.

A real X server ships the misc-fixed PCF set and a `fonts.dir` to go with it.
AmberX needs the same — bundled, licence-checked and added to the font path —
and until it has one, any application that uses core fonts rather than Xft
will not start. Applications that use Xft (every modern toolkit, and xterm
with `-fa`) are unaffected, because they render glyphs themselves and send
them through the RENDER extension.

This is the first thing the compatibility suite found, and it is a real gap
rather than a misconfiguration.

### Finding 2 — the pointer is only tracked over forwarded windows (xeyes)

xeyes polls the server for the pointer position. AmberX learns that position
from Windows messages delivered to its own frames, so it is current while the
cursor is over a forwarded window and frozen as soon as it leaves — the eyes
stop following. The window, its shape and its drawing are all correct.

It is a deliberate consequence of how input reaches the server rather than an
oversight: nothing polls the global cursor. Making `XQueryPointer` truthful
everywhere would mean the host sampling the cursor position continuously,
which hands the X server — and therefore every forwarded client — a record of
where the user's mouse goes across the whole desktop. That is a security
trade-off to decide deliberately, not a bug to fix quietly, and it is written
up here so the decision is visible either way.

### Finding 3 — Composite's implicit redirection corrupts the frame origin (GTK 4)

`gnome-text-editor` (GTK 4, `GSK_RENDERER=cairo GDK_BACKEND=x11`) killed
the host on its first `PutImage`: an access violation in `fbBlt`, writing
**184 836 bytes before the start** of the window's buffer. The window was at
(9, 66) with a 2 800-byte stride, and 66 × 2 800 + 9 × 4 = 184 836 — the
window's screen position, subtracted twice.

The two subtractions come from two layers that were never meant to share a
window pixmap. miext/rootless bases the frame's scratch pixmap to screen
coordinates so that clip-list coordinates index the buffer directly. The
Composite extension, when a window takes its alternate ARGB (depth 32)
visual under a depth-24 root, *implicitly* redirects that window and stamps
`screen_x/screen_y` onto whatever pixmap the window currently has — which,
under rootless, is that same scratch header — and fb subtracts the stamp
from every coordinate. GTK 4 asks for the ARGB visual for its client-side
shadows; `xeyes` is depth 24 and never touched any of this.

AmberX now withdraws Composite (`noCompositeExtension`, `os_misc.c`). The
justification is structural rather than a policy preference: there is no
compositing manager for the extension to serve, no layered-window path for
the alpha its visual would promise, and rootless cannot carry a redirected
window at all. Withdrawing it also removes the only visual the BGRX buffer
could not honestly draw. Toolkits detect the absence and fall back, exactly
as they do on any X server without a compositing manager. Composite had
never been on the restricted allowlist; this makes the trusted case match.

The crash handler, the `PutImage` probe and the `address is BEFORE/inside/
PAST` describer that found this stay in the host: they are bounded, log
geometry only, and are what turned three theories into one measurement.

### Finding 4 — the session loop found host frames on a tick (GTK 4, "laggy")

With Composite withdrawn, `gnome-text-editor` drew its whole window, took
clicks (its menu popover opened and repainted) and took keys — the host's
input log shows every button and keycode arriving — but the user's verdict
was "sorta responsive but laggy and inconsistent". Not lost input: latency.

AmberSSH's session loop waited on the SSH socket alone (`select` with a
30 ms timeout) and *polled* the AmberX host's pipe once per pass. A named
pipe cannot sit in `select()`, so a frame from the host — every event, every
reply — that arrived while the socket was quiet waited for the tick. A GTK
interaction is several round trips; 0–30 ms of jitter per trip compounds
into exactly what was described. `xeyes` never showed it because it polls
the server continuously, and the harness never saw it because its client is
chatty and the harness does not go through SSH at all.

The controller now owns the pipe read on a thread, queues frames, and holds
an event high while the queue has anything in it; the session loop waits on
that event beside the socket (`WSAEventSelect`, armed for the one wait). The
F3 diagnostics line reports the measured result — `host wait max/avg` — the
time frames sat on that queue, which is the loop's own pickup latency.
Before this change that number was, by construction, 0–30 ms. What GTK 4
feels like now is the user's call, not this file's.

## Toolkits

| toolkit | application | backend | distro + version | result | notes |
|---|---|---|---|---|---|
| GTK 3 *(both modes)* | `gedit` or `gnome-text-editor` | X11 | | NOT RUN | |
| GTK 4 | `gnome-text-editor`, `GSK_RENDERER=cairo GDK_BACKEND=x11` | X11 | Fedora 44 (Server) | draws, takes input; **latency judged "laggy"** — Finding 4, fixed, rerun pending | restricted mode; Finding 3 fixed the startup crash |
| Qt 5 | `qt5ct` or `dolphin` | xcb | | NOT RUN | |
| Qt 6 | any, `QT_QPA_PLATFORM=xcb` | xcb | | NOT RUN | |
| Tk | `wish` with a small script | X11 | | NOT RUN | |
| Java AWT/Swing | any Swing application | X11 | | NOT RUN | if practical |

The toolkit rows are the ones that matter most, and the reason is written
down in PHASE-6-GATE.md: upstream's SECURITY extension allows an untrusted
client two extensions, which is not enough to start a toolkit at all. AmberX
widens that to ten by name (`ddx_policy.c`). If a toolkit fails in restricted
mode and works in trusted mode, the allowlist is the first place to look, and
the failure will say which extension it wanted.

## Practical utilities

| kind | application | distro + version | result | notes |
|---|---|---|---|---|
| text editor | `mousepad`, `leafpad` or `gedit` | | NOT RUN | |
| file manager | `pcmanfm` or `thunar` | | NOT RUN | |
| package / configuration utility | `synaptic`, `system-config-printer` | | NOT RUN | |
| plotting | `gnuplot` with the X11 terminal, or `xgraph` | | NOT RUN | |

## Expected failures

These are not bugs to file. They are the boundary of what AmberX is, as of
this release.

| test | expected | why |
|---|---|---|
| `glxgears` | **fails**, "couldn't get an RGB, Double-buffered visual" or similar | There is no GLX. AmberX advertises no GLX extension and has no OpenGL path; the prompt puts it in a later phase. |
| any application requiring DRI/DRI3 | fails | same |
| `xrandr --output ... --mode ...` | refused | Windows owns the monitors; AmberX refuses every mode-setting request from every client (`ddx_randr.c`) |
| `xdotool type` into another client's window | refused in restricted mode | XTEST is not on the restricted allowlist |
| `import` / `xwd` capturing another client's window | works between forwarded clients | forwarded clients are not isolated from each other — a documented boundary, see THREAT-MODEL.md #21 |
| audio from a forwarded application | silent | no audio forwarding; a later phase |

## What a failure should record

The point of the form is that a failure is actionable. For each:

- the exact command and the exact error the application printed;
- what `%TEMP%\amberx-host.log` said at the same moment (it holds the
  server's side and contains no window contents or protocol bytes);
- whether it also fails in trusted mode, which separates "the allowlist" from
  "the server";
- whether an ordinary X server on the same host runs it — if it fails there
  too, it is not AmberX.

## Status

**Compatibility suite: started 2026-09-04.** The first real X application ran
against AmberX that day — `xeyes`, over SSH to a Fedora 44 Server guest, in
restricted mode, drawn as a native window. That closes the Phase 4 live gate
on the narrow question of "does anything work at all". The suite itself is
barely begun: three applications tried, one passed, one found a real gap (no
core font path), and the first toolkit row — GTK 4 — crashed the host and
exposed a structural conflict between Composite and rootless (Finding 3),
now fixed and awaiting its rerun.
