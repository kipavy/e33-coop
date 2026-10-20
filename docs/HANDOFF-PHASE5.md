Reprends le projet de mod coop en ligne pour Clair Obscur: Expedition 33. Les phases 0 à 4 sont finies. Killian a accepté la phase 4 (session et exploration) le 2026-10-20, et elle est fusionnée dans `main`. Attaque maintenant la **phase 5 (robustesse)**, puis enchaîne sur la **phase 6 (transport Steam)** dans la même reprise. Une seule validation, à deux PC avec Killian et son ami, viendra à la fin des deux.

Avant toute action, lis dans cet ordre :
1. `~/dev/e33-coop/docs/BRIEF.md` : le cahier des charges complet. Les phases, les décisions déjà prises, les règles et les limites s'appliquent toutes. Relis en particulier « Déconnexions », « Version », « Robustesse réseau », « Builds » et « Transport ».
2. `~/dev/e33-coop/docs/PROGRESS.md` : l'état courant, les limites connues (dont celles de la phase 4) et l'historique.
3. `~/dev/e33-coop/docs/DECISIONS.md`, surtout D10 (tirages dérivés d'une graine de l'hôte), D11 (rejeu symétrique du combat) et D12 (session et exploration : instantané de l'hôte, pilote, interactions rejouées, barrière narrative).
4. `~/dev/e33-coop/docs/GAME-INTERNALS.md` : §10 (sauvegardes), §11 (transitions, cinématiques, pause), §13 (entrées), §15 (vérifié en jeu, avec les pièges) et §16 (session et exploration, vérifié en jeu en phase 4).

## Où on en est
- `main` contient tout jusqu'à la fin de la phase 4. Crée une branche `phase-5` depuis `main`, puis `phase-6` depuis `phase-5` une fois la 5 finie.
- **Protocole v5** (`native/protocol/messages.hpp`) : combat (v4) + session et exploration : `SaveOffer` / `SaveChunk` (instantané de l'hôte par morceaux de 16 Ko), `JoinWait`, `JoinReady`, `PilotState` (~20/s, non fiable), `PilotRequest` / `PilotChange`, `WorldInteract`, `DialogueChoice`, `MapChange`, `NarrativeState`, `BarrierRelease` (réservé, inutilisé), `PauseState`. `Hello.join` et les champs « ennemi du monde » de `BattleStart` sont des extensions optionnelles en fin de message, pour que les enregistrements v4 restent lisibles.
- **Module d'exploration** : `native/sync/explore.cpp` (rejoindre, pilote et suiveurs, interactions, choix, transitions, barrière, pause, déplacement coupé chez le suiveur). Le combat reste dans `native/sync/coop.cpp`.
- **Rejoindre** : `-e33join=1` côté client. Au premier point sûr, l'hôte écrit `SaveGames\e33coop\snapshot.sav` et l'envoie. Le client l'écrit dans `SaveGames\e33coop\host.sav` et le charge, autosave coupée. Les saves solo ne sont jamais ouvertes en écriture.
- **Test runner** : étapes d'exploration (`explore` : `wait`, `wait_world`, `wait_join`, `move`, `interact`, `choose`, `wait_dialogue_end`, `change_map`, `world_battle`, `battle`, `zone_action`, `skip_cinematic`, `wait_follow`, `request_pilot`, `wait_key`, `end`), options `join`, `pause_at_turn`, `end_on_battle`.
- **Faux pair** : faux client `session` / `join` / `take_pilot` / `pause_in_battle` / `narrative_echo` avec attentes `expect`. Le faux hôte renvoie l'instantané depuis `--snapshot` et peut rejouer au rythme enregistré (`--paced 1`).
- **Batterie** (`tools/run-scenario/battery.ps1`) : 18 scénarios (p2 ×2, p3 ×10, p4 ×6). Elle n'a **pas été repassée en entier** depuis la phase 4 (à la demande de Killian). Les p4 sont verts un par un sur le build 39442301723. `p4_cinematic_client` n'a pas été revérifié depuis le passage du délai de barrière à 10 min.
- **Démarrage** : le mod reste inerte pendant 10 s après son initialisation (sinon le jeu gèle sur l'écran d'avertissement). Ne réintroduis aucune activité du mod dans ces premières secondes.

## À faire en premier
- Repasse la batterie complète **une fois** au début (environ 1 h 30, sans Killian), pour avoir une base verte avant la phase 5. Corrige ce qui est rouge avant d'aller plus loin.
- Ensuite, ne lance que les scénarios touchés par chaque changement. Repasse la batterie complète une seule fois avant la validation de fin.

## Phase 5 (critère du brief) : robustesse
- **Déconnexion d'un client** : l'hôte reprend ses héros (combat en cours compris) et le client peut revenir en cours de partie, par le même chemin que « rejoindre » (instantané au prochain point sûr ; écran d'attente s'il revient pendant un combat). Aujourd'hui, seul `Bye` fait quitter un pair, et `Link::timed_out` n'est pas exploité côté hôte.
- **Déconnexion de l'hôte** : la session se termine proprement pour tout le monde, avec un message clair (pas de migration d'hôte). Le client reçoit déjà un `Bye` à la fin d'un test, mais ne le traite pas.
- **Délais d'attente partout** : défenses et QTE (déjà 3 s), commandes d'action d'un héros distant (aujourd'hui seulement journalisées après 30 s), barrière narrative (10 min), instantané, rejoindre.
- **Resynchro complète** : combat (déjà par `StateSync`) et exploration. Il n'y a pas de hash d'état d'exploration : or, inventaire, drapeaux, `InteractedObjects`. Une désync d'exploration se répare au besoin par un nouvel instantané au point sûr.
- **Refus de version incompatible** : déjà fait au handshake (build du jeu, version du mod, liste des mods de gameplay, message précis). Ajoute un scénario qui le vérifie s'il n'existe pas encore.
- **Menus de montée de niveau et d'équipement pour les joueurs distants** : chaque joueur gère le niveau, les attributs, les compétences et l'équipement des personnages qu'il contrôle ; les changements passent par l'hôte, qui les valide et les diffuse. C'est le plus gros morceau de la phase 5 : cartographie d'abord les menus et les fonctions de `CharactersCollection` / `S_jRPG_CharacterSaveState` en jeu (Lua `E33Explore`).
- **Limites héritées de la phase 4 à régler** :
  - **Démarrage de combat coordonné** : une cinématique qui enchaîne sur un combat (boss) lance ce combat malgré la pause de la barrière. Personne ne doit commencer un combat avant les autres.
  - **Suiveur** : ses interactions et choix de dialogue locaux ne sont pas bloqués (`local_actions_on_follower`).
  - **Indicateurs** : « tour de X » et « pause demandée par X » (aucune interface aujourd'hui), avec textes en français et en anglais via un système de traduction simple.
  - **Option** : minuteur de tour, désactivé par défaut.

Toute fonctionnalité arrive avec un scénario (faux pair ou enregistrement) qui la vérifie. Le faux pair peut simuler une déconnexion : arrêt brutal, silence, reconnexion.

## Phase 6 (critère du brief) : transport Steam
- Steam Networking Sockets (P2P + relais) derrière l'interface existante `net::Transport` (`native/net/transport.hpp`, UDP aujourd'hui dans `udp.cpp`). Les invitations passent par les lobbies Steam : l'hôte ouvre sa partie au coop, le client rejoint depuis le menu principal ou en acceptant l'invitation.
- **Contrainte** : on ne peut pas lancer deux instances sur le même compte Steam. Un vrai test P2P demande deux comptes sur deux PC (Killian et son ami). Écris et compile tout, teste seul ce qui peut l'être (UDP inchangé, sélection du transport, sérialisation, éventuellement deux processus hors jeu), puis prépare la validation à deux PC. Ne contourne jamais Steam ni sa protection (règle du brief).
- Tout ce qui touche au compte Steam de Killian (clés, réglages, invitations) passe par lui.

## Environnement
- Repo GitHub public `kipavy/e33-coop`, CI GitHub Actions gratuite (environ 2 min 30 par build). On ne compile jamais en local : pas de toolchain, pas de Python, rien à installer sur le PC.
- **Récupérer un build** : `.tools/fetch.sh` attend le build CI de la branche courante, le télécharge dans `.tools/artifacts/<id>`, tue les `fakepeer.exe` restants, copie les outils dans `.tools/bin` et écrit `.tools/last_build`.
- **Tests** :
  - un scénario avec le dernier build : `.tools/run.sh <nom>` (n'affiche que le verdict et les échecs) ;
  - plusieurs : `tools/run-scenario/battery.ps1 -Package .tools/artifacts/<id>/e33coop-mod -Only "a,b"` (la liste **entre guillemets** en PowerShell) ;
  - régénérer un enregistrement de combat : `.tools/pair.sh <base>` (hôte puis client) ;
  - régénérer un enregistrement d'exploration et son instantané : `.tools/pair4.sh <base>` (copie aussi `snapshot.sav` dans `.tools/testsaves/<base>_snapshot.sav`).
- **Instantanés de test** : `.tools/testsaves/p4_session_snapshot.sav` et `p4_cinematic_snapshot.sav` sont locaux, **jamais versionnés** (données des saves de Killian, repo public). L'enregistreur n'écrit jamais les octets d'une save (`SaveChunk` ignorés). S'ils manquent, `pair4.sh` les régénère.
- **Exploration en jeu** : `tools/reset-env/reset-env.ps1 -Package ... -LuaMods E33Explore`, puis lancement Steam avec `-e33run=<repo>\runs\explore`, puis `tools/dev/remote.ps1 -File x.lua` ou `-Code "..."`. N'envoie aucune commande Lua avant le menu principal : attends la ligne `main_menu_created` dans `runs/explore/explore.jsonl`. Snippets utiles dans `lua/E33Explore/snippets/` : `hook_world.lua` (journal des transitions, cinématiques, dialogues, combats), `world_info.lua`, `snapshot_save.lua`, `load_snap.lua`, `load_test_a.lua`, `load_test_b.lua`, `list_zones.lua`, `world_battle.lua`.
- **Entrées réelles simulées** : `tools/dev/focus-game.ps1`, puis `sendkey.ps1` / `sendmouse.ps1` (AZERTY). Pas de manette virtuelle.
- **Saves** : `reset-env` copie les saves perso puis les met en lecture seule, et vide `SaveGames\e33coop\`. Ne touche jamais en écriture au dossier `SaveGames\76561198355797267`. Le backup est dans `~/dev/e33-coop-backups`. Pour rendre le jeu à Killian : `tools/reset-env/vanilla.ps1`.
- **Saves de test** : TEST_B (Vallons fleuris, NG+ de fin de partie, Verso seul dans l'équipe, autres héros disponibles via `OverrideParty`) et TEST_A (Lumière acte 3 ; zone `…_1914046506` : cinématique du boss Renoir).

## Pièges appris en phase 4
- **Édition de fichiers** : un `perl -0pi` dont le motif ne correspond pas peut corrompre le haut d'un fichier (vu sur `coop.cpp`). Vérifie le diff après chaque substitution, ou utilise l'outil Edit. Les heredocs bash contenant `'` dans une commande complexe échouent : écris d'abord le fichier avec Write, puis concatène.
- `CurrentChoices` du système de dialogue reste vide pendant une question : suis `StartChoice` / `OnChoiceMade`.
- **Fuite d'un combat** : pas de `StartBattleEndFlow`, seulement `ResumeExplorationOnBattleEnd`. L'hôte peut commencer un tour de plus que le client.
- `OnSelectSkip` (passer le tour) ne fait rien quand le héros est seul dans l'équipe ; un scénario qui ne fait que passer bloque le combat.
- Une cinématique jamais vue ne se passe pas (`SkipCinematic` sans effet), et `SetGamePaused` n'empêche pas le combat qui suit une cinématique de démarrer.
- Le nombre de coups d'une compétence (Combo1 : 9 ou 10) varie d'un run à l'autre, même chez l'hôte seul. Un tirage en plus ou en moins en fin de flux est compté à part (`draw_count_diffs`).
- `ue::get_object(controller, "BP_CinematicSystem")` est bien le système de cinématique (un seul objet de cette classe).
- **Pièges des phases précédentes, toujours valables** :
  - jamais de `ProcessEvent` dans un hook très fréquent ;
  - ne hooke jamais `KismetArrayLibrary:Array_Random` ;
  - pas de texte accentué dans un `.ps1` écrit via bash ;
  - pas de `Start-Sleep` en tête de commande ;
  - seul `RemoveMappingContext` coupe vraiment une touche.

## Règles
- Tiens `PROGRESS.md` à jour. Fais des petits commits sur la branche de la phase.
- Ne déclare rien « fait » sans run vert.
- Ne m'affirme rien que tu n'as pas mesuré.
- Sollicite Killian seulement :
  - pour la validation de fin (phases 5 et 6 ensemble, à deux PC avec son ami : checklist courte avec les commandes exactes pour chacun des deux) ;
  - pour tout ce qui touche à ses comptes (Steam, GitHub) ;
  - pour une save avec une autre équipe ;
  - pour une impasse réelle.
- Killian préfère éviter les batteries complètes quand un sous-ensemble de scénarios suffit.
