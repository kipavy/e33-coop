# E33 Coop - test a distance : remet la version du mod d'avant la derniere mise a jour a distance
# (remote\backup\<date>\, ecrite par apply-update.ps1). Utilisable a la main par l'ami :
#   powershell -NoProfile -ExecutionPolicy Bypass -File "<jeu>\Sandfall\Binaries\Win64\ue4ss\Mods\E33Coop\remote\rollback.ps1"
# -Relaunch : relance le jeu par Steam ensuite (retour arriere automatique).
param([switch]$Relaunch)
$ErrorActionPreference = "Stop"
$remote = $PSScriptRoot
$mod = Split-Path $remote -Parent
$win64 = (Resolve-Path (Join-Path $mod "..\..\..")).Path
$log = Join-Path $remote "helper.log"
function Log([string]$m) { Add-Content -Path $log -Value ("{0} rollback {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $m); Write-Output $m }

$deadline = (Get-Date).AddSeconds(120)
while ((Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
if (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) { Log "close the game first"; exit 1 }
$lastFile = Join-Path $remote "backup\last.txt"
if (-not (Test-Path $lastFile)) { Log "no backup to restore"; exit 1 }
$stamp = (Get-Content $lastFile -Raw).Trim()
if ($stamp -notmatch '^\d{8}-\d{6}$') { Log "bad backup name"; exit 1 }
$bak = Join-Path $remote "backup\$stamp"
$files = Join-Path $bak "files"
if (Test-Path $files) {
    foreach ($f in Get-ChildItem $files -Recurse -File) {
        $rel = $f.FullName.Substring($files.Length + 1)
        $dst = Join-Path $win64 $rel
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath $f.FullName $dst -Force
    }
}
# Fichiers ajoutes par la mise a jour : retires (memes regles de chemin qu'a l'installation).
$newList = Join-Path $bak "new.txt"
if (Test-Path $newList) {
    foreach ($rel in Get-Content $newList) {
        if (-not $rel -or $rel -match '\.\.' -or $rel -notmatch '^(dwmapi\.dll|ue4ss/UE4SS\.dll|ue4ss/Mods/E33Coop/.+)$') { continue }
        if ($rel -match '^ue4ss/Mods/E33Coop/(remote|logs)/' -or $rel -eq 'ue4ss/Mods/E33Coop/remotetest.txt') { continue }
        Remove-Item -LiteralPath (Join-Path $win64 ($rel.Replace('/', '\'))) -Force -ErrorAction SilentlyContinue
    }
}
Move-Item $lastFile (Join-Path $bak "restored.txt") -Force
Log "restored backup $stamp"
if ($Relaunch) { Start-Sleep -Seconds 3; Start-Process "steam://rungameid/1903340"; Log "relaunched" }
