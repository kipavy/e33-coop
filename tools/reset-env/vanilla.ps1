# Rend le jeu à Killian pour qu'il joue normalement :
#  - arrête le jeu s'il tourne
#  - retire UE4SS (dwmapi.dll + dossier ue4ss) du dossier Win64 : jeu vanilla
#  - remet ses saves perso en écriture
# reset-env.ps1 (lancé par run-scenario) refait une copie des saves et les repasse en lecture seule.
param([string]$GameDir = "E:\SteamLibrary\steamapps\common\Expedition 33")
$ErrorActionPreference = "Stop"
function Log($m) { Write-Output "[vanilla] $m" }

$p = Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue
if ($p) { Log "arrêt du jeu (pid $($p.Id))"; $p | Stop-Process -Force; $p | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue }

$win64 = Join-Path $GameDir "Sandfall\Binaries\Win64"
foreach ($x in @("dwmapi.dll", "ue4ss")) {
    $path = Join-Path $win64 $x
    if (Test-Path $path) { Remove-Item $path -Recurse -Force; Log "retiré : $x" }
}

$savesRoot = Join-Path $env:LOCALAPPDATA "Sandfall\Saved\SaveGames"
Get-ChildItem $savesRoot -Directory | Where-Object { $_.Name -match '^\d{17}$' } | ForEach-Object {
    Get-ChildItem $_.FullName -File | Where-Object IsReadOnly | ForEach-Object { $_.IsReadOnly = $false; Log "en écriture : $($_.Name)" }
}
Log "jeu vanilla, saves perso en écriture"
