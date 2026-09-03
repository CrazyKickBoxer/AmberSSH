# AmberSSH — Implementation Plan

## 0. Situation assessment (2026-08-30)

The repository was **not empty**. A working implementation dated 2026-08-14
already existed and still compiles today with the installed toolchain:

- `cmake --build build --config Release` → exit 0,
  `build/Release/AmberSSH.exe` produced, 12 HLSL entry points compiled to DXIL
  via DXC, libssh2 + OpenSSL + zlib DLLs deployed.
- ~6,800 lines across `src/{app,dx,glyphs,render,ssh,term,ui}` and
  `shaders/`.

### Engineering decision: evolve, do not restart

The source prompt was written assuming an empty repository ("from scratch").
Deleting a verified, compiling DX12 + libssh2 codebase to retype it would
destroy working code and buy nothing. The prompt permits adjusted structure
provided "strong subsystem separation" is preserved; the existing
`dx/ glyphs/ render/ ssh/ term/ ui/` split already satisfies that.

**Therefore:** keep the existing subsystems, add the missing ones, and replace
the one component the spec explicitly forbids (ImGui as primary UI).

## 1. Verified toolchain

| Component | Value |
|---|---|
| Compiler | MSVC 14.44.35207 (VS Build Tools 2022) at `C:\BuildTools` |
| CMake | 3.31.6-msvc6 (`C:\BuildTools\...\CMake\bin\cmake.exe`, not on PATH) |
| Generator | Visual Studio 17 2022, x64 |
| Windows SDK | 10.0.26100.0 |
| DXC | SDK 10.0.26100.0 + vcpkg `directx-dxc` |
| vcpkg | `C:\Josh\vcpkg` (`VCPKG_ROOT` set), triplet `x64-windows` |

## 2. Gap analysis against Definition of Done

| # | Requirement | Before | Plan |
|---|---|---|---|
| 1 | Builds via VS2022 + CMake | DONE | keep, add presets |
| 2 | PuTTY-style connection manager | ImGui in-engine form | **replace** with native Win32 dialog + category tree |
| 3 | host/port/user/password/key/passphrase | partial | extend into profile model |
| 4 | Save/load/update/delete profiles | missing | `profiles/ProfileStore` (JSON) |
| 5 | No plaintext secrets | missing | `platform/CredentialStore` (Credential Manager) |
| 6-7 | Host-key verify / changed-key failure | prompt exists | `ssh/KnownHosts` + hard-fail on change |
| 8-9 | Shell in a tab / multiple tabs | single session | `sessions/SessionManager` + tab bar |
| 10-11 | Particle render + text model | DONE | keep |
| 12 | VT100/ANSI | present | extend + test |
| 13 | Resize → PTY | present | verify |
| 14-17 | Selection, auto-copy, Miami Sunset, right-click paste | partial | complete |
| 18-20 | Font/particle/effect settings UI | missing | native settings dialog |
| 21 | Unit tests | missing | Catch2 suite |
| 22 | Shaders compile in build | DONE | keep |
| 23 | Docs, scripts, validation report | missing | add |

## 3. Work order

1. **Foundation** — `utility/` (SecureString, Logging, Expected),
   `platform/` (Paths, CredentialStore).
2. **Persistence** — `profiles/` (ConnectionProfile, ProfileStore, Settings).
3. **Tests** — Catch2 over parser, grid, UTF-8, selection, profiles.
4. **Native UI** — ConnectionDialog, HostKeyDialog, SettingsDialog; drop ImGui.
5. **Sessions** — multi-tab SessionManager.
6. **Terminal polish** — Miami Sunset selection, syntax tint, search.
7. **Ship** — build.ps1, package.ps1, CI, docs, validation report.

Each step must compile before the next begins. No step is reported complete
without a build or test transcript.
