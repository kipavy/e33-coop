# Notes d'une release, tirées de CHANGELOG.md (source unique) : section de la version + mode d'installation.
#   release-notes.ps1 -Version 0.6.5 [-Out notes.md]
# Échoue (code 1) si la section « ## [X.Y.Z] » manque ou est vide : pas de release sans notes.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Out = "",
    [string]$Changelog = ""
)
$ErrorActionPreference = "Stop"
if (-not $Changelog) { $Changelog = Join-Path $PSScriptRoot "..\..\CHANGELOG.md" }
$lines = [IO.File]::ReadAllLines((Resolve-Path $Changelog), [Text.Encoding]::UTF8)
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match ('^## \[' + [regex]::Escape($Version) + '\]')) { $start = $i; break } }
if ($start -lt 0) { Write-Error "CHANGELOG.md : section [$Version] introuvable"; exit 1 }
$body = @()
for ($i = $start + 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^## \[' -or $lines[$i] -match '^\[[^\]]+\]: ') { break }
    $body += $lines[$i]
}
$text = ($body -join "`n").Trim()
if (-not ($text -match '(?m)^- ')) { Write-Error "CHANGELOG.md : section [$Version] vide"; exit 1 }
$notes = @"
**E33 Coop $Version** : mod coop en ligne pour *Clair Obscur: Expedition 33* (mod non officiel).

$text

### Installer (chaque joueur)
1. Télécharger **Installer.E33.Coop.exe** ci-dessous et le lancer (double-clic). Windows peut afficher « Windows a protégé votre ordinateur » (exe non signé) : *Informations complémentaires* › *Exécuter quand même*.
2. Cliquer **Installer**. L'installateur trouve le jeu, télécharge le paquet de cette release, vérifie son empreinte SHA-256 et installe UE4SS + le mod.
3. Lancer le jeu par Steam, **sans option de lancement**. Inviter : charger sa partie puis **F7**. Rejoindre : au menu principal, **Rejoindre <ami>** ou **F7**, ou accepter l'invitation Steam.

Mises à jour : automatiques par défaut (installées à la fermeture du jeu). Options : **F9** en jeu.
Hors ligne : poser ``e33coop-player.zip`` et ``SHA256SUMS`` à côté de l'installateur.

Empreintes : voir ``SHA256SUMS``. Historique complet : [CHANGELOG.md](https://github.com/kipavy/e33-coop/blob/main/CHANGELOG.md).
"@
if ($Out) { [IO.File]::WriteAllText($Out, $notes + "`n", (New-Object Text.UTF8Encoding $false)) } else { $notes }
