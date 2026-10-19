# Lance un scénario de test de bout en bout et produit un rapport.
#   run-scenario.ps1 <nom> [-Package <artefact CI>] [-TimeoutSec 900] [-NoReset]
# Le scénario est tests\scenarios\<nom>.json. Le jeu est lancé via Steam (-applaunch) avec
#   -e33test=<scenario> -e33run=<dossier du run>
# Le mod E33Coop (test runner) écrit <run>\result.json puis ferme le jeu.
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Name,
    [string]$Package = "",
    [int]$TimeoutSec = 900,
    [switch]$NoReset,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Expedition 33",
    [string]$ExtraArgs = ""
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$scenario = Join-Path $repo "tests\scenarios\$Name.json"
if (-not (Test-Path $scenario)) { throw "Scénario introuvable : $scenario" }
$sc = Get-Content $scenario -Raw | ConvertFrom-Json

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$run = Join-Path $repo "runs\$stamp-$Name"
New-Item -ItemType Directory -Force $run | Out-Null
Copy-Item $scenario (Join-Path $run "scenario.json")
function Log($m) { $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m; Write-Host $line; Add-Content (Join-Path $run "runner.log") $line }

if (-not $NoReset) {
    # Pas de mod d'exploration pendant les tests : seul E33Coop tourne.
    & (Join-Path $repo "tools\reset-env\reset-env.ps1") -GameDir $GameDir -Package $Package -LuaMods @() | ForEach-Object { Log $_ }
} elseif (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) {
    throw "Le jeu tourne déjà ; relancer sans -NoReset"
}

# Faux pair éventuel ({repo} et {run} sont remplacés dans ses arguments)
$peer = $null
function Start-FakePeer {
    $fp = Join-Path $repo ".tools\bin\fakepeer.exe"
    if (-not (Test-Path $fp)) { throw "fakepeer.exe introuvable : $fp (artefact CI e33coop-tools)" }
    $peerArgs = @($sc.fakepeer.args | ForEach-Object { $_.Replace("{repo}", $repo).Replace("{run}", $run) }) + @("--log", (Join-Path $run "fakepeer.jsonl"))
    $quoted = $peerArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    Log "démarrage du faux pair : $fp $($quoted -join ' ')"
    $p = Start-Process $fp -ArgumentList $quoted -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $run "fakepeer.out") -RedirectStandardError (Join-Path $run "fakepeer.err")
    # Ouvrir le handle tout de suite : sinon ExitCode reste vide une fois le processus terminé.
    $null = $p.Handle
    return $p
}
if ($sc.fakepeer -and $sc.fakepeer.start -ne "after_game") { $peer = Start-FakePeer; Start-Sleep -Seconds 1 }

$gameArgs = @("-windowed", "-ResX=1280", "-ResY=720", "-e33test=`"$scenario`"", "-e33run=`"$run`"")
if (-not $sc.sound) { $gameArgs += "-nosound" }   # "sound": true pour les tests manuels (timing des parades)
if ($sc.launch_args) { $gameArgs += @($sc.launch_args) }
if ($ExtraArgs) { $gameArgs += $ExtraArgs }
Log "lancement : steam -applaunch 1903340 $($gameArgs -join ' ')"
$t0 = Get-Date
& "C:\Program Files (x86)\Steam\steam.exe" -applaunch 1903340 @gameArgs

# Attente du processus du jeu
$game = $null
while (-not $game -and ((Get-Date) - $t0).TotalSeconds -lt 120) {
    Start-Sleep -Seconds 2
    $game = Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $game) { Log "ÉCHEC : le jeu n'a pas démarré en 120 s (fenêtre Steam en attente ?)"; $status = "launch_failed" }
else {
    Log "jeu démarré (pid $($game.Id))"
    if ($sc.fakepeer -and $sc.fakepeer.start -eq "after_game") { $peer = Start-FakePeer }
    $result = Join-Path $run "result.json"
    $status = $null
    $nextShot = 0.0
    if ($sc.shots_every_s) { New-Item -ItemType Directory -Force (Join-Path $run "shots") | Out-Null }
    while (-not $status) {
        Start-Sleep -Seconds 2
        # "shots_every_s": N -> capture de la fenêtre du jeu toutes les N secondes (shots\tNNN.png)
        if ($sc.shots_every_s -and ((Get-Date) - $t0).TotalSeconds -ge $nextShot) {
            $nextShot += [double]$sc.shots_every_s
            $shotPath = Join-Path $run ("shots\t{0:D3}.png" -f [int]((Get-Date) - $t0).TotalSeconds)
            # processus séparé et non bloquant (Add-Type répété dans une même session peut échouer)
            Start-Process powershell -WindowStyle Hidden -ArgumentList @("-NoProfile", "-File", "`"$(Join-Path $repo 'tools\dev\screenshot.ps1')`"", "-Out", "`"$shotPath`"") | Out-Null
        }
        if (Test-Path $result) { Start-Sleep -Seconds 1; $status = "finished" }
        elseif ($game.HasExited) { $status = "game_exited" }
        else {
            # Crash : le jeu affiche une boîte « Fatal error » qui attend un clic. On note son texte et on la ferme.
            $dlg = & (Join-Path $repo "tools\dev\close-crash-dialog.ps1") -ProcessId $game.Id -Out (Join-Path $run "crash-dialog.txt")
            if ($dlg) { $dlg | ForEach-Object { Log "dialogue du jeu fermé : $_" }; $status = "crashed" }
            elseif (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) { $status = "timeout" }
        }
    }
    if (-not $game.HasExited) {
        if ($status -ne "finished") {
            try { & (Join-Path $repo "tools\dev\screenshot.ps1") -Out (Join-Path $run "shots\final-timeout.png") | Out-Null } catch {}
        }
        $game.WaitForExit(30000) | Out-Null
        if (-not $game.HasExited) { Log "arrêt forcé du jeu"; Stop-Process -Id $game.Id -Force }
    }
}
$peerCode = $null
if ($peer) {
    # le faux pair a quelques secondes pour finir proprement après le jeu
    if (-not $peer.WaitForExit(15000)) { Log "arrêt forcé du faux pair"; Stop-Process -Id $peer.Id -Force; $peerCode = -1 }
    else { $peer.WaitForExit(); $peerCode = $peer.ExitCode }
    Log "faux pair terminé, code $peerCode"
}

# Collecte
$ue = Join-Path $GameDir "Sandfall\Binaries\Win64\ue4ss"
Copy-Item (Join-Path $ue "UE4SS.log") $run -ErrorAction SilentlyContinue
Copy-Item (Join-Path $env:LOCALAPPDATA "Sandfall\Saved\Logs\Sandfall.log") (Join-Path $run "game.log") -ErrorAction SilentlyContinue
$crash = Get-ChildItem (Join-Path $env:LOCALAPPDATA "Sandfall\Saved\Crashes") -Directory -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $t0
if ($crash) { Log "CRASH détecté : $($crash.FullName -join ', ')"; $crash | ForEach-Object { Copy-Item $_.FullName (Join-Path $run "crash-$($_.Name)") -Recurse } }
# Dumps écrits par UE4SS (ue4ss\crash_*.dmp) : copiés dans le run et résumés (exception, module fautif).
$dumps = Get-ChildItem $ue -Filter "crash_*.dmp" -File -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $t0
$dumpInfo = @()
foreach ($d in $dumps) {
    Log "dump de crash UE4SS : $($d.Name)"
    Move-Item $d.FullName $run -Force
    try { $dumpInfo += & (Join-Path $repo "tools\dev\dmp-info.ps1") (Join-Path $run $d.Name) -StackWords 0 } catch { $dumpInfo += "dmp-info : $_" }
}
if ($dumpInfo) { Set-Content (Join-Path $run "crash-summary.txt") $dumpInfo -Encoding utf8 }

# Verdict
$ok = $false
$res = $null
if ($status -eq "finished") {
    $res = Get-Content (Join-Path $run "result.json") -Raw | ConvertFrom-Json
    $ok = ($res.status -eq "pass")
}
if ($peer -and $peerCode -ne 0) { $ok = $false }
$dur = [int]((Get-Date) - $t0).TotalSeconds
$report = @("# Run $stamp — $Name", "", "- Verdict : **$(if ($ok) { 'PASS' } else { 'FAIL' })** (état : $status, durée : $dur s)")
if ($res) {
    $report += "- Raison : $($res.reason)"
    $report += "- Résumé : ``$(($res.summary | ConvertTo-Json -Compress -Depth 6))``"
    if ($res.checks) { $report += ""; $report += "| Vérification | Attendu | Obtenu | OK |"; $report += "|---|---|---|---|"
        foreach ($c in $res.checks) { $report += "| $($c.name) | $($c.expected) | $($c.actual) | $(if ($c.ok) { 'oui' } else { 'NON' }) |" } }
}
if ($peer) { $report += "- Faux pair : code de sortie $peerCode$(if ($peerCode -ne 0) { ' (ÉCHEC)' })" }
if (Test-Path (Join-Path $run "crash-dialog.txt")) { $report += "- **Crash** : " + ((Get-Content (Join-Path $run "crash-dialog.txt")) -join " / ") }
if ($dumpInfo) { $report += "- Dump : " + ($dumpInfo[0..1] -join " ; ") }
$shots = Get-ChildItem (Join-Path $run "shots") -File -ErrorAction SilentlyContinue
if ($shots) { $report += ""; $report += "Captures : " + (($shots | ForEach-Object Name) -join ", ") }
Set-Content (Join-Path $run "report.md") $report -Encoding utf8
$report | ForEach-Object { Write-Output $_ }
Write-Output "Dossier du run : $run"
exit $(if ($ok) { 0 } else { 1 })
