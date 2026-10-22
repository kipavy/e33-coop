# E33 Coop - installation (player package), script version of "Installer E33 Coop.exe". Run from the unzipped folder:
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-GameDir "D:\SteamLibrary\steamapps\common\Expedition 33"]
# Copies UE4SS and the E33Coop mod into the game's Win64 folder (dwmapi.dll + ue4ss\). Saves are never touched.
# A previous dwmapi.dll / ue4ss folder is moved to Win64\e33coop-backup-<date> first.
# Remote test (docs/REMOTE-TEST.md), optional, nothing written outside the game's Win64 folder:
#   -RemoteTest <host SteamID64>  trust this host for remote tests (ue4ss\Mods\E33Coop\remotetest.txt)
#   -AllowRestart                 ...and let it restart the game (Steam relaunch)
#   -AllowUpdate                  ...and let it install new versions of the mod (with automatic rollback)
#   -NoRemoteTest                 turn remote test off (otherwise a previous setting is kept on reinstall)
param(
    [string]$GameDir = "",
    [string]$RemoteTest = "",
    [switch]$AllowRestart,
    [switch]$AllowUpdate,
    [switch]$NoRemoteTest
)
$ErrorActionPreference = "Stop"
if ($RemoteTest -and $RemoteTest -notmatch '^7656119\d{10}$') { throw "-RemoteTest: SteamID64 expected (17 digits, 7656119...)" }
if (($AllowRestart -or $AllowUpdate) -and -not $RemoteTest) { throw "-AllowRestart / -AllowUpdate need -RemoteTest <host SteamID64>" }
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

$modDir = Join-Path $win64 "ue4ss\Mods\E33Coop"
$cfgPath = Join-Path $modDir "remotetest.txt"
$previousCfg = if (Test-Path $cfgPath) { Get-Content $cfgPath -Raw } else { $null }
$settingsPath = Join-Path $modDir "settings.ini"
$previousSettings = if (Test-Path $settingsPath) { [IO.File]::ReadAllText($settingsPath) } else { $null }
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

# Options du joueur (settings.ini) : gardées d'une installation à l'autre, sinon valeurs par défaut commentées.
if ($previousSettings) { [IO.File]::WriteAllText($settingsPath, $previousSettings, (New-Object System.Text.UTF8Encoding $false)) }
elseif (-not (Test-Path $settingsPath)) {
    $default = "# E33 Coop - options (aussi modifiables en jeu : F9 ; ou avec l'installateur : Options).`r`n" +
               "# Une option par ligne. Les options de lancement Steam -e33... restent prioritaires.`r`n" +
               "# lang : auto | fr | en          langue des textes du mod`r`n" +
               "# updates : auto | ask | off     mises a jour depuis GitHub (auto : installee a la fermeture du jeu)`r`n" +
               "# aim_cam : 1 | 0                visee libre de l'autre joueur vue depuis son epaule`r`n" +
               "# turndep : 1 | 0                attente du tour de l'hote sans figer le jeu`r`n" +
               "# duo_parry : 1 | 0              (hote) bonus parade en duo`r`n" +
               "# turn_timer_s : 0 | 30 | 60 | 90  (hote) minuteur de tour, 0 = aucun`r`n" +
               "lang = auto`r`nupdates = auto`r`naim_cam = 1`r`nturndep = 1`r`nduo_parry = 1`r`nturn_timer_s = 0`r`n"
    [IO.File]::WriteAllText($settingsPath, $default, (New-Object System.Text.UTF8Encoding $false))
}

# Test a distance : configuration et assistants, uniquement dans le dossier du mod.
$cfg = $null
if ($NoRemoteTest) { $cfg = $null }
elseif ($RemoteTest) {
    $cfg = "# E33 Coop - test a distance (docs/REMOTE-TEST.md). Supprimer ce fichier desactive tout.`r`nhost=$RemoteTest`r`n" +
           "restart=$([int][bool]$AllowRestart)`r`nupdate=$([int][bool]$AllowUpdate)`r`n"
} elseif ($previousCfg) { $cfg = $previousCfg; Write-Output "Remote test setting kept from the previous install (-NoRemoteTest to turn it off)." }
if ($cfg) {
    [IO.File]::WriteAllText($cfgPath, $cfg, (New-Object System.Text.UTF8Encoding $false))
    $restart = $cfg -match '(?m)^\s*restart\s*=\s*(1|true)\s*$'
    $update = $cfg -match '(?m)^\s*update\s*=\s*(1|true)\s*$'
    $hostId = ([regex]::Match($cfg, 'host=(\d{17})')).Groups[1].Value
    Write-Output "Remote test ON: trusted host $hostId ; restart allowed: $restart ; mod update allowed: $update"
    Write-Output "  Turn off: delete $cfgPath"
}
# Assistants du test a distance toujours poses : inertes sans remotetest.txt (le mod verifie les droits du fichier
# avant de les lancer). Un remotetest.txt cree a la main apres l'installation suffit donc.
$helpers = Join-Path $modDir "remote"
New-Item -ItemType Directory -Force $helpers | Out-Null
foreach ($n in "relaunch.ps1", "apply-update.ps1", "rollback.ps1") { Copy-Item (Join-Path $here "remote-test\$n") $helpers -Force }
Write-Output ""
Write-Output "Installed in $win64"
Write-Output (Get-Content (Join-Path $here "build-info.txt") -Raw)
Write-Output "Start the game from Steam WITHOUT launch options (remove any old -e33role / -e33transport / -e33join)."
Write-Output "  Invite: load your game, press F7.   Join: at the main menu, 'Join <friend>' or F7, or accept the Steam invite."
Write-Output "  Options: F9 in game, or $settingsPath"
Write-Output "Uninstall: powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 (or 'Installer E33 Coop.exe')"
