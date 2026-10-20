# Session d'exploration (jeu lancé avec E33Explore) : attend le menu, charge TEST_B, lance un combat avec une équipe imposée.
#   explore-battle.ps1 -Party "Verso,Lune" -Encounter SM_Lancelier_Alpha [-Hooks hook_freeaim.lua,hook_qte.lua]
param(
    [string]$Party = "Verso,Lune",
    [string]$Encounter = "SM_Lancelier_Alpha",
    [string]$Hooks = ""
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$remote = Join-Path $repo "tools\dev\remote.ps1"
$snip = Join-Path $repo "lua\E33Explore\snippets"
function Remote([string]$file, [int]$timeout = 30) { powershell -NoProfile -File $remote -File $file -TimeoutSec $timeout }

$tmp = Join-Path $env:TEMP "e33-menu.lua"
Set-Content $tmp 'local m = FindFirstOf("WBP_MM_MainMenu_C"); return tostring(m ~= nil and m:IsValid())'
for ($i = 0; $i -lt 60; $i++) { if ((Remote $tmp 10) -match "true") { break }; Start-Sleep 3 }
Start-Sleep 4
Remote (Join-Path $snip "load_test_b.lua")
$tmp2 = Join-Path $env:TEMP "e33-world.lua"
Set-Content $tmp2 'local p = FindFirstOf("BP_jRPG_Character_World_C"); local gi = FindFirstOf("BP_jRPG_GI_Custom_C"); return tostring(p ~= nil and p:IsValid())'
Start-Sleep 20
for ($i = 0; $i -lt 60; $i++) { if ((Remote $tmp2 10) -match "true") { break }; Start-Sleep 3 }
Start-Sleep 12
$names = ($Party -split "," | ForEach-Object { "`"$($_.Trim())`"" }) -join ","
$tb = Join-Path $env:TEMP "e33-trigger.lua"
Set-Content $tb "_G.E33_PARTY={$names}`n_G.E33_ENC=`"$Encounter`""
Get-Content (Join-Path $snip "trigger_battle_party.lua") | Add-Content $tb
Remote $tb 60
foreach ($h in ($Hooks -split "," | Where-Object { $_ })) { Remote (Join-Path $snip $h) }
