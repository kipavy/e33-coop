# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-19, matin) — **reprise, phase 2 en cours**

| Phase | État | Branche |
|---|---|---|
| 0 — exploration | **finie** | `phase-0` (fusionnée dans `main`) |
| 1 — environnement de test autonome | **finie** | `phase-1` (fusionnée dans `main`) |
| 2 — preuve de concept réseau | **en cours** | `phase-2` |

Repo **public** depuis le 2026-10-18 (basculé par Killian) : CI GitHub Actions gratuite. Pas encore de `LICENSE` (choix de Killian).

### Fait
- **Phase 0** : UE4SS 1181 sur le build 23765773, dumps, étude Kouzukii (pas de fork, D1), `GAME-INTERNALS.md`, hook C++ qui logge un vrai événement de combat.
- **Phase 1** : `tools/run-scenario/run-scenario.ps1 solo_parry_basic` joue un combat complet sans intervention (chargement de save de test hors Steam Cloud, déclenchement du combat, attaques auto, défense réussie/ratée sur commande par coup reçu — parade **ou saut** selon l'attaque —, `result.json`, rapport). **3 runs d'affilée verts** (2026-10-19 00:2x), tous les ordres de défense obéis, aucun verdict forcé.
- **Phase 2 (partiel)** :
  - protocole v2 versionné + validation de chaque message, transport UDP derrière `net::Transport`, lien fiable ordonné, tests unitaires (aller-retour, troncatures, fuzz, perte/doublon/réordonnancement) en CI ;
  - `fakepeer` (faux client scripté / faux hôte qui rejoue un enregistrement) ;
  - module coop du mod : handshake (build du jeu, version du mod, liste des mods de gameplay → refus avec la différence exacte), propriété des héros, actions distantes, **défense distante** (l'hôte demande le résultat du coup au propriétaire et l'applique : vraie parade / vrai saut), hash d'état par tour, enregistrement des sessions ;
  - **hôte réel vs faux client (build `41d90ed`, 2026-10-19 09:25)** : combat complet, 5 hash identiques, 0 désync, 4 actions distantes, 2 défenses distantes appliquées **sans attente (0 ms**, contre 32–46 ms avant), verdict de parade de l'hôte conforme (parade → `Successful=true`, échec → `false`).
  - outillage : `run-scenario` ferme tout seul la boîte « Fatal error » du jeu après un crash (texte gardé dans `crash-dialog.txt`), récupère le dump UE4SS et le résume (`tools/dev/dmp-info.ps1` : exception, module+offset, thread) ; code de sortie du faux pair enfin lu correctement.

### En cours
- Build CI de `c7c40f8` puis : `p2_host_vs_fakeclient` (avec traces `rng_trace`) → nouvel enregistrement → `p2_client_vs_fakehost`.
- **Tirages de l'IA (D9)** : aucun `coop_ai_draw` côté hôte au run de 09:25 alors que les 6 hooks `KismetMathLibrary` sont enregistrés. Diagnostic `rng_trace` ajouté (appelant, fonction BP, index d'ennemi) pour savoir si le hook ne se déclenche pas ou si le filtre « appelant = ennemi » rejette tout.
- **Crash du client au chargement de la save** (run 09:29, lecture de l'adresse nulle sur le thread de jeu) : le faux hôte envoyait `BattleStart` dès le handshake (client encore au menu) → combat marqué actif → chaque tirage aléatoire pendant le chargement appelait `GetControllerWorld` sur un monde en démontage. Corrigé dans `c7c40f8` (à vérifier) : hooks de tirage limités au thread de jeu et sans aucun appel au jeu ; `BattleStart` mis en attente côté client jusqu'à un monde stable 5 s ; faux hôte tolérant 120 s de silence (chargement).

### Bloqué
- Rien.

### Prochaines étapes
1. Phase 2 : faire passer `p2_client_vs_fakehost` sans désync ; relancer `p2_host_vs_fakeclient` ; préparer la checklist de validation en jeu pour Killian (fin de phase 2).
2. Phase 3 : ciblage IA (`GetRandomAliveHero`), compétences, objets, buffs, attaques de groupe, mort/réanimation, récompenses ; resynchronisation complète sur désync.

## Historique
- 2026-10-18 : démarrage ; phase 0 complète ; test runner écrit.
- 2026-10-19 (nuit) : phase 1 complète (3/3) ; phase 2 : protocole, fakepeer, module coop, premier combat hôte vs faux client sans désync. Pause à la demande de Killian.
- 2026-10-19 (matin) : reprise. Hôte vs faux client vert côté jeu (0 désync, défense distante 0 ms) ; crash du client au chargement trouvé et corrigé ; fermeture auto des crashs dans `run-scenario`.
