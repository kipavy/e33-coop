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
    "p4_cinematic_host", "p4_cinematic_client",
    "p5_version_host", "p5_reject_client", "p5_protocol_client",
    "p5_dropout_host", "p5_hostbye_client", "p5_hostlost_client", "p5_turntimer_host",
    "p5_resync_host", "p5_resync_client", "p5_builds_host", "p5_builds_client", "p5_economy_host", "p5_economy_client",
    "p6_steam_selftest",
    "p6_world_host", "p6_world_client", "p6_world2_host", "p6_world2_client",
    "p6_newgame_host", "p6_newgame_client", "p6_duoparry_host", "p6_ownersui_host", "p7_intent_host", "p7_intent_client", "p7_menumirror_host", "p7_menumirror_off_host",
    "p6_coopsave_client", "p6_coopsave_host", "p6_panel_host", "p6_panelnav_host",
    "p8_sprint_client", "p8_sprint_sm_client", "p8_sprint_long_client", "p8_sprint_hardnet_client",
    "p8_cutscene_host", "p8_cutscene_sync_client", "p8_cutscene_client", "p8_dialogue_host", "p8_dialogue_sync_client",
    "p8_esquie_host", "p8_esquie_client",
    "p8_prologue_client", "p8_prologue_host", "p8_readystall_host", "p8_thinking_host", "p8_defwait_host", "p8_turnalign_host",
    "p9_join_stuck_client", "p9_battle_keys_client", "p9_battle_keys_old_client", "p3_freeaim_old_client",
    "p9_rng_host"
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
