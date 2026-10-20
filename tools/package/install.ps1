# E33 Coop - installation (player package). Run from the unzipped folder:
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-GameDir "D:\SteamLibrary\steamapps\common\Expedition 33"]
# Copies UE4SS and the E33Coop mod into the game's Win64 folder (dwmapi.dll + ue4ss\). Saves are never touched.
# A previous dwmapi.dll / ue4ss folder is moved to Win64\e33coop-backup-<date> first.
param([string]$GameDir = "")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

function Find-Game {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { return $null }
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    $libs = @($steam)
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value.Replace("\\", "\") }
    }
    foreach ($l in $libs) {
        $g = Join-Path $l "steamapps\common\Expedition 33"
        if (Test-Path (Join-Path $g "Sandfall\Binaries\Win64\SandFall-Win64-Shipping.exe")) { return $g }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-Game }
if (-not $GameDir) { throw "Game not found. Use -GameDir ""<...>\steamapps\common\Expedition 33""" }
$win64 = Join-Path $GameDir "Sandfall\Binaries\Win64"
if (-not (Test-Path (Join-Path $win64 "SandFall-Win64-Shipping.exe"))) { throw "Not the game folder: $GameDir" }
if (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "Close the game first." }

$old = @((Join-Path $win64 "dwmapi.dll"), (Join-Path $win64 "ue4ss")) | Where-Object { Test-Path $_ }
if ($old) {
    $bak = Join-Path $win64 ("e33coop-backup-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory $bak | Out-Null
    foreach ($o in $old) {
        try { Move-Item $o $bak }
        catch {
            throw ("Cannot move $o (a file inside is in use). Close any text editor or Explorer window showing the " +
                   "E33Coop logs, wait for the game and its crash reporter to fully exit, then run install.ps1 again. " +
                   "Fichier utilise : fermez l'editeur ou la fenetre de l'Explorateur ouverte sur les journaux, puis relancez. ($($_.Exception.Message))")
        }
    }
    Write-Output "Previous UE4SS files moved to $bak"
}
Copy-Item (Join-Path $here "game\dwmapi.dll") $win64 -Force
Copy-Item (Join-Path $here "game\ue4ss") $win64 -Recurse -Force
Write-Output ""
Write-Output "Installed in $win64"
Write-Output (Get-Content (Join-Path $here "build-info.txt") -Raw)
Write-Output "Now set the Steam launch options (Steam > Expedition 33 > Properties > Launch options):"
Write-Output "  host  : -e33role=host -e33transport=steam"
Write-Output "  friend: -e33role=client -e33transport=steam -e33join=1"
Write-Output "Uninstall: powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1"
