# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-19, fin de matinée) — **phase 2 : critères atteints, un point à éclaircir avant la validation**

| Phase | État | Branche |
|---|---|---|
| 0 — exploration | **finie** | `phase-0` (fusionnée dans `main`) |
| 1 — environnement de test autonome | **finie** | `phase-1` (fusionnée dans `main`) |
| 2 — preuve de concept réseau | **critères atteints** (validation de Killian à venir) | `phase-2` |

Repo **public** depuis le 2026-10-18 (basculé par Killian) : CI GitHub Actions gratuite. Pas encore de `LICENSE` (choix de Killian).

### Fait
- **Phase 0** : UE4SS 1181 sur le build 23765773, dumps, étude Kouzukii (pas de fork, D1), `GAME-INTERNALS.md`, hook C++ qui logge un vrai événement de combat.
- **Phase 1** : `run-scenario solo_parry_basic` joue un combat complet sans intervention, parades/sauts sur commande, 3 runs verts.
- **Phase 2** (build `44b33de`, artefact `.tools/artifacts/39224054500/e33coop-mod`) :
  - **hôte réel vs faux client** (`p2_host_vs_fakeclient`, graine 1865) et **client réel vs faux hôte** (`p2_client_vs_fakehost`, rejoue `tests/recordings/p2_host_session.jsonl`) : **3 runs verts sur 3 dans chaque sens**, compteurs identiques d'un run à l'autre : 6 hash comparés, 0 désync, 168 tirages de gameplay identiques, **0 écart de PV côté client** (la simulation locale suit exactement celle de l'hôte, sans correction), 3 défenses distantes appliquées par l'hôte sans attente (0–16 ms).
  - protocole v3 : `BattleStart` porte la graine ; `Draws` publie les tirages de l'hôte pour vérification.
  - **D10** (amende D9) : tirages de gameplay dérivés de la graine de l'hôte, par flux (appelant BP + fonction), sans attente réseau ; les tirages cosmétiques ne sont pas touchés.
  - vérifications ajoutées au test runner : `hashes_compared > 0`, `gameplay_draws > 0`, `draw_mismatches = 0`, `hp_overrides = 0` (client). Un « 0 désync » ne peut plus cacher un combat différent.
  - corrections : crash du client au chargement de la save (hook de tirage qui appelait le jeu pendant le démontage du monde) ; `BattleStart` mis en attente jusqu'à un monde prêt ; vrai appelant BP des fonctions natives via `PreviousFrame`.
  - outillage : fermeture auto de la boîte « Fatal error » + résumé du dump (`tools/dev/dmp-info.ps1`) ; `tools/dev/draw-preview.ps1` (prévoit les tirages d'une graine, vérifié contre le jeu) ; défense `manual` et `"sound": true` pour les tests à la main ; `p2_manual_client`.

### En cours
- **Doute sur D10** : avec D10, les choix 0–2 de l'IA du Fleuret ne touchent jamais le héros (4 graines), alors que les runs à tirages naturels donnaient une attaque parable à 2 coups ; le choix 3 (combo sautable) marche. Soit D10 change le comportement du jeu (bug), soit l'état de l'ennemi diffère. Build de diagnostic `c0db3b7` (`-e33draws=none|<flux>`, paramètres des tirages loggés) pour isoler. Conséquence actuelle : les tests de phase 2 n'exercent plus la **parade** distante, seulement le **saut**.

### Bloqué
- Rien.

### Prochaines étapes
1. Isoler le doute D10 ; obtenir un enregistrement avec attaque parable (`p2_host_session_parry.jsonl`) pour le test manuel.
2. Checklist de validation en jeu pour Killian (`docs/VALIDATION-PHASE2.md`), puis pause pour sa validation.
3. Phase 3 : `Array_Random` (ciblage), compétences, objets, buffs, attaques de groupe, mort/réanimation, récompenses ; resynchronisation complète sur désync.

## Historique
- 2026-10-18 : démarrage ; phase 0 complète ; test runner écrit.
- 2026-10-19 (nuit) : phase 1 complète (3/3) ; phase 2 : protocole, fakepeer, module coop, premier combat hôte vs faux client sans désync. Pause.
- 2026-10-19 (matin) : reprise. Crash client au chargement corrigé ; fermeture auto des crashs ; écarts de PV rendus visibles ; D10 ; les deux sens verts 3/3 avec 0 écart.
