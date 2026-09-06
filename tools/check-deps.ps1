<#
.SYNOPSIS
    Reports how far the pinned vcpkg baseline has drifted from upstream.

.DESCRIPTION
    Pinning the baseline fixed reproducibility and created staleness: a pin
    nobody moves is how a project ships a two-year-old OpenSSL. This is the
    other half. It compares the versions vcpkg.json's baseline resolves to
    against what the registry holds at HEAD, and says which have moved.

    It changes nothing. Moving the baseline is a decision, and it should be
    taken by someone who then rebuilds and reruns the tests.

    Exit code 1 when a security-relevant dependency has moved, so a scheduled
    job can use it as a signal.
#>
[CmdletBinding()]
param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    # The ones whose staleness is a security question rather than a chore.
    [string[]]$Critical = @('openssl', 'libssh2')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not $VcpkgRoot) {
    foreach ($c in @('C:\Josh\vcpkg', 'C:\vcpkg', "$env:USERPROFILE\vcpkg")) {
        if (Test-Path (Join-Path $c 'versions')) { $VcpkgRoot = $c; break }
    }
}
if (-not $VcpkgRoot -or -not (Test-Path $VcpkgRoot)) {
    Write-Host 'vcpkg not found. Pass -VcpkgRoot or set VCPKG_ROOT.' -ForegroundColor Red
    exit 2
}

$manifest = Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
$baseline = $manifest.'builtin-baseline'
if (-not $baseline) {
    Write-Host 'vcpkg.json has no builtin-baseline: nothing is pinned.' -ForegroundColor Red
    exit 2
}

# The version a package resolves to is recorded in versions/baseline.json at
# whichever commit is pinned, so both answers come from the same file read at
# two different commits.
function Get-BaselineVersions([string]$commit) {
    $json = & git -C $VcpkgRoot show "${commit}:versions/baseline.json" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $json) { return $null }
    return ($json -join "`n" | ConvertFrom-Json).default
}

Write-Host "pinned baseline: $baseline"
& git -C $VcpkgRoot cat-file -e "$baseline^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "That commit is not in $VcpkgRoot. Fetch it first:" -ForegroundColor Yellow
    Write-Host "  git -C $VcpkgRoot fetch --unshallow" -ForegroundColor Yellow
    exit 2
}

$head = (& git -C $VcpkgRoot rev-parse origin/master 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $head) { $head = (& git -C $VcpkgRoot rev-parse HEAD) }

$pinned = Get-BaselineVersions $baseline
$latest = Get-BaselineVersions $head
if (-not $pinned -or -not $latest) {
    Write-Host 'Could not read versions/baseline.json at one of the commits.' -ForegroundColor Red
    exit 2
}

$names = @($manifest.dependencies | ForEach-Object { if ($_ -is [string]) { $_ } else { $_.name } })
$moved = 0
$criticalMoved = 0

Write-Host ''
Write-Host ('{0,-18} {1,-14} {2,-14}' -f 'package', 'pinned', 'upstream')
foreach ($n in $names) {
    $p = $pinned.$n
    $l = $latest.$n
    if (-not $p -or -not $l) { continue }
    $pv = "$($p.baseline)#$($p.'port-version')"
    $lv = "$($l.baseline)#$($l.'port-version')"
    $same = ($pv -eq $lv)
    if (-not $same) {
        $moved++
        if ($Critical -contains $n) { $criticalMoved++ }
    }
    $mark = if ($same) { '' } elseif ($Critical -contains $n) { '   <-- security-relevant' } else { '   moved' }
    Write-Host ('{0,-18} {1,-14} {2,-14}{3}' -f $n, $pv, $lv, $mark)
}

Write-Host ''
if ($criticalMoved -gt 0) {
    Write-Host "$criticalMoved security-relevant dependency/dependencies have moved upstream." -ForegroundColor Yellow
    Write-Host 'To take them: set builtin-baseline in vcpkg.json to' -ForegroundColor Yellow
    Write-Host "  $head" -ForegroundColor Yellow
    Write-Host 'then reconfigure, rebuild, and rerun the tests before committing.' -ForegroundColor Yellow
    exit 1
}
if ($moved -gt 0) {
    Write-Host "$moved dependency/dependencies have moved; none of them security-relevant."
    exit 0
}
Write-Host 'Everything matches the pinned baseline.' -ForegroundColor Green
exit 0
