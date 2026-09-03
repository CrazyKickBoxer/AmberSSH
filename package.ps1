<#
.SYNOPSIS
    Produces a portable AmberSSH ZIP plus a SHA-256 checksum.

.EXAMPLE
    .\package.ps1
    .\package.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$Version = '0.1.0'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'
$releaseDir = Join-Path $buildDir 'Release'
$distDir = Join-Path $root 'dist'

function Fail($message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

if (-not $SkipBuild) {
    & (Join-Path $root 'build.ps1') -Config Release
    if ($LASTEXITCODE -ne 0) { Fail 'Release build failed; not packaging.' }
}

$exe = Join-Path $releaseDir 'AmberSSH.exe'
if (-not (Test-Path $exe)) { Fail "No Release build found at $exe. Run build.ps1 first." }

$stage = Join-Path $buildDir "package\AmberSSH-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# --- executable + runtime DLLs --------------------------------------------
Copy-Item $exe $stage
$dllCount = 0
Get-ChildItem $releaseDir -Filter *.dll -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName $stage
    $dllCount++
}
Write-Host "Bundled $dllCount runtime DLL(s)."

# --- shaders: HLSL sources and the compiled DXIL --------------------------
$shaderSrc = Join-Path $releaseDir 'shaders'
if (Test-Path $shaderSrc) {
    Copy-Item $shaderSrc (Join-Path $stage 'shaders') -Recurse
} else {
    Fail 'Compiled shaders not found next to the executable.'
}
$cso = @(Get-ChildItem (Join-Path $stage 'shaders') -Filter *.cso -ErrorAction SilentlyContinue)
if ($cso.Count -eq 0) { Fail 'No compiled .cso shaders in the package; refusing to ship.' }
Write-Host "Bundled $($cso.Count) compiled shader(s)."

# --- docs and notices ------------------------------------------------------
foreach ($doc in @('README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md')) {
    $p = Join-Path $root $doc
    if (Test-Path $p) { Copy-Item $p $stage }
}
if (Test-Path (Join-Path $root 'docs')) {
    Copy-Item (Join-Path $root 'docs') (Join-Path $stage 'docs') -Recurse
}

# --- never ship user data --------------------------------------------------
foreach ($forbidden in @('profiles.json', 'settings.json', 'known_hosts', 'logs', '*.pem', 'id_*')) {
    Get-ChildItem $stage -Filter $forbidden -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object {
            Write-Host "Removing user data from package: $($_.Name)" -ForegroundColor Yellow
            Remove-Item $_.FullName -Recurse -Force
        }
}

# --- zip + checksum --------------------------------------------------------
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
$zip = Join-Path $distDir "AmberSSH-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ascii

$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Write-Host "`nPackage created." -ForegroundColor Green
Write-Host "  $zip  ($sizeMb MB)"
Write-Host "  $zip.sha256"
Write-Host "  sha256: $hash"
exit 0
