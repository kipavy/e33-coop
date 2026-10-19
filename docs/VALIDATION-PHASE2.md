# Validation en jeu — fin de phase 2

> **Faite par Killian le 2026-10-19 (12:16–12:25)** sur la save de Lumière : tout bon, sauf un écran noir pendant les attaques ennemies. Cause trouvée, en deux parties :
> 1. le combat était forcé dans le hub de Lumière, où le jeu n'en fait pas (nouvelle save de test `TEST_B`, dans les Prairies) ;
> 2. un hook de diagnostic cassait le ciblage des ennemis en coop (retiré).
>
> La checklist ci-dessous est la version mise à jour (Prairies, Lancelier, **parades**). Elle peut servir à revérifier en 10 minutes.

But : vérifier à l'écran ce que les tests automatiques mesurent déjà. Une défense est résolue sur l'instance du joueur, sans latence, et l'hôte l'applique. Les deux jeux jouent exactement le même combat.

Limite : un seul PC et un seul compte Steam, donc l'autre côté est toujours le faux pair (`fakepeer`).

## Avant de commencer
- Ferme le jeu s'il tourne. Ne touche pas au PC pendant le chargement (environ 1 min) : la fenêtre doit pouvoir prendre le focus.
- `cd ~/dev/e33-coop`
- Le lanceur réinstalle UE4SS et le mod, et repasse tes saves en lecture seule après copie. Pour rejouer normalement ensuite : `powershell -NoProfile -File tools/reset-env/vanilla.ps1`.

## 1. Hôte réel face au faux client (≈ 1 min 30, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_host_vs_fakeclient -Package .tools/artifacts/39251250001/e33coop-mod
```
Verso appartient au **faux client** : ton jeu est l'hôte et applique ce que le client lui envoie.
- [ ] Combat contre le Lancelier chromatique dans les Vallons fleuris, avec un rendu normal (pas d'écran noir, caméra pas sous la carte).
- [ ] Au 1er tour du Lancelier : Verso **pare le 1er coup** (puis contre-attaque, « Verso effectue un contre ») et **prend le 2e**.
- [ ] Pas de gel au moment des impacts. Rapport **PASS**.

## 2. Client réel face au faux hôte (≈ 1 min 30, tu regardes)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_client_vs_fakehost -Package .tools/artifacts/39251250001/e33coop-mod
```
- [ ] Même combat qu'au test 1 (mêmes tours, même schéma parade puis coup pris).
- [ ] **PASS**, avec `hp_overrides = 0`.

## 3. C'est toi qui pares, côté client (≈ 3 min, tu joues)
```powershell
powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_manual_client -Package .tools/artifacts/39251250001/e33coop-mod
```
Le son est activé. Le mod joue les attaques de Verso ; **tu ne fais que les défenses** (touche **Parer**, `E` par défaut). Le Lancelier attaque à ses tours.
- Clique une fois dans la fenêtre du jeu au début du combat.
- [ ] La parade part **instantanément**, sans différence avec le solo.
- [ ] Une parade réussie déclenche le contre de Verso.
- [ ] Note ce que tu as fait sur chaque coup (par ex. « réussi, raté ») : je le comparerai aux logs.
- `hp_overrides` peut être > 0 : normal si tes parades diffèrent de la session enregistrée (non bloquant).

## En cas de souci
- Un crash ferme tout seul sa boîte « Fatal error » ; texte et dump dans `runs/<date>-<scénario>/`.
- Les runs prennent une capture toutes les 4 s (`runs/<date>-<scénario>/shots/`).
- Envoie-moi la ligne « Dossier du run » de chaque test.
