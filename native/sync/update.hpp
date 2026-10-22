// Mises à jour depuis les releases GitHub (kipavy/e33-coop). Option « updates » (settings.ini / -e33updates) :
//  - auto (défaut) : au menu principal, vérification en arrière-plan ; version plus récente -> l'installateur du mod
//    (« Installer E33 Coop.exe --stage ») la télécharge et la vérifie (SHA-256), puis l'installe à la fermeture du jeu ;
//  - ask : carte « Nouvelle version X disponible — U pour mettre à jour » au menu principal ;
//  - off : rien (sauf le message clair d'écart de version à la jonction).
// Écart de version à la jonction : le côté le plus ancien est invité à mettre à jour (auto : tout de suite, puis
// relance par Steam et nouvelle jonction de la partie de l'ami). Rien en mode test. Aucun téléchargement par le mod :
// l'installateur (chemin fixe dans le dossier du mod, arguments fixes) fait tout, avec vérification SHA-256.
#pragma once

#include <cstdint>
#include <string>

namespace e33::update {

void init();
void tick(double dt);                       // game thread
// Écart de version avec un autre joueur. we_older : notre mod est le plus ancien. joining : client qui rejoignait
// (au menu principal). friend_id : SteamID de l'hôte (pour rejoindre après la mise à jour), 0 sinon.
void on_version_mismatch(bool we_older, const std::string& other, const std::string& other_version, bool joining, uint64_t friend_id);
// Hôte à rejoindre après une mise à jour (relance), 0 sinon ; lu une fois.
uint64_t take_rejoin();
std::string policy();                       // "auto" | "ask" | "off"
const char* current_version();

} // namespace e33::update
