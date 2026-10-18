# Capture la fenêtre de rendu du jeu (classe UnrealWindow) via PrintWindow :
# fonctionne même si la fenêtre est masquée par d'autres, et ne capture jamais le reste de l'écran.
param([string]$Out = "$PSScriptRoot\..\..\runs\shot.png", [string]$Process = "SandFall-Win64-Shipping")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class GW {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Ri, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => { uint wp; GetWindowThreadProcessId(h, out wp);
      if (wp == pid && IsWindowVisible(h)) { var sb = new StringBuilder(64); GetClassName(h, sb, 64);
        if (sb.ToString() == "UnrealWindow") { found = h; return false; } }
      return true; }, IntPtr.Zero);
    return found;
  }
}
"@
[GW]::SetProcessDPIAware() | Out-Null
$p = Get-Process $Process -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Error "Processus $Process introuvable"; exit 2 }
$h = [GW]::Find([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { Write-Error "Fenêtre UnrealWindow introuvable"; exit 3 }
$r = New-Object GW+R; [GW]::GetClientRect($h, [ref]$r) | Out-Null
$w = [Math]::Max(1, $r.Ri - $r.L); $hh = [Math]::Max(1, $r.B - $r.T)
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[GW]::PrintWindow($h, $hdc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output $Out
