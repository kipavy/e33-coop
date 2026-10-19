# Repère les boîtes de dialogue Windows (#32770) visibles d'un processus (ex. « Fatal error » du jeu
# après un crash), écrit leur texte et les ferme en cliquant leur premier bouton (OK).
#   close-crash-dialog.ps1 -ProcessId <pid> [-Out <fichier texte>] [-NoClose]
# Sortie : une ligne par dialogue trouvé (titre | textes). Rien si aucun dialogue.
param([Parameter(Mandatory = $true)][int]$ProcessId, [string]$Out = "", [switch]$NoClose)
$ErrorActionPreference = "Stop"
if (-not ("E33Dlg" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class E33Dlg {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    const uint WM_CLOSE = 0x10, WM_COMMAND = 0x111;
    static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    static string Txt(IntPtr h) { var s = new StringBuilder(8192); GetWindowText(h, s, 8192); return s.ToString(); }
    public static List<string> Handle(uint pid, bool close) {
        var found = new List<string>();
        var dialogs = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h) && Cls(h) == "#32770") dialogs.Add(h); return true; }, IntPtr.Zero);
        foreach (var d in dialogs) {
            var texts = new List<string>(); IntPtr button = IntPtr.Zero;
            EnumChildWindows(d, (h, l) => { string c = Cls(h), t = Txt(h);
                if (c == "Button" && button == IntPtr.Zero) button = h;
                if (t.Length > 0) texts.Add(t.Replace("\r", " ").Replace("\n", " ")); return true; }, IntPtr.Zero);
            found.Add(Txt(d) + " | " + string.Join(" | ", texts));
            // Post (jamais Send) : le processus planté peut ne plus répondre. IDOK = 1, puis WM_CLOSE en secours.
            if (close) { PostMessage(d, WM_COMMAND, (IntPtr)1, button); PostMessage(d, WM_CLOSE, IntPtr.Zero, IntPtr.Zero); }
        }
        return found;
    }
}
"@
}
$found = [E33Dlg]::Handle([uint32]$ProcessId, -not $NoClose)
foreach ($f in $found) {
    Write-Output $f
    if ($Out) { Add-Content -Path $Out -Value ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $f) -Encoding utf8 }
}
