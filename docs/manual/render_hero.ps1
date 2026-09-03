# Renders the AmberSSH mark and wordmark as a field of glowing particles — the
# way the app draws every glyph — into a wide banner for the manual's cover.
# Samples the logo's alpha and a GDI+-rendered wordmark on a fine grid, jitters
# each sample, and draws a soft glow pass under a crisp core pass.
param(
  [string]$Logo = (Join-Path $PSScriptRoot '../../assets/logo.png'),
  [string]$OutPath = (Join-Path $PSScriptRoot 'manual_hero.png')
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$W = 2400; $H = 720
$rnd = New-Object System.Random 4077

# ---- the mark, scaled into a square at the left ---------------------------
$src = [System.Drawing.Bitmap]::FromFile($Logo)
$markPx = 560
$mark = New-Object -TypeName System.Drawing.Bitmap -ArgumentList $markPx, $markPx
$g0 = [System.Drawing.Graphics]::FromImage($mark)
$g0.InterpolationMode = 'HighQualityBicubic'
$g0.DrawImage($src, 0, 0, $markPx, $markPx)
$g0.Dispose(); $src.Dispose()

# ---- the wordmark, rendered crisp then sampled like a glyph -----------------
$wm = New-Object -TypeName System.Drawing.Bitmap -ArgumentList 1500, 300
$g1 = [System.Drawing.Graphics]::FromImage($wm)
$g1.Clear([System.Drawing.Color]::Black)
$g1.TextRenderingHint = 'AntiAliasGridFit'
$font = New-Object System.Drawing.Font('Bahnschrift', 150, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$g1.DrawString('AmberSSH', $font, [System.Drawing.Brushes]::White, 0, 40)
$g1.Dispose()

# ---- particle samples --------------------------------------------------------
# value: 0..1 from the source; each grid point above the floor becomes one
# particle with a jittered position, a size and a temperature.
function Sample([System.Drawing.Bitmap]$bmp, [int]$ox, [int]$oy, [int]$pitch, [double]$floor, [switch]$Alpha) {
  $pts = New-Object System.Collections.Generic.List[object]
  for ($y = 0; $y -lt $bmp.Height; $y += $pitch) {
    for ($x = 0; $x -lt $bmp.Width; $x += $pitch) {
      $p = $bmp.GetPixel($x, $y)
      if ($Alpha) { $v = ($p.A / 255.0) * ((0.30 * $p.R + 0.59 * $p.G + 0.11 * $p.B) / 255.0 * 0.6 + 0.4) }
      else        { $v = (0.30 * $p.R + 0.59 * $p.G + 0.11 * $p.B) / 255.0 }
      if ($v -lt $floor) { continue }
      # sparse dropout keeps it a field, not a fill
      if ($rnd.NextDouble() -gt (0.35 + 0.65 * $v)) { continue }
      $jx = ($rnd.NextDouble() - 0.5) * $pitch * 1.6
      $jy = ($rnd.NextDouble() - 0.5) * $pitch * 1.6
      $pts.Add([pscustomobject]@{ X = $ox + $x + $jx; Y = $oy + $y + $jy; V = $v; T = $rnd.NextDouble() })
    }
  }
  return $pts
}
$markPts = Sample $mark 130 80 4 0.10 -Alpha
$wmPts   = Sample $wm   780 210 4 0.18

# ---- draw --------------------------------------------------------------------
$canvas = New-Object -TypeName System.Drawing.Bitmap -ArgumentList $W, $H
$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.SmoothingMode = 'AntiAlias'
$g.Clear([System.Drawing.Color]::Black)

# a faint amber haze behind the whole composition
$hazePath = New-Object System.Drawing.Drawing2D.GraphicsPath
$hazePath.AddEllipse(-200, -300, 1900, 1300)
$haze = New-Object System.Drawing.Drawing2D.PathGradientBrush $hazePath
$haze.CenterColor = [System.Drawing.Color]::FromArgb(28, 255, 176, 0)
$haze.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 255, 176, 0))
$g.FillRectangle($haze, 0, 0, $W, $H)

function DrawField($pts, [double]$scale) {
  # glow pass: wide, dim
  foreach ($p in $pts) {
    $r = (5.0 + 7.0 * $p.V) * $scale
    $a = [int](14 + 30 * $p.V)
    $c = [System.Drawing.Color]::FromArgb($a, 255, 176, 0)
    $b = New-Object System.Drawing.SolidBrush $c
    $g.FillEllipse($b, [single]($p.X - $r), [single]($p.Y - $r), [single](2 * $r), [single](2 * $r))
    $b.Dispose()
  }
  # core pass: small, bright; the hottest few go white-gold
  foreach ($p in $pts) {
    $r = (0.9 + 1.9 * $p.V) * $scale
    if ($p.T -gt 0.93 -and $p.V -gt 0.6) { $col = [System.Drawing.Color]::FromArgb(255, 255, 232, 170) }
    elseif ($p.T -lt 0.12)              { $col = [System.Drawing.Color]::FromArgb([int](150 + 105 * $p.V), 255, 140, 0) }
    else                                { $col = [System.Drawing.Color]::FromArgb([int](120 + 135 * $p.V), 255, 176, 0) }
    $b = New-Object System.Drawing.SolidBrush $col
    $g.FillEllipse($b, [single]($p.X - $r), [single]($p.Y - $r), [single](2 * $r), [single](2 * $r))
    $b.Dispose()
  }
}
DrawField $markPts 1.0
DrawField $wmPts 0.95

# tagline and title, crisp, letterspaced
$g.TextRenderingHint = 'AntiAliasGridFit'
$tag = New-Object System.Drawing.Font('Bahnschrift', 34, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$dim = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 192, 128, 64))
$x = 790.0
foreach ($ch in 'GPU PARTICLE TERMINAL'.ToCharArray()) {
  $s = [string]$ch
  $g.DrawString($s, $tag, $dim, [single]$x, 470)
  $x += $g.MeasureString($s, $tag).Width * 0.72 + 8
}
$sub = New-Object System.Drawing.Font('Georgia', 44, [System.Drawing.FontStyle]::Italic, [System.Drawing.GraphicsUnit]::Pixel)
$ink = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 208, 128))
$g.DrawString('User Manual', $sub, $ink, 790, 540)
$g.Dispose()
$canvas.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
"hero saved: $OutPath  ($($markPts.Count) + $($wmPts.Count) particles)"
