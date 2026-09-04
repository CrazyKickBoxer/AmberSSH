# amberx-kill-test.ps1 — the one Phase 5 gate criterion the in-process
# preview cannot check itself: "closing AmberSSH kills the complete
# AmberXHost process tree". The preview would have to survive its own death
# to report on it, so it is a script instead.
#
# It starts the preview with a hold, waits for the host to appear, kills
# AmberSSH the hardest way there is (TerminateProcess, no cleanup path, no
# WM_CLOSE, no destructors), and then looks for the host. The host must be
# gone: the job object is closed when the last handle to it goes with the
# process, and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE does the rest.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\amberx-kill-test.ps1
#         [-Exe build-amberx\Release\AmberSSH.exe] [-HoldMs 25000]

param(
    [string]$Exe = "build-amberx\Release\AmberSSH.exe",
    [int]$HoldMs = 25000
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    Write-Output "not found: $Exe  (the AmberX core build is build-amberx, not build)"
    exit 2
}

# The settings file is the user's, and a scripted run must not be the reason
# it changes. Backed up here, restored in the finally block.
$settings = Join-Path $env:LOCALAPPDATA "AmberSSH\settings.json"
$backup = "$settings.kill-test-backup"
if (Test-Path $settings) { Copy-Item $settings $backup -Force }

$verdict = 1
try {
    $env:AMBERX_PREVIEW_HOLD_MS = "$HoldMs"
    $app = Start-Process -FilePath $Exe -ArgumentList "--preview-amberx" -PassThru
    Write-Output "AmberSSH pid $($app.Id)"

    $host_ = $null
    for ($i = 0; $i -lt 200; $i++) {
        $host_ = Get-Process -Name AmberXHost -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $host_) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $host_) {
        Write-Output "FAIL  no AmberXHost ever started"
        exit 1
    }
    Write-Output "AmberXHost pid $($host_.Id)"

    Stop-Process -Id $app.Id -Force
    Write-Output "killed AmberSSH with TerminateProcess"

    $alive = $true
    for ($i = 0; $i -lt 50; $i++) {
        if ($null -eq (Get-Process -Id $host_.Id -ErrorAction SilentlyContinue)) { $alive = $false; break }
        Start-Sleep -Milliseconds 100
    }
    if ($alive) {
        Write-Output "FAIL  AmberXHost $($host_.Id) outlived AmberSSH"
    }
    else {
        Write-Output "PASS  the host tree died with AmberSSH"
        $verdict = 0
    }
}
finally {
    Get-Process -Name AmberXHost -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if (Test-Path $backup) {
        Copy-Item $backup $settings -Force
        Remove-Item $backup -Force
    }
    Remove-Item Env:\AMBERX_PREVIEW_HOLD_MS -ErrorAction SilentlyContinue
}
exit $verdict
