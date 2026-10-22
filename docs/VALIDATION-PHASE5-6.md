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
Cela rend tes saves modifiables et retire l'environnement de test.

**Toi et ton ami** — télécharger `Installer.E33.Coop.exe` depuis la dernière release
(github.com/kipavy/e33-coop/releases/latest), double-clic, **Installer**. Il trouve le jeu tout seul (sinon *Choisir le
dossier du jeu…*), télécharge le paquet, vérifie son empreinte SHA-256 et copie UE4SS et le mod dans le dossier du jeu.
Il ne touche à aucune save. Avant la première release : prendre l'artefact `release-dry-run` (workflow `release` lancé
à blanc) et poser `e33coop-player.zip` + `SHA256SUMS` à côté de l'exe (installation hors ligne).

**Plus d'option de lancement** : vider celles d'avant (`-e33role…`) dans Steam > Expedition 33 > Propriétés.
Les deux installations doivent être de la même version : sinon le refus dit qui doit mettre à jour (touche **U** au
menu, ou mise à jour automatique).

## 1. Inviter et rejoindre (≈ 3 min)
- [ ] **Toi** : lance le jeu et charge ta partie. Un message rappelle « Coop : F7 pour ouvrir votre partie à vos amis Steam ».
- [ ] **Toi** : **F7**. Bandeau « Partie ouverte au coop », puis la fenêtre d'invitation de Steam s'ouvre : invite ton ami.
- [ ] **Ton ami** : accepte l'invitation, jeu fermé ou ouvert (au menu principal). Autres possibilités : bouton
      **Rejoindre <toi>** au menu principal, ou **F7** au menu. Invitation acceptée en pleine partie : message « revenez au
      menu principal », puis il rejoint seul en y revenant.
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
      principal. Ensuite, chacun de vous peut de nouveau inviter (F7 en partie) ou rejoindre (menu) sans relancer le jeu.

## 5. Copie coop chez ton ami (≈ 5 min)
- [ ] Après quelques minutes de partie ensemble, chez **ton ami** : le dossier
      `%LOCALAPPDATA%\Sandfall\Saved\SaveGames\e33coop\copies\` contient `<ton nom>.sav` et `.json` (date récente).
      Ses saves perso (`SaveGames\<17 chiffres>\`) n'ont pas changé de date.
- [ ] Toi : menu pause > Écran titre. Lui, sans rien relancer : au menu principal, « Coop : F10 pour héberger la partie
      coop de <toi> (copie du JJ/MM HH:MM) ». **F10** : la partie se charge à l'endroit où vous étiez.
- [ ] Il joue 2 minutes (un coffre, un combat), revient au menu principal par le menu pause, relance F10 : le coffre est
      ouvert, le combat gagné. Ses saves perso n'ont toujours pas changé.
- [ ] Optionnel : toi, au menu, **Rejoindre <lui>** (ou F7) : tu rejoins sa partie (la copie).

## 6. Panneau Coop, options, mises à jour (≈ 5 min)
- [ ] **Toi**, en partie avec ton ami, hors combat : Échap. Panneau « Expédition coop » à droite, avec une section
      **Options**. **F9** : la première ligne (un personnage) est surlignée ; **Maj+F9** : le personnage passe à ton ami
      (chez lui : « … est maintenant contrôlé par … »). **F9** encore : on descend jusqu'aux options ; **Maj+F9** change la
      valeur (enregistrée dans `ue4ss\Mods\E33Coop\settings.ini`). Les flèches et Entrée du menu pause marchent comme avant.
- [ ] **Ton ami** : même panneau, options « Parade en duo (hôte) » et « Minuteur de tour (hôte) » en « choisi par l'hôte ».
- [ ] Hors du menu pause, **F9** : une carte montre la ligne choisie ; elle disparaît seule après 20 s.
- [ ] Mise à jour (si une release plus récente que la vôtre existe) : au menu principal, rien à faire en `updates = auto`
      (message « Mise à jour X prête : installée à la fermeture du jeu ») ; après avoir quitté le jeu, l'installateur la
      pose seul ; au lancement suivant, `ue4ss\Mods\E33Coop\version.txt` porte la nouvelle version. En `updates = ask` :
      carte « … U pour mettre à jour », U : le jeu se ferme, l'installateur met à jour et relance le jeu.

## Après
- Chacun, pour désinstaller : `Installer.E33.Coop.exe` > **Désinstaller** (ou `uninstall.ps1` du zip).
- Journaux, si quelque chose cloche : `<jeu>\Sandfall\Binaries\Win64\ue4ss\Mods\E33Coop\logs\<date-heure>\`, un dossier par
  lancement. Envoie-moi les deux dossiers de la session, le tien et celui de ton ami.

## Limites connues
- Réattribution des personnages : touche F6 de l'hôte, pas de vrai menu.
- Suiveur : un clic de souris sur une réponse de dialogue n'est pas bloqué (le clavier et la manette le sont).
- Une dépense faite au moment exact d'un événement partagé (coffre, combat) peut être reprise par la resynchro.
- Pas de migration d'hôte : si l'hôte part, la session se termine (cahier des charges).
