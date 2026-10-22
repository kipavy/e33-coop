# E33 Coop - test a distance : installe une mise a jour du mod recue de l'hote de confiance (installe par
# install.ps1 -AllowUpdate dans ue4ss\Mods\E33Coop\remote\). Lance par le mod, cache, apres verification des
# empreintes. Aucun argument venant du reseau : seulement le PID du jeu ; ne lit que remote\staging\.
#  1. attend la fin du jeu ;
#  2. reverifie chaque fichier (liste de destinations permises, SHA-256 du manifeste) ;
#  3. sauvegarde les fichiers remplaces dans remote\backup\<date>\ ;
#  4. copie, puis relance le jeu par Steam (steam://rungameid/1903340) ;
#  5. si le mod ne demarre pas (pas de "mod_init" dans un nouveau journal en 3 min) : retour arriere automatique
#     (rollback.ps1 -Relaunch).
param([int]$GamePid = 0)
$ErrorActionPreference = "Stop"
$remote = $PSScriptRoot
$mod = Split-Path $remote -Parent
$win64 = (Resolve-Path (Join-Path $mod "..\..\..")).Path
$staging = Join-Path $remote "staging"
$log = Join-Path $remote "helper.log"
function Log([string]$m) { Add-Content -Path $log -Value ("{0} update {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $m) }

# Memes regles que update_dest_allowed (native/remote/remote_proto.cpp).
function Test-Dest([string]$p) {
    if ($p.Length -gt 240 -or $p -notmatch '^[A-Za-z0-9._+() -]+(/[A-Za-z0-9._+() -]+)*$') { return $false }
    $parts = $p.Split('/')
    if ($parts.Count -gt 8) { return $false }
    foreach ($part in $parts) { if ($part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part.StartsWith(' ')) { return $false } }
    $l = $p.ToLowerInvariant()
    if ($l -eq 'dwmapi.dll' -or $l -eq 'ue4ss/ue4ss.dll') { return $true }
    if (-not $l.StartsWith('ue4ss/mods/e33coop/')) { return $false }
    $rest = $l.Substring(19)
    if ($rest -eq '' -or $rest.StartsWith('remote/') -or $rest.StartsWith('logs/') -or $rest -eq 'remotetest.txt' -or $rest -eq 'remote' -or $rest -eq 'logs') { return $false }
    return $true
}

function Wait-GameExit([int]$GamePid) {
    if ($GamePid -gt 0) { Wait-Process -Id $GamePid -Timeout 180 -ErrorAction SilentlyContinue }
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    return -not (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue)
}

try {
    Log "start (pid $GamePid)"
    # Garde-fou : l'ami doit toujours avoir autorise les mises a jour.
    $cfg = Join-Path $mod "remotetest.txt"
    if (-not (Test-Path $cfg) -or -not ((Get-Content $cfg) -match '^\s*update\s*=\s*(1|true)\s*$')) { Log "updates not allowed: abort"; exit 1 }
    $manifest = Join-Path $staging "manifest.txt"
    if (-not (Test-Path $manifest)) { Log "no manifest: abort"; exit 1 }
    if (-not (Wait-GameExit $GamePid)) { Log "game still running: abort (staging kept)"; exit 1 }

    $entries = @()
    foreach ($line in Get-Content $manifest) {
        if (-not $line.Trim()) { continue }
        $m = [regex]::Match($line, '^([0-9a-f]{64}) (\d+) (.+)$')
        if (-not $m.Success) { throw "bad manifest line: $line" }
        $rel = $m.Groups[3].Value
        if (-not (Test-Dest $rel)) { throw "destination refused: $rel" }
        $src = Join-Path $staging ($rel.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "missing staged file: $rel" }
        if ((Get-Item -LiteralPath $src).Length -ne [int64]$m.Groups[2].Value) { throw "size mismatch: $rel" }
        if ((Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash.ToLowerInvariant() -ne $m.Groups[1].Value) { throw "hash mismatch: $rel" }
        $entries += [pscustomobject]@{ Rel = $rel; Src = $src; Dst = (Join-Path $win64 ($rel.Replace('/', '\'))) }
    }
    if ($entries.Count -eq 0) { throw "empty manifest" }

    # Sauvegarde : fichiers remplaces (files\) et fichiers nouveaux (new.txt, supprimes au retour arriere).
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $bak = Join-Path $remote "backup\$stamp"
    New-Item -ItemType Directory -Force (Join-Path $bak "files") | Out-Null
    $new = @()
    foreach ($e in $entries) {
        if (Test-Path -LiteralPath $e.Dst -PathType Leaf) {
            $b = Join-Path $bak ("files\" + $e.Rel.Replace('/', '\'))
            New-Item -ItemType Directory -Force (Split-Path $b -Parent) | Out-Null
            Copy-Item -LiteralPath $e.Dst $b -Force
        } else { $new += $e.Rel }
    }
    Set-Content (Join-Path $bak "new.txt") $new -Encoding ascii
    Copy-Item $manifest (Join-Path $bak "manifest.txt")
    Set-Content (Join-Path $remote "backup\last.txt") $stamp -Encoding ascii

    foreach ($e in $entries) {
        New-Item -ItemType Directory -Force (Split-Path $e.Dst -Parent) | Out-Null
        Copy-Item -LiteralPath $e.Src $e.Dst -Force
    }
    Remove-Item $staging -Recurse -Force
    Log "applied $($entries.Count) files (backup $stamp)"
} catch {
    Log "failed before relaunch: $($_.Exception.Message)"
    # Copie partielle possible : on remet la sauvegarde si elle existe.
    if ($bak -and (Test-Path (Join-Path $remote "backup\last.txt"))) { & (Join-Path $remote "rollback.ps1") -Relaunch; exit 1 }
    Start-Process "steam://rungameid/1903340"
    exit 1
}

# Relance et surveillance : le nouveau mod doit ecrire "mod_init" dans un nouveau journal.
$t0 = Get-Date
Start-Sleep -Seconds 3
Start-Process "steam://rungameid/1903340"
Log "relaunched, waiting for mod_init"
$deadline = $t0.AddMinutes(3)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $fresh = Get-ChildItem (Join-Path $mod "logs") -Recurse -Filter "e33coop-*.jsonl" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 }
    foreach ($f in $fresh) {
        if (Select-String -LiteralPath $f.FullName -SimpleMatch '"ev":"mod_init"' -Quiet) { Log "ok: mod_init in $($f.FullName)"; exit 0 }
    }
}
Log "no mod_init within 3 min: rollback"
Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 5
& (Join-Path $remote "rollback.ps1") -Relaunch
