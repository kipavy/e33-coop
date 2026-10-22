# Validation à deux PC — fin des phases 5 (robustesse) et 6 (transport Steam)

But : jouer une vraie session à deux par Steam (P2P + relais), toi en hôte et ton ami en client, et vérifier à l'écran ce
que les scénarios mesurent déjà seuls : invitation, rejoindre, combat à deux, builds et ressources communes, départ et retour d'un
joueur, départ de l'hôte.

Durée : environ 20 minutes de jeu, plus 5 minutes d'installation.

État : BUILD_ID (branche `phase-6`). Batterie complète : BATTERY_RESULT. Le transport Steam est vérifié seul (`p6_steam_selftest` :
messages P2P à son propre compte) ; lobby, invitation et liaison entre deux comptes ne peuvent se tester qu'à deux.

## Installation (5 min, chacun sur son PC)

**Toi (hôte)** — dans `~/dev/e33-coop` :
```powershell
powershell -NoProfile -File tools/reset-env/vanilla.ps1
```
Cela rend tes saves modifiables et retire l'environnement de test. Puis envoie à ton ami le fichier
`.tools\e33coop-player.zip` (≈ 14 Mo).

**Toi et ton ami** — dézipper `e33coop-player.zip`, puis dans le dossier dézippé :
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
```
Le script trouve le jeu tout seul (sinon : `-GameDir "<...>\steamapps\common\Expedition 33"`). Il copie UE4SS et le mod dans
le dossier du jeu. Il ne touche à aucune save.

Ensuite, options de lancement Steam (Steam > Expedition 33 > Propriétés > Options de lancement) :
- toi : `-e33role=host -e33transport=steam`
- ton ami : `-e33role=client -e33transport=steam -e33join=1`

Les deux installations doivent être identiques : le jeu refuse la connexion sinon, avec un message qui dit ce qui diffère.

## 1. Inviter et rejoindre (≈ 3 min)
- [ ] **Toi** : lance le jeu et charge ta partie. Une dizaine de secondes après, un bandeau en haut indique « Partie ouverte au
      coop : F7 pour inviter un ami ».
- [ ] **Toi** : **F7**. La fenêtre d'invitation de Steam s'ouvre : invite ton ami.
- [ ] **Ton ami** : accepte l'invitation, jeu fermé ou ouvert. Autre possibilité : il lance le jeu et appuie sur **F7** au menu
      principal (il rejoint la partie coop d'un ami).
- [ ] Chez lui : « Connexion à la partie de l'hôte… », puis « Chargement de la partie de l'hôte… ». Sa partie se charge
      seule, il apparaît à côté de toi.
- [ ] Chez toi : « <son nom Steam> a rejoint la partie ».

## 2. Exploration et combat (≈ 6 min)
- [ ] Tu marches : son personnage te suit. **Lui** ne peut ni bouger ni interagir (touche E). **F8** chez lui : il prend la
      main, tu le suis.
- [ ] Engagez un combat : vous y entrez **ensemble**. Pendant le chargement du plus lent, l'autre voit « En attente des autres
      joueurs… ».
- [ ] Bandeau « **Tour de <nom>** » pendant les tours de l'autre joueur, « **À vous de jouer** » pendant les tiens. Ton ami
      contrôle le **2e personnage** de ton équipe. **F6** chez toi, hors combat : change le joueur d'un personnage.
- [ ] Il met le jeu en pause (Échap) : chez toi, « **Pause demandée par <nom>** », le combat s'arrête puis reprend.

## 3. Builds et ressources communes (≈ 4 min)
- [ ] **Ton ami** ouvre le menu (Tab) et modifie **son** personnage (un Lumina, un attribut, une compétence), puis ferme le
      menu. Chez toi : « <nom> a modifié <personnage> », et la modification est visible dans ton menu.
- [ ] Il essaie de modifier **un de tes** personnages : à la fermeture du menu, c'est annulé chez lui (« … est contrôlé par … :
      modification annulée »).
- [ ] L'un de vous **dépense** (achat chez un marchand, amélioration d'arme au camp) : l'or et les objets de l'autre changent pareil.

## 4. Départ et retour d'un joueur, départ de l'hôte (≈ 5 min)
- [ ] Pendant un combat, **ton ami ferme brutalement son jeu** (Alt+F4 ou Gestionnaire des tâches). Au bout de 15 s environ,
      chez toi : « Connexion perdue avec <nom> : l'hôte reprend ses personnages », puis tu joues aussi son personnage.
- [ ] Il relance le jeu et rejoint (F7 au menu) : « **En attente de la fin du combat de l'hôte…** », puis il rentre dans la
      partie à la fin du combat.
- [ ] **Toi** : menu pause > Écran titre. Chez lui : « L'hôte a quitté la partie. Session terminée. », puis retour au menu
      principal.

## 5. Copie coop chez ton ami (≈ 5 min)
- [ ] Après quelques minutes de partie ensemble, chez **ton ami** : le dossier
      `%LOCALAPPDATA%\Sandfall\Saved\SaveGames\e33coop\copies\` contient `<ton nom>.sav` et `.json` (date récente).
      Ses saves perso (`SaveGames\<17 chiffres>\`) n'ont pas changé de date.
- [ ] Toi : menu pause > Écran titre. Lui : options de lancement `-e33role=host -e33transport=steam`, relance le jeu.
      Au menu principal : « Coop : F10 pour héberger la partie coop de <toi> (copie du JJ/MM HH:MM) ». **F10** : la partie
      se charge à l'endroit où vous étiez.
- [ ] Il joue 2 minutes (un coffre, un combat), revient au menu principal par le menu pause, relance F10 : le coffre est
      ouvert, le combat gagné. Ses saves perso n'ont toujours pas changé.
- [ ] Optionnel : toi en `-e33role=client`, F7 au menu : tu rejoins sa partie (la copie).
- Puis remettre ses options de lancement client.

## Après
- Chacun : `powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1` dans le dossier dézippé, puis vider les options
  de lancement Steam.
- Journaux, si quelque chose cloche : `<jeu>\Sandfall\Binaries\Win64\ue4ss\Mods\E33Coop\logs\<date-heure>\`, un dossier par
  lancement. Envoie-moi les deux dossiers de la session, le tien et celui de ton ami.

## Limites connues
- Réattribution des personnages : touche F6 de l'hôte, pas de vrai menu.
- Suiveur : un clic de souris sur une réponse de dialogue n'est pas bloqué (le clavier et la manette le sont).
- Une dépense faite au moment exact d'un événement partagé (coffre, combat) peut être reprise par la resynchro.
- Pas de migration d'hôte : si l'hôte part, la session se termine (cahier des charges).
