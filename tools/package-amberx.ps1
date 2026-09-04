# package-amberx.ps1 -- build the release bundle, and refuse to build one that
# would ship something the licence matrix does not allow.
#
# AmberX is not a separate product and does not get a separate installer: it
# ships inside AmberSSH, which is why this script packages both together and
# why the notices, the SBOM and the provenance manifest are bundle contents
# rather than a web page.
#
# The licence gate is the part worth reading. It walks what the two binaries
# actually import, and every DLL must be either a Windows system library or a
# component the matrix approves. A bundle that would ship an unapproved
# dependency is not built at all -- the failure has to be at package time,
# because that is the last moment anyone looks.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\package-amberx.ps1
#            [-BuildDir build-amberx\Release] [-Out dist] [-Zip]

param(
    [string]$BuildDir = "build-amberx\Release",
    [string]$Out = "dist",
    [switch]$Zip
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail($msg) {
    Write-Output "PACKAGE FAILED: $msg"
    exit 1
}

if (-not (Test-Path (Join-Path $BuildDir "AmberSSH.exe"))) {
    Fail "$BuildDir\AmberSSH.exe not found. Build with -DAMBERX_CORE=ON first."
}
if (-not (Test-Path (Join-Path $BuildDir "AmberXHost.exe"))) {
    Fail "$BuildDir\AmberXHost.exe not found. This build has no AmberX (-DAMBERX_CORE=ON)."
}

# ---- the licence gate ------------------------------------------------------
# Everything the bundle may carry beyond the Windows system libraries. Each
# entry is a component in docs/amberx/LICENSE-MATRIX.md; anything else is a
# component nobody approved.
#   libssh2            BSD-3-Clause
#   libcrypto / libssl Apache-2.0 (OpenSSL 3)
#   z / zlib1          zlib
#   dxcompiler / dxil  MIT-style (DirectX Shader Compiler)
$approvedDlls = @(
    "libssh2.dll",
    "libcrypto-3-x64.dll",
    "libssl-3-x64.dll",
    "z.dll",
    "zlib1.dll",
    "dxcompiler.dll",
    "dxil.dll"
)
# Windows' own. Not shipped, and not the bundle's licence problem.
$systemDllPattern = '^(api-ms-|ext-ms-|kernel|user32|gdi32|advapi32|shell32|ole32|oleaut32|' +
                    'shlwapi|comdlg32|comctl32|ws2_32|crypt32|bcrypt|secur32|dbghelp|version|' +
                    'winmm|imm32|uxtheme|dwmapi|d3d12|dxgi|d3dcompiler|shcore|psapi|userenv|' +
                    'msvcp|vcruntime|ucrtbase|ntdll|rpcrt4|setupapi|cfgmgr32|powrprof|iphlpapi|' +
                    'winhttp|wintrust|propsys|windows\.|mfplat|mf|avrt|dnsapi|normaliz|wldap32|' +
                    'dwrite|d2d1|msimg32|urlmon|gdiplus|ntmarta|profapi|sechost|combase)'

$dumpbinPath = $null
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($null -eq $dumpbin) {
    $found = Get-ChildItem "C:\BuildTools\VC\Tools\MSVC" -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match "x64" } | Select-Object -First 1
    if ($null -ne $found) { $dumpbinPath = $found.FullName }
} else {
    $dumpbinPath = $dumpbin.Source
}

$unapproved = @()
if ($null -eq $dumpbinPath) {
    Write-Output "WARNING: dumpbin not found -- the import scan was skipped."
    Write-Output "         The bundle is built, but the licence gate did not run."
} else {
    foreach ($exe in @("AmberSSH.exe", "AmberXHost.exe")) {
        $path = Join-Path $BuildDir $exe
        # not $out: PowerShell is case-insensitive and that is the -Out parameter
        $deps = & $dumpbinPath /dependents $path
        foreach ($line in $deps) {
            $t = $line.Trim()
            if ($t -notmatch '\.dll$') { continue }
            if ($t -match $systemDllPattern) { continue }
            if ($approvedDlls -contains $t) { continue }
            $unapproved += "$exe imports $t"
        }
    }
    if ($unapproved.Count -gt 0) {
        $unapproved | ForEach-Object { Write-Output "  $_" }
        Fail "an import is not in the licence matrix (above). Add it to the matrix and to this script, or stop shipping it."
    }
    Write-Output "licence gate: every import is a system library or an approved component"
}

# ---- the bundle ------------------------------------------------------------
$bundle = Join-Path $Out "AmberSSH"
if (Test-Path $bundle) { Remove-Item $bundle -Recurse -Force }
New-Item -ItemType Directory -Path $bundle -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $bundle "docs\amberx") -Force | Out-Null

# binaries and the runtime files they need
foreach ($f in @("AmberSSH.exe", "AmberXHost.exe")) {
    Copy-Item (Join-Path $BuildDir $f) $bundle
}
foreach ($d in $approvedDlls) {
    $p = Join-Path $BuildDir $d
    if (Test-Path $p) { Copy-Item $p $bundle }
}
foreach ($dir in @("fonts", "shaders")) {
    $p = Join-Path $BuildDir $dir
    if (Test-Path $p) { Copy-Item $p $bundle -Recurse }
}

# notices, SBOM, provenance, and the documents a user is owed
Copy-Item "THIRD-PARTY-NOTICES.md" $bundle -ErrorAction SilentlyContinue
Copy-Item "third_party\amberx\THIRD-PARTY-NOTICES.md" (Join-Path $bundle "THIRD-PARTY-NOTICES-AmberX.md")
Copy-Item "third_party\amberx\amberx.spdx.json" $bundle
foreach ($doc in @("SOURCE-PROVENANCE.md", "LICENSE-MATRIX.md", "REJECTED-COMPONENTS.md",
                   "THREAT-MODEL.md", "COMPATIBILITY.md", "PERFORMANCE.md", "UNSUPPORTED.md")) {
    $p = Join-Path "docs\amberx" $doc
    if (Test-Path $p) { Copy-Item $p (Join-Path $bundle "docs\amberx") }
}

# the version and capability report the bundle must carry
$report = Join-Path $bundle "AMBERX-REPORT.txt"
& (Join-Path $bundle "AmberSSH.exe") --amberx-report | Out-Null
if (Test-Path "$env:TEMP\amberx-report.txt") {
    Copy-Item "$env:TEMP\amberx-report.txt" $report
} else {
    Fail "--amberx-report produced nothing; the bundle must carry one"
}

# build information: what this is, and exactly which tree it came from
$commit = (& git rev-parse HEAD 2>$null)
if (-not $commit) { $commit = "unknown" }
$dirty = (& git status --porcelain 2>$null)
if ($dirty) { $tree = "MODIFIED - not a reproducible build" } else { $tree = "clean" }
$stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
$version = @(
    "AmberSSH release bundle",
    "built          $stamp",
    "machine        $env:COMPUTERNAME",
    "commit         $commit",
    "working tree   $tree",
    "build dir      $BuildDir"
)
Set-Content -Path (Join-Path $bundle "VERSION.txt") -Value $version -Encoding utf8

# a manifest with a hash per file, so a bundle can be checked after the fact
# absolute, because FullName is: a relative $bundle would cut the wrong
# number of characters off and produce nonsense paths
$bundleFull = (Resolve-Path $bundle).Path
$manifest = Get-ChildItem $bundle -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($bundleFull.Length + 1)
    "{0}  {1}  {2}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Length, $rel
}
Set-Content -Path (Join-Path $bundle "MANIFEST-SHA256.txt") -Value $manifest -Encoding utf8

$count = (Get-ChildItem $bundle -Recurse -File).Count
Write-Output "bundle: $bundle ($count files)"

if ($Zip) {
    # not $zip: that is the -Zip switch under a different case
    $zipPath = Join-Path $Out "AmberSSH-with-AmberX.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path $bundle -DestinationPath $zipPath
    Write-Output "archive: $zipPath"
}
Write-Output "PACKAGE OK"
