<#
.SYNOPSIS
    Configures and builds AmberSSH.

.EXAMPLE
    .\build.ps1                     # Release build
    .\build.ps1 -Config Debug -Test # Debug build, then run the test suite
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'

function Fail($message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

# --- vcpkg -----------------------------------------------------------------
if (-not $env:VCPKG_ROOT) {
    foreach ($candidate in @('C:\Josh\vcpkg', 'C:\vcpkg', 'C:\dev\vcpkg')) {
        if (Test-Path (Join-Path $candidate 'scripts\buildsystems\vcpkg.cmake')) {
            $env:VCPKG_ROOT = $candidate
            break
        }
    }
}
if (-not $env:VCPKG_ROOT) {
    Fail 'VCPKG_ROOT is not set and vcpkg was not found in the usual locations.'
}
$toolchain = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) { Fail "vcpkg toolchain not found at $toolchain" }
Write-Host "vcpkg:  $env:VCPKG_ROOT"

# --- cmake -----------------------------------------------------------------
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)?.Source
if (-not $cmake) {
    $candidates = @(
        'C:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\CMake\bin\cmake.exe'
    )
    foreach ($c in $candidates) {
        $hit = Get-Item $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $cmake = $hit.FullName; break }
    }
}
if (-not $cmake) { Fail 'cmake.exe not found. Install CMake or Visual Studio 2022 C++ tools.' }
Write-Host "cmake:  $cmake"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host 'Cleaning build directory...'
    Remove-Item $buildDir -Recurse -Force
}

# --- configure -------------------------------------------------------------
Write-Host "`nConfiguring ($Config)..." -ForegroundColor Cyan
& $cmake -S $root -B $buildDir -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed ($LASTEXITCODE)." }

# --- build (compiles HLSL through DXC as a build step) ---------------------
Write-Host "`nBuilding ($Config)..." -ForegroundColor Cyan
& $cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { Fail "Build failed ($LASTEXITCODE)." }

$exe = Join-Path $buildDir "$Config\AmberSSH.exe"
if (-not (Test-Path $exe)) { Fail "Build reported success but $exe is missing." }

# --- tests -----------------------------------------------------------------
if ($Test) {
    $tests = Join-Path $buildDir "tests\$Config\AmberTests.exe"
    if (-not (Test-Path $tests)) { Fail "Test binary not found at $tests" }
    Write-Host "`nRunning tests..." -ForegroundColor Cyan
    & $tests
    if ($LASTEXITCODE -ne 0) { Fail "Tests failed ($LASTEXITCODE)." }
    Write-Host 'All tests passed.' -ForegroundColor Green
}

Write-Host "`nBuild succeeded." -ForegroundColor Green
Write-Host "Executable: $exe"
exit 0
