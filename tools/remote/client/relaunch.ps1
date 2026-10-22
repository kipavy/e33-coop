# E33 Coop - test a distance : relance du jeu par Steam (installe par install.ps1 -AllowRestart dans
# ue4ss\Mods\E33Coop\remote\). Lance par le mod, cache. Aucun argument venant du reseau : seulement le PID du jeu.
# Attend la fin du jeu, puis ouvre l'URL Steam fixe steam://rungameid/1903340 (options de lancement de l'ami).
param([int]$GamePid = 0)
$ErrorActionPreference = "SilentlyContinue"
$log = Join-Path $PSScriptRoot "helper.log"
function Log([string]$m) { Add-Content -Path $log -Value ("{0} relaunch {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $m) }

Log "start (pid $GamePid)"
if ($GamePid -gt 0) { Wait-Process -Id $GamePid -Timeout 180 }
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Process "SandFall-Win64-Shipping") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
if (Get-Process "SandFall-Win64-Shipping") { Log "game still running: no relaunch"; exit 1 }
Start-Sleep -Seconds 3   # Steam doit voir le jeu ferme
Start-Process "steam://rungameid/1903340"
Log "relaunched"
