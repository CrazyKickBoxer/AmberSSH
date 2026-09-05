# capture-reel.ps1 — grab the reel, cut it to the beat, lay the track on it.
#   -Capture   run the app's --reel under an FFmpeg desktop grab
#   -Assemble  trim to the flash markers, add title cards, mux the track
#   -SshOnly   the terminal act alone, no desktop and no VNC profile
#
# The reel performs to a clock of its own (see src/app_reel.cpp) and marks
# beat zero by flashing the frame white for a sixth of a beat. -Assemble
# finds that flash and trims to it, so the track's first beat lands on the
# reel's first beat without either side knowing the other's timing.
#
# -Profile is the id of a saved VNC profile for the desktop act; -Song is
# the track, and -Bpm/-FirstBeat describe it. Nothing here is written back
# to the user's settings: -Capture backs settings.json up and restores it.
param([switch]$Capture, [switch]$Assemble, [switch]$SshOnly, [double]$Bpm = 90.5, [double]$FirstBeat = 0.35,
      [string]$Profile = "", [double]$Beats = 101.0,
      [string]$Song = "", [string]$Out = "$env:TEMP\amberssh-reel",
      [string]$Ffmpeg = "", [string]$Exe = "")
if ($SshOnly) { $Beats = 76.0 }   # end before the VNC act
$ErrorActionPreference = 'Continue'
$ff = $Ffmpeg
if (-not $ff) {
  $ff = (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue).Source
  if (-not $ff) {
    $ff = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue |
          Select-Object -First 1 -ExpandProperty FullName
  }
}
if (-not $ff) { throw "ffmpeg.exe not found: pass -Ffmpeg <path>" }
$exe = $Exe
if (-not $exe) { $exe = Join-Path (Split-Path $PSScriptRoot -Parent) "build\Release\AmberSSH.exe" }
if (-not (Test-Path $exe)) { throw "AmberSSH.exe not found at $exe : pass -Exe <path>" }
$song = $Song
if ($Assemble -and -not $song) { throw "no track: pass -Song <file.mp3>" }
if (-not $SshOnly -and $Capture -and -not $Profile) { throw "no VNC profile: pass -Profile <id>, or -SshOnly" }
$out = $Out
New-Item -ItemType Directory -Force $out | Out-Null
$cap = "$out\capture.mp4"
$beat = 60.0 / $Bpm
$length = $Beats * $beat

if ($Capture) {
  $s = "$env:LOCALAPPDATA\AmberSSH\settings.json"; $bak = "$out\settings.bak"; if (Test-Path $s) { Copy-Item $s $bak -Force }
  try {
    $env:AMBER_REEL_BPM = "$Bpm"
    # no profile for the terminal-only cut: the desktop act is then skipped
    if ($SshOnly) { Remove-Item Env:AMBER_REEL_PROFILE -ErrorAction SilentlyContinue } else { $env:AMBER_REEL_PROFILE = $Profile }
    # the grab first, so beat zero is inside it whatever the app's start-up takes
    $grab = Start-Process $ff -ArgumentList @("-hide_banner","-loglevel","error","-y","-f","gdigrab","-framerate","30","-draw_mouse","0","-video_size","1920x1080","-t","$([int]($length + 20))","-i","desktop","-c:v","libx264","-preset","ultrafast","-crf","16","-pix_fmt","yuv420p",$cap) -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $app = Start-Process $exe -ArgumentList "--reel" -PassThru
    $grab.WaitForExit()
    "grab finished"
    if (-not $app.HasExited) { Stop-Process -Id $app.Id -Force }
  } finally {
    Remove-Item Env:AMBER_REEL_BPM, Env:AMBER_REEL_PROFILE -ErrorAction SilentlyContinue
    if (Test-Path $bak) { Copy-Item $bak $s -Force }
  }
}

if ($Assemble) {
  # Beat zero is the reel's flash: the appearance flips to paper for a sixth
  # of a second, so a patch in the middle of the terminal goes bright. It is
  # identified as the first bright frame preceded by a full second of dark —
  # which the window's own first paint, and any wallpaper before it, are not.
  Set-Location $env:TEMP
  & $ff -hide_banner -loglevel error -y -i $cap -vf "crop=700:400:610:310,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=amber-y.txt" -an -f null - 2>$null
  $times = @(); $ys = @()
  $pending = $null
  foreach ($line in Get-Content "$env:TEMP\amber-y.txt") {
    if ($line -match "pts_time:([\d.]+)") { $pending = [double]$Matches[1] }
    elseif ($line -match "YAVG=([\d.]+)" -and $pending -ne $null) { $times += $pending; $ys += [double]$Matches[1]; $pending = $null }
  }
  if ($ys.Count -lt 10) { "no luma samples"; return }
  $t0 = -1.0
  for ($i = 1; $i -lt $ys.Count; $i++) {
    if ($ys[$i] -lt 100) { continue }
    $dark = $true
    for ($j = $i - 1; $j -ge 0 -and ($times[$i] - $times[$j]) -le 1.0; $j--) { if ($ys[$j] -gt 40) { $dark = $false; break } }
    if ($dark -and ($times[$i] - $times[0]) -gt 1.0) { $t0 = $times[$i]; break }
  }
  if ($t0 -lt 0) { "no flash marker found"; return }
  "beat zero at $t0 s in the capture"
  # drawtext wants a forward-slash path with the drive colon escaped
  $fonts = (Join-Path (Split-Path $exe) 'fonts').Replace('\', '/').Replace(':', '\:')
  $orb = "$fonts/Orbitron-Regular.ttf"; $mic = "$fonts/Michroma-Regular.ttf"; $mono = "$fonts/ShareTechMono-Regular.ttf"
  # a card: text, start beat, beats shown, size, font, y as a fraction
  function Card($text, $b0, $bn, $size, $font, $yf) {
    $t1 = $b0 * $beat; $t2 = ($b0 + $bn) * $beat
    $a = "if(lt(t,$($t1+0.25)),(t-$t1)/0.25,if(gt(t,$($t2-0.4)),($t2-t)/0.4,1))"
    # ${size} braces are required: "$size:fontcolor" would parse as a scoped variable
    "drawtext=fontfile='$font':text='$text':fontsize=${size}:fontcolor=white:x=(w-text_w)/2:y=h*$yf-text_h/2:shadowcolor=black@0.7:shadowx=3:shadowy=3:enable='between(t,$t1,$t2)':alpha='$a'"
  }
  if ($SshOnly) {
    $cards = @(
      (Card "AMBERSSH" 0.3 6 150 $orb 0.42),
      (Card "a particle terminal" 1.0 5.3 44 $mic 0.56),
      (Card "23 MOTION STYLES" 32 5 72 $orb 0.12),
      (Card "one a beat" 32.6 4.4 34 $mic 0.20),
      (Card "15 INTERFACE STYLES" 56 5 72 $orb 0.12),
      (Card "4 APPEARANCES" 71.5 3 60 $orb 0.12),
      (Card "AMBERSSH" 69 7 130 $orb 0.42),
      (Card "ssh   telnet   serial   local   vnc" 70.5 5.5 36 $mono 0.56)
    )
  } else {
    $cards = @(
      # the desktop, first
      (Card "A LINUX DESKTOP" 0.7 4.5 82 $orb 0.09),
      (Card "drawn in particles, live over VNC" 1.4 3.8 38 $mic 0.16),
      (Card "SHOCKWAVES" 3.2 5.6 60 $orb 0.09),
      (Card "every click is a real click" 3.8 5 32 $mic 0.15),
      (Card "REAL INPUT" 12.2 4 60 $orb 0.09),
      (Card "commands running on the far side" 12.8 4 34 $mic 0.15),
      (Card "IRIS" 15.2 2.4 52 $orb 0.09),
      (Card "SONIC BOOM" 18.2 2.4 52 $orb 0.09),
      (Card "SHATTER" 21.2 2.6 52 $orb 0.09),
      (Card "ODOMETER" 25.2 2.4 52 $orb 0.09),
      (Card "SHEAR PLATES" 28.2 2.6 52 $orb 0.09),
      (Card "LIGHT SPEED" 32.2 3 52 $orb 0.09),
      (Card "BURN" 37.2 2.2 52 $orb 0.09),
      # the terminal
      (Card "AND A TERMINAL" 43.15 1.9 86 $orb 0.40),
      (Card "made of the same particles" 43.65 1.4 36 $mic 0.50),
      (Card "2000 GLYPHS, ONE FRAME" 45.2 2.4 58 $orb 0.09),
      (Card "a real directory, re-forming on every half beat" 45.7 1.9 30 $mic 0.15),
      (Card "DIGITAL RAIN" 48.6 2.3 54 $orb 0.09),
      (Card "SONIC BOOM" 51.1 2.3 54 $orb 0.09),
      (Card "MAGNETIC ASSEMBLE" 53.6 2.3 54 $orb 0.09),
      (Card "CYCLONE" 56.1 2.3 54 $orb 0.09),
      (Card "GLITCH" 58.6 2.3 54 $orb 0.09),
      (Card "STARWAKE" 61.1 2.3 54 $orb 0.09),
      (Card "FILM BURN" 63.6 2.3 54 $orb 0.09),
      (Card "MURMURATION" 66.1 2.3 54 $orb 0.09),
      (Card "HAMMER" 68.6 2.3 54 $orb 0.09),
      (Card "15 INTERFACE STYLES" 73 4.5 66 $orb 0.09),
      (Card "4 APPEARANCES" 89 3.2 56 $orb 0.09),
      (Card "AMBERSSH" 94 7 150 $orb 0.42),
      (Card "ssh   telnet   serial   local   vnc" 95.5 5.5 40 $mono 0.56)
    )
  }
  $fadeOut = $length - 1.6
  # The app is maximized, so the Windows taskbar is the only thing in the
  # grab that is not it: crop to the work area, in physical pixels.
  Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class Dpi{[DllImport("user32.dll")]public static extern bool SetProcessDpiAwarenessContext(IntPtr c);}' -ErrorAction SilentlyContinue
  try { [Dpi]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null } catch {}
  Add-Type -AssemblyName System.Windows.Forms
  $wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
  $cw = [int]([math]::Floor($wa.Width / 2) * 2); $ch = [int]([math]::Floor($wa.Height / 2) * 2)
  "crop ${cw}x${ch}+$($wa.X)+$($wa.Y)"
  $vf = "[0:v]crop=${cw}:${ch}:$($wa.X):$($wa.Y)," + ($cards -join ",") +
        ",fade=t=in:st=0:d=0.3,fade=t=out:st=${fadeOut}:d=1.6[v];" +
        "[1:a]afade=t=in:st=0:d=0.05,afade=t=out:st=${fadeOut}:d=1.6[a]"
  $final = if ($SshOnly) { "$out\AmberSSH-ssh-reel.mp4" } else { "$out\AmberSSH-reel.mp4" }
  $a = @("-hide_banner","-loglevel","error","-y","-ss","$t0","-i",$cap,"-ss","$FirstBeat","-i",$song,
         "-t","$length","-filter_complex",$vf,"-map","[v]","-map","[a]",
         "-c:v","libx264","-preset","slow","-crf","17","-pix_fmt","yuv420p",
         "-c:a","aac","-b:a","192k","-movflags","+faststart",$final)
  & $ff @a
  if (Test-Path $final) { "reel: $final  ($([math]::Round((Get-Item $final).Length/1MB,1)) MB, $([math]::Round($length,1)) s)" }
}
