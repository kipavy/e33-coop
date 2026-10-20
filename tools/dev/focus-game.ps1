# Donne le focus au jeu par un vrai clic (souris) au centre-haut de sa fenetre.
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class C {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct IN { [FieldOffset(0)] public uint type; [FieldOffset(8)] public int dx; [FieldOffset(12)] public int dy; [FieldOffset(16)] public uint data; [FieldOffset(20)] public uint flags; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, IN[] i, int size);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static void M(uint f) { var a = new IN[1]; a[0].type = 0; a[0].flags = f; SendInput(1, a, 40); }
}
"@
$p = Get-Process "SandFall-Win64-Shipping" | Select-Object -First 1
$r = New-Object C+R; [C]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
[C]::SetCursorPos([int](($r.l + $r.r) / 2), $r.t + 12) | Out-Null   # barre de titre : aucun effet en jeu
Start-Sleep -Milliseconds 50
[C]::M(2); Start-Sleep -Milliseconds 50; [C]::M(4); Start-Sleep -Milliseconds 300
"rect=$($r.l),$($r.t),$($r.r),$($r.b) fg=$([C]::GetForegroundWindow() -eq $p.MainWindowHandle)"
