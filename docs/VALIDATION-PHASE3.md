# Validation en jeu — fin de phase 3

But : vérifier à l'écran ce que la batterie automatique mesure déjà (12 scénarios verts). Un joueur voit les héros des autres agir comme eux : mêmes actions, mêmes QTE, mêmes tirs en visée libre, mêmes parades, avec environ 1 s de décalage. Ses propres QTE et parades restent instantanés.

Limite : un seul PC et un seul compte Steam, donc l'autre joueur est toujours le faux pair (`fakepeer`).

Durée : environ 15 minutes.

## Avant de commencer
- Ferme le jeu s'il tourne. Ne touche pas au PC pendant le chargement (environ 1 min 30) : la fenêtre doit pouvoir prendre le focus.
- `cd ~/dev/e33-coop`
- Paquet : `.tools/artifacts/39276875178/e33coop-mod` (artefact CI déjà téléchargé, build du commit en tête de la branche `phase-3`).
- Le lanceur réinstalle UE4SS et le mod, et repasse tes saves en lecture seule après copie. Pour rejouer normalement ensuite : `powershell -NoProfile -File tools/reset-env/vanilla.ps1`.

## 1. Tu es l'hôte et tu joues Lune et Maelle (≈ 10 min, tu joues)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p3_manual_host -Package .tools/artifacts/39276875178/e33coop-mod
```
Combat contre le Lancelier chromatique, équipe Verso, Maelle, Lune. **Verso appartient au joueur 2** (le faux client). Tu joues Lune et Maelle avec la manette ou le clavier, comme d'habitude. Le son est activé. Clique une fois dans la fenêtre au début du combat.

Pendant les tours de **Verso** (joueur 2) :
- [ ] Tu ne peux rien choisir pour lui : sa roue d'actions ne réagit pas.
- [ ] Il vise le Lancelier en **visée libre**, tire 3 fois (3 impacts), annule la visée, puis attaque. Chaque étape arrive avec environ 1 s de décalage sur le joueur 2, sans à-coups.
- [ ] Au tour suivant, il lance **Combo1** : le 1er QTE est **parfait** (retour visuel du parfait), le 2e est **raté**.
- [ ] Quand le Lancelier frappe Verso, il **pare un coup sur deux** tout seul.

Pendant tes tours (Lune, Maelle) :
- [ ] Les menus, les compétences, les objets et la visée réagissent normalement. Tes QTE (appui au bon moment) donnent le même retour qu'en solo.
- [ ] Quand le Lancelier te vise, ta parade (touche **Parer**, `E` par défaut) part instantanément, comme en solo.
- [ ] Si la jauge gradient est pleine, essaie une **attaque gradient** avec Lune (facultatif).

Fin :
- [ ] Attention, Maelle (1 PV) inflige des dégâts énormes : garde son attaque pour finir le combat.
- [ ] Écran de victoire avec les récompenses, puis le rapport affiche **PASS**.
- Note ce que tu as vu d'anormal (moment, héros) : je le retrouverai dans les logs.

## 2. Tu es le client : rejeu de la visée libre (≈ 2 min, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p3_freeaim_client -Package .tools/artifacts/39276875178/e33coop-mod
```
Ton jeu est le client. Il joue Verso tout seul (vise et tire 3 fois, puis attaque), et rejoue Lune, jouée par l'hôte enregistré.
- [ ] Les tirs de Verso touchent le Lancelier. Les tours de Lune s'enchaînent sans attente visible.
- [ ] **PASS**. Les vérifications incluent `raw_hash_diffs = 0` : aucune correction n'a été nécessaire, ton jeu a calculé exactement le même combat que l'hôte.

## 3. Équipe de 3, mort et réanimation, côté client (≈ 3 min, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p3_party_client -Package .tools/artifacts/39276875178/e33coop-mod
```
- [ ] Au 1er tour, Verso et Maelle tombent (réglage du test). Maelle se relève seule (pictos). Lune réanime Verso avec une teinte à son premier tour.
- [ ] Maelle (ton héros) boit des teintes d'énergie ou passe son tour. Le Lancelier vise différents héros.
- [ ] **PASS**.

## En cas de souci
- Un crash ferme tout seul sa boîte « Fatal error » ; texte et dump dans `runs/<date>-<scénario>/`.
- Les runs prennent des captures périodiques (`runs/<date>-<scénario>/shots/`).
- Envoie-moi la ligne « Dossier du run » de chaque test.

## Batterie automatique (pour info, ≈ 35 min, sans toi)
```powershell
powershell -NoProfile -File tools/run-scenario/battery.ps1 -Package .tools/artifacts/39276875178/e33coop-mod
```
