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
# Steam doit voir le jeu ferme : lanceur Expedition33_Steam.exe ferme aussi et Apps\1903340\Running a 0 (lecture
# seule du registre de Steam). Sinon Steam repond « jeu deja en cours ». Un second essai si le jeu ne demarre pas.
function Start-Game {
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline) {
        $p = Get-Process "SandFall-Win64-Shipping", "Expedition33_Steam" -ErrorAction SilentlyContinue
        $r = (Get-ItemProperty "HKCU:\Software\Valve\Steam\Apps\1903340" -ErrorAction SilentlyContinue).Running
        if (-not $p -and -not $r) { break }
        Start-Sleep -Seconds 1
    }
    Start-Sleep -Seconds 5
    for ($try = 1; $try -le 2; $try++) {
        Start-Process "steam://rungameid/1903340"
        Log "launch requested (try $try)"
        $until = (Get-Date).AddSeconds(90)
        while ((Get-Date) -lt $until) {
            if (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) { Log "game started"; return $true }
            Start-Sleep -Seconds 2
        }
    }
    Log "game did not start"
    return $false
}
Start-Game | Out-Null
