# E33 Coop - test a distance, cote hote (PC de Killian, jeu lance avec le mod). Voir docs/REMOTE-TEST.md.
# Depose une commande dans ue4ss\Mods\E33Coop\remote\inbox\ ; le mod de l'hote l'envoie au joueur par Steam et ecrit
# la reponse (et les fichiers recus) dans remote\outbox\<id>\. Le jeu de l'hote doit tourner.
#   remote.ps1 status
#   remote.ps1 fetch-logs [-Count 2]          derniers journaux du mod, UE4SS.log, saves coop -> runs\remote\<id>\files
# Chaque réponse est recopiée du dossier outbox du mod vers runs\remote\<id>\ (le dossier du mod est effacé par reset-env).
#   remote.ps1 run <scenario.json> [-Wait]    scenario du test runner (relance du jeu de l'ami ; -Wait : attend le resultat puis rapatrie les journaux)
#   remote.ps1 menu | load-snapshot | quit | restart
#   remote.ps1 update <dossier artefact e33coop-mod | e33coop-player.zip>
#   remote.ps1 peer                            dernier « bonjour » recu du joueur (version, droits)
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("status", "fetch-logs", "run", "menu", "load-snapshot", "quit", "restart", "update", "peer")]
    [string]$Command,
    [Parameter(Position = 1)][string]$Path = "",
    [string]$Peer = "76561198045698677",
    [int]$Count = 2,
    [int]$Timeout = 0,
    [switch]$Wait,
    [string]$GameDir = ""
)
$ErrorActionPreference = "Stop"

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
if (-not $GameDir) { throw "Jeu introuvable : -GameDir ""<...>\steamapps\common\Expedition 33""" }
$mod = Join-Path $GameDir "Sandfall\Binaries\Win64\ue4ss\Mods\E33Coop"
if (-not (Test-Path $mod)) { throw "Mod E33Coop absent : $mod" }
$remote = Join-Path $mod "remote"
foreach ($d in "inbox", "outbox", "updates") { New-Item -ItemType Directory -Force (Join-Path $remote $d) | Out-Null }
$utf8 = New-Object System.Text.UTF8Encoding $false
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

function Send-Remote([string]$cmd, [hashtable]$extra = @{}, [int]$timeoutS = 240) {
    $id = (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + $cmd + "-" + (Get-Random -Maximum 100000)
    $o = @{ id = $id; peer = $Peer; cmd = $cmd }
    foreach ($k in $extra.Keys) { $o[$k] = $extra[$k] }
    $tmp = Join-Path $remote "inbox\$id.tmp"
    [IO.File]::WriteAllText($tmp, ($o | ConvertTo-Json -Depth 6 -Compress), $utf8)
    Move-Item $tmp (Join-Path $remote "inbox\$id.json")
    $dir = Join-Path $remote "outbox\$id"
    $stateFile = Join-Path $dir "state.json"
    $t0 = Get-Date
    $warned = $false
    $last = ""
    while ($true) {
        Start-Sleep -Milliseconds 500
        if (-not $warned -and -not (Test-Path $dir) -and ((Get-Date) - $t0).TotalSeconds -gt 10) {
            Write-Warning "Commande pas encore prise : le jeu de l'hote tourne-t-il avec le mod ?"
            $warned = $true
        }
        if (Test-Path $stateFile) {
            $state = $null
            try { $state = [IO.File]::ReadAllText($stateFile) | ConvertFrom-Json } catch { continue }
            if ($state.state -eq "done" -or $state.state -eq "error") {
                # Copie hors du dossier du mod (reset-env.ps1 l'efface avant chaque test local) : runs\remote\<id>.
                Start-Sleep -Milliseconds 300
                $kept = Join-Path $repo "runs\remote\$id"
                New-Item -ItemType Directory -Force (Split-Path $kept -Parent) | Out-Null
                Copy-Item $dir $kept -Recurse -Force
                $dir = $kept
                return [pscustomobject]@{ Id = $id; Dir = $dir; State = $state.state; Error = $state.error; Reply = $state.reply; Files = $state.files; Bytes = $state.bytes }
            }
            $line = "$($state.state) $($state.files) fichier(s), $($state.bytes) octets"
            if ($line -ne $last) { Write-Host "  ... $line"; $last = $line }
        }
        if (((Get-Date) - $t0).TotalSeconds -gt $timeoutS) {
            return [pscustomobject]@{ Id = $id; Dir = $dir; State = "timeout"; Error = "pas de reponse en $timeoutS s"; Reply = $null; Files = 0; Bytes = 0 }
        }
    }
}

function Show($r) {
    Write-Output ("[{0}] {1}" -f $r.State, $r.Id)
    if ($r.Error) { Write-Output ("erreur : " + $r.Error) }
    if ($r.Reply) { Write-Output ($r.Reply | ConvertTo-Json -Depth 6) }
    if ($r.Files) { Write-Output ("fichiers : " + (Join-Path $r.Dir "files")) }
}

# Fichiers d'une mise a jour (disposition du dossier Win64) : seulement les destinations permises.
function Test-Dest([string]$p) {
    if ($p.Length -gt 240 -or $p -notmatch '^[A-Za-z0-9._+() -]+(/[A-Za-z0-9._+() -]+)*$') { return $false }
    foreach ($part in $p.Split('/')) { if ($part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ')) { return $false } }
    $l = $p.ToLowerInvariant()
    if ($l -eq 'dwmapi.dll' -or $l -eq 'ue4ss/ue4ss.dll') { return $true }
    if (-not $l.StartsWith('ue4ss/mods/e33coop/')) { return $false }
    $rest = $l.Substring(19)
    return -not ($rest -eq '' -or $rest.StartsWith('remote/') -or $rest.StartsWith('logs/') -or $rest -eq 'remotetest.txt')
}

switch ($Command) {
    "peer" {
        $f = Join-Path $remote "peers\$Peer.json"
        if (Test-Path $f) { Get-Content $f -Raw } else { "Aucun bonjour recu de $Peer (il arrive apres une premiere commande)." }
    }
    "status" { Show (Send-Remote "status" @{} ($(if ($Timeout) { $Timeout } else { 60 }))) }
    "fetch-logs" { Show (Send-Remote "fetch-logs" @{ count = $Count } ($(if ($Timeout) { $Timeout } else { 900 }))) }
    { $_ -in "menu", "load-snapshot", "quit", "restart" } { Show (Send-Remote $Command @{} ($(if ($Timeout) { $Timeout } else { 60 }))) }
    "run" {
        if (-not $Path -or -not (Test-Path $Path)) { throw "Scenario introuvable : $Path" }
        $text = [IO.File]::ReadAllText((Resolve-Path $Path).Path)
        $r = Send-Remote "run" @{ scenario_text = $text } 60
        Show $r
        if (-not $Wait -or $r.State -ne "done") { break }
        $runId = $r.Reply.run_id
        if (-not $r.Reply.restarting) { Write-Output "Le jeu de l'ami doit etre relance a la main pour lancer le scenario." }
        $limit = (Get-Date).AddSeconds($(if ($Timeout) { $Timeout } else { 2400 }))
        $finished = $false
        Write-Output "Attente du resultat de $runId ..."
        while ((Get-Date) -lt $limit) {
            Start-Sleep -Seconds 20
            $s = Send-Remote "status" @{} 30
            if ($s.State -ne "done") { continue }   # jeu de l'ami en cours de relance
            $lr = $s.Reply.last_run
            if ($lr -and $lr.id -eq $runId -and -not $lr.running -and $lr.status -ne "none") {
                Write-Output ("resultat : {0} ({1})" -f $lr.status, $lr.reason)
                $finished = $true
                break
            }
        }
        if (-not $finished) { Write-Output "Pas de resultat dans le delai." }
        Show (Send-Remote "fetch-logs" @{ count = 2 } 900)
    }
    "update" {
        if (-not $Path -or -not (Test-Path $Path)) { throw "Paquet introuvable : $Path" }
        $id = (Get-Date -Format "yyyyMMdd-HHmmss") + "-update"
        $dst = Join-Path $remote "updates\$id"
        $src = (Resolve-Path $Path).Path
        $tmp = $null
        if ((Get-Item $src).PSIsContainer -eq $false) {
            $tmp = Join-Path $env:TEMP ("e33coop-upd-" + [guid]::NewGuid().ToString("N"))
            Expand-Archive $src $tmp
            $src = $tmp
        }
        if (Test-Path (Join-Path $src "game")) { $src = Join-Path $src "game" }   # paquet joueur : game\dwmapi.dll, game\ue4ss\...
        $n = 0
        foreach ($f in Get-ChildItem $src -Recurse -File) {
            $rel = $f.FullName.Substring($src.Length + 1).Replace('\', '/')
            if (-not (Test-Dest $rel)) { Write-Verbose "ignore : $rel"; continue }
            $to = Join-Path $dst ($rel.Replace('/', '\'))
            New-Item -ItemType Directory -Force (Split-Path $to -Parent) | Out-Null
            Copy-Item $f.FullName $to
            $n++
        }
        if ($tmp) { Remove-Item $tmp -Recurse -Force }
        if ($n -eq 0) { throw "Aucun fichier du mod dans $Path" }
        Write-Output "$n fichier(s) a envoyer"
        $r = Send-Remote "update" @{ update_id = $id } ($(if ($Timeout) { $Timeout } else { 1800 }))
        Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue
        Show $r
        if ($r.State -eq "done" -and $r.Reply.applied) { Write-Output "Mise a jour installee : le jeu de l'ami redemarre (retour arriere automatique si le mod ne demarre pas en 3 min). Verifier ensuite : remote.ps1 status" }
    }
}
