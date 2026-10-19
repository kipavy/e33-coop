# Télécommande du mod Lua E33Explore (outil de dev, sessions d'exploration seulement).
#   remote.ps1 -File snippet.lua    |   remote.ps1 -Code "return 1+1"
# Écrit la commande de façon atomique (cmd.tmp puis renommage en cmd.lua), attend cmd.out et l'affiche.
param(
    [string]$File = "",
    [string]$Code = "",
    [int]$TimeoutSec = 30,
    [string]$RunDir = "$PSScriptRoot\..\..\runs\explore"
)
$ErrorActionPreference = "Stop"
$src = if ($File) { Get-Content $File -Raw } else { $Code }
if (-not $src) { throw "rien à exécuter (-File ou -Code)" }
$out = Join-Path $RunDir "cmd.out"
$tmp = Join-Path $RunDir "cmd.tmp"
$cmd = Join-Path $RunDir "cmd.lua"
if (Test-Path $out) { Remove-Item $out -Force }
[IO.File]::WriteAllText($tmp, $src, (New-Object Text.UTF8Encoding $false))
Move-Item $tmp $cmd -Force
$t0 = Get-Date
while (-not (Test-Path $out)) {
    if (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) { Write-Output "TIMEOUT (pas de cmd.out après $TimeoutSec s)"; exit 1 }
    Start-Sleep -Milliseconds 300
}
Start-Sleep -Milliseconds 100
Get-Content $out -Raw
