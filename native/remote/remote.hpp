// Test à distance (docs/REMOTE-TEST.md) : le développeur, sur le PC de l'hôte, pilote le jeu d'un ami consentant par
// Steam, sans logiciel de prise en main. Indépendant de la session coop (canal Steam 1, voir remote_proto.hpp) : marche
// au menu principal, avant, pendant et après une session, et d'une version du mod à l'autre.
//  - Client (PC de l'ami) : actif si ue4ss/Mods/E33Coop/remotetest.txt désigne un SteamID de confiance (ou
//    -e33remotetest=<id>). N'obéit qu'à ce SteamID et à une liste fermée de commandes. Indicateur à l'écran.
//  - Hôte (PC du développeur) : actif si le dossier ue4ss/Mods/E33Coop/remote/inbox existe ; lit les commandes déposées
//    par tools/remote/remote.ps1, écrit réponses et fichiers reçus dans remote/outbox/<id>/.
#pragma once

#include <cstdint>

namespace e33::remote {

void tick(double dt);          // game thread, après le délai de démarrage du mod
bool client_enabled();         // mode test à distance actif ici (côté ami)
uint64_t trusted_host();
// Client coop au menu principal : lobby coop de l'hôte de confiance à rejoindre (0 sinon). Limité à un essai toutes les 5 s.
uint64_t auto_join_lobby();
// Fin d'un scénario reçu à distance (le runner ferme le jeu) : relance par Steam si l'ami l'a autorisé.
void on_test_exit();

} // namespace e33::remote
