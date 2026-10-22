<div align="center">

# Expedition 33 · Coop en ligne

**Un vrai mode coopératif en ligne, de 2 à 3 joueurs, pour _Clair Obscur: Expedition 33_.**
Chacun joue sur son PC, avec son propre jeu. Pas de streaming vidéo : les parties se synchronisent par le réseau Steam.

![Exploration à deux dans les Vallons fleuris](docs/img/exploration.jpg)

<a href="https://github.com/kipavy/e33-coop/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/kipavy/e33-coop/build.yml?branch=phase-6&label=build" alt="Build"></a>
<img src="https://img.shields.io/badge/status-en%20d%C3%A9veloppement-f59e0b" alt="En développement">
<img src="https://img.shields.io/badge/joueurs-2%E2%80%933-3b82f6" alt="2 à 3 joueurs">
<img src="https://img.shields.io/badge/C%2B%2B-UE4SS-00599c?logo=cplusplus" alt="C++ UE4SS">
<img src="https://img.shields.io/badge/r%C3%A9seau-Steam%20P2P-171a21?logo=steam" alt="Steam P2P">
<a href="https://ko-fi.com/kipavy"><img src="https://img.shields.io/badge/Ko--fi-soutenir-ff5e5b?logo=ko-fi&logoColor=white" alt="Soutenir sur Ko-fi"></a>

</div>

> [!WARNING]
> **En développement.** Le mod est testé automatiquement sur un PC et en vraies parties à deux PC entre amis. Il n'est publié nulle part, il n'y a pas encore de version stable, et des bugs sont attendus. Mod non officiel, sans lien avec Sandfall Interactive ni Kepler Interactive.

---

## L'idée

Dans Expedition 33, le combat au tour par tour repose sur des **réflexes** : parades, esquives, sauts et timings de compétences. Si on les jouait à distance, la latence réseau les rendrait injouables. Ce mod fait donc tourner **le jeu complet chez chaque joueur** :

- **L'hôte décide de tout ce qui est aléatoire ou choisi par l'IA** : dégâts, critiques, actions ennemies, ordre des tours. Il diffuse des résultats, pas des touches.
- **Chaque joueur résout ses propres réflexes sur son PC, sans latence.** C'est vrai pour les parades et esquives de ses personnages comme pour leurs timings d'attaque et leurs tirs en visée libre. Le résultat part ensuite chez les autres.
- **Une empreinte de l'état du combat est comparée à chaque tour.** Au moindre écart, l'état de l'hôte est réappliqué.

## Ce que ça donne

<table>
<tr>
<td width="72%"><img src="docs/img/combat-repartition.jpg" alt="Combat à deux joueurs"></td>
<td><img src="docs/img/ordre-des-tours.png" alt="Ordre des tours avec l'initiale du joueur"><br><sub>Dans l'ordre des tours, un losange porte l'initiale du pseudo du joueur qui contrôle chaque héros (ici <b>K</b>illian et <b>T</b>OTOFF), avec une couleur par joueur.</sub></td>
</tr>
</table>

| | |
|---|---|
| ![Attaque en combat](docs/img/combat-attaque.jpg) | ![Lumière](docs/img/lumiere.jpg) |
| Chaque joueur joue les tours de ses personnages et pare les coups qui les visent. | Exploration : un pilote, les autres le suivent, d'une zone à l'autre. |

## Fonctionnalités

Les cases cochées passent les tests automatiques. « À valider » signifie : écrit et compilé, en attente d'une vraie partie à deux.

**Combat**
- [x] Ordre des tours, actions, compétences, objets, fuite et passage de tour synchronisés
- [x] Parades, esquives et sauts résolus par le joueur qui contrôle le héros
- [x] Timings des compétences et visée libre résolus localement, puis rejoués chez les autres
- [x] Attente du début de tour de l'hôte sans figer le jeu (plus de mini-freezes à la fin des compétences)
- [x] Vérification de l'état à chaque tour et réparation automatique des écarts
- [x] Récompenses de fin de combat identiques pour tous
- [x] Attaque surprise et équipe de l'hôte appliquées chez tous
- [x] Losanges de joueur dans l'ordre des tours
- [ ] Voir ses menus s'ouvrir en direct : roue d'actions, compétences, gradients, cible *(en cours)*
- [ ] Bonus « parade en duo » : +1 PA si chaque joueur pare la même attaque de groupe *(à valider)*
- [ ] Vue de la visée libre de l'autre joueur depuis l'épaule de son personnage *(à valider)*

**Exploration et session**
- [x] Rejoindre à tout moment : l'hôte envoie un instantané de sa partie, chargé dans un emplacement coop séparé
- [x] Pilote et suiveurs, changements de zone, dialogues et cinématiques avec point de rendez-vous
- [x] Nouvelle partie ensemble : chacun voit la cinématique d'introduction
- [x] Pause partagée en combat, avec le nom de qui a mis en pause
- [x] Départs et retours de joueurs : l'hôte reprend les héros d'un joueur parti
- [x] Écrans d'attente qui disent ce que fait l'autre (« En attente de TOTOFF, qui regarde une cinématique »)
- [x] Sauvegarde côté client : copie à jour de la partie commune, hébergeable avec **F10**, sans toucher aux sauvegardes perso
- [ ] Bouton « Rejoindre *ami* » dans le menu principal *(à valider)*
- [ ] Panneau « Coop » dans le menu pause : qui joue qui, raccourcis *(à valider)*

**Réseau**
- [x] Steam Networking Messages : P2P avec relais Steam, lobby « amis seulement », invitation par l'overlay
- [x] Vérification des versions du jeu, du mod et du protocole, avec un message clair en cas d'écart
- [ ] Mode test à distance pour le développement : commandes en liste fermée, journaux renvoyés par Steam *(à valider)*

## Installer

Il faut **la même version du jeu** (Steam) et **la même version du mod** chez tous les joueurs.

1. Récupérer le paquet joueur `e33coop-player.zip` et le décompresser.
2. Jeu fermé, dans le dossier décompressé :
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
   ```
   Le script installe UE4SS et le mod dans le dossier `Win64` du jeu. Rien d'autre n'est modifié.
3. Options de lancement Steam (*Propriétés › Options de lancement*) :

   | Rôle | Options |
   |---|---|
   | Hôte | `-e33role=host -e33transport=steam` |
   | Ami | `-e33role=client -e33transport=steam -e33join=1` |

Pour désinstaller, lancer `uninstall.ps1` depuis le même dossier, puis vider les options de lancement.

## Jouer

1. **L'hôte** charge sa partie. Un message indique que la partie est ouverte au coop. **F7** ouvre l'invitation Steam.
2. **L'ami** accepte l'invitation, ou appuie sur **F7** au menu principal. Il entre dans la partie au prochain moment calme : hors combat, cinématique et chargement.

| Touche | Qui | Effet |
|---|---|---|
| **F7** | hôte | Inviter des amis Steam |
| **F7** | ami, au menu | Rejoindre la partie coop d'un ami |
| **F8** | tous | Prendre la main en exploration (devenir le pilote) |
| **F5** | hôte, hors combat | Choisir un personnage |
| **F6** | hôte, hors combat | Donner ce personnage au joueur suivant |
| **F10** | au menu, rôle hôte | Héberger la copie coop gardée par le client |

Les raccourcis sont aussi rappelés dans le menu pause.

<details>
<summary>Options avancées</summary>

| Option | Effet |
|---|---|
| `-e33lang=fr` / `en` | Langue des textes du mod (par défaut : celle de Windows) |
| `-e33turndep=0` | Revenir à l'ancienne attente bloquante en début de tour |
| `-e33duoparry=0` | Désactiver le bonus « parade en duo » (côté hôte) |
| `-e33aimcam=0` | Désactiver la vue de visée libre depuis l'épaule |
| `-e33turntimer=N` | Minuteur de tour de N secondes (désactivé par défaut) |

</details>

## Sous le capot

```
native/
  hooks/       accès au jeu : appels Blueprint, lecture des propriétés, crochets avant/après
  net/         lien fiable sur UDP ou Steam Networking Messages, lobbies Steam
  protocol/    messages versionnés et validés (un message invalide est ignoré, jamais fatal)
  sync/        combat (coop.cpp), exploration et session (explore.cpp), builds, copie coop, présence
  ui/          textes FR/EN, cartes d'information du jeu, losanges et panneau pause
  remote/      mode test à distance (canal Steam séparé, commandes en liste fermée)
  testrunner/  joueur automatique piloté par des scénarios JSON
tools/
  fakepeer/    faux hôte ou faux client qui rejoue des parties enregistrées
  run-scenario/ lancement d'un scénario, batterie de non-régression
  package/     paquet joueur, installation et désinstallation
```

- **Pas de compilation locale.** Chaque push est compilé par GitHub Actions (mod, outils, tests unitaires).
- **Tests en vrai jeu.** Un scénario lance le jeu, un faux joueur rejoue une partie enregistrée, et le joueur automatique combat, pare, vise et explore. Le scénario vérifie ensuite des dizaines de compteurs : écarts d'état, tirages aléatoires, récompenses, attentes. La batterie compte plus de 40 scénarios.
- **Documentation :**
  - [`docs/PROGRESS.md`](docs/PROGRESS.md) : l'état du projet ;
  - [`docs/DECISIONS.md`](docs/DECISIONS.md) : les décisions ;
  - [`docs/GAME-INTERNALS.md`](docs/GAME-INTERNALS.md) : les fonctions du jeu utilisées ;
  - [`docs/REMOTE-TEST.md`](docs/REMOTE-TEST.md) : le mode test à distance.

## Soutenir

Le projet vous plaît ? Un café aide à le faire avancer :

<a href="https://ko-fi.com/kipavy"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Soutenir sur Ko-fi"></a>

---

<sub>Projet personnel entre amis. Aucun fichier du jeu n'est inclus ni modifié : le mod s'appuie sur <a href="https://github.com/UE4SS-RE/RE-UE4SS">UE4SS</a>. Les captures viennent des tests automatiques.</sub>
