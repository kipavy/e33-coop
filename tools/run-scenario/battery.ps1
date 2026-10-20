# Batterie de non-régression (phases 2 à 4) : lance chaque scénario l'un après l'autre et résume.
#   battery.ps1 -Package <dossier e33coop-mod> [-Only p3_party_host,p3_party_client]
# Les scénarios *_client rejouent les enregistrements commités (tests/recordings) : ils ne sont pas réécrits.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Only = ""
)
$ErrorActionPreference = "Stop"
$all = @(
    "p2_host_vs_fakeclient", "p2_client_vs_fakehost",
    "p3_actions_host", "p3_actions_client",
    "p3_party_host", "p3_party_client",
    "p3_freeaim_host", "p3_freeaim_client",
    "p3_defeat_host", "p3_defeat_client",
    "p3_resync_client",
    "p3_three_host",
    "p4_session_host", "p4_session_client",
    "p4_pilot_host", "p4_pause_host",
    "p4_cinematic_host", "p4_cinematic_client"
)
$list = if ($Only) { $Only -split "," } else { $all }
$results = @()
foreach ($name in $list) {
    # Une erreur d'un scenario (texte sur stderr) ne doit pas arreter toute la batterie.
    $ErrorActionPreference = "Continue"
    $out = & powershell -NoProfile -File (Join-Path $PSScriptRoot "run-scenario.ps1") $name -Package $Package 2>&1
    $ErrorActionPreference = "Stop"
    $verdict = if ($out -match "PASS") { "PASS" } else { "FAIL" }
    $dir = ($out | Where-Object { $_ -match "^Dossier du run" }) -replace "^Dossier du run : ", ""
    $failed = ($out | Where-Object { $_ -match "\| NON \|" }) -join " ; "
    $results += [pscustomobject]@{ Scenario = $name; Verdict = $verdict; Failed = $failed; Run = $dir }
    Write-Output ("{0,-24} {1}  {2}" -f $name, $verdict, $failed)
}
$pass = @($results | Where-Object Verdict -eq "PASS").Count
Write-Output ""
Write-Output "Batterie : $pass / $($results.Count) verts"
exit $(if ($pass -eq $results.Count) { 0 } else { 1 })
