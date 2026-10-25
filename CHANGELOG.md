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

### Corrections
- Combat : moins de gels de 3 s quand un monstre attaque. Si l'attaque touche moins de fois votre héros chez vous que
  chez l'autre joueur, son jeu n'attend plus votre parade pour les coups en trop.

## [0.6.34] - 2026-10-25

### Corrections
- Combat : plus de combat figé puis abandonné quand l'ordre des tours diffère d'un cran entre les deux joueurs
  (combat engagé différemment, héros mort chez un seul) ; l'action du héros est retrouvée.
- Combat : les tirs en visée libre faits dès le début d'un combat que votre personnage a engagé sont bien envoyés à
  l'autre joueur ; avant, leurs dégâts disparaissaient au tour suivant.
- Combat : la remise à niveau de l'état en début de tour ne décale plus les coups critiques et effets aléatoires
  de la suite du tour.
- Personnages : l'expérience n'est plus doublée quand le build d'un personnage est partagé (un personnage pouvait
  gagner un niveau chez un seul joueur).
- Personnages : plus de message « ce personnage n'est pas à vous » à tort après chaque combat.
- Exploration : un choix de dialogue resté en attente ne bloque plus les vérifications de synchro après un
  changement de zone.

## [0.6.33] - 2026-10-25

### Corrections
- Combat : plus de gel de 3 s quand une attaque ennemie frappe plus de fois chez vous que chez l'autre joueur (il
  est déjà passé au tour suivant) ; avant, le jeu restait figé 3 s pour chaque coup en trop.

## [0.6.32] - 2026-10-25

### Corrections
- Monstres : quand l'autre joueur mène, les monstres de l'hôte ne foncent plus sur son personnage et ne lancent plus
  de combat au contact ; seul le meneur déclenche les combats (avant, l'hôte pouvait être pris dans un combat que le
  meneur n'avait pas touché).
- Améliorations d'armes (Curateur du camp) : envoyées aussitôt à l'autre joueur ; avant, elles n'étaient jamais
  partagées et le niveau d'une arme différait d'un joueur à l'autre.
- Objets utilisés hors combat (élixir de chroma, objet donné à un personnage depuis le menu) : l'effet (soin) a
  aussi lieu chez l'autre joueur ; avant, seule la quantité suivait.
- Pétanks : ils fuient aussi chez le joueur qui suit (avant, ils restaient immobiles chez lui).
- Suivi du meneur : quand le jeu annule une téléportation du personnage suiveur, le mod attend avant de réessayer au
  lieu de le téléporter à chaque image.

## [0.6.31] - 2026-10-25

### Corrections
- Cinématiques : une cinématique lancée par une zone chez le meneur (ex. au camp après le boss Goblu) se lance aussi
  chez l'autre joueur même si sa zone ne l'a pas déclenchée ; avant, il restait bloqué à attendre.

## [0.6.30] - 2026-10-25

### Corrections
- Équipement : une arme ou un picto changé par l'autre joueur sur son personnage apparaît aussi chez vous (avant,
  l'ancienne arme restait affichée et utilisée).

## [0.6.29] - 2026-10-25

### Corrections
- Combat lancé par l'autre joueur quand il mène : le combat part aussitôt chez l'hôte aussi (même ordre des tours,
  même état) ; avant, chacun lançait le sien, parfois à 17 s d'écart.
- Marchand : l'autre joueur ne peut plus changer la sélection ni acheter pendant que le meneur achète ; l'or et le
  stock affichés suivent chaque achat du meneur. Le message « Le meneur a acheté un article » est retiré.
- Exploration : les tirs du meneur sur les objets du décor (sphères, cages, boutons) sont rejoués chez l'autre joueur.

## [0.6.28] - 2026-10-24

### Corrections
- Marchand : l'autre joueur voit l'article sélectionné par le meneur, et chaque achat du meneur est fait chez lui aussi
  (or et stock à jour). Si sa fenêtre du marchand ne s'est pas ouverte (ex. après le combat du marchand), elle
  s'ouvre seule.

## [0.6.27] - 2026-10-24

### Corrections
- Combat avec toute l'équipe : chaque joueur contrôle de nouveau le bon héros et l'ordre des tours est le même chez
  les deux (cassé en 0.6.25 : TOTOFFF pouvait voir Maelle comme la sienne et un autre ordre des tours).

## [0.6.26] - 2026-10-24

### Corrections
- Combat : les pictos et effets que l'autre joueur ne possède pas (ex. un picto du meneur) sont maintenant chargés et
  appliqués chez lui ; avant, les dégâts différaient et un monstre pouvait mourir chez l'un et pas chez l'autre.

## [0.6.25] - 2026-10-24

### Corrections
- Combat contre un marchand : l'autre joueur a la même équipe que le meneur (le ou les héros choisis pour le duel),
  au lieu de toute l'équipe avec des rôles décalés.
- Marchand : l'article sélectionné par le meneur devrait maintenant apparaître chez l'autre joueur, avec un message
  à chaque achat.

## [0.6.24] - 2026-10-24

### Corrections
- Messages du mod (dont « Combat lancé : fermez le menu pour le rejoindre ») affichés par-dessus les menus du jeu
  (ils restaient cachés derrière l'inventaire).

## [0.6.23] - 2026-10-24

### Corrections
- Marchand : l'autre joueur voit la fenêtre du marchand du meneur en même temps que lui (article sélectionné, achats
  qui arrivent dans l'inventaire), sans pouvoir y toucher : plus d'achats séparés ni de combat du marchand lancé de son
  côté. Sa fenêtre se ferme avec celle du meneur.

## [0.6.22] - 2026-10-24

### Corrections
- Combat lancé par l'hôte pendant que l'autre joueur est dans l'inventaire ou un autre menu : il attend aussi la
  fermeture de ces menus (la 0.6.19 ne reconnaissait que le menu pause).

## [0.6.21] - 2026-10-24

### Corrections
- Combat : un buff présent chez l'hôte mais absent chez l'autre joueur lui est maintenant ajouté au tour suivant (la
  correction échouait toujours, et l'écart restait jusqu'à la fin du combat).

## [0.6.20] - 2026-10-24

### Corrections
- Écran de défaite : seul l'hôte choisit (équipe de réserve, « Réessayer » ou « Abandonner ») ; les autres joueurs voient
  « l'hôte choisit… » puis suivent son choix (avant : chacun choisissait, et un choix différent finissait sur un écran noir).

## [0.6.19] - 2026-10-24

### Corrections
- Combat lancé par l'hôte pendant que l'autre joueur est dans un menu (équipe, pictos…) : son jeu ne se fige plus ; un
  message lui demande de fermer le menu, puis il rejoint le combat.

## [0.6.18] - 2026-10-24

### Ajouts
- Dialogues : l'autre joueur voit en surbrillance la réponse que le meneur survole avant de choisir.

### Corrections
- Exploration : les éléments du décor activés par le meneur (ex. le pont de la carte du monde) s'activent aussi chez
  l'autre joueur, qui ne le voit plus marcher dans le vide.

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

[Non publié]: https://github.com/kipavy/e33-coop/compare/v0.6.34...HEAD
[0.6.34]: https://github.com/kipavy/e33-coop/compare/v0.6.33...v0.6.34
[0.6.33]: https://github.com/kipavy/e33-coop/compare/v0.6.32...v0.6.33
[0.6.32]: https://github.com/kipavy/e33-coop/compare/v0.6.31...v0.6.32
[0.6.31]: https://github.com/kipavy/e33-coop/compare/v0.6.30...v0.6.31
[0.6.30]: https://github.com/kipavy/e33-coop/compare/v0.6.29...v0.6.30
[0.6.29]: https://github.com/kipavy/e33-coop/compare/v0.6.28...v0.6.29
[0.6.28]: https://github.com/kipavy/e33-coop/compare/v0.6.27...v0.6.28
[0.6.27]: https://github.com/kipavy/e33-coop/compare/v0.6.26...v0.6.27
[0.6.26]: https://github.com/kipavy/e33-coop/compare/v0.6.25...v0.6.26
[0.6.25]: https://github.com/kipavy/e33-coop/compare/v0.6.24...v0.6.25
[0.6.24]: https://github.com/kipavy/e33-coop/compare/v0.6.23...v0.6.24
[0.6.23]: https://github.com/kipavy/e33-coop/compare/v0.6.22...v0.6.23
[0.6.22]: https://github.com/kipavy/e33-coop/compare/v0.6.21...v0.6.22
[0.6.21]: https://github.com/kipavy/e33-coop/compare/v0.6.20...v0.6.21
[0.6.20]: https://github.com/kipavy/e33-coop/compare/v0.6.19...v0.6.20
[0.6.19]: https://github.com/kipavy/e33-coop/compare/v0.6.18...v0.6.19
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
