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

[Non publié]: https://github.com/kipavy/e33-coop/compare/v0.6.5...HEAD
[0.6.5]: https://github.com/kipavy/e33-coop/compare/v0.6.4...v0.6.5
[0.6.4]: https://github.com/kipavy/e33-coop/compare/v0.6.3...v0.6.4
[0.6.3]: https://github.com/kipavy/e33-coop/compare/v0.6.2...v0.6.3
[0.6.2]: https://github.com/kipavy/e33-coop/compare/v0.6.1...v0.6.2
[0.6.1]: https://github.com/kipavy/e33-coop/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/kipavy/e33-coop/releases/tag/v0.6.0
