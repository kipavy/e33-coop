# Journal d'avancement

> Lire en 2 minutes : état courant en haut, historique en bas.

## État courant (2026-10-19, 15h45) — **phase 3 en cours** (branche `phase-3`)

| Phase | État | Branche |
|---|---|---|
| 0 — exploration | **finie** | `phase-0` (fusionnée dans `main`) |
| 1 — environnement de test autonome | **finie** | `phase-1` (fusionnée dans `main`) |
| 2 — preuve de concept réseau | **finie** (validée en jeu le 2026-10-19) | `phase-2` (fusionnée dans `main`) |
| 3 — synchro complète du combat | **en cours** | `phase-3` |

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

### Tests (dernier état)
- `p2_host_vs_fakeclient` / `p2_client_vs_fakehost` : **verts** en protocole v4 (enregistrement refait), 0 écart brut.
- `p3_actions_host` : vert. `p3_actions_client` : a révélé l'écart de QTE ci-dessus (corrigé, à revérifier).
- `p3_party_*`, `p3_resync_client`, `p3_defeat_*` : écrits, pas encore lancés.

### En cours
- Revalider `p3_actions` avec l'appui rejoué, puis lancer la batterie (équipe de 3, resynchro, défaite).

### Reste à faire (phase 3)
- Visée libre (tirs de Verso), attaques gradient, contre-attaques gradient, mécaniques propres (Lune, Sciel, Monoco, Maelle).
- Combat plus long pour la couverture (rencontre à plusieurs ennemis).
- 3 joueurs (hôte + 2 faux clients).
- Checklist de validation en jeu.

### Bloqué
- Rien.

## Historique
- 2026-10-18 : démarrage ; phase 0 complète ; test runner écrit.
- 2026-10-19 (nuit) : phase 1 complète (3/3) ; phase 2 : protocole, fakepeer, module coop, premier combat hôte vs faux client sans désync. Pause.
- 2026-10-19 (matin) : reprise. Crash client au chargement corrigé ; fermeture auto des crashs ; écarts de PV rendus visibles ; D10 ; les deux sens verts 3/3 avec 0 écart.
- 2026-10-19 (après-midi) : validation de Killian OK ; écran noir identifié (environnement de test) ; phase 2 fusionnée dans `main`.
- 2026-10-19 (14h30) : écran noir et ennemis qui ne touchent pas en coop corrigés (TEST_B ; hook Array_Random retiré) ; CI ~2 min.
- 2026-10-19 (15h45) : phase 3 démarrée : D11, protocole v4, équipe imposée, compétences/objets/QTE/défenses relayés, état complet, ciblage, récompenses.
