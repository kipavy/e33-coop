# QTE diagnosis (2026-10-20): measures when each real QTE press arrives, in game real time, against the window bounds.
#   qte-diag.ps1 -Mode vanilla   game without the coop mod (UE4SS + Lua logger only): battle Maelle + Lune
#   qte-diag.ps1 -Mode mod       coop mod as host (p3_manual_host) + the same Lua logger
# Play Maelle's Toile brulante with the gamepad, one press per QTE, then close the game.
# Result: runs/explore/explore.jsonl (events "qte_timing").
param([Parameter(Mandatory = $true)][ValidateSet("vanilla", "mod")][string]$Mode, [string]$Package = "")
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$explore = Join-Path $repo "runs\explore"
$remote = Join-Path $repo "tools\dev\remote.ps1"
$hook = Join-Path $repo "lua\E33Explore\snippets\hook_qte_timing.lua"
if (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "Close the game first" }
New-Item -ItemType Directory -Force $explore | Out-Null
foreach ($f in "cmd.lua", "cmd.out", "cmd.tmp") { Remove-Item (Join-Path $explore $f) -ErrorAction SilentlyContinue }
Add-Content (Join-Path $explore "explore.jsonl") ('{"ev":"qte_diag_start","data":"' + $Mode + " " + (Get-Date -Format s) + '"}')

if ($Mode -eq "vanilla") {
    & (Join-Path $repo "tools\reset-env\reset-env.ps1") -LuaMods @("E33Explore") | Out-Null
    & "C:\Program Files (x86)\Steam\steam.exe" -applaunch 1903340 -windowed -ResX=1280 -ResY=720 "-e33run=$explore"
    Write-Output "Game starting; the battle starts by itself in about 1 min 30. Do not touch the PC until then."
    Start-Sleep 50
    & (Join-Path $repo "tools\dev\explore-battle.ps1") -Party "Maelle,Lune" -Hooks hook_qte_timing.lua
} else {
    if (-not $Package) { $Package = Join-Path $repo (".tools\artifacts\" + (Get-Content (Join-Path $repo ".tools\last_build")).Trim() + "\e33coop-mod") }
    & (Join-Path $repo "tools\reset-env\reset-env.ps1") -Package $Package -LuaMods @("E33Explore") | Out-Null
    $runner = Start-Process powershell -PassThru -WindowStyle Minimized -ArgumentList @("-NoProfile", "-File", "`"$(Join-Path $repo 'tools\run-scenario\run-scenario.ps1')`"", "p3_manual_host", "-NoReset")
    Write-Output "Game starting (coop host). Do not touch the PC until the battle starts."
    Start-Sleep 20
    $run = Get-ChildItem (Join-Path $repo "runs") -Directory -Filter "*p3_manual_host" | Sort-Object Name | Select-Object -Last 1
    for ($i = 0; $i -lt 90; $i++) {
        $log = Join-Path $run.FullName "e33coop-host.jsonl"
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern '"ev":"turn_start"' -Quiet)) { break }
        Start-Sleep 2
    }
    powershell -NoProfile -File $remote -File $hook
    Write-Output "Run folder: $($run.FullName)"
}
Write-Output "Ready: play Maelle's Toile brulante with the gamepad (one press per QTE), then close the game."
