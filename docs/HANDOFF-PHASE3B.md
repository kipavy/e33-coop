> **Terminé le 2026-10-19 au soir** : étapes 1 à 6 faites (build 39284007631, batterie 12/12). Reste la validation en jeu de Killian.

Reprends le projet de mod coop en ligne pour Clair Obscur: Expedition 33. Phase 3 (synchro complète du combat) quasiment finie, sur la branche `phase-3` : il reste à valider le dernier commit (mécaniques propres), régénérer les enregistrements, repasser la batterie, puis me solliciter pour la validation en jeu.

Avant toute action, lis dans cet ordre :
1. ~/dev/e33-coop/docs/BRIEF.md (cahier des charges : phases, règles, limites).
2. ~/dev/e33-coop/docs/PROGRESS.md (état courant).
3. ~/dev/e33-coop/docs/DECISIONS.md, surtout **D11** (rejeu symétrique : actions, QTE, défenses, visée libre relayées par l'hôte ; état complet à chaque tour), et ~/dev/e33-coop/docs/GAME-INTERNALS.md §15 (fin : « Ajouts du 2026-10-19 après-midi »).
4. ~/dev/e33-coop/docs/VALIDATION-PHASE3.md (checklist en jeu déjà rédigée pour moi).

Où on en est :
- Batterie `tools/run-scenario/battery.ps1` : **12/12 verts** sur le build 39276875178 (avant le dernier commit) : p2 (2), p3_actions (2), p3_party (2), p3_freeaim (2), p3_defeat (2), p3_resync_client, p3_three_host.
- Dernier commit `e390be0` : la mécanique propre des héros (composants `BP_PerfectionMechanicComponent_C`, `BP_StanceMechanicComponent_C`, `BP_StainHolderComponent_C` : champs entiers/booléens/énumérations et tableaux) entre dans `StateSync`, le hash, le diff et la réparation. **Pas encore compilé ni testé** (CI lancée au moment de la passation). Ça change l'encodage de `StateSync` : tous les enregistrements `tests/recordings/*_host.jsonl` sont à régénérer.

Prochaines étapes :
1. Vérifier la CI (`gh run list -L 2`). Si erreur de compilation (probable : `CastField<FIntProperty/FByteProperty/FEnumProperty>` ou `TFieldRange` dans `native/hooks/game.cpp`, `for_each_mech_field`), corriger. UEPseudo n'est pas dispo localement : seule la CI fait foi.
2. Télécharger : `id=<run>; gh run download $id -D .tools/artifacts/$id; cp .tools/artifacts/$id/e33coop-tools/*.exe .tools/bin/; echo $id > .tools/last_build`.
3. Régénérer chaque enregistrement avec `.tools/pair.sh <base>` (lance `<base>_host`, copie son `recording-host.jsonl` dans `tests/recordings/<base>_host.jsonl`, puis lance `<base>_client`) pour : p3_actions, p3_party, p3_freeaim, p3_defeat. Pour p2 : lancer `p2_host_vs_fakeclient`, copier `runs/<run>/recording-host.jsonl` vers `tests/recordings/p2_host_session.jsonl`, puis `p2_client_vs_fakehost` et `p3_resync_client`. Les scripts `.tools/run.sh <scénario>` / `.tools/pair.sh` utilisent `.tools/last_build`. Lance-les en arrière-plan (un run ≈ 2-4 min, le jeu est exclusif : un seul run à la fois).
4. Si un champ de mécanique bouge tout seul (faux écarts `raw_hash_diffs` dans les logs `coop_resync`, champ « mech »), l'exclure par nom dans `for_each_mech_field`.
5. Repasser toute la batterie (`powershell -NoProfile -File tools/run-scenario/battery.ps1 -Package .tools/artifacts/<id>/e33coop-mod`, ~40 min, en arrière-plan) : elle doit rester 12/12.
6. Mettre à jour le chemin du paquet dans `docs/VALIDATION-PHASE3.md`, PROGRESS.md, committer, puis me donner la checklist de validation (≈ 15 min) avec les commandes exactes.

Pièges connus (en plus de ceux de PROGRESS/GAME-INTERNALS) :
- N'écris jamais de texte accentué dans un .ps1 via bash puis exécuté par PowerShell 5 (lu en ANSI → texte corrompu) ; pour éditer du C++ accentué, outil Edit.
- Pas de `Start-Sleep`/`sleep` en tête de commande (bloqué) ; pour attendre un run en arrière-plan : `powershell -NoProfile -Command "Start-Sleep N"` puis lire la sortie.
- Faux client : il n'applique aucune règle du jeu (objets épuisés, PA) ; les scripts client et faux client doivent produire exactement les mêmes actions (le faux hôte compte les `c2h_mismatch`).
- Les héros morts sont dans `DeadPlayerCharacters` ; mort = `Dead?` ou 0 PV.
- Fin de combat sans victoire : jamais d'attente bloquante dans le flux de fin (clôture dans le tick).
- Exploration en jeu : `tools/dev/explore-battle.ps1 -Party "Verso,Lune" -Hooks hook_freeaim.lua` après avoir lancé le jeu avec E33Explore (`tools/reset-env/reset-env.ps1 -Package ...` puis `steam -applaunch 1903340 -windowed -nosound -e33run=.../runs/explore`), commandes via `tools/dev/remote.ps1 -File x.lua`. Fermer le jeu avant tout test.

Limites connues à noter dans PROGRESS (phase 4/5) : une seule visée libre par tour rejouée (une 2e écrase la 1re) ; attaques et contres gradient non testés en automatique (à voir pendant ma validation) ; mécaniques de Sciel/Monoco non vues (pas dans l'équipe de test) ; interface (indicateur « tour de X ») absente.

Sollicite-moi seulement pour la validation de fin de phase 3, mes comptes, ou une impasse réelle.
