# Entree synthetique vers la fenetre du jeu : -Dx/-Dy deplacement souris relatif (en Steps pas), -Click clic gauche.
param([int]$Dx = 0, [int]$Dy = 0, [int]$Steps = 20, [switch]$Click, [switch]$RClick)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct IN { public uint type; public MI mi; }
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, IN[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static void Mouse(int dx, int dy, uint flags) { var a = new IN[1]; a[0].type = 0; a[0].mi.dx = dx; a[0].mi.dy = dy; a[0].mi.flags = flags; uint r = SendInput(1, a, Marshal.SizeOf(typeof(IN))); if (r != 1) Console.WriteLine("SendInput err " + Marshal.GetLastWin32Error()); }
}
"@
$p = Get-Process "SandFall-Win64-Shipping" | Select-Object -First 1
[U]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
for ($i = 0; $i -lt $Steps; $i++) { if ($Dx -or $Dy) { [U]::Mouse($Dx, $Dy, 1) }; Start-Sleep -Milliseconds 15 }
if ($Click) { [U]::Mouse(0, 0, 2); Start-Sleep -Milliseconds 80; [U]::Mouse(0, 0, 4) }
if ($RClick) { [U]::Mouse(0, 0, 8); Start-Sleep -Milliseconds 80; [U]::Mouse(0, 0, 16) }
"ok"
