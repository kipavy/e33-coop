Reprends le projet de mod coop en ligne pour Clair Obscur: Expedition 33. Les phases 5 (robustesse) et 6 (transport Steam) sont **écrites et vertes seules** ; il reste à finir la batterie complète, préparer le paquet de validation et solliciter Killian pour la validation à deux PC (lui + son ami).

Lis d'abord : `docs/PROGRESS.md` (état courant), `docs/DECISIONS.md` D13 (builds, ressources communes, option 1 choisie par Killian) et D14 (robustesse), `docs/GAME-INTERNALS.md` §17, `docs/VALIDATION-PHASE5-6.md` (checklist à deux PC, à compléter).

## Où on en est (2026-10-20, soir)
- Branches : `phase-5` (fini) puis `phase-6` (en cours, contient tout). Rien n'est fusionné dans `main` (après validation de Killian).
- Batterie finale lancée en arrière-plan sur le build **39472366662** (32 scénarios, ≈ 2 h) ; sortie dans le scratchpad de la session précédente (`battery_final.txt`), sinon relire `runs/` (un dossier par scénario, `report.md`). Si elle a été interrompue : `powershell -NoProfile -File tools/run-scenario/battery.ps1 -Package .tools/artifacts/<id>/e33coop-mod`.
- Dernier commit natif après ce build : journaux des parties normales dans `ue4ss/Mods/E33Coop/logs/<date-heure>/` (un dossier par lancement). Il faut donc un nouveau build (`.tools/fetch.sh`, **jamais pendant une batterie** : il tue les faux pairs) pour le paquet de validation.

## À faire
1. Lire le résultat de la batterie ; corriger les rouges (relancer seulement les scénarios touchés). Rouges déjà vus et expliqués une fois : pause du jeu sur perte de focus (le runner referme maintenant le menu pause).
2. `.tools/fetch.sh` (nouveau build), puis `powershell -NoProfile -File tools/package/make-player-package.ps1 -Package .tools/artifacts/<id>/e33coop-mod` → `.tools/e33coop-player.zip`.
3. Compléter `docs/VALIDATION-PHASE5-6.md` (remplacer BUILD_ID et BATTERY_RESULT), mettre à jour `PROGRESS.md`, committer, puis présenter la checklist à Killian (validation à deux PC, avec son ami ; tout ce qui touche à Steam passe par lui).
4. Après sa validation : fusionner `phase-6` dans `main`.

## Rappels
- Tests : `.tools/run.sh <nom>` ; plusieurs : `battery.ps1 -Only "a,b"` ; paires : `.tools/pair4.sh <base>` (exploration + instantané), `.tools/pair5b.sh` (builds : copie aussi `build-*.sav`). Les instantanés et états de build de test restent locaux (`.tools/testsaves`, jamais versionnés).
- Pièges : heredoc bash avec apostrophes → écrire avec Write ; `check` est une macro UE ; pas de `perl` sur des motifs incertains sans vérifier le diff ; ne rien activer dans les 10 premières secondes du mod.
- Steam : jamais de contournement ; lobby et invitations = compte de Killian, à tester seulement à la validation.
