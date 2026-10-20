Reprends le projet de mod coop en ligne pour Clair Obscur: Expedition 33. Les phases 0 à 3 sont finies. J'ai validé la phase 3 (synchro complète du combat) en jeu le 2026-10-20, et elle est fusionnée dans `main`. Attaque maintenant la phase 4.

Avant toute action, lis dans cet ordre :
1. `~/dev/e33-coop/docs/BRIEF.md` : le cahier des charges complet. Les phases 0 à 6, les décisions déjà prises, les règles et les limites s'appliquent toutes.
2. `~/dev/e33-coop/docs/PROGRESS.md` : l'état courant, les limites connues et l'historique.
3. `~/dev/e33-coop/docs/DECISIONS.md`, surtout D10 (tirages dérivés d'une graine de l'hôte) et D11 (rejeu symétrique : chaque instance joue tous les héros, l'hôte relaie actions, QTE, défenses et visée libre, puis envoie l'état complet à chaque tour).
4. `~/dev/e33-coop/docs/GAME-INTERNALS.md` : §11 (chargement de save, transitions, écrans de chargement), §12 (cinématiques), §13 (entrées : contextes Enhanced Input, priorités, ce qui les retire vraiment) et §15 (vérifié en jeu, avec les pièges).

## Où on en est
- `main` contient tout jusqu'à la fin de la phase 3. Crée une branche `phase-4` depuis `main`.
- **Combat synchronisé (phase 3)** : protocole v4 (actions, cibles, compétences, objets, résultats de QTE avec l'instant de l'appui, défenses par héros, visée libre, `StateSync` avec hash, diff et réparation, récompenses, fin de combat). Équipe imposée via `OverrideParty`. Pendant le tour d'un héros d'un autre joueur, le mod retire les contextes d'entrée de combat (`IMC_Battle_PlayerTurn` / `MainWheel` / `Targeting` / `QTE` / `FreeAim`) : le joueur local n'a plus aucun contrôle.
- **Session aujourd'hui** : chaque instance charge elle-même la save de test (TEST_B), puis le client lance le combat de l'hôte (`BattleStart`) une fois son monde prêt. Il n'y a encore ni transfert de save, ni exploration synchronisée.
- **Démarrage** : le mod reste inerte pendant 10 s après son initialisation. Sans ce délai, le jeu gelait sur l'écran d'avertissement (3 lancements sur 8 en client). Ne réintroduis aucune activité du mod (hooks, réseau, `FindFirstOf`) dans ces premières secondes.

## À faire en premier
- `p3_actions_client` n'a pas été revérifié depuis le correctif du lanceur (commit 5303a9c : appui « parfait » à 85 % au lieu de 88 %). Relance ce scénario seul ; s'il est vert, la batterie est considérée verte.
- Ne repasse **pas** la batterie complète (12 scénarios, environ 40 min) à chaque changement : lance les scénarios touchés par le changement, et la batterie complète une seule fois avant chaque validation de fin de phase.

## Phase 4 (critère du brief)
Session et exploration :
- rejoindre une partie (transfert de la save de l'hôte vers un emplacement coop chez le client, sans jamais toucher aux saves perso) ;
- synchro du pilote (le joueur qui se déplace) et prise de main ;
- transitions de niveau ;
- cinématiques avec barrière (tout le monde attend tout le monde) ;
- entrée en combat depuis l'exploration (aujourd'hui, le combat est déclenché par le test runner) ;
- pause.

Toute fonctionnalité de synchro arrive avec un scénario (fakepeer ou enregistrement) qui la vérifie. Les scénarios p2 et p3 doivent rester verts.

## Environnement
- Repo GitHub public `kipavy/e33-coop`, CI GitHub Actions gratuite (environ 2 min 30 par build). On ne compile jamais en local : pas de toolchain, pas de Python, rien à installer sur le PC.
- Récupération d'un build :
  - `gh run download <id> -D .tools/artifacts/<id>` ;
  - copie de `e33coop-tools/*.exe` dans `.tools/bin` (tue d'abord tout `fakepeer.exe` qui tourne encore, sinon la copie échoue) ;
  - `echo <id> > .tools/last_build`.
- Tests :
  - un scénario : `tools/run-scenario/run-scenario.ps1 <nom> -Package .tools/artifacts/<id>/e33coop-mod` ;
  - plusieurs scénarios : `tools/run-scenario/battery.ps1 -Package ... [-Only a,b]` ;
  - le lanceur détecte le gel au démarrage et relance tout seul (3 tentatives au plus).
- Exploration en jeu : `tools/reset-env/reset-env.ps1 -Package ... -LuaMods E33Explore`, puis `tools/dev/explore-battle.ps1` et `tools/dev/remote.ps1 -File x.lua`. N'envoie aucune commande Lua avant le menu principal (une commande trop tôt a fait geler le jeu).
- Entrées réelles simulées : `tools/dev/focus-game.ps1` (un vrai clic dans la fenêtre du jeu ; c'est le seul moyen fiable de lui donner le focus, Windows refuse `SetForegroundWindow`), puis `tools/dev/sendkey.ps1` (clavier) ou `tools/dev/sendmouse.ps1` (souris). La disposition du clavier est AZERTY. Pas de manette virtuelle (il faudrait installer un pilote) : ce qui dépend de la manette se valide avec moi.
- Saves : `reset-env` copie mes saves perso puis les met en lecture seule. Ne touche jamais en écriture au dossier `SaveGames\76561198355797267`. Le backup est dans `~/dev/e33-coop-backups`. Pour me rendre le jeu : `tools/reset-env/vanilla.ps1`.
- À la reprise, le jeu est encore moddé (UE4SS, E33Coop et E33Explore) et mes saves sont en lecture seule.

## Pièges appris en phase 3
- `SetBattleInputEnabled(false)` et `Disable Targeting and Input()` ne retirent aucun contexte d'entrée. Seul `EnhancedInputLocalPlayerSubsystem:RemoveMappingContext` coupe vraiment une touche. Le jeu remet tour, roue et ciblage à chaque début de tour d'un héros.
- Le test runner appuie sur les QTE en appelant `OnComboPressed` : il ne passe pas par le chemin d'entrée réel. Pour reproduire un problème d'entrée, simule une vraie touche.
- Les événements de fenêtre de QTE arrivent 65 à 310 ms après `CurrentWindowStart/EndRealTime`, et le jeu accepte l'appui jusqu'à l'événement de fin. Mesure toujours en temps réel du jeu (`GameplayStatics:GetRealTimeSeconds`) : `tools/dev/qte-diag.ps1` le fait.
- Le nombre de coups d'une compétence dépend de l'instant de l'appui dans la fenêtre. Un décalage d'une image peut ajouter un coup et casser un rejeu d'enregistrement.
- Jamais de `ProcessEvent` dans un hook très fréquent ; ne hooke jamais `KismetArrayLibrary:Array_Random`.
- N'écris jamais de texte accentué dans un `.ps1` via bash (PowerShell 5 lit le fichier en ANSI) : garde les `.ps1` en ASCII ou passe par l'outil Edit. Pas de `Start-Sleep` en tête de commande.

## Règles
- Tiens `PROGRESS.md` à jour. Fais des petits commits sur `phase-4`.
- Ne déclare rien « fait » sans run vert.
- Ne m'affirme rien que tu n'as pas mesuré : en phase 3, une conclusion non vérifiée (« ton appui était en retard ») m'a coûté deux validations.
- Sollicite-moi seulement :
  - pour la validation de fin de phase 4 (checklist en jeu de moins de 20 minutes, avec les commandes exactes) ;
  - pour tout ce qui touche à mes comptes ;
  - pour une save avec une autre équipe ;
  - pour une impasse réelle.
