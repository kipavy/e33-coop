# Capture la fenêtre du jeu (ou l'écran principal) en PNG.
param([string]$Out = "$PSScriptRoot\..\..\runs\shot.png", [string]$Process = "SandFall-Win64-Shipping")
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W { [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Ri, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[W]::SetProcessDPIAware() | Out-Null
$p = Get-Process $Process -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if ($p) { $r = New-Object W+R; [W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null; $x=$r.L; $y=$r.T; $w=$r.Ri-$r.L; $h=$r.B-$r.T }
else { $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $x=$b.X; $y=$b.Y; $w=$b.Width; $h=$b.Height }
if ($w -le 0 -or $h -le 0) { $x=0; $y=0; $w=1920; $h=1080 }
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output $Out
