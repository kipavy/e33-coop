# Builds the player package (zip) for a two-PC session: UE4SS (pinned build) + E33Coop, a fixed mods.txt (both PCs must
# have the same gameplay mods: checked at the handshake), install.ps1 / uninstall.ps1.
#   make-player-package.ps1 -Package .tools/artifacts/<id>/e33coop-mod -Out .tools/e33coop-player.zip
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Out = "",
    [string]$Ue4ssDir = ""
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Ue4ssDir) { $Ue4ssDir = Join-Path $repo ".tools\dl\ue4ss-1181\std" }
if (-not $Out) { $Out = Join-Path $repo ".tools\e33coop-player.zip" }
$stage = Join-Path $env:TEMP ("e33coop-pkg-" + [guid]::NewGuid().ToString("N"))
$game = Join-Path $stage "game"
New-Item -ItemType Directory -Force (Join-Path $game "ue4ss") | Out-Null
Copy-Item (Join-Path $Ue4ssDir "ue4ss\*") (Join-Path $game "ue4ss") -Recurse -Force
Copy-Item (Join-Path $Package "dwmapi.dll") $game -Force
Copy-Item (Join-Path $Package "ue4ss\UE4SS.dll") (Join-Path $game "ue4ss") -Force
Copy-Item (Join-Path $Package "ue4ss\Mods\*") (Join-Path $game "ue4ss\Mods") -Recurse -Force
# Same default mods as the test environment, E33Coop before Keybinds; no other gameplay mod.
$lines = @("CheatManagerEnablerMod : 1", "ConsoleCommandsMod : 1", "ConsoleEnablerMod : 1", "SplitScreenMod : 0",
           "LineTraceMod : 0", "BPML_GenericFunctions : 0", "BPModLoaderMod : 0", "E33Coop : 1", "Keybinds : 1")
Set-Content (Join-Path $game "ue4ss\Mods\mods.txt") $lines -Encoding ascii
Copy-Item (Join-Path $PSScriptRoot "install.ps1") $stage
Copy-Item (Join-Path $PSScriptRoot "uninstall.ps1") $stage
Copy-Item (Join-Path $Package "build-info.txt") $stage
if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out
Remove-Item $stage -Recurse -Force
Write-Output "Package: $Out"
