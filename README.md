# AmberSSH

A native Windows 11 terminal and remote-desktop client where every character
in the viewport is drawn as a constellation of glowing GPU particles — amber
phosphor, nixie glow, black glass — with a familiar PuTTY-style connection
workflow.

C++20 · Win32 · DirectX 12 · HLSL compute · DirectWrite · libssh2. No Electron,
no browser, no Qt, no third-party terminal widget, no CPU-rendered viewport.

![status](https://img.shields.io/badge/build-passing-brightgreen)
![tests](https://img.shields.io/badge/tests-passing-brightgreen)

## What it does

- **Six protocols in one client**: SSH, Telnet, Rlogin, Raw, Serial, and a
  local shell (PowerShell / cmd / WSL) — plus a seventh that isn't a terminal
  at all.
- **A live remote desktop over VNC**, rendered by the same particle engine as
  the terminal. The framebuffer arrives over RFB (3.3/3.7/3.8, VeNCrypt/TLS,
  ZRLE/Hextile/Raw/CopyRect) and is drawn, not screenshotted — window edges
  glow, clicks send a shockwave through the field, and a repaint rematerialises
  under one of the motion styles below instead of just appearing.
- **Real SSH** over libssh2 — password, public-key, and keyboard-interactive
  authentication, with host-key verification and an explicit accept step for
  anything unknown or changed. Verified against a real Linux server during
  development, not just compiled and linked.
- **Real terminal** — a VT100/xterm-256color state machine over a genuine text
  cell model, with scrollback, alternate screen, truecolor, and X10/SGR mouse
  reporting so full-screen mouse-aware programs (vim, tmux, htop, mc) work.
- **Particles are presentation, not truth.** The grid underneath holds actual
  characters, so selection, copy and scrollback are exact no matter what the
  renderer is doing.
- **23 motion styles** for how a changed cell gets from its old state to its
  new one — burn, dissolve, shatter, light-speed streak, digital rain, and
  eighteen more — **15 interface skins**, and **4 appearances** (Dark, Light,
  a warm Paperwhite e-ink look, and a 16-colour Pixel Art mode).
- **Multiple sessions** in tabs, each with its own worker thread, grid and
  scrollback. One stalled session cannot block another.
- **Secrets never touch disk.** Remembered passwords and passphrases live in
  the Windows Credential Manager; `profiles.json` holds only a boolean saying
  one exists.

## The look

Pure black ground and `#FFB000` amber glyphs by default. Seven other colour
themes ship alongside it — Emerald CRT, Ice Cathode, Violet Haze, Blood Cell,
Paper White, Brass Gaslight, and one you build yourself from three picked
colours.

Each glyph is 96 particles by default (32 to 256, five presets), drawn in
four layers: a wide diffuse glow, the character body, a sharp detail pass,
and a sparse HDR sparkle. Brightness breathes on a slow sine with a faster
low-amplitude flicker. Glyph changes disperse and reconverge under whichever
of the 23 motion styles is active. Selected text switches to a drifting
**Miami Sunset** gradient.

Effects are subtle by default and every one can be turned off. The terminal has
to stay readable; that is the whole point.

## Build

```powershell
.\build.ps1                       # Release
.\build.ps1 -Config Debug -Test   # Debug, then run the unit tests
.\package.ps1                     # signed, portable ZIP + SHA-256
```

Needs Visual Studio 2022 (or Build Tools) with the C++ workload, the Windows 11
SDK, and vcpkg with `VCPKG_ROOT` set. HLSL is compiled to DXIL by DXC as a
build step — a shader error fails the build. See [docs/BUILDING.md](docs/BUILDING.md).

`package.ps1` signs the binaries it produces if given a code-signing
certificate (`-CertThumbprint` or `AMBERSSH_CERT_THUMBPRINT`); without one it
ships unsigned and says so. `tools/new-dev-cert.ps1` makes a development
certificate for testing — it is tamper-evident, not publicly trusted, and
Windows will still warn on it. Public distribution needs a certificate from a
CA already in the Windows trust store.

## Using it

The connection manager opens first: a category tree on the left, settings on
the right, and a Saved Sessions list with Load / Save / Delete. Pick a
protocol — for SSH/Telnet/Rlogin/Raw enter a host, port and username; for VNC
the default port fills in automatically; for a local shell nothing else is
needed at all, just press Open.

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
written to a log or a JSON file.

This project has had a security-focused audit pass, and every finding from it
— path traversal in the SFTP browser, an ungated remote-clipboard write, a
paste-escape injection, and others — is fixed and covered by tests. What the
audit found, what changed, and what is still unverified (an unresolved crash
does not lie about what it can prove) is in [AUDIT.md](AUDIT.md), in full,
including the parts that reflect badly on an earlier version of the code.
[docs/SSH_SECURITY.md](docs/SSH_SECURITY.md) covers credential storage and
host-key handling specifically; [docs/MANUAL-VERIFICATION.md](docs/MANUAL-VERIFICATION.md)
is the checklist for the parts that need a person and a screen rather than a
test.

## Known limitations

Stated plainly rather than left implicit:

- **AmberX (X11 remote-app forwarding) is in the source tree and is not part
  of this release.** No real X11 application has ever been run against it —
  that gate has never closed — so its host process is not shipped, and
  nothing in this build depends on it working.
- OSC 8 hyperlinks are consumed and highlighted on hover but not clickable.
- There is no UI Automation provider. The terminal grid is particles with no
  text layer, so a screen reader has nothing to read there.
- Keyboard-only navigation through the settings dialogs, and contrast beyond
  the automated WCAG check on faint text, have not been driven end-to-end by
  a person. See the checklist in [docs/MANUAL-VERIFICATION.md](docs/MANUAL-VERIFICATION.md).
- Interoperability has been exercised against one real SSH/VNC server
  (OpenSSH and TigerVNC-family, on Linux) during development, not against a
  wide matrix of server implementations.

A dated, fully-superseded early snapshot from before VNC or AmberX existed is
kept for history at [docs/VALIDATION_REPORT.md](docs/VALIDATION_REPORT.md) —
it says plainly at the top that it is not current.

## Documentation

| Document | Contents |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | threads, data flow, module map |
| [RENDERING.md](docs/RENDERING.md) | glyph sampling, particle model, bloom, GPU contract |
| [TERMINAL_COMPATIBILITY.md](docs/TERMINAL_COMPATIBILITY.md) | supported / partial / unsupported sequences |
| [vnc.md](docs/vnc.md) | the VNC client and desktop-as-particles renderer |
| [REMOTE-DISPLAY.md](docs/REMOTE-DISPLAY.md) | AmberX / X11 remote-app forwarding (experimental, not shipped) |
| [SSH_SECURITY.md](docs/SSH_SECURITY.md) | credential storage, host keys, hardening |
| [KEYBOARD_AND_MOUSE.md](docs/KEYBOARD_AND_MOUSE.md) | every shortcut and gesture |
| [PERFORMANCE.md](docs/PERFORMANCE.md) | targets, budget, what is not measured |
| [BUILDING.md](docs/BUILDING.md) | prerequisites and exact commands |
| [AUDIT.md](AUDIT.md) | the security audit: findings, fixes, and what remains unverified |
| [docs/MANUAL-VERIFICATION.md](docs/MANUAL-VERIFICATION.md) | the checklist for what needs a person and a screen |

## Credits

The four-layer particle model, dual-sine brightness animation, pointer force
field and shockwave follow the design of
[amber-particle-ssh](https://github.com/CrazyKickBoxer/amber-particle-ssh) (MIT).
That project is Qt6 + OpenGL on Linux; AmberSSH is an independent DirectX 12 /
HLSL implementation of the same ideas. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
