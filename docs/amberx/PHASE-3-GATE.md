# AmberX — Phase 3 gate

Phase 3 is native multiwindow mode and AmberXWM: every top-level X window
a distinct Windows window, a window manager sufficient for common forwarded
applications, and a host identity element the remote cannot spoof. This is
the report, in the order the prompt asks for.

```
Rootless frames ..................... PASS  each mapped top-level X window is a native window
Title from _NET_WM_NAME / WM_NAME ... PASS  sanitised, bounded, UTF-8
Transient / owner relationship ...... PASS  WM_TRANSIENT_FOR → owned native window
WM_DELETE_WINDOW on native close .... PASS  ClientMessage, then a 5 s grace before the client is closed
Focus in/out ........................ PASS  native activation → SetInputFocus (+ WM_TAKE_FOCUS) → FocusIn
Native move/resize → X .............. PASS  ConfigureNotify with the settled position
_NET_CLIENT_LIST, _NET_SUPPORTING_WM_CHECK, _NET_SUPPORTED, WM_STATE ... PASS
Host identity strip ................. PASS  a separate window above the X pixels; X drawing cannot reach it
Input in through frames ............. PASS  ButtonPress / KeyPress at the posted coordinates
xterm, xeyes, xclock, xmessage ...... NOT RUN  needs the live SSH path (Phase 4's async integration) and the VM
Two sessions never mix .............. BY CONSTRUCTION  one host process per session; not yet exercised with two
```

**Phase 3 gate: pass on the mechanism, with the real-application half
explicitly not run.** The harness that proves it is `--preview-amberx`
(now `src/amberx/PreviewClient.cpp`): 37 checks, all pass, twice in a row.
The frame was photographed with `tools/capture-amberx.ps1 … "AmberX
preview"`: native caption, the skin-coloured identity strip, the amber X
window beneath it.

## Files

| file | what |
|---|---|
| `third_party/amberx/upstream/xserver/miext/rootless/` (5 files, MIT, unmodified) | the per-window backing layer xquartz uses; joined the allowlist; compiles clean under MSVC, its `__APPLE__` parts compile out |
| `src/amberx/server/ddx_frames.c` | the `RootlessFrameProcs`: each callback is one `amberwin_frame_*` call; regions cross as box quads |
| `src/amberx/server/ddx_wm.c` | **AmberXWM inside the server**: properties read from the server's own structures, `PropertyStateCallback` for changes, the XACE send hook for `_NET_WM_STATE` / `_NET_ACTIVE_WINDOW` / `_NET_CLOSE_WINDOW`, native events → `ConfigureWindow` / `SetInputFocus` / `WM_DELETE_WINDOW`; announces itself through `_NET_SUPPORTING_WM_CHECK` |
| `src/amberx/server/ddx_screen.c` | rootless branch: the X screen is the virtual desktop with no framebuffer (NULL bits, zero pitch); DAMAGE beneath rootless; native cursor (no-op sprite funcs); rootful kept behind `--rootful` |
| `src/amberx/server/amberwin.h` | frame API and frame events; `desktop_x/y` so X root coordinates are desktop coordinates |
| `src/amberx/host/WinBackend.cpp` | frames: owner window + identity strip + view child + DIB; ops marshalled to the UI thread; placement into the work area; hints, transient/modal, states, icons, shapes, DPI change |
| `src/amberx/host/main.cpp` | `--identity`, `--mode`, `--sigil`, `--skin`, `--rootful` |
| `src/amberx/AmberXController.{h,cpp}` | `Launch` options carried on the host command line (quoted; embedded quotes dropped) |
| `src/amberx/PreviewClient.{h,cpp}` | the X client of the gate, with sequence tracking and an event queue |
| `src/ui/Chrome.cpp` | now also linked into the host, so the strip follows the user's skin |

## Decisions

1. **The native window is the frame.** No reparenting: X sees its window at
   the position of the view child; native stacking is the stacking. This is
   the xwin/xquartz model and it is what makes "two sessions never mix"
   structural — a session's windows are owned by its own process.
2. **The manager lives in the server.** A WM client would need a listener
   or a client library; AmberX has neither by rule. Working from
   `WindowPtr` and `PropertyPtr` directly is smaller and cannot lose a race
   with the client it manages. It observes rather than intercepts: no
   `SubstructureRedirect`, so clients map where they ask and the manager
   reacts.
3. **The identity strip is a separate child window** (`AmberXStrip`) above
   the view, painted from `ChromeAt(skin)` — background, text, accent,
   uppercase rule, face — so it takes the theme treatment with every other
   surface. The X pixels are confined to the view's DIB and its window; the
   strip's window is never a drawable of any kind. A remote title can only
   change the caption text. Override-redirect windows (menus, tooltips)
   carry no strip and no caption: they are short-lived popups owned by a
   framed window that does.
4. **Placement.** A client asking for (x, y) does not know a caption and a
   strip sit above its window; a managed frame is kept inside the work
   area of the monitor it lands on and the client learns the settled
   position through `ConfigureNotify` — the ordinary WM behaviour.
5. **Close is graceful, then firm.** Native close → `WM_DELETE_WINDOW` if the
   client advertises it (a 5 s timer then closes the client if the window is
   still there), else the client is closed at once, as `XKillClient` would.
6. **Coordinates.** The X screen is the virtual desktop and its origin may
   be negative; every crossing subtracts/adds `desktop_x/y`, so multi-monitor
   positions are consistent by construction. Per-monitor DPI changes keep the
   pixel size and accept the new position.

## Provenance

`miext/rootless` is X.Org's (MIT, Apple's form); unmodified. Nothing from
`hw/xquartz` was copied: the frame procs were written to the documented
contract in `rootless.h`; the one fact taken from xquartz was that a NULL
framebuffer with zero pitch is what the layer expects, which its own header
comment on `RootlessUpdateScreenPixmap` also states.

## Tests

- `AmberSSH.exe --preview-amberx`: 37 checks (setup, drawing, pixel
  read-back, Expose, frame/strip/view layout, input, transient ownership,
  `_NET_CLIENT_LIST`, `_NET_SUPPORTING_WM_CHECK`, FocusIn, ConfigureNotify
  after a native move, `WM_DELETE_WINDOW` after a native close), pass ×2.
- `AmberTests.exe` on the normal tree: 375 test cases, 67,961 assertions.
- Bugs found by the gate on the way: device events carry their window at
  byte 12, not 4 (the harness, not the server); and the placement issue
  above, found by the capture photographing a caption above the screen edge.

## Limitations — what this does not do

- **No real application has run.** `xterm`/`xeyes`/`xclock`/`xmessage` need
  Phase 4's asynchronous controller in the SSH worker and the Fedora VM;
  until then the gate's client is the hand-written one.
- **Icons, shapes, fullscreen, maximised, minimised, urgency, modal** are
  implemented and untested by the gate.
- **No `MapRequest` interposition**: a client's initial position is honoured
  then clamped; no smart placement, no cascading.
- **Keyboard focus is Windows' focus**: X focus follows native activation;
  `PointerRoot` focus mode is not modelled.
- **Minimised windows** keep their X window mapped (hidden state in
  `_NET_WM_STATE` and `WM_STATE`), which is what most managers do; clients
  that expect UnmapNotify on iconify will not get one.
- **The X cursor is not shown**: the native arrow is used everywhere; cursor
  images are Phase 6.
- **Present timing**: the view repaints from the DIB on `WM_PAINT`; a resize
  reallocates the DIB and the window redraws on the next Expose.
- **Two simultaneous sessions** are separate processes by design but have
  not been run together.

## Performance

Not measured. Each damaged rectangle is one `InvalidateRect`; the UI thread
coalesces them into one `WM_PAINT` blit per frame per round.

## Security

- The strip is the host-identity element the prompt requires: painted by
  the host from arguments AmberSSH passed on the command line, in a window
  X content has no path to. The caption shows the remote's title, which is
  sanitised (control characters removed, 256 bytes, UTF-8 boundary) and
  cannot alter the strip.
- `_NET_WM_ICON` is validated against the property length before any
  pixel is read and capped at 256×256.
- Client messages the manager acts on are only ever those the XACE send
  hook shows it, and only for windows it manages.
- Nothing new is logged; frame events carry ids and geometry only.

## Rollback

`--rootful` restores the Phase 2 single-window display. `-DAMBERX_CORE=OFF`
builds the transport-only host as before.

## Gate verdict

**Phase 3: pass on mechanism; the real-application gate is deferred to
the first live run**, which needs Phase 4's asynchronous integration. That
integration is the next piece of work.
