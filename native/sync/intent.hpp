// Ce que prépare l'autre joueur (aperçu) et caméra de sa visée libre.
//  - Propriétaire d'un héros, pendant son tour : action et compétence/objet choisis dans la roue (hooks du gestionnaire de
//    combat), cibles visées (relevées 10 fois/s sur le gestionnaire de ciblage) ; envoyés à chaque changement
//    (IntentPreview). Chez les autres : « X prépare Compétence → Cible » dans la carte d'état, effacé quand l'action
//    arrive ou que le tour change.
//  - Visée libre : le propriétaire envoie sa caméra (AimView, ~20 fois/s). Chez les autres, la visée rejouée est vue
//    depuis une caméra du mod placée comme la sienne (ou, sans elle, derrière l'épaule du héros vers le point visé),
//    au lieu de la caméra de combat qui tourne au-dessus de l'arène (signalé par Killian le 2026-10-22).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../protocol/messages.hpp"

namespace RC::Unreal { class UObject; }

namespace e33::intent {

struct BattleRef { uint32_t id = 0; uint32_t turn = 0; bool active = false; };

struct Net {
    std::function<void(const proto::Message&, bool reliable)> publish;   // hôte : à tous les clients ; client : à l'hôte
    std::function<void(const char* counter, int n)> bump;
    std::function<std::string(uint8_t player)> name_of;
    std::function<uint8_t(int spot)> owner_of_spot;
    std::function<bool(int spot)> controls;                              // héros contrôlé ici
    std::function<bool(uint32_t turn, int spot)> action_known;           // commande déjà reçue (ou jouée) pour ce tour
    std::function<bool()> local_aiming;                                  // visée libre d'un héros d'ici en cours
    std::function<BattleRef()> battle;                                   // combat courant (numéro de tour du mod)
};

void init(const Net& net);
void subscribe();                                   // hooks de la roue et du ciblage
void receive(const proto::IntentPreview& m);
void receive(const proto::AimView& m);
void tick(double now);                              // game thread : envoi des aperçus d'ici, affichage de ceux des autres

// Visée libre d'un héros d'ici : caméra envoyée (appelé à chaque image tant que la visée est ouverte).
void aim_owner_tick(double now, int spot, uint32_t t_ms);

// Visée libre rejouée (héros d'un autre joueur), à chaque image. r nul : aucune visée rejouée, la caméra du jeu est rendue.
struct AimReplay {
    RC::Unreal::UObject* hero = nullptr;
    int spot = -1;
    uint32_t elapsed_ms = 0;                        // temps de rejeu (même base que t_ms du propriétaire)
    bool have_point = false;
    double point[3]{};                              // point visé imposé
};
void aim_camera_tick(double now, double dt, const AimReplay* r);

} // namespace e33::intent
