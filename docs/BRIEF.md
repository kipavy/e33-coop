# Brief de la mission (copie de référence, 2026-10-18)

> Copie du cahier des charges donné par Killian. Les décisions listées ici ne se rediscutent pas.

# Mission : mod coop en ligne pour Clair Obscur: Expedition 33

Tu travailles en autonomie sur un mod qui ajoute un vrai mode coop **en ligne** (2 à 3 joueurs, à distance) à Clair Obscur: Expedition 33 (PC, Steam, Unreal Engine 5.4). Chaque joueur fait tourner sa propre instance du jeu. On ne veut pas de streaming vidéo (Parsec, RemotePlayWhatever) : les instances se synchronisent via un canal réseau dédié.

Tu tournes sur un PC Windows physique avec GPU, sur lequel le jeu est installé. Je ne suis pas devant l'écran la plupart du temps. Ton objectif est de pouvoir développer et **tester sans moi**, et de ne me solliciter qu'aux jalons prévus plus bas.

---

## Décisions déjà prises (ne pas rediscuter)

- **Couche native en C++ via UE4SS** : réseau, hooks, test runner, sérialisation. Lua UE4SS autorisé pour du scripting léger (scénarios de test, commandes de debug). Blueprints/pak uniquement si un point d'accroche l'exige absolument, et dans ce cas tu le documentes.
- **Hôte autoritaire.** L'hôte calcule tout ce qui est aléatoire ou décidé par l'IA (dégâts, critiques, actions ennemies, ordre des tours) et diffuse des **résultats**, pas des inputs. Les clients rejouent sans recalculer.
- **Parades et esquives résolues en local** par l'instance du joueur concerné. Le résultat (réussite / échec / type) est envoyé à l'hôte, qui l'applique. C'est la raison d'être du projet : zéro latence sur les QTE.
- **Les QTE offensifs aussi.** Tout ce qui demande un timing ou une visée pendant le tour d'un personnage (timing des compétences, tir en visée libre, mécaniques propres à chaque personnage) est résolu sur l'instance de son contrôleur, puis le résultat est envoyé à l'hôte. Recense ces mécaniques en phase 0.
- **Attaques de groupe** : l'hôte attend le résultat de défense de chaque joueur concerné (barrière avec timeout, échec par défaut si le timeout expire).
- **Exploration** : un seul pilote à la fois (l'hôte par défaut), n'importe quel joueur peut prendre la main. Les autres suivent (synchro de position et des transitions de niveau).
- **Cinématiques et dialogues** : jouées localement, avec un point de synchro à la fin.
- **La sauvegarde de l'hôte fait foi.** Les clients ne sauvegardent pas l'état de la partie partagée.
- **Transport** : UDP direct (IP:port) pendant tout le développement, derrière une interface abstraite. Steam Networking Sockets (P2P + relais) est implémenté en dernier, derrière la même interface.
- **Joueurs** : 2 ou 3. Un personnage par joueur en combat, réassignable. S'il y a moins de joueurs que de personnages, l'hôte contrôle les personnages restants.
- **Le 3ᵉ personnage à 2 joueurs** appartient à l'hôte par défaut, mais il peut être attribué à n'importe quel joueur via un menu. Celui qui le contrôle choisit ses actions et résout ses parades et esquives sur sa propre instance. Lors d'une attaque de groupe, une seule parade d'un joueur couvre tous les personnages qu'il contrôle.
- **Rejoindre une partie (drop-in à tout moment)** :
  - L'hôte ouvre sa partie au coop depuis le menu pause, à n'importe quel moment d'une partie solo en cours. Il obtient une invitation Steam ou un code de partie (IP:port en dev).
  - Le client lance le jeu avec le mod et rejoint depuis le menu principal, ou en acceptant l'invitation Steam.
  - Un joueur peut rejoindre **à tout moment**, mais l'entrée effective se fait au prochain **point sûr** : en exploration, hors combat, cinématique et chargement. S'il arrive pendant un combat, il voit un écran d'attente (« en attente de la fin du combat ») et entre dès la fin.
  - Au point sûr, l'hôte génère un **instantané de l'état actuel**, pas sa dernière save sur disque, qui peut être ancienne. Il l'envoie au client, qui le charge dans un **emplacement dédié au coop**. Les saves solo du client ne sont jamais touchées. Le client apparaît à côté du pilote, exactement dans le même état (zone, équipe, équipement, progression).
  - Le nouveau joueur reçoit un personnage (par défaut, l'un de ceux que contrôlait l'hôte), réassignable via le menu.
  - Le même mécanisme sert à revenir après une déconnexion.
  - En phase 0, vérifie si le jeu permet de déclencher une sauvegarde programmatique n'importe où, et pas seulement aux drapeaux d'expédition. Si ce n'est pas possible, propose un plan B dans `DECISIONS.md` : par exemple charger la dernière save, puis appliquer un delta envoyé par l'hôte (position, coffres ouverts, objets, drapeaux de quête).
- **Builds** : chaque joueur gère le niveau, les attributs, les compétences et l'équipement des personnages qu'il contrôle. Les modifications passent par l'hôte, qui les valide et les diffuse.
- **Pause** : en combat, une pause demandée par un joueur met tout le monde en pause, avec un indicateur de qui a mis en pause. En exploration, un menu ouvert par un joueur ne bloque pas les autres, sauf s'il s'agit du pilote.
- **Pas de minuteur de tour** par défaut (jeu entre amis). Prévois une option désactivée par défaut.
- **Déconnexions** : si un client se déconnecte, l'hôte reprend ses personnages et le client peut revenir en cours de partie. Si l'hôte se déconnecte, la session se termine proprement pour tout le monde, avec un message clair. On ne fait pas de migration d'hôte.
- **Version** : le handshake vérifie la version du jeu (build ID Steam), celle du mod et la liste des autres mods de gameplay installés. En cas d'écart, la connexion est refusée avec un message qui dit précisément ce qui diffère.
- **Robustesse réseau** : chaque message reçu est validé (taille, version, champs). Un message invalide est loggé et ignoré, il ne doit jamais faire planter le jeu. Pas d'anti-triche : on joue entre amis.
- **Interface du mod** : textes en français et en anglais, via un système de traduction simple.

## Décision à prendre en phase 0 : fork ou pas

Il existe un mod de coop **local** : https://github.com/Kouzukii/expedition33-coop (MIT). Il est fait en Blueprints, packagé en pak avec UE 5.4, et s'appuie sur le sous-module `ue-hook-blueprints`. Il route déjà plusieurs manettes vers des personnages différents en combat.

Étudie-le, puis tranche selon cette règle : **on forke seulement si ça ne nous enferme pas dans une architecture inadaptée au coop en ligne**. Points à évaluer :

- Son modèle (inputs manette → personnage) est-il compatible avec « l'hôte diffuse des résultats, les QTE sont résolus localement » ? Ou faudrait-il le contourner partout ?
- Sa logique en Blueprints est-elle exploitable depuis notre couche C++ UE4SS, ou va-t-elle nous imposer de tout faire en Blueprints ?
- Qu'est-ce qui est réutilisable même sans forker (connaissance des classes et fonctions de combat, points d'accroche, mapping `.usmap`) ?

Il est tout à fait acceptable de partir de zéro en utilisant ce mod comme **référence** pour trouver les bons hooks. Consigne ta décision et ses raisons dans `docs/DECISIONS.md`. Si tu forkes, garde la licence MIT et les crédits de l'auteur.

---

## Architecture cible

```
e33-coop/
  native/            # mod C++ UE4SS
    net/             # transport abstrait (UDP, puis Steam), sérialisation
    protocol/        # messages versionnés, partagés avec les outils
    sync/            # combat, exploration, cinématiques, détection de désync
    hooks/           # points d'accroche dans le jeu
    testrunner/      # mode test autonome
  lua/               # scénarios de test, commandes de debug
  tools/
    fakepeer/        # faux hôte / faux client hors jeu
    run-scenario/    # lanceur CLI : démarre le jeu, attend, collecte les résultats
    reset-env/       # remet l'environnement de test dans un état propre
  tests/
    scenarios/       # définitions de scénarios
    recordings/      # échanges réseau enregistrés pour rejeu
  docs/
    DECISIONS.md     # décisions d'architecture et leurs raisons
    PROGRESS.md      # journal d'avancement
    GAME-INTERNALS.md # carte des classes et fonctions du jeu découvertes
```

### Protocole
- Messages versionnés avec un numéro de protocole et la version du jeu dans le handshake. Une connexion entre versions différentes est refusée proprement.
- Chaque message de combat porte un identifiant de combat et un numéro de tour.
- **Détection de désync** : à chaque fin de tour, chaque instance envoie un hash de l'état du combat (PV, PA, buffs/débuffs, ordre des tours). En cas d'écart, il y a un log détaillé puis une resynchronisation complète de l'état depuis l'hôte.

### Logs
Chaque hook et chaque message écrit une ligne JSON (horodatage, instance, rôle, événement, tour, données, hash) dans un fichier par instance. Ces logs sont ta source de vérité : tu dois pouvoir diagnostiquer un problème en les lisant, sans rien voir à l'écran.

---

## Environnement de test autonome (priorité absolue après la phase 0)

### Test runner dans le jeu
Il s'active par un argument de lancement ou un fichier de config. Il doit savoir :
- charger une sauvegarde de test précise et sauter les cinématiques ;
- déclencher un combat précis ;
- **jouer tout seul** : choisir les actions selon le scénario, réussir ou rater les parades/esquives sur commande (en s'accrochant à l'événement d'attaque entrante, pas avec un timing fixe) ;
- dumper l'état à chaque tour et prendre des captures d'écran à des moments définis ;
- quitter avec un code de sortie (0 = succès, autre = échec) et un fichier de résultat.

### Faux pair (`tools/fakepeer`)
C'est un processus hors jeu qui parle le protocole. Il peut jouer le rôle de faux client (scripté) face au vrai jeu hôte, ou de faux hôte (rejouant un enregistrement) face au vrai jeu client. **La plupart des tests se font avec une seule vraie instance plus ce faux pair.** Tu enregistres les échanges des sessions réelles dans `tests/recordings/` pour en faire des tests de non-régression.

### Lanceur et remise à zéro
- `run-scenario <nom>` lance le jeu (fenêtré, basse résolution, sans son si possible), démarre le faux pair si besoin, attend la fin avec un timeout, collecte les logs, les captures et le code de sortie, et produit un rapport.
- `reset-env` : il n'y a pas de snapshots de VM sur cette machine. Au premier lancement, fais une **sauvegarde complète** des saves du joueur et de l'état du dossier du jeu. Ensuite, restaure les saves de test et le dossier des mods dans un état connu avant chaque campagne. Les saves personnelles ne doivent jamais être écrasées.
- Essaie `-nullrhi` pour un lancement sans rendu. Si le build Shipping ne le supporte pas ou si le combat en dépend, rabats-toi sur le fenêtré basse résolution et note-le dans `DECISIONS.md`.

---

## Phases et critères de fin

**Phase 0 : exploration.** Installer UE4SS et vérifier qu'il fonctionne avec la version actuelle du jeu. Dumper les classes et fonctions (dump d'objets UE4SS, et générateur de SDK si utile). Extraire les assets avec retoc. Étudier le mod Kouzukii. Cartographier dans `GAME-INTERNALS.md` : gestion du combat, ordre des tours, choix d'action, calcul des dégâts, aléatoire, QTE parade/esquive, chargement de save, transitions de niveau, cinématiques. Trancher la question du fork.
*Fini quand* : la carte des internals couvre le combat de bout en bout, la décision fork/zéro est documentée et un hook C++ minimal logge un événement de combat réel.

**Phase 1 : environnement de test autonome.** Test runner, lanceur, reset-env. Pas encore de réseau.
*Fini quand* : `run-scenario` joue un combat complet en solo sans intervention, avec parades réussies puis ratées sur commande, et produit un rapport. Trois runs d'affilée donnent le même résultat.

**Phase 2 : preuve de concept réseau.** Transport UDP, protocole minimal, faux pair. Un seul type d'action synchronisé, plus une parade résolue côté client.
*Fini quand* : le jeu hôte face au faux client et le jeu client face au faux hôte passent tous les deux un combat simple, sans désync détectée.

**Phase 3 : synchro complète du combat.** Toutes les actions, compétences, objets, IA ennemie, attaques de groupe, mort et réanimation, fin de combat, récompenses.
*Fini quand* : une batterie de scénarios couvrant ces cas passe, et les enregistrements rejoués restent verts.

**Phase 4 : session et exploration.** Rejoindre une partie (transfert de la save vers l'emplacement coop), synchro du pilote, prise de main, transitions de niveau, cinématiques avec barrière, entrée en combat depuis l'exploration, pause.

**Phase 5 : robustesse.** Déconnexion et reconnexion, resync complète, timeouts, refus de version incompatible, menus de level-up et d'équipement pour les joueurs distants.

**Phase 6 : transport Steam.** Steam Networking Sockets derrière l'interface existante, invitations via des lobbies Steam.

---

## Règles de travail

- **Autonomie** : avance seul de phase en phase. Ne m'arrête que pour :
  1. une **session de validation en jeu** à la fin des phases 2, 3, 4 et 5 : prépare-moi une checklist courte (moins de 20 minutes de jeu) et la commande exacte à lancer ;
  2. tout ce qui touche à mes comptes (Steam, Nexus, GitHub public) ou qui demande un achat ;
  3. une impasse réelle après plusieurs approches documentées.
- **Les tests d'abord** : toute fonctionnalité de synchro arrive avec au moins un scénario ou un enregistrement qui la vérifie. Ne déclare rien « fait » sans run vert.
- **Journal** : tiens `docs/PROGRESS.md` à jour (fait, en cours, bloqué, prochaine étape) pour que je puisse reprendre la situation en 2 minutes.
- **Git** : commits petits et descriptifs, une branche par phase.

## Limites à respecter

- Ne modifie l'installation du jeu que dans les dossiers de mods (`~mods`, dossier UE4SS). Ne patche jamais l'exécutable ni les paks d'origine.
- Aucun contournement de DRM ni de protection Steam. Si une contrainte Steam bloque un test (par exemple deux instances sous le même compte), contourne-la par l'architecture (faux pair, UDP direct), pas en touchant à Steam.
- Les assets extraits du jeu (`Export/`, dumps) restent locaux et sont dans `.gitignore`. Ils ne sont jamais commités ni redistribués.
- Sauvegarde mes saves personnelles avant toute chose et ne les écrase jamais.
- Si une mise à jour du jeu casse les hooks, note la version et signale-le dans `PROGRESS.md` avant de réparer. Note le build ID du jeu dans `GAME-INTERNALS.md` à chaque cartographie, pour savoir sur quelle version elle a été faite.
- Ne publie rien (dépôt public, Nexus) sans mon accord. Le choix de la licence du mod me revient.

---

## Accords complémentaires (2026-10-18)

- Repo GitHub **privé** `kipavy/e33-coop` autorisé, builds via GitHub Actions (aucune toolchain locale).
- Outils portables autorisés dans `.tools/` (gitignoré).
