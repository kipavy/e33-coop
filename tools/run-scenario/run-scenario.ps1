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
function Log($m) { $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m; Write-Output $line; Add-Content (Join-Path $run "runner.log") $line }

if (-not $NoReset) {
    # Pas de mod d'exploration pendant les tests : seul E33Coop tourne.
    & (Join-Path $repo "tools\reset-env\reset-env.ps1") -GameDir $GameDir -Package $Package -LuaMods @() | ForEach-Object { Log $_ }
} elseif (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) {
    throw "Le jeu tourne déjà ; relancer sans -NoReset"
}

# Faux pair éventuel
$peer = $null
if ($sc.fakepeer) {
    $fp = Join-Path $repo ".tools\bin\fakepeer.exe"
    $peerArgs = @($sc.fakepeer.args) + @("--log", (Join-Path $run "fakepeer.jsonl"))
    Log "démarrage du faux pair : $fp $($peerArgs -join ' ')"
    $peer = Start-Process $fp -ArgumentList $peerArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $run "fakepeer.out") -RedirectStandardError (Join-Path $run "fakepeer.err")
}

$gameArgs = @("-windowed", "-ResX=1280", "-ResY=720", "-nosound", "-e33test=`"$scenario`"", "-e33run=`"$run`"")
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
    $result = Join-Path $run "result.json"
    $status = $null
    while (-not $status) {
        Start-Sleep -Seconds 2
        if (Test-Path $result) { Start-Sleep -Seconds 1; $status = "finished" }
        elseif ($game.HasExited) { $status = "game_exited" }
        elseif (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) { $status = "timeout" }
    }
    if (-not $game.HasExited) {
        if ($status -ne "finished") {
            try { & (Join-Path $repo "tools\dev\screenshot.ps1") -Out (Join-Path $run "shots\final-timeout.png") | Out-Null } catch {}
        }
        $game.WaitForExit(30000) | Out-Null
        if (-not $game.HasExited) { Log "arrêt forcé du jeu"; Stop-Process -Id $game.Id -Force }
    }
}
if ($peer -and -not $peer.HasExited) { Stop-Process -Id $peer.Id -Force }

# Collecte
$ue = Join-Path $GameDir "Sandfall\Binaries\Win64\ue4ss"
Copy-Item (Join-Path $ue "UE4SS.log") $run -ErrorAction SilentlyContinue
Copy-Item (Join-Path $env:LOCALAPPDATA "Sandfall\Saved\Logs\Sandfall.log") (Join-Path $run "game.log") -ErrorAction SilentlyContinue
$crash = Get-ChildItem (Join-Path $env:LOCALAPPDATA "Sandfall\Saved\Crashes") -Directory -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $t0
if ($crash) { Log "CRASH détecté : $($crash.FullName -join ', ')"; $crash | ForEach-Object { Copy-Item $_.FullName (Join-Path $run "crash-$($_.Name)") -Recurse } }

# Verdict
$ok = $false
$res = $null
if ($status -eq "finished") {
    $res = Get-Content (Join-Path $run "result.json") -Raw | ConvertFrom-Json
    $ok = ($res.status -eq "pass")
}
$dur = [int]((Get-Date) - $t0).TotalSeconds
$report = @("# Run $stamp — $Name", "", "- Verdict : **$(if ($ok) { 'PASS' } else { 'FAIL' })** (état : $status, durée : $dur s)")
if ($res) {
    $report += "- Raison : $($res.reason)"
    $report += "- Résumé : ``$(($res.summary | ConvertTo-Json -Compress -Depth 6))``"
    if ($res.checks) { $report += ""; $report += "| Vérification | Attendu | Obtenu | OK |"; $report += "|---|---|---|---|"
        foreach ($c in $res.checks) { $report += "| $($c.name) | $($c.expected) | $($c.actual) | $(if ($c.ok) { 'oui' } else { 'NON' }) |" } }
}
$shots = Get-ChildItem (Join-Path $run "shots") -File -ErrorAction SilentlyContinue
if ($shots) { $report += ""; $report += "Captures : " + (($shots | ForEach-Object Name) -join ", ") }
Set-Content (Join-Path $run "report.md") $report -Encoding utf8
$report | ForEach-Object { Write-Output $_ }
Write-Output "Dossier du run : $run"
exit $(if ($ok) { 0 } else { 1 })
