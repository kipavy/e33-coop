# Test à distance

Le développeur (sur le PC de Killian, qui fait tourner le jeu **hôte**) peut tester avec le jeu de l'ami (le **client**)
par Steam, sans logiciel de prise en main à distance et sans s'envoyer de fichiers à la main.

## Pour l'ami : ce que ça permet, ce que ça ne permet jamais

**Le mode est désactivé par défaut.** Il ne s'active que si tu l'installes avec l'option `-RemoteTest`, et il ne fait
confiance qu'à **un seul compte Steam** : celui de Killian (`76561198355797267`). Les messages de tout autre compte sont
ignorés.

Ce que Killian peut faire, **seulement pendant que ton jeu tourne** :
- demander l'état du jeu (version du mod, carte, combat ou pas, images par seconde) ;
- récupérer les **journaux du mod** (`ue4ss\Mods\E33Coop\logs\…`, les plus récents), le journal d'UE4SS
  (`ue4ss\UE4SS.log`) et les **saves du coop** (`SaveGames\e33coop\*.sav`, la copie de la partie de Killian) ;
- lancer un **scénario de test** du mod (le même « test runner » que sur son PC : rejoindre sa partie, combattre,
  explorer) ;
- ramener le jeu au menu principal, charger la save du coop, quitter le jeu ;
- avec `-AllowRestart` : **redémarrer** le jeu (il quitte puis se relance par Steam, avec tes options de lancement) ;
- avec `-AllowUpdate` : **installer une nouvelle version du mod** (voir plus bas).

Ce qui est **impossible**, quoi qu'envoie le réseau :
- aucune commande Windows, aucun programme lancé à part le jeu lui-même (par l'adresse fixe `steam://rungameid/1903340`) ;
- aucun fichier lu en dehors de la liste ci-dessus : **jamais tes saves perso** (`SaveGames\<ton SteamID>\…`), jamais
  tes documents ni rien d'autre sur le PC ;
- un scénario ne peut charger que les saves du coop ou de test (`e33coop/…`, `e33coop-test/…`), ses options sont
  filtrées (liste fermée, connexion Steam seulement) ;
- quitter ou redémarrer est **refusé si tu joues ta propre partie solo** (seulement au menu principal, en partie coop ou
  pendant un test : rien n'est sauvegardé dans tes emplacements) ;
- rien quand le jeu est fermé : le mode n'existe que dans le mod, pendant que le jeu tourne.

Un bandeau reste affiché à l'écran tant que le mode est actif : **« Mode test à distance actif — piloté par Killian »**,
et chaque commande reçue s'affiche un instant.

### Mise à jour du mod à distance (`-AllowUpdate`) : à lire avant de l'activer

Avec cette option, **Killian peut installer du nouveau code du mod sur ton PC** (le mod est un programme : une nouvelle
version peut faire tout ce qu'un programme peut faire). Ne l'active que si tu lui fais confiance pour ça.

Garde-fous :
- seulement depuis le compte Steam de Killian ; le paquet est vérifié (empreinte SHA-256 de chaque fichier et de la
  liste complète) ;
- seulement dans le dossier du jeu : `dwmapi.dll`, `ue4ss\UE4SS.dll` et `ue4ss\Mods\E33Coop\…` (jamais `remotetest.txt`,
  donc la confiance ne se change pas à distance) ; 128 Mo au plus ;
- avant de remplacer quoi que ce soit, l'ancienne version est copiée dans `ue4ss\Mods\E33Coop\remote\backup\<date>\` ;
- **retour arrière automatique** : si, dans les 3 minutes après la relance, le nouveau mod n'a pas écrit son journal de
  démarrage (`mod_init`), le jeu est fermé, l'ancienne version remise et le jeu relancé ;
- retour arrière à la main : `powershell -NoProfile -ExecutionPolicy Bypass -File "<jeu>\Sandfall\Binaries\Win64\ue4ss\Mods\E33Coop\remote\rollback.ps1"`.

### Couper le mode

- Supprimer le fichier `…\Expedition 33\Sandfall\Binaries\Win64\ue4ss\Mods\E33Coop\remotetest.txt` (effet au prochain
  lancement du jeu), **ou**
- réinstaller avec `install.ps1 -NoRemoteTest` (une réinstallation sans option garde le réglage précédent), **ou**
- désinstaller le mod (`uninstall.ps1`).

Pour enlever seulement la mise à jour à distance : réinstaller avec `-RemoteTest 76561198355797267 -AllowRestart`
(sans `-AllowUpdate`).

## Pour l'ami : installation (une fois)

1. Décompresser le paquet du mod reçu de Killian, ouvrir PowerShell dans ce dossier.
2. Lancer (une seule ligne ; enlever `-AllowUpdate` et/ou `-AllowRestart` si tu n'en veux pas) :
   ```
   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -RemoteTest 76561198355797267 -AllowRestart -AllowUpdate
   ```
3. Lancer le jeu et le laisser au **menu principal**. Les options de lancement Steam actuelles peuvent rester ; si elles
   sont vides, le mode test à distance se comporte en client coop par Steam.
4. La première fois que le jeu est relancé à distance, Steam peut afficher une fenêtre de confirmation : l'accepter.

Rien n'est écrit en dehors du dossier `Sandfall\Binaries\Win64` du jeu.

## Pour le développeur (terminal, PC de Killian)

Le jeu de Killian doit tourner avec le mod (n'importe quel mode : partie normale ou scénario hôte lancé par
`run-scenario.ps1` avec `-e33transport=steam`). Le script crée `ue4ss\Mods\E33Coop\remote\inbox`, ce qui active le côté
hôte du mod.

```
powershell -NoProfile -File tools/remote/remote.ps1 status
powershell -NoProfile -File tools/remote/remote.ps1 fetch-logs [-Count 3]
powershell -NoProfile -File tools/remote/remote.ps1 run tests/scenarios/<client>.json [-Wait]
powershell -NoProfile -File tools/remote/remote.ps1 menu | load-snapshot | quit | restart
powershell -NoProfile -File tools/remote/remote.ps1 update .tools/artifacts/<id>/e33coop-mod   (ou le zip du paquet joueur)
powershell -NoProfile -File tools/remote/remote.ps1 peer
```
- `-Peer <SteamID64>` (défaut : l'ami, `76561198045698677`), `-Timeout <s>`, `-GameDir`.
- Réponses et fichiers : écrits par le mod dans `…\ue4ss\Mods\E33Coop\remote\outbox\<id>\` (`state.json`,
  `reply.json`, `files\`), recopiés par le script dans `runs\remote\<id>\` du repo (`reset-env.ps1` efface le dossier du
  mod avant chaque test local). Le
  « bonjour » du client (version, droits) : `remote\peers\<SteamID>.json`.
- `run` : le scénario est validé chez l'ami (voir plus haut), déposé dans `remote\pending\`, puis le jeu redémarre
  (`-AllowRestart`) et l'exécute comme `-e33test` (ses `launch_args` filtrées ; `-e33connect` / `-e33port` retirés,
  `-e33transport=steam` ajouté au rôle client). Le run écrit dans `logs\remote-<date>\` (`scenario.json`,
  `result.json`, journaux), le jeu se ferme à la fin puis se relance. `-Wait` interroge `status` jusqu'au résultat
  (`last_run`), puis rapatrie les journaux. Sans `-AllowRestart`, l'ami doit relancer le jeu à la main.
- Rejoindre : au menu principal, le client rejoint tout seul le lobby coop de l'hôte de confiance (essai toutes les
  5 s ; il faut être amis Steam). Une session coop terminée ne se relance pas dans le même processus : `restart`.
- `update` : copie les fichiers permis du paquet dans `remote\updates\<id>\`, les envoie (SHA-256), le client vérifie,
  prépare `remote\staging\`, quitte, puis `apply-update.ps1` installe, relance et surveille (retour arrière automatique).
  Vérifier ensuite avec `status` (`mod_version`, `remote\helper.log` dans `fetch-logs`).

## Fonctionnement

- **Canal à part** : canal Steam 1 (le coop utilise le canal 0), messages fiables de Steam Networking Messages, par le
  `steam_api64.dll` du jeu. Indépendant de la session coop : marche au menu, avant, pendant et après une session.
- **Compatibilité entre versions** : paquets `E33R` avec leur propre version (`native/remote/remote_proto.hpp`), sans
  rapport avec `proto::kProtocolVersion`. On n'y ajoute que des commandes ou des champs en fin de paquet (ignorés par
  les anciennes versions) : une mise à jour qui change le protocole coop ne coupe jamais `status`, `fetch-logs`,
  `restart` ni `update`.
- **Client** : ne lit que les messages du SteamID de confiance (sessions acceptées seulement pour lui), liste fermée
  de commandes, chemins générés localement. **Hôte** : n'accepte une réponse que du pair interrogé, vérifie chaque
  chemin reçu (relatif, sans `..`, caractères simples) avant d'écrire dans `outbox\<id>\files\`, limite de taille.
- Assistants PowerShell installés par `install.ps1` dans `remote\` (`relaunch.ps1`, `apply-update.ps1`,
  `rollback.ps1`) : lancés cachés par le mod avec pour seul argument son PID ; ils attendent la fin du jeu.
  Journal : `remote\helper.log`.
- Code : `native/remote/` (protocole, règles de sécurité, testés dans `tools/tests/test_remote.cpp`),
  `native/common/remotecfg.hpp` (configuration), `native/common/cmdline.hpp` (scénario reçu = `-e33test`),
  `tools/remote/`.
