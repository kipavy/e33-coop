# Installe UE4SS (build épinglé) + nos mods Lua dans le dossier Win64 du jeu.
# Ne touche qu'aux dossiers autorisés : Win64\dwmapi.dll, Win64\ue4ss\.
param(
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Expedition 33",
    [string]$Ue4ssDir = "$PSScriptRoot\..\..\.tools\dl\ue4ss-1181\std",
    [string[]]$LuaMods = @("E33Explore"),
    [hashtable]$Settings = @{}
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$win64 = Join-Path $GameDir "Sandfall\Binaries\Win64"
if (-not (Test-Path (Join-Path $win64 "SandFall-Win64-Shipping.exe"))) { throw "Jeu introuvable : $win64" }

Copy-Item (Join-Path $Ue4ssDir "dwmapi.dll") $win64 -Force
$dst = Join-Path $win64 "ue4ss"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item (Join-Path $Ue4ssDir "ue4ss\*") $dst -Recurse -Force

foreach ($m in $LuaMods) {
    $src = Join-Path $repo "lua\$m"
    $md = Join-Path $dst "Mods\$m"
    if (Test-Path $md) { Remove-Item $md -Recurse -Force }
    Copy-Item $src $md -Recurse
    Set-Content (Join-Path $md "enabled.txt") "" -Encoding ascii
}

# mods.txt : mods par défaut utiles + les nôtres, Keybinds en dernier
$lines = @("CheatManagerEnablerMod : 1", "ConsoleCommandsMod : 1", "ConsoleEnablerMod : 1", "SplitScreenMod : 0",
           "LineTraceMod : 0", "BPML_GenericFunctions : 0", "BPModLoaderMod : 0")
$lines += $LuaMods | ForEach-Object { "$_ : 1" }
$lines += "Keybinds : 1"
Set-Content (Join-Path $dst "Mods\mods.txt") $lines -Encoding ascii

# Réglages UE4SS
$ini = Join-Path $dst "UE4SS-settings.ini"
$text = Get-Content $ini -Raw
foreach ($k in $Settings.Keys) {
    $text = [regex]::Replace($text, "(?m)^$k\s*=.*$", "$k = $($Settings[$k])")
}
Set-Content $ini $text -Encoding utf8 -NoNewline
Write-Output "UE4SS installé dans $dst (mods : $($LuaMods -join ', '))"
