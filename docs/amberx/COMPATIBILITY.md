# AmberX — compatibility matrix

Which applications have been run against AmberX, and what happened.

**Nothing has been run yet.** Every row below says NOT RUN, and that is the
honest state: AmberX has been driven only by the hand-written X client inside
`--preview-amberx`, which exercises the protocol but is not an application.
This file is the plan and the form to fill in, published in advance so that
the result cannot be quietly narrowed to whatever happened to work.

Running it needs two things this machine does not have: a Linux host to
forward from, and a person at the keyboard. The steps are exact so that
whoever does it does not have to invent them.

## How to run the suite

1. Build the AmberX tree — it is not in the default build:
   ```
   cmake -S . -B build-amberx -DAMBERX_CORE=ON
   cmake --build build-amberx --config Release
   ```
2. Run `build-amberx\Release\AmberSSH.exe` — **not** the one in `build\`.
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
| `xterm` *(both modes)* | | NOT RUN | | | |
| `xeyes` | | NOT RUN | | | |
| `xclock` | | NOT RUN | | | |
| `xmessage` | | NOT RUN | | | |
| `xcalc` | | NOT RUN | | | |

## Toolkits

| toolkit | application | backend | distro + version | result | notes |
|---|---|---|---|---|---|
| GTK 3 *(both modes)* | `gedit` or `gnome-text-editor` | X11 | | NOT RUN | |
| GTK 4 | any, `GDK_BACKEND=x11` | X11 | | NOT RUN | GTK 4 prefers Wayland; the variable forces X11 |
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

**Compatibility suite: NOT RUN.** No application has been run against AmberX.
The gate in PHASE-8-GATE.md records this as the open item it is, and it is
also the Phase 4 live gate under a different name: the first real application
closes both.
