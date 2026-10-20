# Validation en jeu — fin de phase 4 (session et exploration)

But : vérifier à l'écran ce que la batterie mesure déjà (scénarios p2, p3 et p4 verts) : rejoindre une partie, suivre le pilote, transitions de niveau, combat lancé depuis l'exploration, dialogues, cinématique avec barrière, pause en combat.

Limite : un seul PC et un seul compte Steam, donc l'autre joueur est toujours le faux pair (`fakepeer`).

Durée : environ 18 minutes.

État : batterie complète **pas repassée** (à ta demande). Verts sur ce code : `p4_session_host`, `p4_session_client`, `p4_pilot_host`, `p4_pause_host`, `p3_actions_client`. Le correctif du délai de barrière (10 min) n'a pas encore été revérifié par `p4_cinematic_client` (test 3 ci-dessous).

## Avant de commencer
- Ferme le jeu s'il tourne. Ne touche pas au PC pendant le chargement (environ 1 min 30) : la fenêtre doit pouvoir prendre le focus.
- `cd ~/dev/e33-coop`
- Paquet : `.tools/artifacts/39439326222/e33coop-mod` (artefact CI déjà téléchargé, build du commit en tête de la branche `phase-4`).
- Le lanceur réinstalle UE4SS et le mod, et repasse tes saves en lecture seule après copie. Pour rejouer normalement ensuite : `powershell -NoProfile -File tools/reset-env/vanilla.ps1`.
- Tes saves ne sont jamais ouvertes en écriture : l'instantané de l'hôte est écrit dans `SaveGames\e33coop\`, un dossier à part, vidé avant chaque test.

## 1. Tu es l'hôte et le pilote (≈ 10 min, tu joues)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p4_manual_host -Package .tools/artifacts/39439326222/e33coop-mod
```
Save TEST_B (Vallons fleuris, Verso). Le faux client rejoint ta partie quelques secondes après le chargement : il reçoit l'instantané de ta partie et suit ton personnage. Le son est activé. Clique une fois dans la fenêtre au début.

- [ ] Le chargement et le déplacement se passent comme en solo : aucun à-coup visible quand le faux client rejoint (instantané de ta partie pris à ce moment-là).
- [ ] Va au contact d'un ennemi du monde (n'importe lequel) : le combat démarre normalement.
- [ ] Pendant le combat, ouvre le **menu pause** (Échap / Start) puis reprends. Fais-le **deux fois**.
- [ ] Finis le combat (victoire ou fuite) : retour à l'exploration normal.
- [ ] Prends un **drapeau d'expédition** et va au **camp** (changement de niveau).
- [ ] Au camp, **parle à un compagnon** et réponds à une question (Oui ou Non).
- [ ] Appuie sur **F9** (fenêtre du jeu active) : le rapport affiche **PASS**. Il vérifie : instantané envoyé, client entré, combat d'exploration, menu pause, changement de niveau et interaction publiés.

## 2. Tu es le client : rejoindre et suivre (≈ 3 min, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p4_session_client -Package .tools/artifacts/39439326222/e33coop-mod
```
Ton jeu démarre au menu principal, puis rejoint la partie d'un hôte enregistré (rejeu de `p4_session_host`, au rythme réel).
- [ ] Depuis le menu, la partie de l'hôte se charge toute seule (Vallons fleuris) en quelques secondes.
- [ ] Ton personnage **marche tout seul** derrière le pilote, avec l'animation de marche, sans glisser ni trembler.
- [ ] Un combat démarre contre le **même groupe d'ennemis**, puis le héros fuit.
- [ ] Changement de niveau vers le **camp**, puis dialogue avec **Lune**. La réponse « **Non** » est choisie toute seule, et le dialogue se termine.
- [ ] **PASS**.

## 3. Cinématique avec barrière, côté client (≈ 6 min, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p4_cinematic_client -Package .tools/artifacts/39439326222/e33coop-mod
```
Save de l'hôte : TEST_A (Lumière, acte 3). L'hôte enregistré regarde la cinématique d'intro du boss Renoir (≈ 5 min, impossible à passer).
- [ ] Après le chargement, ton jeu se **met en pause** : l'hôte est dans sa cinématique, tu ne peux pas avancer.
- [ ] Environ 5 min plus tard, quand l'hôte a fini sa cinématique, ton jeu **reprend**.
- [ ] **PASS**.

## Prendre la main (facultatif, ≈ 1 min)
Pendant le test 2, appuie sur **F8** : ton jeu demande la main à l'hôte. Le faux hôte rejoue un enregistrement et ne la donne pas, donc il ne doit rien se passer à l'écran. La demande apparaît dans le journal (`coop_pilot_request`). La prise de main réelle est vérifiée par `p4_pilot_host` : le faux client prend la main et marche, et ton jeu, hôte, le suit à moins de 250 u.

## En cas de souci
- Un crash ferme tout seul sa boîte « Fatal error » ; texte et dump dans `runs/<date>-<scénario>/`.
- Les runs prennent des captures périodiques (`runs/<date>-<scénario>/shots/`).
- Envoie-moi la ligne « Dossier du run » de chaque test.

## Batterie automatique (pour info, ≈ 1 h, sans toi)
```powershell
powershell -NoProfile -File tools/run-scenario/battery.ps1 -Package .tools/artifacts/39439326222/e33coop-mod
```
Les scénarios `p4_*_client` ont besoin des instantanés locaux `.tools/testsaves/p4_session_snapshot.sav` et `p4_cinematic_snapshot.sav`. Ils ne sont jamais versionnés, car ce sont des données de tes saves. S'ils manquent, `.tools/pair4.sh p4_session` (ou `p4_cinematic`) les régénère avec l'enregistrement.
