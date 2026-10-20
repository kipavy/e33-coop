# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-20, nuit) — **phase 5 en cours** (branche `phase-5`)

| Phase | État | Branche |
|---|---|---|
| 0 — exploration | **finie** | `phase-0` (fusionnée dans `main`) |
| 1 — environnement de test autonome | **finie** | `phase-1` (fusionnée dans `main`) |
| 2 — preuve de concept réseau | **finie** (validée en jeu le 2026-10-19) | `phase-2` (fusionnée dans `main`) |
| 3 — synchro complète du combat | **finie** (validée en jeu le 2026-10-20) | `phase-3` (fusionnée dans `main`) |
| 4 — session et exploration | **finie** (acceptée le 2026-10-20) | `phase-4` (fusionnée dans `main`) |
| 5 — robustesse | **en cours** | `phase-5` |
| 6 — transport Steam | à faire (code du transport écrit hors dépôt, à intégrer) | `phase-6` |

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

### Phase 5 : reste à faire
- Menus de builds (niveau, attributs, compétences, équipement, améliorations, marchands) : cartographie en jeu (`lua/E33Explore/snippets/hook_builds.lua` prêt), puis rejeu des opérations via l'hôte (D13).
- Suiveur : bloquer ses interactions et choix locaux (`IMC_UI_Exploration`, `IMC_UI_Dialogue` à vérifier en jeu).
- Affichage des textes à l'écran (widget du jeu à trouver).
- Revérifier en jeu la cinématique → combat de boss avec la pause réappliquée (`p4_cinematic_*`).
- Scénarios `p5_resync_*` (hôte puis client avec deux instantanés).

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
