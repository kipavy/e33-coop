// Builds des personnages en coop (phase 5, D13). Chaque joueur gère les personnages qu'il contrôle (niveau, attributs,
// compétences, Luminas, équipement) dans ses propres menus. Synchro par état, pas par rejeu des clics (les menus du jeu
// modifient parfois les données directement, vu le 2026-10-20 sur le panneau des Luminas) :
//  - chaque instance relève l'empreinte du build de chaque personnage (SaveCharacterState) hors menu et hors combat ;
//  - un changement part comme un état sérialisé par le jeu (BuildOffer + BuildChunk) : client -> hôte -> autres ;
//  - l'hôte n'accepte que les personnages du joueur qui les envoie ; pour les autres, il renvoie son propre état
//    (la modification est annulée chez son auteur, avec un message) ;
//  - chez l'hôte, une modification d'un personnage d'un client est annulée de la même façon (état gardé en cache).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../protocol/messages.hpp"

namespace e33::builds {

struct Net {
    std::function<void(const proto::Message&)> publish;                    // hôte : à tous les clients en jeu ; client : à l'hôte
    std::function<void(uint8_t player, const proto::Message&)> send_to;    // hôte : à un client
    std::function<void(const proto::Message&, uint8_t origin)> relay;      // hôte : à tous sauf origin
    std::function<void(const char* counter, int n)> bump;
    std::function<std::string(uint8_t player)> name_of;
    std::function<uint8_t(const std::string& character)> owner_of;       // joueur qui contrôle ce personnage
    std::function<bool()> sharing;                                         // la partie est partagée (joueurs entrés)
    std::function<double()> last_event_age;                                // calme de la partie (aucun événement rejoué récent)
    std::function<void(const std::string& reason)> request_resync;          // client : nouvel instantané demandé à l'hôte
};

void init(bool host, const Net& net);
void reset();               // retour au mode neutre : inactif, état remis à zéro
void set_player_id(uint8_t id);
bool handle(uint8_t from, const proto::Message& m);   // true si consommé
void tick(double now);
void rebaseline();          // après le chargement d'un instantané : l'état chargé est la nouvelle référence
bool menu_open();           // un menu de jeu est ouvert ici (les changements partent à sa fermeture)

} // namespace e33::builds
