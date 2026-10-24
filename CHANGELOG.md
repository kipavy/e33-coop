# Journal des versions

Toutes les évolutions visibles par les joueurs sont notées ici. C'est la **source unique** des notes de version :
le workflow de release recopie la section de la version publiée dans la release GitHub et refuse de publier si
elle manque.

Format : [Keep a Changelog 1.1.0](https://keepachangelog.com/fr/1.1.0/). Numérotation :
[versionnage sémantique](https://semver.org/lang/fr/) (`MAJEUR.MINEUR.CORRECTIF`) ; tant que la version est en
`0.x`, le MINEUR monte pour une nouveauté ou un changement de protocole, le CORRECTIF pour des corrections.

Rubriques, dans cet ordre, seulement si elles ont du contenu : **Ajouts**, **Modifications**, **Corrections**,
**Suppressions**, **Sécurité**. Une ligne par changement, écrite pour un joueur (ce qu'il voit ou ce qui change
pour lui), pas pour un développeur. Les changements internes (tests, outillage) ne sont pas listés.

## [Non publié]

## [0.6.18] - 2026-10-24

### Ajouts
- Dialogues : l'autre joueur voit en surbrillance la réponse que le meneur survole avant de choisir.

## [0.6.17] - 2026-10-24

### Corrections
- Combat : sur une attaque de zone, la parade de chaque joueur compte même quand le jeu attend un instant la parade
  de l'autre (elle pouvait être jugée ratée et faire perdre des PV).

## [0.6.16] - 2026-10-24

### Corrections
- Combat : une compétence lancée sur un allié (ex. « Puissant » de Gustave sur Lune) touche le même allié chez tous les
  joueurs ; chez l'autre joueur elle pouvait partir sur un ennemi, et le combat divergeait ensuite (PV différents,
  héros mort chez l'un et vivant chez l'autre).

## [0.6.15] - 2026-10-24

### Corrections
- Visée libre : les tirs de l'autre joueur sur un point faible (boule bleue) l'éclatent bien chez l'hôte (la 0.6.14
  pouvait encore la frôler sans la toucher).

## [0.6.14] - 2026-10-24

### Corrections
- Visée libre : un tir de l'autre joueur sur un point faible (boule bleue) l'éclate aussi chez l'hôte ; le tir rejoué
  pouvait la manquer de peu et partir dans le vide.

## [0.6.13] - 2026-10-24

### Corrections
- Combat : les tours ennemis démarrent en même temps chez tous les joueurs ; plus de petits gels pendant les parades
  de l'autre joueur (surtout sur les attaques de zone).
- Changements de zone : quand le meneur change à nouveau de zone pendant que l'autre joueur charge encore la
  précédente, celui-ci le rejoint directement (avant : il pouvait rester dans la mauvaise zone).
- Changements de zone : le perso de l'autre joueur ne prend plus de lui-même une sortie de zone en suivant le meneur ;
  il change de zone uniquement avec lui.
- Combat : les ennemis qui tirent leur comportement au sort au début du combat (ex. le Portier) font les mêmes attaques
  chez tous les joueurs (l'un pouvait voir une attaque de zone, l'autre une attaque sur un seul héros).

## [0.6.12] - 2026-10-24

### Corrections
- Combat : la roue de Monoco démarre sur la même case chez tous les joueurs (elle était tirée au hasard chez chacun
  et ne se recalait qu'au premier tour).
- Combat : les effets tirés au hasard au début du combat (équipement, butin préparé) sont identiques chez tous les
  joueurs ; si le butin diffère quand même, l'autre joueur reçoit les mêmes objets que l'hôte.

## [0.6.11] - 2026-10-24

### Corrections
- Menu principal : la version du mod s'affiche enfin, en petit en bas à droite, juste à gauche du numéro du jeu
  (elle était invisible).
- Combat : quand un autre joueur lance une compétence, une attaque ou un objet, ses menus ne s'ouvrent et ne se
  ferment plus plusieurs fois très vite juste avant l'action.

## [0.6.10] - 2026-10-24

### Corrections
- Combat : à la manette, la parade (RB) et le tir en visée libre (RT) de l'autre joueur marchent de nouveau. Ces
  boutons restaient pris par les commandes de déplacement de l'exploration, rendues par erreur au début du combat
  (l'esquive et le clavier n'étaient pas touchés).

## [0.6.9] - 2026-10-24

### Corrections
- Combat : parade et esquive de l'autre joueur à nouveau prises en compte quand le combat est lancé chez lui par le mod
  (ses appuis n'arrivaient pas au jeu).
- Exploration : plus de rechargement de la partie toutes les 10 à 20 s quand le meneur passe dans des zones du décor
  (l'autre joueur ne restait plus exclu d'un combat lancé pendant ce rechargement).

## [0.6.8] - 2026-10-24

### Corrections
- Pause : la partie de l'hôte n'est plus envoyée ni chargée pendant que l'autre joueur est dans le menu pause (le
  chargement y restait bloqué).
- Escalade : le perso de l'autre joueur ne reste plus accroché dans les airs en pose d'escalade quand le meneur a fini
  de grimper.
- Changement de personnage en exploration : l'autre joueur voit le même personnage que le meneur.

## [0.6.7] - 2026-10-23

### Corrections
- Rejoindre / resynchro : plus d'écran noir « Chargement de la partie de l'hôte » sans fin. La partie de l'hôte
  n'arrive plus à la fin d'un dialogue ; si le chargement bloque quand même, il est relancé puis redemandé tout seul.
- Combat lancé pendant une resynchro (ex. le mime du prologue) : l'autre joueur reste dans le combat de l'hôte au lieu
  de le jouer seul chez lui (il ne peut plus jouer un perso qui ne lui est pas attribué).
- Inventaire : les tenues et coiffures des éditions / DLC (présentes chez un joueur, absentes chez l'autre) ne
  provoquent plus de rechargement de la partie à chaque dialogue.

## [0.6.6] - 2026-10-23

### Modifications
- Version du mod au menu principal : en petit en bas à droite, à côté du numéro du jeu.

### Corrections
- Combat lancé par un dialogue (ex. duel contre Maelle au prologue) : les points d'action du premier tour ne sautent
  plus d'un point chez le joueur invité.
- Dialogues en même temps : un dialogue déclenché par le meneur en passant à un endroit (ex. Maelle avant le duel du
  prologue) démarre aussitôt chez les autres joueurs, au lieu de les laisser en attente.
- Exploration : quand le meneur court ou sprinte, le perso de l'autre joueur le suit de façon fluide (plus d'à-coups,
  d'arrêts ni de téléportations), et la partie n'est plus rechargée pendant qu'on se déplace.
- Duel du prologue contre Maelle en nouvelle partie ensemble : plus de blocage, l'autre joueur entre dans le combat
  même si le meneur est encore dans le dialogue.
- Combat : plus aucune attente sans fin. Un joueur qui charge, regarde une scène ou choisit son action est toujours
  attendu ; aucune action n'est jouée à sa place tant que son jeu montre qu'il est là.

## [0.6.5] - 2026-10-23

### Ajouts
- Cinématiques en même temps : quand le meneur déclenche une scène, elle démarre aussitôt chez les autres joueurs (au
  lieu de se jouer l'une après l'autre) ; passer une scène la passe chez tout le monde.
- Version du mod affichée au menu principal (sous les boutons) et en bas du panneau Coop (F9).
- Carte du monde : le perso de l'autre joueur monte sur Esquie avec le meneur et le suit en marche, en vol et sur
  l'eau (au lieu de « nager en l'air »).

### Modifications
- La bannière « Équipe : … » ne s'affiche plus que quand la répartition des personnages change (plus à chaque
  arrivée ou rechargement d'un joueur) ; le rappel « F7 pour ouvrir votre partie » une seule fois par lancement du jeu.

### Suppressions
- Bannières inutiles : « L'hôte commence une nouvelle partie… » (client), « Nouvelle partie : N joueur(s) regardent
  l'intro avec vous » (hôte), « X a modifié <personnage> » à chaque changement de build de l'autre joueur.

## [0.6.4] - 2026-10-23

### Corrections
- Relance du jeu après une mise à jour : Steam ne répond plus « le jeu est déjà en cours d'exécution » (la relance
  part désormais d'un processus indépendant du jeu).

## [0.6.3] - 2026-10-23

### Ajouts
- Test à distance : l'hôte propose son salon au joueur de confiance, qui le rejoint même quand Steam ne le montre
  pas dans la liste des amis.

## [0.6.2] - 2026-10-23

Corrections du premier vrai essai à deux PC.

### Corrections
- Fin de combat : quand l'hôte gagne, le combat se termine aussi chez le client (il ne continuait plus seul, et le
  client n'est plus renvoyé au menu avec « Connexion à l'hôte perdue »).
- Duel du prologue contre Maelle : plus de blocage des deux côtés (le client restait en pause à cause du dialogue de
  l'hôte, et l'hôte attendait le client).
- Cinématiques : un délai de grâce laisse le suiveur déclencher la scène de l'hôte au lieu d'être figé tout de suite.
- Plus de rechargements de partie en boucle chez le client (écart d'inventaire qui ne se réparait jamais).
- Exploration : le perso suiveur sprinte quand l'hôte sprinte (il restait en course et était téléporté toutes les
  2 s) ; tâches lourdes du mod étalées et reportées pendant les déplacements.
- Carte du monde : le suiveur ne fait plus d'allers-retours eau/air quand l'hôte vole sur Esquie.
- Relance du jeu après une mise à jour plus fiable.

## [0.6.1] - 2026-10-22

### Ajouts
- Menus de combat synchronisés : on voit le menu de l'autre joueur pendant qu'il choisit son action (miroir des
  menus, activé par défaut).
- Nouvelle partie ensemble sans option de lancement : F7 au menu principal rejoint la partie d'un ami, sinon ouvre la
  sienne.

### Corrections
- Les touches et clics locaux ne peuvent plus valider une action pendant le tour d'un autre joueur.

## [0.6.0] - 2026-10-22

Première version publique.

### Ajouts
- Coop en ligne à 2-3 joueurs par Steam : l'hôte invite (F7), les amis rejoignent depuis le menu principal ou par
  l'invitation Steam ; la partie chargée est celle de l'hôte.
- Combat partagé : chaque joueur joue ses héros, mêmes résultats partout, parades et QTE de chacun rejoués chez les
  autres, aperçu de l'action en préparation, caméra de la visée libre de l'autre joueur.
- Bonus « parade en duo » : +1 PA quand les héros de deux joueurs parent tous les coups d'une attaque.
- Exploration suivie : un pilote dirige, les autres suivent ; interactions, dialogues, transitions et cinématiques
  partagés ; prise de main (F8).
- Panneau Coop (F9) : répartition des héros entre joueurs, options.
- Builds par joueur et ressources communes (or, objets) ; sauvegarde coop du client.
- Installateur en un fichier (installer, mettre à jour, régler, désinstaller) et mises à jour automatiques depuis les
  releases GitHub.

[Non publié]: https://github.com/kipavy/e33-coop/compare/v0.6.18...HEAD
[0.6.18]: https://github.com/kipavy/e33-coop/compare/v0.6.17...v0.6.18
[0.6.17]: https://github.com/kipavy/e33-coop/compare/v0.6.16...v0.6.17
[0.6.16]: https://github.com/kipavy/e33-coop/compare/v0.6.15...v0.6.16
[0.6.15]: https://github.com/kipavy/e33-coop/compare/v0.6.14...v0.6.15
[0.6.14]: https://github.com/kipavy/e33-coop/compare/v0.6.13...v0.6.14
[0.6.13]: https://github.com/kipavy/e33-coop/compare/v0.6.12...v0.6.13
[0.6.12]: https://github.com/kipavy/e33-coop/compare/v0.6.11...v0.6.12
[0.6.11]: https://github.com/kipavy/e33-coop/compare/v0.6.10...v0.6.11
[0.6.10]: https://github.com/kipavy/e33-coop/compare/v0.6.9...v0.6.10
[0.6.9]: https://github.com/kipavy/e33-coop/compare/v0.6.8...v0.6.9
[0.6.8]: https://github.com/kipavy/e33-coop/compare/v0.6.7...v0.6.8
[0.6.7]: https://github.com/kipavy/e33-coop/compare/v0.6.6...v0.6.7
[0.6.6]: https://github.com/kipavy/e33-coop/compare/v0.6.5...v0.6.6
[0.6.5]: https://github.com/kipavy/e33-coop/compare/v0.6.4...v0.6.5
[0.6.4]: https://github.com/kipavy/e33-coop/compare/v0.6.3...v0.6.4
[0.6.3]: https://github.com/kipavy/e33-coop/compare/v0.6.2...v0.6.3
[0.6.2]: https://github.com/kipavy/e33-coop/compare/v0.6.1...v0.6.2
[0.6.1]: https://github.com/kipavy/e33-coop/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/kipavy/e33-coop/releases/tag/v0.6.0
