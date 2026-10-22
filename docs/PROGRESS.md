# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-21, nuit) — phases 5-6 : batterie 36/36 verte ; prochain essai à deux PC avec le paquet du build 39515111132

| Phase | État | Branche |
|---|---|---|
| 0 — exploration | **finie** | `phase-0` (fusionnée dans `main`) |
| 1 — environnement de test autonome | **finie** | `phase-1` (fusionnée dans `main`) |
| 2 — preuve de concept réseau | **finie** (validée en jeu le 2026-10-19) | `phase-2` (fusionnée dans `main`) |
| 3 — synchro complète du combat | **finie** (validée en jeu le 2026-10-20) | `phase-3` (fusionnée dans `main`) |
| 4 — session et exploration | **finie** (acceptée le 2026-10-20) | `phase-4` (fusionnée dans `main`) |
| 5 — robustesse | **finie** (scénarios verts ; validation à deux PC avec la phase 6) | `phase-5` |
| 6 — transport Steam | **en cours** | `phase-6` |

### Phase 5 : fait (écrit, compilé ; vert en jeu là où c'est indiqué)
- **Base** : batterie complète repassée sur le build de fin de phase 4 : 15/18. Rouges expliqués : `p3_party_client` (le jeu s'est mis en pause, perte de focus probable → le runner referme désormais tout menu pause non demandé), `p4_cinematic_host` (limite connue : le combat de boss démarre malgré la barrière), `p4_cinematic_client` (le faux hôte attendait l'écho « cinématique commencée » que le vrai client n'envoie pas → messages informatifs non bloquants).
- **Protocole v6** : époque par lien (redémarrage d'un pair détecté), battement « occupé » envoyé par un thread à part pendant les chargements (le thread de jeu bloque 30 à 60 s), `Owners`, `BattleReady`, `Roster`, `WorldHash`, `ResyncRequest`, minuteur de tour (`BattleStart.turn_timer_s`, `ActionCommand.forced`). Paquet d'une autre version de protocole : réponse avec la nôtre, refus clair des deux côtés. Tests unitaires du lien (redémarrage, occupé, version).
- **Départs** : silence de 15 s (ou 300 s si le pair n'envoie que « occupé »), `Bye`, ou redémarrage → l'hôte retire le joueur, reprend ses héros (combat en cours compris, tour en cours joué par l'hôte), prévient les autres (`Owners`, `Roster`). Retour par le chemin « rejoindre » (`JoinWait` pendant un combat, instantané après ; instantané refait si un combat a commencé pendant le chargement).
- **Fin de session côté client** : hôte parti (`Bye`), injoignable (90 s sans réponse), perdu (15 s) ou refus → message traduit, entrées et pause rendues, retour au menu principal. Retour au menu par un joueur (menu pause) = départ propre.
- **Démarrage de combat coordonné** : l'hôte tient son premier tour en pause jusqu'au `BattleReady` de chaque client de la partie (10 min max). Pause partagée par raisons (barrière, pause d'un autre joueur, démarrage) et réappliquée si le jeu l'enlève (fin de cinématique).
- **Minuteur de tour** (`-e33turntimer=N`, désactivé par défaut) : attaque automatique imposée et diffusée par l'hôte.
- **Textes FR/EN** (`native/ui`, `-e33lang=fr|en`, sinon langue de Windows) : « tour de X », « pause demandée par X », attente des joueurs, départs, fin de session. **Journalisés seulement : l'affichage à l'écran reste à brancher.**
- **Hash d'état d'exploration** (or, inventaire, objets utilisés, ennemis vaincus, drapeaux, GPE, cinématiques et dialogues vus) : lecture générique par réflexion (`native/hooks/reflect`), envoyé par l'hôte au calme, comparé par le client au calme ; deux écarts de suite → nouvel instantané (`ResyncRequest`).
- **Décisions** : D13 (builds et ressources communes : chacun gère ses personnages, tout le monde peut dépenser l'inventaire commun, l'hôte arbitre ; option 1 choisie par Killian). Idée « exploration libre » notée, reportée après la phase 6.

### Phase 5 : scénarios (build 39453697725, premiers passages)
- Verts : `p2_host_vs_fakeclient`, `p2_client_vs_fakehost`, `p5_turntimer_host` (démarrage coordonné tenu 6 s, minuteur, indicateurs).
- `p5_dropout_host` : départ par silence et par `Bye` détectés, héros repris (y compris en plein tour), retour avec écran d'attente ; échec final car l'écran de victoire restait ouvert (runner corrigé).
- `p5_hostbye_client` / `p5_hostlost_client` : fin de session, message et retour au menu OK ; échecs dus aux vérifications de combat par défaut (corrigé).
- `p5_version_host` : les 3 refus Hello OK (message précis) ; le client « vieux protocole » sondait trop peu longtemps (corrigé). `p5_reject_client` / `p5_protocol_client` : scénario invalide (corrigé).
- Relance en cours sur le build 39457856628.

### Phase 5 : fait ensuite (tous verts, build 39470160044)
- Affichage des textes : cartes « toast » du jeu (une persistante pour « tour de X », « pause demandée par X », attente ; une par message), vérifié sur capture en combat.
- Builds par état (D13, D14) : `p5_builds_host` / `p5_builds_client` (état de Verso appliqué chez le client avec la même empreinte ; proposition du client pour son personnage acceptée, pour celui de l'hôte refusée et annulée ; réattribution F6 diffusée).
- Ressources communes : `p5_economy_host` / `p5_economy_client` (dépense de l'hôte appliquée chez le client, dépense du client acceptée, dépense impossible refusée -> resynchro).
- Resynchro d'exploration : `p5_resync_host` / `p5_resync_client` (or faussé, deux écarts, nouvel instantané, empreinte identique ensuite).
- Suiveur : interactions et réponses au clavier / à la manette bloquées (`follower_actions_blocked` dans `p4_session_client`) ; un clic de souris sur une réponse reste possible.
- Cinématique -> combat de boss : la pause réappliquée tient le combat jusqu'à la fin de la cinématique de l'autre joueur (`p4_cinematic_host`).
- Minuteur de tour : ne compte que le temps de jeu (pas l'attente du démarrage coordonné).
- Batterie : 31 scénarios (`battery.ps1`), à repasser en entier avant la validation commune.

### Phase 6 : transport Steam (branche `phase-6`)
- `native/net/steam.cpp` : Steam Networking Messages (P2P + relais SDR) derrière `net::Transport`, par l'API « flat » du `steam_api64.dll` du jeu (Steamworks 1.57, résolue à l'exécution : pas de SDK, pas d'initialisation de Steam par le mod). Adresse `steam:<SteamID64>` ; le lien fiable reste le même qu'en UDP. `-e33transport=steam` ; UDP inchangé par défaut.
- Lobbies : l'hôte crée un lobby « amis seulement » (3 places) au premier tick ; **F7** ouvre la fenêtre d'invitation de l'overlay ; les membres du lobby sont « ouverts » (paquet `E33P` + acceptation de session). Client : `-e33lobby=<id>`, `+connect_lobby <id>` (invitation acceptée jeu fermé), invitation acceptée jeu ouvert (callback `GameLobbyJoinRequested_t`, même déclaration de `CCallbackBase` que le SDK), ou **F7** au menu principal (lobby coop d'un ami). Nom affiché = nom Steam.
- Vérifié seul : `p6_steam_selftest` (API résolue, 26/26 messages P2P à son propre SteamID, sans lobby). Tests unitaires : adresses Steam, chargement sans Steam. Lobby, invitation et liaison entre deux comptes : validation à deux PC.
- Paquet joueur : `tools/package/make-player-package.ps1` (UE4SS épinglé + E33Coop, liste de mods fixe, `install.ps1` / `uninstall.ps1`). Partie normale : journaux dans `ue4ss/Mods/E33Coop/logs/<date-heure>/`.
- Checklist : `docs/VALIDATION-PHASE5-6.md`.

### Phase 6 : premier essai réel à deux PC (2026-10-20, ~23h30, build 39472366662)
- Connexion Steam OK (Killian hôte, son ami client) : lobby, jonction, instantané, exploration partagée.
- **Combat 1 bloqué** : Killian attaque le monstre par surprise (`engage` 1, envoyé dans `BattleStart`), mais le pion suiveur du client touche le monstre de lui-même en contact normal (`engage` 0) avant que le mod lance le combat. Ordre des tours différent dès le tour 1 (hôte : Maelle, client : Verso) ; le client attend une commande pour Verso au tour 1, l'hôte attend Lune au tour 5. `coop_actor_mismatch` était seulement journalisé.
- **Combat 2 bloqué** : le combat 1 n'était jamais clos chez le client (carte rechargée sans fin de combat). Son premier tour du combat 2 est compté comme tour 2 du combat 1, avant l'arrivée du `BattleStart` ; aucun `BattleReady` → l'hôte reste figé « en attente des autres joueurs » (Verso arrêté dans son animation d'entrée).
- Corrections (à builder et revalider) : le client impose le personnage du `TurnStart` de l'hôte dans `StartCharacterTurn` (pré-hook, attente 15 s max, `coop_actor_forced`) ; rattrapage du premier tour quand le combat local a commencé avant le `BattleStart` (`coop_turn_start_catchup`) ; clôture du combat chez le client sur `BattleEnd` de l'hôte si le jeu local n'a pas fini (`finalize_battle("host_end")`).
- 2e essai (build 39503631855, 2026-10-21 ~00h) : démarrage coordonné OK (hôte tenu 9 s, `hash_ok` au tour 1). Visée libre ouverte au moment où l'hôte sort de la pause d'attente, puis quittée avec la touche d'annulation : aucun événement de fin envoyé (ce chemin ne passe ni par `CancelFreeAimFromUI` ni par `EndFreeAim`) → le client reste en visée et bloque l'action suivante. Corrigé (build 39505365764) : le propriétaire surveille `IsAiming`, le rejeu se ferme quand l'action suivante est arrivée, et repart sur une nouvelle visée.
- Engagement : le client voit toujours l'hôte « pris par surprise » au lieu de l'attaque surprise de l'hôte (son pion suiveur touche le monstre de son côté).
- 3e essai : équipe changée chez l'hôte (Maelle, Verso, Monoco), le client combat toujours avec Verso seul (les combats de la carte partaient avec `party` vide). Corrigé (build 39506053677) : `BattleStart.party` rempli, et chez le client un pré-hook sur `StartBattle` de l'ennemi impose l'équipe (`CharactersManager.SetParty`, `coop_party_applied`) et l'engagement de l'hôte (`coop_engage_forced`), y compris quand le pion suiveur touche l'ennemi en premier (si le `BattleStart` est déjà arrivé). Touches du mod affichées dans le menu pause (demande de Killian).
- Journal client du 2e essai : rattrapage du premier tour validé en jeu (combat 2 : `coop_turn_start_catchup`, `hash_ok`). Mais le `StartBattle` du pion suiveur arrive ~0,4 s **après** que le mod a marqué le combat lancé, et aux combats 2-3 ~3 s **avant** le `BattleStart` (envoyé au premier tour de l'hôte). Build 39506682501 : le pré-hook garde le dernier `BattleStart`, et l'hôte envoie un avis précoce (`BattleStart` avec `battle_id` 0) depuis son `StartBattle`.
- 4e essai (00h36) : l'ennemi du client repère le pion suiveur et lance le combat 3,5 s **avant** l'attaque surprise de l'hôte (avis précoce reçu après) ; premier tour non forcé car le combat n'était pas encore « actif ». Build 39510485783 : chez le client, ennemis de la carte sans détection, poursuite ni combat au contact (`passive_world_enemies`, toutes les 2 s) ; acteur forcé aussi au premier tour d'un combat annoncé ; F5 choisit un personnage, F6 le donne au joueur suivant, répartition affichée chez tous (`owners_summary`).
- **Reproduction sur un PC (2026-10-21, nuit)** : un seul jeu par compte Steam, donc vrai jeu + faux pair. Runner : étape `set_party`, visée libre `"cancel": "key"` (`TryCancelFreeAim`). Scénarios `p6_world_host/client` (équipe changée après l'instantané, attaque surprise sur un ennemi de la carte, visée quittée par la touche) et `p6_world2_host/client` (deux combats de la carte dans la session, « passer » rejoué, réattribution F6 entre les deux). Le test a trouvé : bandeau « Équipe : » vide à l'arrivée du client (corrigé).
- **Piège des tests** : lancés par Steam, ils recevaient les options de lancement de Killian (`-e33role=host -e33transport=steam`) ; un test UDP est parti en Steam. En mode test, les options `-e33…` viennent maintenant du seul `launch_args` du scénario (`native/common/cmdline.hpp`). C'est probablement aussi la cause des 3 rouges non analysés de la batterie précédente.
- **Batterie complète : 36/36 verts** (build 39515111132), dont les 4 nouveaux p6. Paquet joueur refait sur ce build.
- Connu : la jauge de masque de Monoco démarre parfois différemment chez le client (6 contre 5) ; réparée au tour 1 (`raw_hash_diffs` toléré à 2 dans les p6).
- À valider à deux PC : le transport Steam lui-même, et l'attaque surprise en vrai (le test ne simule pas un ennemi qui fonce sur le pion suiveur ; la correction rend les ennemis passifs chez le client).
- **Nouvelle partie ensemble** (demande de Killian, 2026-10-21 soir, build 39668659884) : l'hôte lance une nouvelle partie pendant qu'un client attend au menu → `JoinWait "new_game"` → le client lance la sienne (autosave coupée, aucune sauvegarde écrite, vérifié), chacun voit l'intro, la barrière attend le plus lent, puis l'instantané part au premier point sûr (jamais pendant l'intro du client). `OnInitialCinematicCompleted` ne se déclenche pas : fin d'intro = 5 s de calme après la première cinématique du prologue (~240 s). `p6_newgame_host` et `p6_newgame_client` verts. Vérification des tests p4/p5/p6 voisins (règle « point sûr » touchée) annulée à la demande de Killian : à repasser.
- **Sauvegarde côté client** (demande de Killian, 2026-10-22, branche `feat/client-save`, D15) : le client garde la progression commune dans `SaveGames\e33coop\copies\<hôte>.sav` (instantané reçu, puis copie locale à chaque empreinte d'exploration identique à l'hôte) ; en rôle hôte, **F10** au menu principal héberge la copie la plus récente, la progression continue dans la copie. Saves perso protégées (autosave retenue coupée, écriture hors `e33coop` bloquée). Module `native/sync/coopsave.cpp`. Scénarios `p6_coopsave_client` / `p6_coopsave_host` écrits, **pas encore lancés**.
- **En pause (2026-10-22, nuit)** — chantiers ouverts :
  - Mini-freezes du client (~1-2 s à la fin de ses compétences) : essai `-e33turndep=1` (désactivé par défaut) : le client s'inscrit à `BM.TurnStartNotifyDependencies` quand le jeu commence à écouter (`StartListenToDependencies`), vérifie l'état (photo au début du tour, écarts d'ici reportés sur l'hôte, bonus compris) puis résout. Vert sur `p6_turndep_client` ; la version « inscription à l'écoute » (dernier commit) n'est **pas encore testée** : relancer `p3_party_client_turndep`, `p3_freeaim_client_turndep`, `p6_turndep_client` (témoin sans essai : `p3_party_client_paced`, vert). Si vert : activer par défaut, batterie.
  - Suivi en exploration lissé (cible lissée, vitesse du pilote, hystérésis arrêt/reprise) : codé, pas testé en vrai.
  - Blocage client sur la carte du monde (animations figées, fuite locale jamais finie) : non reproduit ; il faut une save carte du monde (`SaveGames\e33coop\host.sav` chez l'ami) pour un scénario `p6_worldmap`.
  - Chargements répétés aux changements de zone chez le client : piste = écart d'inventaire permanent du hash d'exploration -> instantané renvoyé ; il faut le journal client d'une session avec changements de zone.
  - Prochain chantier accepté par Killian et son ami : test piloté par Steam (`-e33remotetest=<SteamID hôte>`, le client rejoint seul le salon de l'ami, n'exécute qu'une liste fermée d'actions du testeur, renvoie ses journaux par le lien).
  - Outils corrigés : les tests ne laissent plus les saves perso en lecture seule (la progression de Killian du 2026-10-21 soir n'a pas été enregistrée à cause de ça) et copient les journaux de partie dans `e33coop-backups\game-logs-*` avant de réinstaller.
- **Reprise (2026-10-22 matin)** : les essais `-e33turndep` de la nuit montraient deux défauts, corrigés (build 39750815158) : un `CheckDependencies` de trop après `SolveDependency` relançait la suite du tour (l'ennemi tirait deux fois sa cible : écart de bouclier au tour 22 de `p3_party`) ; le testeur agissait pendant l'attente (`p3_freeaim` bloqué au tour 3) : `coop::holding()` couvre maintenant la dépendance.
- **Expérience de jeu, validé par Killian (2026-10-22)** : voir ce que l'autre prépare (compétence + cible) et corriger la vue de la visée libre chez l'observateur (caméra vue de haut qui bouge au lieu du personnage qui vise) ; bonus de parade en duo ; écran d'attente plus vivant ; sauvegarde côté client (copie de la progression commune) ; répartition des persos plus claire et « Rejoindre en un clic » : maquettes à montrer d'abord (crainte d'encombrer l'écran de combat, piste : menu pause). Exploration libre : plus tard. Travail en branches `feat/*` (sous-agents), fusion dans `phase-6` après tests.
- **Attente sans figer (`-e33turndep`) : activée par défaut** (commit c87ba13, `-e33turndep=0` = ancienne attente bloquante). Derniers défauts trouvés et corrigés : tout `CheckDependencies` forcé après `SolveDependency` relançait la suite du tour (les drapeaux `IsListening`/`HasTriggered` restent faux un moment : on ne force plus rien, simple journal `coop_turn_dep_still_listening`) ; pas de dépendance pour un tour déjà vérifié à `OnTurnStart` (état de l'hôte arrivé avant). Verts : `p3_party_client_turndep`, `p3_freeaim_client_turndep`, `p6_turndep_client`.
- **Chargements répétés aux changements de zone** (cause trouvée en lisant le code, commit abede30) : le suiveur arrivé avant le pilote gardait sa dernière position (ancienne zone) et y retournait au bout de 8 s. Corrigé : position oubliée au `MapChange`, et retour au niveau du pilote seulement sur une position fraîche (< 1,5 s). À confirmer à deux PC.
- Récompenses : le client attend 4 s de plus quand le `BattleEnd` de l'hôte arrive pendant l'attente (`p3_freeaim` : récompenses de l'hôte ~2 s après).
- **Intégration (branche `integ`, 2026-10-22)** : `phase-6` + `feat/client-save`, `feat/duo-parry` (D16), `feat/intent-preview`, `feat/remote-test`, `feat/owners-ui`, `feat/join-wait-ui`. Verts sur le build 39760882979 : les 3 essais turndep, `p6_coopsave_host`, `p7_intent_host`, `p7_intent_client`, `p6_ownersui_host` (losanges K/T visibles sur captures), `p4_cinematic_host/client`. `p6_coopsave_client` : vérifications OK, faux hôte arrêté de force (option `stop_ok` ajoutée). `p6_duoparry_host` : le boss mourait au tour 3, avant toute attaque (les héros passent maintenant 4 tours sur 5). Panneau « Coop » du menu pause construit mais absent des captures (le menu pause du testeur n'était pas visible) : à vérifier en vrai.
- Reste à faire : l'attente du résultat de parade de l'autre joueur fige encore jusqu'à 3 s (`coop_defense_imposed`) : à passer en attente sans blocage ; enregistrement `p6_duoparry_host` → `p6_duoparry_client`.
- Reste : un écart déjà commencé (tour lancé avant que le mod soit prêt) n'est toujours que journalisé ; le pion suiveur peut encore engager un combat lui-même (l'ordre est corrigé, pas l'engagement).

### Test à distance (branche `feat/remote-test`, 2026-10-22) — écrit, compilé, tests unitaires verts ; à valider avec l'ami
- Doc : `docs/REMOTE-TEST.md` (installation de l'ami, garanties de sécurité, commandes). Côté ami : `install.ps1 -RemoteTest 76561198355797267 [-AllowRestart] [-AllowUpdate]` écrit `ue4ss/Mods/E33Coop/remotetest.txt` (le supprimer coupe tout) ; bandeau « Mode test à distance actif — piloté par … ».
- Canal Steam 1 à part (messages fiables de Steam), format `E33R` versionné à part du protocole coop (v6 inchangé) : `status`, `fetch-logs`, `restart`, `update` survivent aux changements de protocole. Seul le SteamID de confiance est écouté, liste fermée de commandes.
- Commandes (`tools/remote/remote.ps1`, réponses dans `remote/outbox/<id>/`) : `status`, `fetch-logs` (derniers `logs/<date>/`, `UE4SS.log`, `SaveGames/e33coop/*.sav`, `remote/helper.log`), `run <scénario>` (validé, déposé dans `remote/pending/`, exécuté au lancement suivant comme `-e33test`, dossier `logs/remote-<date>/`), `menu`, `load-snapshot`, `quit`, `restart` (relance par `steam://rungameid/1903340`), `update` (paquet vérifié SHA-256, sauvegarde, installation par `apply-update.ps1`, retour arrière automatique sans `mod_init` en 3 min).
- Client : rejoint seul le lobby de l'hôte de confiance au menu principal (amis Steam) ; sans options de lancement, le mode vaut `-e33role=client -e33transport=steam -e33join=1`.
- Non testé en vrai (un seul jeu par compte Steam ici) : tout ce qui passe par Steam entre deux comptes, la relance par Steam (fenêtre de confirmation possible la 1re fois), `QuitGame`, les assistants PowerShell lancés depuis le jeu (job Steam), l'auto-jonction par `GetFriendGamePlayed`.

### Limites connues (phase 5)
- Une dépense faite dans les 2 s d'un événement rejoué (coffre, combat) est absorbée dans la référence : le hash d'exploration la détecte et la resynchro reprend l'état de l'hôte (dépense perdue).
- Réattribution des personnages : touche F6 de l'hôte (hors combat), pas de vrai menu.
- Suiveur : un clic de souris sur une réponse de dialogue n'est pas bloqué.

## Historique des phases précédentes

### Fait (phase 3)
- **Modèle D11** (rejeu symétrique) : chaque instance joue tous les héros ; l'hôte relaie à chacun les actions, résultats de QTE et défenses des héros qu'il ne contrôle pas ; état complet de l'hôte à chaque début de tour (correction seulement en cas d'écart). Détails : `DECISIONS.md` D11.
- **Protocole v4** : cibles (camp, emplacement), compétences et objets (`SelectSecondaryAction`), `ComboResult` (résultat + instant de l'appui), défenses par héros (parade, parade gradient, esquive, esquive parfaite, saut), `StateSync`, `BattleRewards`, équipe imposée dans `BattleStart`, passer le tour / fuir.
- **Équipe de test à 3 sans nouvelle save** : `OverrideParty` des paramètres de combat (`"party": ["Verso","Maelle","Lune"]` dans un scénario). TEST_B contient Lune, Maelle, Sciel, Monoco, Verso. Maelle est à 1 PV (build « dernière chance ») : utile pour mort/réanimation, mais elle tue le Lancelier en un coup.
- **Libellés lus en jeu** : `E_jRPG_ActionType` 1 attaque, 2 défense, 3 objets, 4 fuite, 5 compétence, 6 passer ; `EBattleEndResult` 1 victoire, 2 défaite, 3 neutre.
- **Ciblage ennemi** synchronisé (`GetRandomAliveHero`, `GetSpecificHero` si le héros voulu est mort) ; **récompenses** (or, XP, butin) tirées pareil des deux côtés et comparées ; fin de combat comparée.
- **Lien réseau** : fragmentation des messages fiables > 1 000 o (l'état complet dépassait 2 Ko et bloquait tout le flux) ; tampon de réception 64 Ko.
- **QTE offensifs** : imposer réussi/parfait en fin de fenêtre ne reproduit PAS un vrai appui (un coup de plus chez l'hôte, trouvé par le rejeu). Désormais le propriétaire publie l'instant de son appui ; les autres instances jouent l'action ~1 s plus tard et rejouent un vrai appui au même instant.
- Test runner : actions par héros (`attack`/`skill`/`item`/`skip`/`flee`, cibles `auto`/`enemy:N`/`hero:Nom`/`dead_hero`/`all_enemies`/`self`, repli `else`), QTE joués par de vrais appuis (`"combo": ["perfect","success","fail"]`), défenses `parry`/`dodge`/`perfect_dodge`/`jump`/`fail`, vérifications génériques sur tous les compteurs.
- Outils : `tools/dev/remote.ps1` (télécommande Lua d'exploration), snippets Lua (combat à 3, compétence, objet, QTE).

- Ajouts de fin d'après-midi : visée libre rejouée (point visé imposé via `ComputeFreeAim3rdPersonTargetLocation`, ~1 s de décalage, choix approuvé par Killian), réanimation (héros morts dans `DeadPlayerCharacters`), réglage initial de scénario (`setup`), passer le tour, fin de combat non bloquante, test à 3 joueurs (`fakepeers`), mode manuel du runner, `battery.ps1`, `.tools/run.sh` / `.tools/pair.sh`.

- **Retours de la 1re validation (2026-10-19 soir)** : visée libre d'un héros distant, les commandes locales de visée/tir sont verrouillées (`DisableFreeAimControls`, compteur `free_aim_controls_locked`) ; QTE de Maelle « raté » : attribué à tort au joueur, voir 2e validation ; métrique `frame_hitches` (images > 50 ms en combat).
- **Gel au démarrage corrigé** : le mod reste inerte 10 s après `on_unreal_init` (aucune recherche de fonctions, réseau ni runner). Sonde client : 3 gels / 8 avant, 0 / 16 après. Le lanceur détecte aussi le gel (fenêtre bloquée sans hook de jeu après 45 s) et relance jusqu'à 3 fois (statut `startup_freeze`).

- **Retours de la 2e validation (2026-10-20)** : RT ouvrait le menu gradient de Verso chez l'hôte. `SetBattleInputEnabled(false)` ne coupe pas la roue : pendant le tour d'un héros distant, le mod retire à chaque image les contextes `IMC_Battle_PlayerTurn` / `MainWheel` / `Targeting` / `QTE` / `FreeAim` (compteur `input_contexts_removed`) et rend tour/roue/ciblage si le jeu les a remis juste avant le début d'un tour d'ici (`input_contexts_restored`). QTE de Maelle raté : non reproduit au clavier avec appuis réels (même latence 52 ms avec et sans mod) ; journal `coop_combo_pressed` / `coop_combo_try` ajouté pour l'essai manette.

- **Mécaniques propres dans l'état contrôlé** (perfection de Verso, posture de Maelle, taches de Lune : champs entiers/booléens/énumérations et tableaux des composants `*Mechanic*` / `StainHolder`) : envoyées dans `StateSync`, hashées, comparées et réparées. Enregistrements régénérés : 0 écart brut sur tous les rejeux.

### Tests (dernier état)
- 2026-10-20 (phase 4) : `p3_actions_client` revérifié : nombre de coups de Combo1 variable (9 ou 10) même chez l'hôte seul, l'enregistrement était un run à 9. Coup en plus compté à part (`draw_count_diffs`, toléré dans ce seul scénario, l'état complet le corrige) ; vert sur le build 39411918718.
- Batterie **12/12 verte** (build 39316121262, 2026-10-20 matin, aucune relance) : p2 ×2, p3_actions ×2, p3_party ×2, p3_freeaim ×2, p3_defeat ×2, p3_resync_client, p3_three_host.
- Build 39401401406 (blocage des entrées) : p3_freeaim_host, p3_party_client verts ; p3_actions_client rouge 3/3 à cause du lanceur (appui « parfait » à 910 ‰, un coup de plus que l'enregistrement), corrigé dans 5303a9c (85 %) mais **pas encore revérifié** : repasser la batterie au début de la phase 4. Fusion sans batterie complète, à la demande de Killian.

### En cours (phase 4, D12)
- Mesuré en jeu : instantané de la partie (`CreateSaveGameDataFromGameState` → V8 → `SaveGameToSlot("e33coop/…")`, 438 Ko) rechargé exactement à la position du pion ; pion piloté par `AddMovementInput` ; combat d'ennemi du monde (`StartBattle`, paramètres dans `CurrentBattleStartParams`) ; transition `ChangeMapByAssetName` ; dialogue par `OnInteractionRequested`, réponse par `OnChoiceMade`.
- Écrit : protocole v5 (instantané par morceaux, pilote, interactions, choix, transitions, barrière narrative, pause), module `native/sync/explore.cpp`, étapes d'exploration du test runner, faux client « session » et faux hôte qui renvoie l'instantané depuis `.tools/testsaves` (jamais d'octets de save dans le repo public).
- `p4_session_host` (1er run) : instantané envoyé et vérifié (hash, GVAS), client entré, 16 positions/s, combat d'exploration, transition vers le camp, interaction avec Lune publiés ; échec sur le choix de dialogue (`CurrentChoices` vide pendant la question → suivi par `StartChoice`) et la fuite (fin de combat neutre ajoutée). Relance en cours.
- Reste : `p4_session_client` (rejeu), pause, cinématique réelle (barrière testée sur dialogue), prise de main (F8), batterie complète.

### Pièges récents
- Gel sur l'écran d'avertissement au démarrage (fenêtre bloquée, CPU ~0, thread principal en attente dans le moteur, `main.dll` sur aucune pile). Mesures 2026-10-19 : UE4SS seul 0/9, mod solo 1/8, mod client sans hôte 1/8, mod client + hôte 3/8. Cause : l'activité du mod dans ses premières secondes. Corrigé par le délai de 10 s (0/16) ; le lanceur relance en cas de récidive.

### Limites connues
- Phase 4 : une cinématique qui enchaîne directement sur un combat (boss Renoir, TEST_A) : la barrière met l'hôte en pause à la fin de sa cinématique, mais le jeu lance quand même le combat environ 5 s plus tard. En exploration, la barrière tient. À traiter avec le démarrage de combat coordonné (phase 5).
- Phase 4 : les cinématiques jamais vues ne se passent pas (`SkipCinematic` sans effet) ; le test cinématique dure donc ≈ 5 min.
- Phase 4 : chez un suiveur, le déplacement local est coupé (contextes de locomotion retirés, signalé par Killian pendant la validation) ; interactions et choix de dialogue locaux ne sont pas encore bloqués, seulement journalisés (`local_actions_on_follower`).
- QTE offensifs : le jeu accepte l'appui jusqu'à son traitement de la fin de fenêtre, 65 à 310 ms après `CurrentWindowEndRealTime` selon la partie (mod ou pas) : marge variable. Outil `tools/dev/qte-diag.ps1` (appuis manette mesurés en temps réel, avec et sans mod) ; 1 essai chacun le 2026-10-20 : pas d'écart net. Menu pause toujours accessible pendant le tour d'un autre joueur.
- Une seule visée libre par tour rejouée ; attaques/contres gradient non testés en automatique ; Sciel/Monoco non vus ; pas d'indicateur « tour de X » (phase 5).

### Bloqué
- Rien.

## Historique
- 2026-10-20 (soir) : phase 5 démarrée : batterie de base 15/18 (rouges expliqués) ; protocole v6, départs et retours, fin de session, démarrage coordonné, minuteur de tour, textes FR/EN, hash d'exploration ; premiers scénarios p5.
- 2026-10-20 (soir) : phase 4 : rejoindre (instantané), pilote et prise de main, transitions, combat d'exploration, dialogues, barrière de cinématique, pause en combat, déplacement coupé chez le suiveur ; acceptée par Killian, fusionnée dans `main`.
- 2026-10-18 : démarrage ; phase 0 complète ; test runner écrit.
- 2026-10-19 (nuit) : phase 1 complète (3/3) ; phase 2 : protocole, fakepeer, module coop, premier combat hôte vs faux client sans désync. Pause.
- 2026-10-19 (matin) : reprise. Crash client au chargement corrigé ; fermeture auto des crashs ; écarts de PV rendus visibles ; D10 ; les deux sens verts 3/3 avec 0 écart.
- 2026-10-19 (après-midi) : validation de Killian OK ; écran noir identifié (environnement de test) ; phase 2 fusionnée dans `main`.
- 2026-10-19 (14h30) : écran noir et ennemis qui ne touchent pas en coop corrigés (TEST_B ; hook Array_Random retiré) ; CI ~2 min.
- 2026-10-19 (15h45) : phase 3 démarrée : D11, protocole v4, équipe imposée, compétences/objets/QTE/défenses relayés, état complet, ciblage, récompenses.
- 2026-10-20 (midi) : 3e validation : plus aucun contrôle pendant le tour d'un autre joueur (contextes d'entrée retirés), QTE manette mesuré ; phase 3 fusionnée dans `main`.
- 2026-10-20 (matin) : verrou de visée locale, gel au démarrage corrigé (délai 10 s), lanceur qui relance ; batterie 12/12 (build 39316121262).
- 2026-10-19 (soir) : mécaniques propres dans l'état (build corrigé : en-tête `FEnumProperty`), enregistrements régénérés, batterie 12/12 ; checklist de validation prête.
