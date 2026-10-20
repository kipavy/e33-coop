# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-20, 12h20) — **phase 3 finie** (validée en jeu, fusionnée dans `main`) ; prochaine : phase 4

| Phase | État | Branche |
|---|---|---|
| 0 — exploration | **finie** | `phase-0` (fusionnée dans `main`) |
| 1 — environnement de test autonome | **finie** | `phase-1` (fusionnée dans `main`) |
| 2 — preuve de concept réseau | **finie** (validée en jeu le 2026-10-19) | `phase-2` (fusionnée dans `main`) |
| 3 — synchro complète du combat | **finie** (validée en jeu le 2026-10-20) | `phase-3` (fusionnée dans `main`) |

Repo **public** depuis le 2026-10-18 (basculé par Killian) : CI GitHub Actions gratuite. Pas encore de `LICENSE` (choix de Killian).

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
- Batterie **12/12 verte** (build 39316121262, 2026-10-20 matin, aucune relance) : p2 ×2, p3_actions ×2, p3_party ×2, p3_freeaim ×2, p3_defeat ×2, p3_resync_client, p3_three_host.
- Build 39401401406 (blocage des entrées) : p3_freeaim_host, p3_party_client verts ; p3_actions_client rouge 3/3 à cause du lanceur (appui « parfait » à 910 ‰, un coup de plus que l'enregistrement), corrigé dans 5303a9c (85 %) mais **pas encore revérifié** : repasser la batterie au début de la phase 4. Fusion sans batterie complète, à la demande de Killian.

### En cours
- Rien (phase 4 à démarrer).

### Pièges récents
- Gel sur l'écran d'avertissement au démarrage (fenêtre bloquée, CPU ~0, thread principal en attente dans le moteur, `main.dll` sur aucune pile). Mesures 2026-10-19 : UE4SS seul 0/9, mod solo 1/8, mod client sans hôte 1/8, mod client + hôte 3/8. Cause : l'activité du mod dans ses premières secondes. Corrigé par le délai de 10 s (0/16) ; le lanceur relance en cas de récidive.

### Limites connues
- QTE offensifs : le jeu accepte l'appui jusqu'à son traitement de la fin de fenêtre, 65 à 310 ms après `CurrentWindowEndRealTime` selon la partie (mod ou pas) : marge variable. Outil `tools/dev/qte-diag.ps1` (appuis manette mesurés en temps réel, avec et sans mod) ; 1 essai chacun le 2026-10-20 : pas d'écart net. Menu pause toujours accessible pendant le tour d'un autre joueur.
- Une seule visée libre par tour rejouée ; attaques/contres gradient non testés en automatique ; Sciel/Monoco non vus ; pas d'indicateur « tour de X » (phase 5).

### Bloqué
- Rien.

## Historique
- 2026-10-18 : démarrage ; phase 0 complète ; test runner écrit.
- 2026-10-19 (nuit) : phase 1 complète (3/3) ; phase 2 : protocole, fakepeer, module coop, premier combat hôte vs faux client sans désync. Pause.
- 2026-10-19 (matin) : reprise. Crash client au chargement corrigé ; fermeture auto des crashs ; écarts de PV rendus visibles ; D10 ; les deux sens verts 3/3 avec 0 écart.
- 2026-10-19 (après-midi) : validation de Killian OK ; écran noir identifié (environnement de test) ; phase 2 fusionnée dans `main`.
- 2026-10-19 (14h30) : écran noir et ennemis qui ne touchent pas en coop corrigés (TEST_B ; hook Array_Random retiré) ; CI ~2 min.
- 2026-10-19 (15h45) : phase 3 démarrée : D11, protocole v4, équipe imposée, compétences/objets/QTE/défenses relayés, état complet, ciblage, récompenses.
- 2026-10-20 (midi) : 3e validation : plus aucun contrôle pendant le tour d'un autre joueur (contextes d'entrée retirés), QTE manette mesuré ; phase 3 fusionnée dans `main`.
- 2026-10-20 (matin) : verrou de visée locale, gel au démarrage corrigé (délai 10 s), lanceur qui relance ; batterie 12/12 (build 39316121262).
- 2026-10-19 (soir) : mécaniques propres dans l'état (build corrigé : en-tête `FEnumProperty`), enregistrements régénérés, batterie 12/12 ; checklist de validation prête.
