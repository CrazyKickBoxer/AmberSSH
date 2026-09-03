# AmberSSH

A native Windows 11 SSH terminal where every character in the viewport is drawn
as a constellation of glowing GPU particles — amber phosphor, nixie glow, black
glass — with a familiar PuTTY-style connection workflow.

C++20 · Win32 · DirectX 12 · HLSL compute · DirectWrite · libssh2. No Electron,
no browser, no Qt, no third-party terminal widget, no CPU-rendered viewport.

![status](https://img.shields.io/badge/build-passing-brightgreen)
![tests](https://img.shields.io/badge/tests-102%20cases%2C%20284%20assertions-brightgreen)

## What it does

- **Real SSH** over libssh2 — password, public-key, and keyboard-interactive
  authentication, with host-key verification and an explicit accept step.
- **Real terminal** — a VT100/xterm-256color state machine over a genuine text
  cell model, with scrollback, alternate screen, and truecolor.
- **Particles are presentation, not truth.** The grid underneath holds actual
  characters, so selection, copy and scrollback are exact no matter what the
  renderer is doing.
- **Multiple sessions** in tabs, each with its own worker thread, grid and
  scrollback. One stalled session cannot block another.
- **Secrets never touch disk.** Remembered passwords and passphrases live in
  the Windows Credential Manager; `profiles.json` holds only a boolean saying
  one exists.

## The look

Pure black ground, `#FFB000` amber glyphs, ~32 particles per cell in four
layers — a wide diffuse glow, the character body, a sharp detail pass, and a
sparse HDR sparkle. Brightness breathes on a slow sine with a faster
low-amplitude flicker. Glyph changes disperse and reconverge in ~250 ms.
Selected text switches to a drifting **Miami Sunset** gradient.

Effects are subtle by default and every one can be turned off. The terminal has
to stay readable; that is the whole point.

## Build

```powershell
.\build.ps1                       # Release
.\build.ps1 -Config Debug -Test   # Debug, then run the unit tests
.\package.ps1                     # portable ZIP + SHA-256
```

Needs Visual Studio 2022 (or Build Tools) with the C++ workload, the Windows 11
SDK, and vcpkg with `VCPKG_ROOT` set. HLSL is compiled to DXIL by DXC as a
build step — a shader error fails the build. See [docs/BUILDING.md](docs/BUILDING.md).

## Using it

The connection manager opens first: a category tree on the left, settings on
the right, and a Saved Sessions list with Load / Save / Delete. Enter a host,
port, username, pick password or key authentication, and press Open.

Select with a left drag — **releasing the mouse copies automatically** and the
status line says how much. Right-click pastes. `Ctrl+Shift+T` opens another
session, `Ctrl+Tab` cycles tabs, `Alt`+drag pushes the particles around without
disturbing the text. Full list: [docs/KEYBOARD_AND_MOUSE.md](docs/KEYBOARD_AND_MOUSE.md).

### Private keys

OpenSSH-format keys, of the types your libssh2 build supports. PuTTY `.ppk`
files are **not** loaded directly — the dialog tells you to convert them rather
than failing with something cryptic.

## Security

Host keys are verified and require an explicit decision on first contact. A
changed key is treated as a serious failure, never a silent continue. No
telemetry, no log upload, no automatic key acceptance, and no secret is ever
written to a log or a JSON file. Details and current limitations:
[docs/SSH_SECURITY.md](docs/SSH_SECURITY.md).

## Known limitations

Stated plainly, and kept current in [docs/VALIDATION_REPORT.md](docs/VALIDATION_REPORT.md):

- **No SSH connection has been tested yet** — no server was available in the
  build environment. The transport compiles and links; that is all that can
  honestly be claimed until someone runs it.
- **No frame timing has been measured.** The 4K/120 FPS figure is a target.
- Mouse tracking modes are parsed but pointer events are not yet forwarded to
  remote applications.
- OSC 8 hyperlinks are consumed but not clickable.
- Accessibility beyond clipboard and dialog keyboard navigation is not
  implemented; there is no UI Automation provider yet.

## Documentation

| Document | Contents |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | threads, data flow, module map |
| [RENDERING.md](docs/RENDERING.md) | glyph sampling, particle model, bloom, GPU contract |
| [TERMINAL_COMPATIBILITY.md](docs/TERMINAL_COMPATIBILITY.md) | supported / partial / unsupported sequences |
| [SSH_SECURITY.md](docs/SSH_SECURITY.md) | credential storage, host keys, hardening |
| [KEYBOARD_AND_MOUSE.md](docs/KEYBOARD_AND_MOUSE.md) | every shortcut and gesture |
| [PERFORMANCE.md](docs/PERFORMANCE.md) | targets, budget, what is not measured |
| [BUILDING.md](docs/BUILDING.md) | prerequisites and exact commands |
| [VALIDATION_REPORT.md](docs/VALIDATION_REPORT.md) | what was actually verified |

## Credits

The four-layer particle model, dual-sine brightness animation, pointer force
field and shockwave follow the design of
[amber-particle-ssh](https://github.com/CrazyKickBoxer/amber-particle-ssh) (MIT).
That project is Qt6 + OpenGL on Linux; AmberSSH is an independent DirectX 12 /
HLSL implementation of the same ideas. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
