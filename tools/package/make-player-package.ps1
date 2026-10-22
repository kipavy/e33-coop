# Builds the player package (zip): UE4SS (pinned build) + E33Coop, a fixed mods.txt (both PCs must have the same
# gameplay mods: checked at the handshake), the installer exe, install.ps1 / uninstall.ps1.
#   make-player-package.ps1 -Package .tools/artifacts/<id>/e33coop-mod [-Installer ".tools/artifacts/<id>/e33coop-tools/Installer E33 Coop.exe"] -Out .tools/e33coop-player.zip
# Layout: game\ (copied as is into the game's Win64 folder), Installer E33 Coop.exe, install.ps1, uninstall.ps1,
# remote-test\, build-info.txt. The installer exe is also placed in game\ue4ss\Mods\E33Coop\ (in-game update key).
# Used by .github/workflows/release.yml (CI) and by hand.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Out = "",
    [string]$Ue4ssDir = "",
    [string]$Installer = ""
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Ue4ssDir) { $Ue4ssDir = Join-Path $repo ".tools\dl\ue4ss-1181\std" }
if (-not $Out) { $Out = Join-Path $repo ".tools\e33coop-player.zip" }
$version = (Get-Content (Join-Path $repo "VERSION") -Raw).Trim()
$stage = Join-Path ([IO.Path]::GetTempPath()) ("e33coop-pkg-" + [guid]::NewGuid().ToString("N"))
$game = Join-Path $stage "game"
$modDir = Join-Path $game "ue4ss\Mods\E33Coop"
New-Item -ItemType Directory -Force (Join-Path $game "ue4ss") | Out-Null
Copy-Item (Join-Path $Ue4ssDir "ue4ss\*") (Join-Path $game "ue4ss") -Recurse -Force
Copy-Item (Join-Path $Package "dwmapi.dll") $game -Force
Copy-Item (Join-Path $Package "ue4ss\UE4SS.dll") (Join-Path $game "ue4ss") -Force
Copy-Item (Join-Path $Package "ue4ss\Mods\*") (Join-Path $game "ue4ss\Mods") -Recurse -Force
# Same default mods as the test environment, E33Coop before Keybinds; no other gameplay mod.
$lines = @("CheatManagerEnablerMod : 1", "ConsoleCommandsMod : 1", "ConsoleEnablerMod : 1", "SplitScreenMod : 0",
           "LineTraceMod : 0", "BPML_GenericFunctions : 0", "BPModLoaderMod : 0", "E33Coop : 1", "Keybinds : 1")
Set-Content (Join-Path $game "ue4ss\Mods\mods.txt") $lines -Encoding ascii
# Version du mod (lue par l'installateur pour « installé : X »).
[IO.File]::WriteAllText((Join-Path $modDir "version.txt"), $version)
if ($Installer) {
    Copy-Item $Installer (Join-Path $stage "Installer E33 Coop.exe")
    Copy-Item $Installer (Join-Path $modDir "Installer E33 Coop.exe")   # touche U / mises à jour automatiques
}
Copy-Item (Join-Path $PSScriptRoot "install.ps1") $stage
Copy-Item (Join-Path $PSScriptRoot "uninstall.ps1") $stage
# Assistants du test à distance (copiés par install.ps1 seulement avec -AllowRestart / -AllowUpdate).
New-Item -ItemType Directory -Force (Join-Path $stage "remote-test") | Out-Null
Copy-Item (Join-Path $repo "tools\remote\client\*.ps1") (Join-Path $stage "remote-test")
Copy-Item (Join-Path $Package "build-info.txt") $stage
if (Test-Path $Out) { Remove-Item $Out -Force }
New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) | Out-Null
# ZipFile : chemins avec '/' (tar.exe de Windows, utilisé par l'installateur, les lit sans souci).
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, [IO.Path]::GetFullPath($Out), [IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item $stage -Recurse -Force
Write-Output "Package $version : $Out"
