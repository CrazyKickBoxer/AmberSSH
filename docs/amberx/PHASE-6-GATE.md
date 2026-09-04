# AmberX — Phase 6 gate

Phase 6 is the phase where AmberX stops being a display and starts being
*this machine's* display: the user's own keyboard layout, their monitors,
their wheel, their clipboard. This is the report.

Everything below was measured by `AmberSSH.exe --preview-amberx` from the
`build-amberx` tree. The transcript of the run this report describes is
`docs/amberx/phase6-preview-run.txt`: 101 checks, no failures, repeated
twice.

## The prompt's four gate criteria

```
1. keyboard layout test suite passes for configured layouts ... PARTIAL
     The server's map is compared against what Windows itself says the
     keys produce, on whatever layout the machine is running — the
     comparison is the test, not a table of expected values, so it runs
     for any layout. Only the layout installed here (US, 00000409) has
     actually run: a suite over several layouts needs layouts installed,
     and installing keyboard layouts on the user's machine is not
     something the gate should do by itself. Named honestly rather than
     claimed: what is proven is the mechanism, on one layout.
2. mixed-DPI / multi-monitor tests pass ...................... PARTIAL
     RANDR reports one CRTC and output per monitor, at the monitor's own
     position, size and physical size, and the preview compares every
     rectangle against EnumDisplayMonitors. It matched on this machine's
     single 1920x1080 monitor. Two monitors at different scale factors
     have not been run: this machine has one.
3. clipboard policy is enforced in both directions ........... PASS
     Proven by what each mode refuses as well as by what it carries:
       both directions   local text reaches an X client, an X client's
                         selection reaches AmberSSH
       remote → local    the same selection still arrives; local text is
                         refused (no selection owner at all)
       disabled          neither: nothing is installed, nothing crosses
4. clipboard transfers cannot freeze the terminal or the UI .. PASS by construction
     The session worker never touches the Windows clipboard and never
     waits for it; the UI thread never waits for the session. See below.
```

## Keyboard

The X keymap is built from the layout the user is actually typing on.
`WinKeymap.cpp` asks the layout, for every character key and four modifier
states, what that key produces — the same question a text editor asks — and
reports Unicode code points over `amberwin.h`. `ddx_keymap.c` turns those
into keysyms and key types and writes them over the built-in US table's
character keys.

| item the prompt asks for | what happens |
|---|---|
| current Windows layout | read at start-up and on `WM_INPUTLANGCHANGE`; the new map is installed with `XkbDeviceApplyKeymap`, so a running application follows a layout switch |
| left/right modifiers | distinct evdev keycodes already; unchanged |
| dead keys | `ToUnicodeEx` reports them; the spacing character maps to the matching `XK_dead_*` keysym, and an unrecognised one keeps its literal character rather than vanishing |
| AltGr | right-alt becomes `ISO_Level3_Shift` bound to Mod5 **only on a layout that has a third level**, and stays `Alt_R` otherwise; the phantom left-control Windows sends with it is dropped, press and release together |
| Caps/Num/Scroll sync | on frame activation Windows' lock state is compared with XKB's, and the lock key is pressed and released to reconcile — through XKB's own path, so indicators and every client's idea of the state move together. Scroll Lock has no modifier in this map and is accepted and ignored |
| repeat rate and delay | `SPI_GETKEYBOARDDELAY`/`SPEED` become XKB's `repeat_delay` and `repeat_interval`; Windows' own repeats are not forwarded, because XKB repeats |
| structure is never overwritten | only keys whose first level is a printable character are taken from the layout. Escape, Return, Tab, the function keys, the keypad and the modifiers come from AmberX's table, so an exotic layout can leave a key blank but can never take Escape away |

Measured: the keysym the server reports for the AC01 key equals what
`ToUnicodeEx` gives for that scan code, plain and shifted; right-alt is
`ISO_Level3_Shift` exactly when the layout has an AltGr level, and `Alt_R`
otherwise; a key press with Shift held arrives at the client carrying
`ShiftMask`, which only happens if the modmap and compat interpretations
survived the layout being written over the built-in map.

`.xkm` files are still the full-fidelity path (`--keymap`) for a layout
Windows describes badly; nothing there changed.

## Pointer

- **High-resolution wheel** — deltas smaller than a notch accumulate
  instead of being truncated, and reversing direction discards the
  part-notch rather than crediting it. Measured: three deltas of 40 make
  exactly one button-4 click; two of −60 make exactly one button-5.
- **Capture** — held while any button is down and released when the last
  one comes up, so a two-button drag is not dropped halfway.
- **Buttons** — 1–3, wheel 4/5, horizontal 6/7, as before.
- **Touch and pen** — not implemented, as the prompt orders: after the
  desktop mouse and keyboard are stable, which means after a real
  application has run.

## Monitors and scaling

`ddx_randr.c` reports one CRTC and one output per Windows monitor, each at
its own position in X screen coordinates, its own mode, and its own
physical size derived from that monitor's DPI — which is how a client on a
150 % monitor and one on a 100 % monitor learn that they differ. The
primary monitor is the RANDR primary output. `WM_DISPLAYCHANGE` rebuilds
the topology and `RRTellChanged` tells clients, but only when something
actually moved: Windows sends that message for changes that move nothing,
and a spurious RANDR notification makes toolkits re-lay-out for nothing.

Mode setting is refused. `rrCrtcSet` and `rrScreenSetSize` return failure
for every client, because AmberX does not own the monitors — Windows does,
and a forwarded application changing the user's screen resolution is not a
feature. Negative monitor coordinates are handled by the same origin
translation the frames already use: the virtual desktop's top-left is X's
(0,0).

Rootful mode (`--rootful`, the debugging path) has no RANDR at all rather
than a wrong one: its screen is one window, not the desktop.

## Clipboard

Text only, UTF-8, both halves bounded, and disabled unless the profile
says otherwise.

```
Windows clipboard ──WM_CLIPBOARDUPDATE──▶ AmberSSH (UI thread)
                                             │ policy, and the "ask" dialog
                                             ▼
                                          session worker (queue, never waits)
                                             │ AmberXControl frame
                                             ▼
                                          host ──▶ server: owns CLIPBOARD
                                                          and PRIMARY,
                                                          answers ConvertSelection

X client takes CLIPBOARD ─▶ server asks it for UTF8_STRING ─▶ property ─▶
    host ─▶ session worker ─▶ SshEvent ─▶ AmberSSH (UI thread): policy,
    the "ask" dialog, then CF_UNICODETEXT
```

**Modes**, per profile, on the SSH → X11 page: disabled (the default), ask
each transfer, remote → local, local → remote, both. There is deliberately
no "advanced/full integration": images, file lists and markup each need
their own threat analysis and their own bounded parser, and until they have
one the bridge is text. Saying so is better than a menu entry that quietly
does less than its name.

**The policy is enforced three times** — in AmberSSH, which is the only one
of the three that can reach the Windows clipboard; in the session worker
before a frame is sent; and in the server, which will not own a selection
or ask for one in a direction the mode forbids. None of the three can move
text on its own.

**Neither thread waits for the other.** The session worker never calls
`OpenClipboard`: opening the Windows clipboard can block on whichever
application currently holds it, and the worker has a terminal to keep
responsive. Text arriving from the session is delivered as an ordinary
`SshEvent` and applied on the UI thread; text leaving is queued by the UI
thread and picked up by the worker on its next pass. Only the most recent
copy is queued — a clipboard has one current value, and delivering a
backlog of older copies would be wrong as well as slow.

**The confirmation dialog never shows the text.** It says the direction,
the host and the size. A confirmation box that displays what it is
protecting is a shoulder-surfing hole, and for a password manager's
clipboard an obvious one. It is painted, not composed from system controls,
so it takes every interface skin like the host-key and blast-radius
dialogs.

**Limits**: one transfer is at most 1 MiB (`clipboard_max_bytes`) and one
selection transfer at most 5 s (`selection_timeout_ms`), both in
`amberlimits.h` with the rest. INCR transfers are assembled piece by piece
against the same ceiling and the same clock.

### Two things worth writing down

*The requestor is the owner's own window.* When the server asks a client
for its selection text, the SelectionRequest names **the owner's** window as
the requestor, not one of the server's. Naming a server-owned window is the
obvious design and it fails in exactly the mode that matters: a restricted
client may not put a property on a window the server owns — the same rule
that makes ChangeProperty on the root fail, which Phase 5 celebrates — so
the transfer would work for trusted clients and fail for the clients the
default mode is built for. Asking the owner to write on its own window is
allowed either way, and the server, being trusted, may read it back.

*The server answers ConvertSelection itself.* There is no ordinary client
for it to be: the server client cannot receive events, so a SelectionRequest
addressed to it would go nowhere. `ProcVector[X_ConvertSelection]` is
wrapped instead, the same mechanism Phase 5 uses for the property and atom
limits.

## The extension allowlist — a Phase 5 decision this phase forced

RANDR is useless if clients cannot see it, and it turned out they could
not. Upstream's SECURITY extension allows an untrusted client exactly two
extensions, XC-MISC and BIG-REQUESTS. With that list, restricted mode
cannot run a toolkit application at all: RENDER, XKEYBOARD and SHAPE are
all denied. A default mode nothing runs in is a default mode nobody uses,
and the mode people would use instead is the trusted one.

`ddx_policy.c` widens the list, deliberately and by name, to: BIG-REQUESTS,
XC-MISC, RENDER, SHAPE, SYNC, XKEYBOARD, XInputExtension, Generic Event
Extension, RANDR, XFIXES. Each entry has its reason written next to it in
that file. Deliberately still excluded, though compiled: **XTEST** (it
synthesises input as the user — the injection threat exactly), **SECURITY**
(an untrusted client must not manage authorizations), **DAMAGE** (reports
what changed in drawables it does not own — screen scraping with a
subscription), **Composite** (screen scraping with a rendering pipeline)
and **MIT-SHM** (not advertised at all, by policy).

It is still an allowlist, and it is still narrower than trusted mode. The
one place it is more permissive than a purist reading: XI2 raw events can
be selected on the root, which lets one forwarded client see input directed
at another forwarded window. That is the same boundary Phase 5 already
documents — forwarded clients are not isolated from each other — and not a
new one: an AmberX root only ever carries input the user aimed at a
forwarded window, because the server never sees the rest of the desktop's
input.

The mechanism is worth remembering: dix **prepends** callbacks and calls
them from the head, so the callback registered *first* is called *last*,
and only the last one to write the status can widen a denial. The hook is
therefore registered during screen init, before `InitExtensions` registers
SECURITY's. Registered afterwards — which looked obvious — it was
overwritten every time. And two hooks are needed, not one: `XACE_EXT_ACCESS`
decides whether a client may see an extension, `XACE_EXT_DISPATCH` whether
a request to it may run, and a denial there becomes `BadRequest`, "pretend
the extension does not exist".

## A bug this phase found: the server was resetting

A stock X server tears itself down and rebuilds when its last client
disconnects — new generation, new screen, new atoms — because that is how a
display gets a clean slate between logins. AmberX did that too, and it is
wrong here: there is no next login, the cookie was delivered once and
cannot be delivered again, and rebuilding the screen while the Windows side
still holds the frames loses windows. The symptom was that closing the last
client left the next one with no answer at all. `dispatchExceptionAtReset`
is now 0 — `-noreset`, set in `OsInit` rather than left to a command line
nobody types.

## What changed in this pass

| file | what |
|---|---|
| `src/amberx/host/WinKeymap.{h,cpp}` | new: reads the live Windows layout, four levels per key, dead keys, repeat rate |
| `src/amberx/server/ddx_keymap.c` | FOUR_LEVEL and FOUR_LEVEL_ALPHABETIC types; the layout written over the character keys; AltGr binding |
| `src/amberx/server/ddx_randr.c` | new: one CRTC and output per monitor, physical sizes, no mode setting, topology changes |
| `src/amberx/server/ddx_policy.c` | new: the extension allowlist for restricted clients |
| `src/amberx/server/ddx_clipboard.c` | new: selection ownership, ConvertSelection answers, INCR reading, the limits |
| `src/amberx/server/ddx_input.c` | lock synchronisation; live keymap reload |
| `src/amberx/server/os_misc.c` | `dispatchExceptionAtReset = 0` |
| `src/amberx/host/WinBackend.cpp` | wheel accumulation, capture counting, AltGr phantom, lock and layout and monitor events, the clipboard relay |
| `src/amberx/{AmberXController,PreviewClient}.*` | `SendClipboard`, the clipboard launch option, the Phase 6 checks |
| `src/ssh/session.{h,cpp}` | `x11Clipboard`, `OfferClipboard`, the clipboard event and the direction checks |
| `src/app.{h,cpp}` | the clipboard listener, the policy, the dialog, `SetClipboardText` on the UI thread |
| `src/ui/SafetyDialog.{h,cpp}` | `ShowClipboardDialog`, skinned like the rest |
| `src/ui/ConnectionDialog.cpp`, `src/profiles/*` | the clipboard mode on the SSH → X11 page, saved with the profile |

## Limitations

- One keyboard layout has been run (US). The comparison is layout-agnostic,
  but "passes for configured layouts" is not proven for layouts that are
  not installed here.
- One monitor has been run. Mixed-DPI behaviour is implemented from each
  monitor's own `GetDpiForMonitor` and unverified.
- The clipboard is text. No images, no file lists, no HTML or RTF.
- Selection ownership is not restricted by trust level (a Phase 5 gap):
  any forwarded client can take CLIPBOARD, and the bridge will ask it.
- Touch and pen are not implemented.
- Still no real X application: the live gate from Phase 4 is still owed.

## How to re-run this gate

```
cmake --build build-amberx --config Release --target AmberSSH
build-amberx\Release\AmberSSH.exe --preview-amberx
```

`%TEMP%\amberx-preview.txt` holds the transcript; the process exits
non-zero if any check failed. The regression suite for the rest of
AmberSSH is `build\tests\Release\AmberTests.exe` (375 cases, 67,961
assertions, passing with these changes).

## Gate verdict

**Phase 6: pass on the clipboard, partial on keyboard and monitors.** The
clipboard criterion is met and measured in three modes; the freeze
criterion is met by construction and by the shape of the code. The keyboard
and monitor criteria are implemented and measured against what Windows
itself reports, but only on one layout and one monitor, which is what this
machine has — so they are reported as partial rather than as passes, and
the live gate on the Fedora VM remains the thing that would close them.
