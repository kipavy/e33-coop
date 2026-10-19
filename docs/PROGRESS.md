# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-19, 14h30) — **phase 2 finie et validée ; environnement de test corrigé** (save `TEST_B`, Prairies)

| Phase | État | Branche |
|---|---|---|
| 0 — exploration | **finie** | `phase-0` (fusionnée dans `main`) |
| 1 — environnement de test autonome | **finie** | `phase-1` (fusionnée dans `main`) |
| 2 — preuve de concept réseau | **finie** (validée en jeu le 2026-10-19) | `phase-2` (fusionnée dans `main`) |

Repo **public** depuis le 2026-10-18 (basculé par Killian) : CI GitHub Actions gratuite. Pas encore de `LICENSE` (choix de Killian).

### Fait
- **Phase 0** : UE4SS 1181 sur le build 23765773, dumps, étude Kouzukii (pas de fork, D1), `GAME-INTERNALS.md`, hook C++ qui logge un vrai événement de combat.
- **Phase 1** : `run-scenario solo_parry_basic` joue un combat complet sans intervention, parades/sauts sur commande, 3 runs verts.
- **Phase 2** (dernier build `c0db3b7`, artefact `.tools/artifacts/39229229182/e33coop-mod`, revérifié à 11:4x) :
  - **hôte réel vs faux client** (`p2_host_vs_fakeclient`, graine 1865) et **client réel vs faux hôte** (`p2_client_vs_fakehost`, rejoue `tests/recordings/p2_host_session.jsonl`) : **3 runs verts sur 3 dans chaque sens**, compteurs identiques d'un run à l'autre : 6 hash comparés, 0 désync, 168 tirages de gameplay identiques, **0 écart de PV côté client** (la simulation locale suit exactement celle de l'hôte, sans correction), 3 défenses distantes appliquées par l'hôte sans attente (0–16 ms).
  - protocole v3 : `BattleStart` porte la graine ; `Draws` publie les tirages de l'hôte pour vérification.
  - **D10** (amende D9) : tirages de gameplay dérivés de la graine de l'hôte, par flux (appelant BP + fonction), sans attente réseau ; les tirages cosmétiques ne sont pas touchés.
  - vérifications ajoutées au test runner : `hashes_compared > 0`, `gameplay_draws > 0`, `draw_mismatches = 0`, `hp_overrides = 0` (client). Un « 0 désync » ne peut plus cacher un combat différent.
  - corrections : crash du client au chargement de la save (hook de tirage qui appelait le jeu pendant le démontage du monde) ; `BattleStart` mis en attente jusqu'à un monde prêt ; vrai appelant BP des fonctions natives via `PreviousFrame`.
  - outillage : fermeture auto de la boîte « Fatal error » + résumé du dump (`tools/dev/dmp-info.ps1`) ; `tools/dev/draw-preview.ps1` (prévoit les tirages d'une graine, vérifié contre le jeu) ; défense `manual` et `"sound": true` pour les tests à la main ; `p2_manual_client`.

### Validation de Killian (2026-10-19 12:16–12:25)
- Les 3 tests passent. Test manuel : il a fait « raté, réussi, raté » sur les 3 coups sautables, et le client a envoyé exactement `Fail`, `Jump`, `Fail` à l'hôte, au moment de chaque coup.
- **Problème signalé** : écran noir pendant les attaques ennemies (interface visible), et caméra dans le décor pendant le reste du combat. C'est l'environnement de test (combat forcé dans le hub de Lumière), pas la synchro : voir GAME-INTERNALS §15. Le déclenchement prend désormais l'arène la plus proche (`a8e7b61`), ce qui ne suffit pas.

### En cours
- Rien. Correctifs après la validation (2026-10-19 après-midi) :
  - **écran noir** : venait du combat forcé dans le hub de Lumière (save TEST_A). Nouvelle save de test `TEST_B` (NG+ de Killian, Vallons fleuris), rencontre `SM_Lancelier_Alpha` : rendu normal, vérifié sur captures ;
  - **caméra sous la carte, ennemis qui ne touchent jamais en coop** : le hook de diagnostic sur `Array_Random` cassait le ciblage des ennemis (retiré ; c'était aussi la cause du « doute D10 ») ;
  - l'hôte attend 1,5 s avant d'exécuter une action distante (comme le runner solo) ; le runner client passe en combat même si le coop l'a lancé avant la fin de son attente ;
  - **CI incrémentale** : un build passe de ~16 min à ~2 min (cache par commit + dates de fichiers ; seul le code modifié est recompilé) ;
  - tests : 3/3 dans chaque sens (build `b87f2d4`, artefact `.tools/artifacts/39251250001`), 7 hash, 200 tirages identiques, 0 écart de PV, **parade** distante de nouveau couverte (réussie avec contre, puis ratée).

### Bloqué
- Rien.

### Prochaines étapes
1. Phase 3 : `Array_Random` (ciblage), compétences, objets, buffs, attaques de groupe, mort/réanimation, récompenses ; resynchronisation complète sur désync.

## Historique
- 2026-10-18 : démarrage ; phase 0 complète ; test runner écrit.
- 2026-10-19 (nuit) : phase 1 complète (3/3) ; phase 2 : protocole, fakepeer, module coop, premier combat hôte vs faux client sans désync. Pause.
- 2026-10-19 (matin) : reprise. Crash client au chargement corrigé ; fermeture auto des crashs ; écarts de PV rendus visibles ; D10 ; les deux sens verts 3/3 avec 0 écart.
- 2026-10-19 (après-midi) : validation de Killian OK ; écran noir identifié (environnement de test) ; phase 2 fusionnée dans `main`.
- 2026-10-19 (14h30) : écran noir et ennemis qui ne touchent pas en coop corrigés (TEST_B ; hook Array_Random retiré) ; CI ~2 min.
