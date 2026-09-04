# AmberX — performance

Measured, not guessed. Every number here came out of
`AmberSSH.exe --preview-amberx`, which prints them on every run, so a change
that costs something shows up the next time the gate is run rather than the
next time a user complains.

**What this is not:** an application benchmark. No real X application has run
against AmberX yet (see PHASE-4-GATE.md for the live gate that is still
owed), so the numbers below are what the server and the transport cost, not
what xterm feels like. They are the floor: whatever an application
experiences, it is at least this.

## The machine

| | |
|---|---|
| host | Windows 11 Home 26200, x64 |
| display | one monitor, 1920×1080, 96 dpi |
| build | MSVC Release, `-DAMBERX_CORE=ON` |
| date | 2026-09-04 |

## What was measured

Three consecutive runs, no other load. The preview reports each of these as
a check with the number attached, so they are also assertions: a round trip
over 5 ms or 32 MiB of growth under churn fails the gate.

| measurement | run 1 | run 2 | run 3 | what it means |
|---|---|---|---|---|
| host startup | 31 ms | 31 ms | 32 ms | from launching AmberXHost to a display that answers a client — process creation, restricted token, job object, handshake, X server init and the cookie |
| request round trip | 31 µs | 31 µs | 29 µs | one `GetGeometry` and its reply, 200 of them, over the pipe. The latency a toolkit feels on every query it makes |
| drawing | 42,036/s | 7,585/s | 40,746/s | 300 `PolyFillRectangle` calls into a mapped 300×200 window, then a round trip to prove the server drained them |
| memory under churn | +0.0 MiB | +0.0 MiB | +0.0 MiB | 500 windows and 500 pixmaps created and destroyed; the host's working set before and after |

Run 2's drawing figure is five times slower than the other two and is worth
naming rather than averaging away: the fills are asynchronous, so what the
number measures is how quickly the client could hand them over, and that
depends on when the host's UI thread happened to take the pipe. The floor
across runs is ~7,500 fills a second, which is 25 times what a 60 Hz
application redrawing its whole window would ask for. The ceiling is not the
interesting end.

Startup is the number the prompt asks for as "profile open to X11 readiness".
31 ms is the host; the SSH connection in front of it takes far longer, and
the host is started inside that time rather than after it.

## Against the prompt's targets

| target | status |
|---|---|
| no measurable regression to keystroke latency while AmberX is idle | **by construction, not measured.** An idle host sends one report every 500 ms and nothing else; the session worker polls the pipe with a zero timeout in the same loop it already ran. There is no path from AmberX to the terminal's render thread. A keystroke-latency harness would need the terminal running against a real server, which is the live gate. |
| no terminal-render thread blocking from X11 activity | **by construction.** Every X11 byte is handled on the session worker thread or in the host process. The render thread touches neither. The one place they meet is the Remote Apps shelf, which copies a small struct under a lock the worker holds for microseconds. |
| smooth 60 Hz for ordinary 2D apps on a LAN | **not measured** — needs an application. The floor above says the server is not the limit. |
| hidden/minimised windows reduce presentation work | **partly.** A minimised frame gets no `WM_PAINT`, so its damage costs an `InvalidateRect` and nothing else. The X side still renders into the frame buffer, because it does not know the window is hidden — worth fixing when there is an application to measure it with. |
| dirty-region upload rather than whole-window | **yes.** Damage arrives as rectangles and is presented as rectangles; with the present cap on, they are unioned, which is still a region rather than the window. |
| bounded memory under window/pixmap churn | **measured, above.** No growth across 1,000 resources. |
| startup time reported | **yes**, 31 ms, printed by the preview. |

## Low-bandwidth mode

The profile's Performance setting caps how often forwarded windows repaint:
balanced 60 a second, low bandwidth 20, auto and quality uncapped. Damage
between repaints is unioned into one rectangle, so nothing is lost — the
picture is late, never wrong, which is what "without altering X11
correctness" requires.

It does **not** compress the X11 stream, and the profile page says so. AmberX
forwards the protocol as it is; the bytes on the SSH channel are the X
protocol's, and making them smaller would mean a proxy that re-encodes
drawing, which is a different project. Prioritising input and control traffic
over drawing is likewise not implemented: everything shares one SSH channel
per client, and reordering within it would need the same re-encoding layer.
Both are named here rather than claimed.

## Reproducing

```
cmake --build build-amberx --config Release --target AmberSSH
build-amberx\Release\AmberSSH.exe --preview-amberx
```

The numbers are in `%TEMP%\amberx-preview.txt`, on the lines reading
`host startup time`, `request round trips`, `drawing keeps up` and
`memory is bounded`.
