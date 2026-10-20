# Remet l'environnement de test dans un état connu, sans jamais toucher aux saves personnelles.
#  - arrête le jeu s'il tourne
#  - vérifie / met à jour la sauvegarde des saves perso (snapshot horodaté si elles ont changé)
#  - garde les saves perso en lecture seule (sauf steam_autocloud.vdf)
#  - restaure les saves de test dans SaveGames\e33coop-test\ (hors Steam Cloud)
#  - réinstalle UE4SS + nos mods dans le dossier Win64 du jeu (seul dossier modifié)
param(
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Expedition 33",
    [string]$Package = "",                 # dossier d'un artefact CI (dwmapi.dll, ue4ss\...) ; vide = UE4SS officiel + Lua seulement
    [string[]]$LuaMods = @("E33Explore"),
    [switch]$KeepGameRunning
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$savesRoot = Join-Path $env:LOCALAPPDATA "Sandfall\Saved\SaveGames"
$backupRoot = "C:\Users\Killian\dev\e33-coop-backups"
$testSavesSrc = Join-Path $repo ".tools\testsaves"
$testSavesDst = Join-Path $savesRoot "e33coop-test"

function Log($m) { Write-Output "[reset-env] $m" }

# 1. Arrêter le jeu
if (-not $KeepGameRunning) {
    $p = Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue
    if ($p) { Log "arrêt du jeu (pid $($p.Id))"; $p | Stop-Process -Force; $p | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue }
    # Le système libère les fichiers du mod (DLL UE4SS) un peu après la fin du processus.
    for ($i = 0; $i -lt 15 -and (Get-Process "SandFall*" -ErrorAction SilentlyContinue); $i++) { Start-Sleep 2 }
    if ($p) { Start-Sleep 3 }
}

# 2. Saves perso : snapshot si elles ont changé depuis le dernier
$personal = Get-ChildItem $savesRoot -Directory | Where-Object { $_.Name -match '^\d{17}$' }
foreach ($dir in $personal) {
    $files = Get-ChildItem $dir.FullName -Recurse -File | Where-Object Name -ne "steam_autocloud.vdf"
    $sig = ($files | Sort-Object FullName | ForEach-Object { (Get-FileHash $_.FullName).Hash }) -join ""
    $sigHash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::ASCII.GetBytes($sig))).Replace("-", "").Substring(0, 16)
    $marker = Join-Path $backupRoot "last-personal-$($dir.Name).sig"
    $last = if (Test-Path $marker) { Get-Content $marker } else { "" }
    if ($last -ne $sigHash) {
        $dst = Join-Path $backupRoot ("saves-{0}-{1}" -f $dir.Name, (Get-Date -Format "yyyyMMdd-HHmmss"))
        Copy-Item $dir.FullName $dst -Recurse
        Set-Content $marker $sigHash
        Log "saves perso modifiées depuis le dernier snapshot -> copiées dans $dst"
    }
    foreach ($f in $files) { if (-not $f.IsReadOnly) { $f.IsReadOnly = $true } }
    $vdf = Join-Path $dir.FullName "steam_autocloud.vdf"
    if (Test-Path $vdf) { (Get-Item $vdf).IsReadOnly = $false }
}

# 3. Saves de test
New-Item -ItemType Directory -Force $testSavesDst | Out-Null
Get-ChildItem $testSavesDst -File | Remove-Item -Force
if (Test-Path $testSavesSrc) {
    Copy-Item (Join-Path $testSavesSrc "*.sav") $testSavesDst
    Get-ChildItem $testSavesDst -File | ForEach-Object { $_.IsReadOnly = $false }
}
Log ("saves de test : " + ((Get-ChildItem $testSavesDst -File | ForEach-Object Name) -join ", "))
# Emplacement coop (instantanés de l'hôte reçus ou envoyés, D12) : vidé avant chaque run.
$coopSaves = Join-Path $savesRoot "e33coop"
if (Test-Path $coopSaves) { Get-ChildItem $coopSaves -File | Remove-Item -Force }

# 4. Mods
$win64 = Join-Path $GameDir "Sandfall\Binaries\Win64"
$ue = Join-Path $win64 "ue4ss"
if (Test-Path (Join-Path $ue "Mods")) {
    # retirer nos mods et tout mod inconnu ; garder les mods livrés avec UE4SS
    $keep = @("BPML_GenericFunctions", "BPModLoaderMod", "CheatManagerEnablerMod", "ConsoleCommandsMod", "ConsoleEnablerMod",
              "Keybinds", "LineTraceMod", "SplitScreenMod", "shared")
    Get-ChildItem (Join-Path $ue "Mods") -Directory | Where-Object { $keep -notcontains $_.Name } | Remove-Item -Recurse -Force
}
& (Join-Path $repo "tools\dev\install-ue4ss.ps1") -GameDir $GameDir -LuaMods $LuaMods | Out-Null
if ($Package) {
    Copy-Item (Join-Path $Package "dwmapi.dll") $win64 -Force
    Copy-Item (Join-Path $Package "ue4ss\UE4SS.dll") $ue -Force
    Copy-Item (Join-Path $Package "ue4ss\Mods\*") (Join-Path $ue "Mods") -Recurse -Force
    $mods = Join-Path $ue "Mods\mods.txt"
    $lines = Get-Content $mods | Where-Object { $_ -notmatch '^E33Coop\s*:' }
    $idx = [Array]::FindIndex([string[]]$lines, [Predicate[string]]{ param($l) $l -match '^Keybinds' })
    if ($idx -lt 0) { $idx = $lines.Count }
    $lines = @($lines[0..($idx - 1)]) + "E33Coop : 1" + @($lines[$idx..($lines.Count - 1)])
    Set-Content $mods $lines -Encoding ascii
    Log "paquet natif installé depuis $Package"
}
# Paks\~mods : on n'en utilise pas ; on vérifie seulement qu'il n'y a rien
$paksMods = Join-Path $GameDir "Sandfall\Content\Paks\~mods"
if (Test-Path $paksMods) { Log ("ATTENTION : Paks\~mods contient : " + ((Get-ChildItem $paksMods | ForEach-Object Name) -join ", ")) }
Log "environnement prêt"
