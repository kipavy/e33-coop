# Real key press (keybd_event) to the focused window. -Vk virtual key, -Scan scan code (AZERTY layout: Q key = 0x41/0x10).
# Focus the game first with focus-game.ps1. Example: sendkey.ps1 -Vk 0x20 -Scan 0x39 (Space = QTE).
param([int]$Vk, [int]$Scan, [int]$HoldMs = 70)
Add-Type 'using System;using System.Runtime.InteropServices;public static class K3{[DllImport("user32.dll")]public static extern void keybd_event(byte v,byte s,uint f,UIntPtr e);}'
[K3]::keybd_event($Vk,$Scan,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds $HoldMs; [K3]::keybd_event($Vk,$Scan,2,[UIntPtr]::Zero)
