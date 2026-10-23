# Publier une version

Une release GitHub de `kipavy/e33-coop` contient trois fichiers :

| Fichier | Rôle |
|---|---|
| `Installer.E33.Coop.exe` (libellé « Installer E33 Coop.exe ») | ce que les joueurs téléchargent ; installe, met à jour, règle les options, désinstalle |
| `e33coop-player.zip` | paquet joueur : UE4SS épinglé + mod + `mods.txt` + installateur + `install.ps1` / `uninstall.ps1` |
| `SHA256SUMS` | empreintes SHA-256 des deux fichiers ci-dessus (format `sha256sum`) |

Notes de chaque release : section de la version dans `CHANGELOG.md` (source unique).

L'installateur et le mod n'installent **que** le zip de la dernière release, téléchargé en HTTPS depuis `github.com` (et
le stockage de GitHub vers lequel il redirige), avec la même empreinte que dans `SHA256SUMS`.

## Version : une seule source

Le fichier `VERSION` (racine du dépôt) donne la version du mod, par exemple `0.6.0`. CMake le lit pour le mod
(`kModVersion`, vérifiée au Hello : deux versions différentes ne jouent pas ensemble), pour l'installateur (ressource
de version) et pour le faux pair des tests (`"mod_version": "current"`). Le workflow de release refuse un tag qui ne
correspond pas à `VERSION`.

## Procédure

1. Vérifier que la branche à publier est verte (`build`) et que la batterie est passée.
2. **Notes de version** : dans `CHANGELOG.md` (source unique, format Keep a Changelog), renommer la section
   `## [Non publié]` en `## [X.Y.Z] - AAAA-MM-JJ`, ouvrir une nouvelle section `## [Non publié]` vide au-dessus, et
   mettre à jour les liens de comparaison en bas du fichier. Vérifier le rendu :
   `powershell -File tools/release/release-notes.ps1 -Version X.Y.Z`. Chaque changement visible par les joueurs est
   ajouté sous `[Non publié]` au moment où il est fusionné (rubriques Ajouts / Modifications / Corrections /
   Suppressions / Sécurité, une ligne écrite pour un joueur).
3. Mettre le nouveau numéro dans `VERSION` (même format `X.Y.Z`), committer et pousser.
4. Optionnel, essai à blanc : *Actions › release › Run workflow* sur la branche (case « essai à blanc » cochée).
   Le workflow compile, fait le paquet et publie seulement l'artefact `release-dry-run` (zip, installateur,
   `SHA256SUMS`, notes). Rien n'est publié.
5. Poser et pousser le tag :
   ```bash
   git tag v0.6.0
   git push origin v0.6.0
   ```
6. Le workflow `release` (`.github/workflows/release.yml`) :
   - vérifie que le tag vaut `v` + `VERSION` et que `CHANGELOG.md` a une section non vide pour cette version ;
   - compile le mod (`Game__Shipping__Win64`) et les outils (`Release`) par `build.yml`, tests unitaires compris ;
   - télécharge UE4SS épinglé (release `experimental-latest` d'UE4SS, fichier `UE4SS_v3.0.1-1181-g83330635.zip`,
     empreinte vérifiée) ;
   - fait le paquet (`tools/package/make-player-package.ps1`), calcule `SHA256SUMS` ;
   - crée la release `vX.Y.Z` (marquée « latest ») avec les trois fichiers ; notes = section du CHANGELOG + mode
     d'installation (`tools/release/release-notes.ps1`).
7. Suivre : `gh run list -w release -L1`, puis `gh run watch <id> --exit-status`.

Dès la publication, les mods installés (option `updates = auto`, par défaut) téléchargent la nouvelle version au
menu principal et l'installent à la fermeture du jeu.

## En cas de problème

- **Mauvaise release** : la supprimer (`gh release delete vX.Y.Z --cleanup-tag`) ou en publier une plus récente. Une
  release supprimée n'est plus proposée ; les joueurs déjà à jour gardent leur version (pas de retour arrière
  automatique : réinstaller l'ancienne en posant son zip et son `SHA256SUMS` à côté de l'installateur).
- **UE4SS** : si le fichier épinglé disparaît ou change d'empreinte, la release échoue (aucun paquet différent n'est
  livré). Mettre à jour `UE4SS_URL` / `UE4SS_SHA256` dans `release.yml` avec le même commit que
  `native/third_party/RE-UE4SS`.
- **SmartScreen** : l'exe n'est pas signé. Le premier lancement peut afficher « Windows a protégé votre ordinateur »
  (*Informations complémentaires* › *Exécuter quand même*). L'exe porte une icône et des informations de version.
