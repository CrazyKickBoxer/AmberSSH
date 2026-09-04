# capture-amberx.ps1 — photographs the AmberX display window from the desktop.
# Used by the Phase 2 gate: GetImage proves the framebuffer, this proves the
# native window shows it. Usage: capture-amberx.ps1 <out.png> [waitMs]
param([string]$Out = "$env:TEMP\amberx-window.png", [int]$WaitMs = 2500)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
}
"@
Start-Sleep -Milliseconds $WaitMs
$h = [W]::FindWindowW("AmberXDisplay", [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { Write-Output "no AmberXDisplay window"; exit 1 }
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object W+R; [W]::GetClientRect($h, [ref]$r) | Out-Null
$p = New-Object W+P; $p.X = 0; $p.Y = 0; [W]::ClientToScreen($h, [ref]$p) | Out-Null
$w = $r.Rt - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($p.X, $p.Y, 0, 0, (New-Object System.Drawing.Size $w, $ht))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "captured ${w}x${ht} to $Out"
