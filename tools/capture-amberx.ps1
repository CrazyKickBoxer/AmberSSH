# capture-amberx.ps1 — photographs an AmberX window from the desktop.
# Used by the gates: GetImage proves the framebuffer, this proves the native
# window shows it. Usage: capture-amberx.ps1 <out.png> [waitMs] [frameTitle]
# With no title the rootful display window is captured; with one, the
# rootless frame carrying that title (window rectangle, strip included).
param([string]$Out = "$env:TEMP\amberx-window.png", [int]$WaitMs = 2500, [string]$Title = "")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class AXCap {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
}
"@
Start-Sleep -Milliseconds $WaitMs
if ($Title -ne "") {
  $h = [AXCap]::FindWindowW("AmberXFrame", $Title)
} else {
  $h = [AXCap]::FindWindowW("AmberXDisplay", [NullString]::Value)
}
if ($h -eq [IntPtr]::Zero) { Write-Output "no AmberX window"; exit 1 }
[AXCap]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object AXCap+R
$ok = [AXCap]::GetWindowRect($h, [ref]$r)
$w = $r.Rt - $r.L
$ht = $r.B - $r.T
if (-not $ok -or $w -le 0 -or $ht -le 0) { Write-Output "bad rectangle $($r.L),$($r.T),$($r.Rt),$($r.B)"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "captured ${w}x${ht} at $($r.L),$($r.T) to $Out"
