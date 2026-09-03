# AmberSSH — Validation Report

Generated 2026-08-30. **Every figure below was measured on this machine.**
Anything not actually exercised is listed under "Not validated" rather than
being reported as a pass.

## Environment

| Item | Value |
|---|---|
| OS | Windows 11 Home 10.0.26200 |
| Compiler | MSVC 14.44.35207 (Visual Studio Build Tools 2022, `C:\BuildTools`) |
| CMake | 3.31.6-msvc6 (VS-bundled; not on `PATH`) |
| Generator | Visual Studio 17 2022, x64 |
| Windows SDK | 10.0.26100.0 |
| Shader compiler | DXC (vcpkg `directx-dxc` + SDK 10.0.26100.0) |
| vcpkg | `C:\Josh\vcpkg`, triplet `x64-windows` |
| GPU | not identified — no GPU-dependent measurement was taken |

## Build status — PASS (both configurations)

```
cmake --build build --config Release   ->  exit 0, zero warnings
  build\Release\AmberSSH.exe            637 KB
  build\tests\Release\AmberTests.exe

cmake --build build --config Debug     ->  exit 0, zero warnings
  build\Debug\AmberSSH.exe            1,827 KB
  build\tests\Debug\AmberTests.exe
```

- **`/W4 /WX` is enabled for project sources and the tree is clean.** The one
  warning that existed (`C4100 'factory': unreferenced parameter` in
  `sampler.cpp`) was fixed by unnaming the parameter and documenting why it
  stays in the signature — not by suppressing the warning.
  Only `/wd4201` and `/wd4324` are disabled, both for Windows SDK headers.
- Shader compilation: **12 DXIL objects** produced by DXC as a build step. A
  shader error fails the build; no `.cso` files are checked in.

## Tests — PASS

```
build\tests\Release\AmberTests.exe
All tests passed (284 assertions in 102 test cases)
exit code 0
```

| Suite | Coverage |
|---|---|
| `AnsiParserTests.cpp` | text, C0, cursor moves, erase, insert/delete, SGR incl. 256-colour and truecolor, alt screen, DECCKM/DECAWM/bracketed paste, DSR + DA replies, OSC via BEL and ST, UTF-8 whole and byte-split, sequences split across chunks, malformed and truncated input, scroll regions, tab stops |
| `TerminalBufferTests.cpp` | init, wrapping on/off, scrollback, reverse index, erase modes, insert/delete chars and lines, scroll region, alt-screen preservation, cursor save/restore, resize grow and shrink, tab stops, `GetText` |
| `SelectionTests.cpp` | forward / single-cell / multi-line extraction, trailing-blank trimming, whole-grid bounds, out-of-range and inverted coordinates, scrollback reads, UTF-8 byte lengths |
| `ProfileStoreTests.cpp` | UUIDv4 shape and uniqueness, round-trip, **no secret fields in the JSON**, malformed-entry skipping, corrupt file, absent file, upsert/remove, range repair, atomic write leaves no `.tmp`, overwrite |
| `SecureStringTests.cpp` | assign/clear/copy/move, scrubbing, credential target names, **live Credential Manager round-trip** (skips cleanly if unavailable) |
| `SyntaxTintTests.cpp` | command / flag / path / string / operator classification, command-after-pipe, leading `./cmd`, escaped quotes, bounds, over-long line refusal, password-prompt detection, prompt-end detection, palette values |
| `ShaderContractTests.cpp` | sources present, **C++ `FrameCB` size == HLSL scalar count**, 16-byte alignment, `CellGpu` layout, template-stride agreement, register bindings, 256-thread group, timestep clamp, four-layer/force-field/shockwave present, HDR format, frames-in-flight |

### Defects the tests found, then fixed

1. **`FrameCB` layout mismatch (serious).** After extending the constant buffer
   it was 152 bytes — not a multiple of 16. HLSL pads its cbuffer to 160, so
   C++ would have uploaded 152 and silently corrupted every field past the
   split. Fixed with explicit padding; the contract test now locks it.
2. **Unbounded OSC payload (security).** The parser appended OSC bytes with no
   limit, so a hostile server could grow the buffer arbitrarily. Capped at
   `kMaxOscLen` = 4096; a 200 KB title attempt is a regression test.

Three further initial failures were **bad tests, not bugs**, and the assertions
were corrected: a truncated `ESC [` legitimately consumes the next final byte
(as xterm does); `FindPromptEnd` returns the index after `"# "`; and an escaped
quote yields a span of exactly 8 characters.

## Packaging — PASS

```
.\package.ps1 -SkipBuild
  Bundled 4 runtime DLL(s).
  Bundled 12 compiled shader(s).
  dist\AmberSSH-0.1.0-windows-x64.zip   (9.99 MB)
  dist\AmberSSH-0.1.0-windows-x64.zip.sha256
  sha256: ffa6666362da8724a5e2159a744ef30f03f5b1767899adee90eaeb4b2d56df30
```

The script refuses to package without compiled shaders and strips
`profiles.json`, `settings.json`, `known_hosts`, logs and key files from the
staging directory.

## SSH validation — NOT VALIDATED

No SSH server was available and no credentials were supplied. These are
**untested** and must not be read as working:

| Item | Status |
|---|---|
| Password authentication | not tested |
| Private-key authentication | not tested |
| Keyboard-interactive authentication | not tested |
| First-use host-key prompt | not tested (implemented as a modal dialog defaulting to No) |
| Changed-host-key rejection | not tested |
| PTY resize propagation | not tested (implemented; fires on every viewport change) |
| Multiple simultaneous tabs | not tested against a server (implemented; each session owns its worker, grid and scrollback) |

## Rendering validation — NOT MEASURED

No frame-timing capture was taken; that needs the GUI running against a live
session. Resolution, GPU, particle count, average and percentile frame times,
and compute/draw/post timings are all unmeasured. The 4K/120 FPS figure remains
a target. The `F3` overlay reports these live once a session can be opened.

## Definition-of-Done status

| # | Requirement | Status |
|---|---|---|
| 1 | Builds via VS2022 + CMake | **done** — Release and Debug, zero warnings under `/W4 /WX` |
| 2 | PuTTY-style connection manager | **done** — native Win32, category tree, opens at startup |
| 3 | host/port/user/password/key/passphrase | **done** |
| 4 | Save/load/update/delete profiles | **done**, unit-tested |
| 5 | No plaintext secrets | **done**, unit-tested — Credential Manager |
| 6 | Host-key verification | **done** — modal prompt, defaults to No; untested live |
| 7 | Changed key is a serious failure | **done** in the prompt path; untested live |
| 8 | Successful connection opens a shell in a tab | implemented; untested live |
| 9 | Multiple simultaneous tabs | **done** — tab bar, `Ctrl+Shift+T` / `Ctrl+Tab` / `Ctrl+1-9`, per-session workers |
| 10 | Text rendered as GPU particles | **done** |
| 11 | Real text-cell model behind it | **done** |
| 12 | VT100/ANSI parsing | **done**, extensively tested |
| 13 | Resize changes grid and notifies server | **done**; untested live |
| 14 | Left-drag selects | **done** |
| 15 | Release copies automatically | **done** — reports "Copied N characters" non-modally |
| 16 | Miami Sunset selection | **done** — per-particle gradient in the shader |
| 17 | Right-click paste | **done** |
| 18 | Fonts changeable | partial — `Ctrl`+wheel resizes live; family picker not in the dialog |
| 19 | Particle density / effects changeable | partial — shader and tunables support it; dialog exposes the controls but they are not yet applied at runtime |
| 20 | Bloom, scanlines, flicker, chromatic aberration, mouse force | **done** in shaders; mouse force bound to `Alt`+drag |
| 21 | Unit tests over parser and core | **done** — 102 cases |
| 22 | Shaders compile as part of the build | **done** |
| 23 | Docs, scripts, tests, validation report | **done** |
| 24 | No fabricated success claims | honoured — see the two NOT VALIDATED sections |

## Known limitations

1. **Nothing has been run against a real SSH server.** Every SSH-path claim is
   "implemented and compiles", not "verified".
2. No performance measurement.
3. Settings dialog: font family and particle density controls exist in the
   connection dialog but are not yet applied to the live renderer.
4. Mouse tracking modes are parsed but pointer events are not forwarded to
   remote applications.
5. OSC 8 hyperlinks are consumed but not clickable.
6. No SSH agent support; `.ppk` keys are rejected with an explanatory message.
7. Accessibility is limited to clipboard and dialog keyboard navigation; there
   is no UI Automation provider.
8. Search over scrollback (`Ctrl+F`) is not implemented.
