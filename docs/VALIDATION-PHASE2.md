# Validation en jeu — fin de phase 2 (≈ 15 minutes)

But : vérifier à l'écran ce que les tests automatiques mesurent déjà. Une défense (ici le **saut**) est résolue sur l'instance du joueur, sans latence, et l'hôte l'applique. Les deux jeux jouent exactement le même combat.

Limite connue : un seul PC et un seul compte Steam, donc pas de partie entre deux vrais jeux. L'autre côté est toujours le faux pair (`fakepeer`). La vraie partie à deux viendra plus tard (phase 4, puis Steam en phase 6).

## Avant de commencer (1 min)
- Ferme le jeu s'il tourne. Ne touche pas au PC pendant le chargement (environ 1 min 30) : la fenêtre du jeu doit pouvoir prendre le focus.
- Ouvre PowerShell dans le repo :
  ```powershell
  cd ~/dev/e33-coop
  ```
- Le lanceur réinstalle tout seul UE4SS et le mod, et remet tes saves en lecture seule après en avoir fait une copie.

## 1. Hôte réel face au faux client (2 min, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_host_vs_fakeclient -Package .tools/artifacts/39229229182/e33coop-mod
```
Ici, Verso appartient au **faux client** : ton jeu est l'hôte et applique ce que le client lui envoie.
- [ ] Verso attaque tout seul à chacun de ses tours (actions envoyées par le client).
- [ ] Au tour de l'ennemi (combo « terre » à 3 coups) : Verso **saute au 1er et au 3e coup**, et **prend le 2e**.
- [ ] Aucun gel ni saccade au moment des impacts (l'hôte attend le résultat du client sur ces coups).
- [ ] Le rapport affiché à la fin dit **PASS**.

## 2. Client réel face au faux hôte (2 min, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_client_vs_fakehost -Package .tools/artifacts/39229229182/e33coop-mod
```
Ton jeu est maintenant le **client** : il rejoue le combat de l'hôte (enregistré) et résout ses défenses lui-même.
- [ ] Le combat se déroule exactement comme au test 1 : mêmes tours, même attaque ennemie, même schéma saut / coup pris / saut.
- [ ] **PASS**, avec `hp_overrides = 0` (les PV du client n'ont jamais eu besoin d'être corrigés par l'hôte).

## 3. C'est toi qui défends, côté client (5 min, tu joues)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_manual_client -Package .tools/artifacts/39229229182/e33coop-mod
```
Le son est activé. Le mod joue les attaques de Verso pour toi ; **tu ne fais que les défenses**. L'ennemi fait son combo sautable à 3 coups (un seul tour ennemi par combat).
- Clique une fois dans la fenêtre du jeu quand le combat commence, pour qu'elle ait le focus.
- **Saute** avec ta touche de saut habituelle. Essaie de réussir certains coups et d'en rater exprès au moins un.
- [ ] Le saut part **instantanément**, sans aucune différence de ressenti avec le jeu solo.
- [ ] Le résultat affiché (esquivé / touché) correspond à ce que tu as fait.
- [ ] Note dans ta réponse ce que tu as fait sur chacun des 3 coups (par ex. « réussi, raté, réussi »). Je le comparerai aux logs : le résultat envoyé à l'hôte doit être le même.
- Normal ici : le rapport peut montrer `hp_overrides` > 0. Le faux hôte rejoue une session où les sauts étaient « réussi, raté, réussi » ; si tu fais autrement, ses PV diffèrent des tiens et il les impose, comme le ferait un vrai hôte. Ces lignes sont marquées « any » et ne font pas échouer le test.

## En cas de souci
- Si le jeu plante, la boîte « Fatal error » se ferme toute seule. Le texte et le dump atterrissent dans `runs/<date>-<scénario>/`.
- Chaque run produit `runs/<date>-<scénario>/report.md`. Copie-moi simplement la dernière ligne « Dossier du run » de chaque test : je lirai les logs.

## Après la validation
Tes saves restent en lecture seule tant que les tests continuent. Pour rejouer normalement entre-temps, dis-le-moi et je les remets en écriture (le jeu doit aussi redevenir vanilla : je retire `dwmapi.dll` et `ue4ss/`).
