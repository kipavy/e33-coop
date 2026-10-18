# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-19, nuit) — **EN PAUSE à la demande de Killian (après la phase 1)**

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
  - **hôte réel vs faux client : combat complet, 0 désync**, 4 actions et 2 défenses distantes (attente de l'hôte 32–46 ms, à réduire : correctif poussé).
  - D9 minimal : l'hôte diffuse les tirages aléatoires de l'IA ennemie, le client les rejoue (build en cours).

### En cours
- Rien (pause). Le dernier commit de `phase-2` (D9 minimal : rejeu des tirages de l'IA) est poussé mais **pas encore testé en jeu**.

### État de la machine pendant la pause
- Jeu remis à l'état vanilla : `dwmapi.dll` et `ue4ss/` retirés du dossier Win64 (`reset-env` les réinstalle).
- Saves perso de Killian **de nouveau en écriture** (il peut jouer normalement) ; `reset-env` les repasse en lecture seule et en refait une copie avant chaque campagne.

### Bloqué
- Rien.

### Prochaines étapes (à la reprise)
0. Reprendre ici : build CI du dernier commit `phase-2`, puis `run-scenario p2_host_vs_fakeclient` → copier `runs/<run>/recording-host.jsonl` dans `tests/recordings/p2_host_session.jsonl` → `run-scenario p2_client_vs_fakehost`.
1. Phase 2 : faire passer `p2_client_vs_fakehost` sans désync ; relancer `p2_host_vs_fakeclient` ; préparer la checklist de validation en jeu pour Killian (fin de phase 2).
2. Phase 3 : ciblage IA (`GetRandomAliveHero`), compétences, objets, buffs, attaques de groupe, mort/réanimation, récompenses ; resynchronisation complète sur désync.

## Historique
- 2026-10-18 : démarrage ; phase 0 complète ; test runner écrit.
- 2026-10-19 : phase 1 complète (3/3) ; phase 2 : protocole, fakepeer, module coop, premier combat hôte vs faux client sans désync.
