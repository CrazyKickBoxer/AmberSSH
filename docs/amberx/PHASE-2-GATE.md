# AmberX — Phase 2 gate

Phase 2 is "a standalone minimal AmberX server": the X.Org core linked
against an AmberX os layer and DDX, running inside AmberXHost, accepting a
client over the control pipe, and putting pixels on a native window. This
is the report, in the order the prompt asks for.

```
Link to zero unresolved ............ PASS  AmberXHost.exe, 1.7 MB, 0 unresolved
Setup packet + cookie check ........ PASS  'l' 11.0 MIT-MAGIC-COOKIE-1 → setup accepted
A window, a pixmap, a GC ........... PASS  CreateWindow / MapWindow / CreateGC
A rectangle ........................ PASS  PolyFillRectangle; GetImage reads back 0x203040
Input in ........................... PASS  ButtonPress at the posted coordinates; KeyPress keycode 38
Pixels out to a native window ...... PASS  captured from the desktop: the amber window and its rectangle
```

**Phase 2 gate: pass.** Reproducible with `AmberSSH.exe --preview-amberx`
from a `-DAMBERX_CORE=ON` build; the report lands in
`%TEMP%\amberx-preview.txt`, the host's log in `%TEMP%\amberx-host.log`.

## Files

| file | what |
|---|---|
| `src/amberx/server/amberwin.h` | the one interface between the X side and the Windows side: fixed-width C types, no X type, no Windows type |
| `src/amberx/server/amberos.h` | the per-client record (`AmberComm`) that replaces `OsCommRec` |
| `src/amberx/server/os_connection.c` | **the port**: clients over channels; request framing and output coalescing from `os/io.c` (MIT, attributed) with the socket calls replaced; `WaitForSomething`; no host list, no fds |
| `src/amberx/server/os_auth.c` | MIT-MAGIC-COOKIE-1 only, installed from the control pipe; a bad cookie is refused, there is no host-list fallback |
| `src/amberx/server/os_timer.c` | timers (the list from `os/WaitFor.c`, attributed), the clock, `ClientSleepUntil`, the screen-saver timer |
| `src/amberx/server/os_log.c` | the bounded, non-sensitive log sink (rule 9) |
| `src/amberx/server/os_alloc.c`, `os_misc.c` | XNF allocators; init, command line, identity stubs that say "unknown" rather than fabricate, `ffs`, `xstrcasecmp` |
| `src/amberx/server/ddx_screen.c` | one screen: fb on a 32 bpp BGRX framebuffer, DAMAGE on the screen pixmap, block handler presents the dirty extents |
| `src/amberx/server/ddx_input.c` | one pointer (absolute, screen pixels), one keyboard; events arrive from the Windows side and go through `QueuePointerEvents`/`QueueKeyboardEvents` |
| `src/amberx/server/ddx_keymap.c` | keymap delivery without `xkbcomp`: an `.xkm` via `XkmReadFile`, or a built-in US map constructed in `XkbDesc` structures, with the four canonical key types built directly |
| `src/amberx/server/server_main.c` | `amberx_server_main` → upstream `dix_main`, unmodified |
| `src/amberx/host/WinBackend.{h,cpp}` | the Windows side: window, DIB framebuffer, message → event translation (evdev keycodes), pipe reader thread, channel buffers, wake event, BCrypt SHA-1, `dirent` |
| `src/amberx/host/main.cpp` | the host: handshake as before, then `BackendInit`/`BackendRun`; DPI-aware; crash line to the log; transport-only stub kept for builds without the core |
| `src/amberx/AmberXController.cpp` | `--preview-amberx` is now an X client written on the wire; host stderr captured to `%TEMP%\amberx-host.log` |
| `third_party/amberx/CMakeLists.txt` | `AmberXPixman`, `AmberXFont` (+zlib), `AmberXServer` (with the retained `os/` allowlist), `AmberXFontProbe`; `xkb/ddxLoad.c` excluded by the gate |
| `third_party/amberx/config-msvc/{compat/dirent.h, xfont-compat.h}` | two more declaration-only headers |
| `tests/amberx/FontProbe.c` | opens the built-in fonts through libXfont2 alone, printing every return code |
| `tools/capture-amberx.ps1` | photographs the display window from the desktop |

## Decisions

1. **The X side never sees `<windows.h>`; the Windows side never sees an X
   header.** Enforced by construction (`amberwin.h`), and it bit twice on the
   way here — `os/xsha1.c`'s CryptoAPI path, and the VS generator's `/DWIN32`
   — each time as a wall of errors rather than a subtle bug.
2. **Threads: three, one queue direction.** UI thread (window), pipe thread
   (frames → events and channel bytes), server thread (the core, single-
   threaded as X.Org assumes). Only the server thread pops; only it calls
   into the core. Channel writes and status frames go out under one mutex.
3. **Clients are channels.** `LimitClients` is 65 (64 channels + the server
   client), matching `ChannelTable::kMaxChannels`. A channel opening is
   `NextAvailableClient`; a channel closing is `CloseDownClient`; a client
   the server kills closes the channel. No listener exists anywhere.
4. **Authorization is the cookie, full stop.** Upstream lets a client with no
   valid cookie in if the host list allows it; there is no host list, and
   `AddHost`/`ChangeAccessControl` return `BadAccess`. `client->local` is
   always false — every AmberX client is on another machine.
5. **Keymaps are not compiled at runtime.** `xkb/ddxLoad.c` left the
   allowlist; the host cannot spawn and would not be allowed to. The
   built-in map is original (keysyms from `keysymdef.h`, conventional key
   names); the `.xkm` path is the full-fidelity route for other layouts.
6. **Rootful.** One native window is the root window. Rootless is Phase 3.
7. **DPI-aware.** The host declares per-monitor awareness; without it the
   display is scaled and blurred and every mouse coordinate in its queue is
   divided by the desktop scale (the gate caught this as clicks landing at
   0.8× their position).
8. **zlib joins the closure.** libXfont2's built-in `fixed`/`cursor` fonts
   are gzipped inside the library; the server cannot start without `fixed`.
   Same zlib AmberSSH ships for libssh2; licence text in the notice bundle.

## Provenance

No upstream file modified; the configure step proves it. Two upstream files
are *derived from* in original AmberX files and say so in their headers:
`os/io.c` (request framing, output coalescing) and `os/WaitFor.c` (the
timer list). Retained unmodified from `os/`: `mitauth.c oscolor.c
xprintf.c strlcpy.c strlcat.c strndup.c reallocarray.c timingsafe_memcmp.c`.
Nothing from `hw/`, nothing from any other X server for Windows.

## Tests

- `AmberSSH.exe --preview-amberx` — 29 checks, all pass (report above).
  It is an X client written on the wire, so the gate depends on no X library.
- `AmberXFontProbe.exe` — 4 opens through libXfont2 alone, all pass.
- `AmberTests.exe` on the normal tree: **375 test cases, 67,961 assertions,
  all pass**; the transport-only host still builds there.
- The three crashes found and fixed on the way are worth recording, because
  each is a class: a pointer returned through an implicitly declared function
  (sign-extended to `0xffffffff8…`); a *sized* screen private registered
  after the screen existed (dix repacked fb's and mi's privates; garbage,
  differently each run); and a gzip decoder stubbed to fail (`AllocError`
  from the PCF reader with nothing logged).

## Limitations — what this does not do

- **Rootless windows, clipboard, RandR, cursor images, Present, Composite
  use** — none exercised. The extensions are compiled and initialise; only
  core protocol has been driven.
- **Byte-swapped clients**, BIG-REQUESTS over the 16 KiB buffer, multiple
  simultaneous clients, and a client that disconnects mid-request are
  untested paths of the port. The code for each is present.
- **The keymap is US only** unless an `.xkm` is supplied; there is no `.xkm`
  in the tree yet and no generator for one.
- **Keyboard LEDs, autorepeat rate changes, the bell's pitch** are ignored;
  the bell is `MessageBeep`.
- **Presenting is a `BitBlt` of the dirty extents from the UI thread** while
  the server thread may be drawing: tearing is possible, corruption is not.
- **Performance is unmeasured**: pixman's portable paths, no SIMD; one
  frame copy per dispatch round. Nothing here is sized for a real desktop.
- **Warnings**: the four libraries carry the 499 + 252 + 161 narrowing
  warnings reported in PHASE-1-GATE.md; `AmberXServer` compiles clean.
- **XREGISTRY** logs one warning at start because no `protocol.txt` path is
  compiled in; request names in XACE audit messages read `<unknown>`.

## Performance

Not measured. The preview round trip (setup → drawn → read back) completes
in well under a second on this machine; that is the only number there is.

## Security

- No listener of any kind; the only input is the DACL'd, handshaken pipe.
- The cookie arrives on the control channel and is copied into mitauth's
  table and nowhere else; it is never logged (os_log.c formats every line).
- Request lengths are checked before use, oversized requests become
  `BadLength` (from `io.c`'s logic), a closed channel becomes
  `CloseDownClient`. Channel data that names an unknown channel is refused
  by the Windows side with a `HostError` and never reaches the core.
- Identity questions about the peer (`GetClientPid`, cmd name, creds)
  answer "unknown" rather than inventing a local identity.
- The host still runs in the Job Object with the process limit of one and
  exits when its parent does (watch thread + job).
- Not yet done: restricted (untrusted) client mode is Phase 5; the SECURITY
  and XACE code is compiled and initialised but not driven.

## Rollback

`-DAMBERX_CORE=OFF` (the default) builds the transport-only host exactly as
before; nothing under `third_party/amberx/` or `src/amberx/server/` is
compiled. The profile's `x11Backend` default is still the external server.

## Gate verdict

**Phase 2: pass.** The server runs, the wire protocol is honoured end to
end, and the pixels are real. Phase 3 (rootless windows, the window
manager's view of a Windows desktop) starts from a working server.
