# Building AmberSSH

## Prerequisites

- Windows 11 x64
- Visual Studio 2022 or VS Build Tools 2022 with the **Desktop development with
  C++** workload (MSVC v143, Windows 11 SDK)
- CMake 3.24+ — the copy bundled with VS works and is found automatically
- vcpkg, with `VCPKG_ROOT` set

## Quick build

```powershell
.\build.ps1                       # Release
.\build.ps1 -Config Debug -Test   # Debug, then run the unit tests
.\build.ps1 -Clean                # wipe build/ first
```

`build.ps1` locates vcpkg and cmake itself, fails loudly with a non-zero exit
code, and prints the final executable path.

## Manual build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
.\build\tests\Release\AmberTests.exe
```

## Packaging

```powershell
.\package.ps1              # builds Release, then packages
.\package.ps1 -SkipBuild   # package an existing Release build
```

Produces `dist\AmberSSH-<version>-windows-x64.zip` and a `.sha256` beside it.
The script refuses to package without compiled shaders and strips any user data
(profiles, known_hosts, logs, keys) from the staging directory.

## Shaders

HLSL is compiled to DXIL by DXC as a CMake build step — 12 entry points across
6 files. A shader error fails the build; there are no checked-in `.cso` files.
Compiled shaders and their sources are deployed next to the executable.

## Dependencies (vcpkg manifest)

`libssh2`, `openssl`, `nlohmann-json`, `catch2`, `imgui` (dx12 + win32
bindings), `directx-dxc`.
