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
$win = "$out\window.txt"
$beat = 60.0 / $Bpm
$length = $Beats * $beat

# The grab has to be of the whole screen — a DX12 swap chain does not come
# out of a window-only BitBlt — so the window's own rectangle is measured
# while it is up and the cut is made to that. Nothing outside AmberSSH is in
# the finished film: not the taskbar, not whatever else is on the desktop.
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public class Win {
  public struct R { public int L, T, Rt, B; }
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr FindWindowW(string c, string n);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] static extern bool SystemParametersInfo(int a, int n, out R r, int f);
  public static void Dpi() { try { SetProcessDpiAwarenessContext((IntPtr)(-4)); } catch {} }
  // "x y w h" for what the named window class actually paints, or "" when
  // it is not up yet. A maximized window's rectangle includes the invisible
  // resize border and overhangs the screen, and the taskbar sits on top of
  // whatever is under it — so the visible part is the window intersected
  // with the work area, which is exactly what the film should be cut to.
  public static string Rect(string cls) {
    IntPtr h = FindWindowW(cls, null);
    if (h == IntPtr.Zero) return "";
    R w, a;
    if (!GetWindowRect(h, out w)) return "";
    if (!SystemParametersInfo(0x0030, 0, out a, 0)) return "";
    int l = Math.Max(w.L, a.L), t = Math.Max(w.T, a.T);
    int rt = Math.Min(w.Rt, a.Rt), b = Math.Min(w.B, a.B);
    if (rt - l < 400 || b - t < 300) return "";
    return l + " " + t + " " + (rt - l) + " " + (b - t);
  }
}
'@
[Win]::Dpi()

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
    # Measure it once it is up and maximized, and keep the rectangle for the cut.
    Remove-Item $win -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 40; $i++) {
      Start-Sleep -Milliseconds 250
      $rect = [Win]::Rect("AmberSSHWindow")
      if ($rect -and ([int](($rect -split ' ')[2]) -gt 400)) {
        $rect | Set-Content $win -Encoding utf8
        "window at $rect"
        break
      }
    }
    if (-not (Test-Path $win)) { "WARNING: window not measured, the cut will use the work area" }
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
      (Card "A LINUX DESKTOP" 0.6 2.6 82 $orb 0.09),
      (Card "drawn in particles, live over VNC" 1.1 2.1 38 $mic 0.16),
      (Card "THE MENU, KEY BY KEY" 3.4 3.6 56 $orb 0.09),
      (Card "every arrow is a real key on the far side" 4.0 3 30 $mic 0.15),
      (Card "A FILE MANAGER" 10.0 3.6 56 $orb 0.09),
      (Card "opened, driven, and closed again" 10.6 3 30 $mic 0.15),
      (Card "REAL INPUT" 19.0 2.8 60 $orb 0.09),
      (Card "commands running on the far side" 19.5 2.3 34 $mic 0.15),
      (Card "IRIS" 20.7 1.6 50 $orb 0.09),
      (Card "SHATTER" 24.7 1.6 50 $orb 0.09),
      (Card "SHEAR PLATES" 28.7 1.6 50 $orb 0.09),
      (Card "LIGHT SPEED" 30.7 1.6 50 $orb 0.09),
      (Card "SONIC BOOM" 34.7 1.6 50 $orb 0.09),
      (Card "BURN" 38.7 1.6 50 $orb 0.09),
      # the terminal
      (Card "AND A TERMINAL" 43.15 1.9 86 $orb 0.40),
      (Card "made of the same particles" 43.65 1.4 36 $mic 0.50),
      (Card "2000 GLYPHS, ONE FRAME" 45.2 2.4 58 $orb 0.09),
      (Card "a real directory, re-forming on every half beat" 45.7 1.9 30 $mic 0.15),
      (Card "DIGITAL RAIN" 48.6 2.3 54 $orb 0.09),
      (Card "ls /etc" 49.0 1.8 28 $mono 0.155),
      (Card "SONIC BOOM" 51.1 2.3 54 $orb 0.09),
      (Card "ls /usr/lib64" 51.5 1.8 28 $mono 0.155),
      (Card "MAGNETIC ASSEMBLE" 53.6 2.3 54 $orb 0.09),
      (Card "ls -lhA /var/log" 54.0 1.8 28 $mono 0.155),
      (Card "CYCLONE" 56.1 2.3 54 $orb 0.09),
      (Card "sftp - 28 files, every bar moving" 56.5 1.8 28 $mono 0.155),
      (Card "GLITCH" 58.6 2.3 54 $orb 0.09),
      (Card "ls /usr/share" 59.0 1.8 28 $mono 0.155),
      (Card "STARWAKE" 61.1 2.3 54 $orb 0.09),
      (Card "ls /usr/bin" 61.5 1.8 28 $mono 0.155),
      (Card "FILM BURN" 63.6 2.3 54 $orb 0.09),
      (Card "rsync - incremental, mid-flight" 64.0 1.8 28 $mono 0.155),
      (Card "MURMURATION" 66.1 2.3 54 $orb 0.09),
      (Card "HAMMER" 68.6 2.3 54 $orb 0.09),
      (Card "15 INTERFACE STYLES" 73 4.5 66 $orb 0.09),
      (Card "4 APPEARANCES" 89 3.2 56 $orb 0.09),
      (Card "AMBERSSH" 94 7 150 $orb 0.42),
      (Card "ssh   telnet   serial   local   vnc" 95.5 5.5 40 $mono 0.56)
    )
  }
  $fadeOut = $length - 1.6
  # Cut to the window's own rectangle, measured during the grab. If that
  # measurement is missing (an -Assemble of an older capture), fall back to
  # the work area, which is the same thing whenever the window was maximized.
  if (Test-Path $win) {
    $p = (Get-Content $win -Raw).Trim() -split '\s+'
    $cx = [int]$p[0]; $cy = [int]$p[1]; $cw = [int]$p[2]; $ch = [int]$p[3]
  } else {
    Add-Type -AssemblyName System.Windows.Forms
    $wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $cx = $wa.X; $cy = $wa.Y; $cw = $wa.Width; $ch = $wa.Height
  }
  $cw = [int]([math]::Floor($cw / 2) * 2); $ch = [int]([math]::Floor($ch / 2) * 2)
  "crop ${cw}x${ch}+${cx}+${cy}"
  $vf = "[0:v]crop=${cw}:${ch}:${cx}:${cy}," + ($cards -join ",") +
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
