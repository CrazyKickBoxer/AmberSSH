<#
.SYNOPSIS
    Produces a portable AmberSSH ZIP plus a SHA-256 checksum.

.DESCRIPTION
    Signs the binaries this repository builds - AmberSSH.exe and AmberXHost.exe
    - with Authenticode, then packages them. Third-party DLLs are left with
    whatever signature they arrived with; re-signing someone else's binary with
    our key would replace their attestation with ours, which is not ours to
    make.

    A signature says two things: this came from the holder of that key, and it
    has not changed since. Only the first depends on who issued the
    certificate. A self-signed certificate still gives the second, which is
    worth having, but it will NOT satisfy SmartScreen and every downloader will
    still see an unknown-publisher warning. For public distribution the
    certificate has to come from a CA in the Windows trust store.

    Timestamping is not optional. Without it every signature stops verifying
    the day the certificate expires; with it they stay valid for the life of
    the timestamp authority's own certificate.

.EXAMPLE
    .\package.ps1
    .\package.ps1 -SkipBuild
    .\package.ps1 -CertThumbprint 71C1DDC1...  -RequireSigned
    .\package.ps1 -PfxPath release.pfx -PfxPassword (Read-Host -AsSecureString)
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$Version = '0.1.0',
    # Which certificate to sign with. Named explicitly or read from
    # AMBERSSH_CERT_THUMBPRINT; never discovered. See the note at the
    # resolution site below for why.
    [string]$CertThumbprint = $env:AMBERSSH_CERT_THUMBPRINT,
    [string]$PfxPath,
    [System.Security.SecureString]$PfxPassword,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    # Refuse to produce a package at all if the binaries could not be signed.
    # Off by default so a build machine without a certificate still works;
    # on for anything that will be handed to another person.
    [switch]$RequireSigned,
    [switch]$SkipSign
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

# --- signing ---------------------------------------------------------------
function Find-SignTool {
    $c = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
    if ($c) { return $c.FullName }
    $g = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    return $null
}

# Our own binaries only. Signing a redistributed third-party DLL would strip
# whatever its author signed it with and put our name on their code.
$ourBinaries = @('AmberSSH.exe', 'AmberXHost.exe') |
    ForEach-Object { Join-Path $releaseDir $_ } | Where-Object { Test-Path $_ }

$signed = $false
$signerName = $null
$selfSigned = $false

if ($SkipSign) {
    Write-Host 'Signing skipped by request.' -ForegroundColor Yellow
} else {
    $signtool = Find-SignTool
    if (-not $signtool) {
        if ($RequireSigned) { Fail 'signtool.exe not found and -RequireSigned was given.' }
        Write-Host 'signtool.exe not found; the package will be UNSIGNED.' -ForegroundColor Yellow
    } else {
        # Which certificate to sign with is named, never discovered.
        #
        # This used to pick the certificate automatically when the store held
        # exactly one, and on this machine that was another project's MSIX
        # development key: AmberSSH went out signed CN=JulieRoseManorDevelopment.
        # A signature is an identity claim, and choosing an identity on the
        # author's behalf is not a default a script gets to have.
        $cert = $null
        if (-not $PfxPath -and $CertThumbprint) {
            $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
                    Where-Object { $_.Thumbprint -eq $CertThumbprint.Replace(' ', '') }
            if (-not $cert) { Fail "No certificate with thumbprint $CertThumbprint in either store." }
        }

        if (-not $cert -and -not $PfxPath) {
            $hint = ("Name one with -CertThumbprint, or set AMBERSSH_CERT_THUMBPRINT. " +
                     "For a development signature: .\tools\new-dev-cert.ps1")
            if ($RequireSigned) { Fail "No signing certificate was named and -RequireSigned was given. $hint" }
            Write-Host "No signing certificate named; the package will be UNSIGNED." -ForegroundColor Yellow
            Write-Host "  $hint" -ForegroundColor Yellow
        } else {
            $args = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', $TimestampUrl)
            if ($PfxPath) {
                if (-not (Test-Path $PfxPath)) { Fail "PFX not found: $PfxPath" }
                $args += @('/f', $PfxPath)
                if ($PfxPassword) {
                    $args += @('/p', [Runtime.InteropServices.Marshal]::PtrToStringAuto(
                        [Runtime.InteropServices.Marshal]::SecureStringToBSTR($PfxPassword)))
                }
                $signerName = [IO.Path]::GetFileName($PfxPath)
            } else {
                $args += @('/sha1', $cert.Thumbprint)
                $signerName = $cert.Subject
                $selfSigned = ($cert.Subject -eq $cert.Issuer)
            }

            foreach ($bin in $ourBinaries) {
                & $signtool @($args + $bin) | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    if ($RequireSigned) { Fail "signtool failed on $bin (exit $LASTEXITCODE)." }
                    Write-Host "WARNING: signtool failed on $bin (exit $LASTEXITCODE)." -ForegroundColor Yellow
                    $ourBinaries = @()   # do not claim a partial signing succeeded
                    break
                }
            }

            # Verify what we just did rather than trusting the exit code. /pa
            # checks it against the policy Windows actually applies to
            # executables, which is the question a downloader's machine asks.
            if ($ourBinaries.Count -gt 0) {
                $allOk = $true
                foreach ($bin in $ourBinaries) {
                    # Suppressed at the process level rather than with 2>&1,
                    # which in Windows PowerShell turns a native command's
                    # stderr into error records and prints them anyway. A
                    # self-signed chain fails here by design, and signtool's
                    # alarm above our own accurate verdict reads as a problem
                    # rather than as the expected result.
                    & cmd.exe /c "`"$signtool`" verify /pa /all `"$bin`" >nul 2>&1"
                    $ok = ($LASTEXITCODE -eq 0)
                    if (-not $ok -and -not $selfSigned) { $allOk = $false }
                    Write-Host ("  {0}  {1}" -f [IO.Path]::GetFileName($bin),
                                $(if ($ok) { 'signed and trusted' }
                                  elseif ($selfSigned) { 'signed (chain not trusted: self-signed)' }
                                  else { 'SIGNATURE DID NOT VERIFY' }))
                }
                $signed = $true
                if (-not $allOk -and $RequireSigned) { Fail 'A signature did not verify.' }
            }
        }
    }
}

if ($signed) {
    Write-Host "Signed by: $signerName" -ForegroundColor Green
    if ($selfSigned) {
        Write-Host ('  This certificate is self-signed. The binaries are tamper-evident, but ' +
                    'SmartScreen will still warn every downloader and the publisher will show ' +
                    'as unknown. A CA-issued certificate is required for public distribution.') -ForegroundColor Yellow
    }
} elseif ($RequireSigned) {
    Fail 'Nothing was signed and -RequireSigned was given.'
}

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
# The docs/ folder in the repo holds the internal development record
# alongside the user-facing manual - stage-by-stage build reports, AmberX's
# phase gates, a threat model, an implementation plan, this project's own
# manual-verification checklist. None of that belongs in what a person
# extracts to run the program; it is process history, not product
# documentation, and bundling all of it (the docs/amberx/ subsystem's own
# design record in particular) is especially wrong given that subsystem's
# binary is not even shipped in this package. Curated allow-list instead of
# a recursive copy, so a new internal doc added later does not silently
# start shipping until someone deliberately adds it here.
$docStage = Join-Path $stage 'docs'
New-Item -ItemType Directory -Path $docStage -Force | Out-Null
foreach ($doc in @(
    'AmberSSH-User-Manual.pdf',
    'BUILDING.md',
    'KEYBOARD_AND_MOUSE.md',
    'vnc.md',
    'SSH_SECURITY.md',
    'REMOTE-DISPLAY.md',
    'TERMINAL_COMPATIBILITY.md',
    'ARCHITECTURE.md',
    'PERFORMANCE.md',
    'RENDERING.md'
)) {
    $p = Join-Path $root "docs\$doc"
    if (Test-Path $p) { Copy-Item $p $docStage }
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
# A ZIP cannot carry an Authenticode signature, so the archive's integrity
# rests on the checksum and the binaries' own signatures inside it. Say which
# of the two the reader is getting rather than letting "packaged" imply both.
if ($signed) {
    Write-Host ("  binaries signed: " + $signerName +
                $(if ($selfSigned) { ' (self-signed - not publicly trusted)' } else { '' }))
} else {
    Write-Host '  binaries UNSIGNED' -ForegroundColor Yellow
}
exit 0
