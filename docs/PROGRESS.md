# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-18, soir)

**Phase 0 — exploration : quasi finie** (branche `phase-0`). Reste : voir le mod C++ logger un vrai événement de combat (build CI en cours).
**Phase 1 — environnement de test autonome : commencée** (branche `phase-1`).

### Fait
- Saves perso sauvegardées (`~/dev/e33-coop-backups/saves-original-20261018-215915.zip` + SHA256) et passées en **lecture seule** ; vérifiées intactes après la session d'exploration.
- Jeu installé : build Steam **23765773**, exe SHA256 `218a2478…639618` (UE 5.4.4 CL-68322). Inventaire du dossier du jeu propre : `~/dev/e33-coop-backups/game-baseline-23765773/`.
- **UE4SS experimental 1181 (`83330635`) fonctionne** sur ce build ; dumps objets / CXX / UHT / usmap / types Lua dans `.tools/dumps/23765773/` (local, jamais commité).
- **Mod Kouzukii étudié → pas de fork** (D1, `research/kouzukii-study.md`).
- `docs/GAME-INTERNALS.md` : carte complète (combat de bout en bout, aléatoire, QTE, saves, transitions, boot) + § 15 « vérifié en jeu ».
- En jeu, via le mod Lua `E33Explore` (télécommande) : chargement d'une save de test hors Steam Cloud, déclenchement d'un combat par script, attaque automatique, séquences de hooks d'une attaque héros et d'une attaque ennemie.
- Build C++ : RE-UE4SS en sous-module épinglé, CI GitHub Actions (repo privé, compte GitHub lié à Epic par Killian, secret `UEPSEUDO_PAT`).
- Phase 1 (code écrit, pas encore testé) : test runner C++ (machine à états menu → save → monde → combat → fin, actions auto, parade réussie/ratée sur commande par coup reçu, dump d'état par tour, `result.json`, code de sortie), `tools/run-scenario/run-scenario.ps1`, `tools/reset-env/reset-env.ps1`, scénario `tests/scenarios/solo_parry_basic.json`.

### En cours
- Premier build CI complet (UE4SS + proxy + E33Coop ; ~30 min sans cache).

### Bloqué
- Rien.

### Prochaines étapes
1. Installer l'artefact CI, vérifier `mod_init` + hooks dans `runs/.../e33coop-solo.jsonl` (fin de phase 0).
2. Faire passer `run-scenario solo_parry_basic` ; vérifier que `TryStartParry` dans le pré-hook suffit à réussir une parade sans forcer le verdict.
3. Trois runs d'affilée identiques → fin de phase 1.
4. Tester `-nullrhi` (D6) et la résolution de fenêtre.

## Historique
- 2026-10-18 : démarrage ; phase 0 (UE4SS, dumps, étude Kouzukii, internals, décisions D1–D7) ; début phase 1.
