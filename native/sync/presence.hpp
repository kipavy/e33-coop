// Ce que fait chaque autre joueur, vu d'ici, pour les écrans d'attente (« TOTOFF charge la zone… », « en attente de
// TOTOFF, qui regarde une cinématique », « Killian a fini et vous attend »). Rien de nouveau sur le réseau : tout vient
// de ce qui circule déjà (battements « occupé » du lien pendant un chargement, silence du lien, NarrativeState,
// PauseState, état de « rejoindre » côté hôte). Le pourcentage de chargement n'est pas lisible (le thread de jeu du
// joueur qui charge est bloqué) : on affiche les secondes écoulées.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace e33::presence {

enum class Activity { Ready, Loading, Joining, Cinematic, Dialogue, Menu, Reconnecting };

// Sources (même horloge que coop/explore)
void set_link(uint8_t player, bool busy, double silent_s, double now);   // coop : chaque tick, par pair direct
void set_narrative(uint8_t player, bool active, bool cinematic, double now);
void set_menu(uint8_t player, bool open, double now);
void set_joining(uint8_t player, bool loading_snapshot, double now);    // hôte : instantané envoyé, JoinReady attendu
void forget(uint8_t player);
void reset();                                                          // retour au mode neutre

Activity activity(uint8_t player, double now);
const char* activity_name(Activity a);

using NameFn = std::function<std::string(uint8_t)>;
// Ligne d'attente pour ces joueurs ; idle_key : texte d'un joueur sans raison connue (ex. "wait_battle_prep").
std::string wait_text(const std::vector<uint8_t>& players, double now, const NameFn& name, const char* idle_key = "wait_generic");
// Joueur en retard : « Killian a fini et vous attend » (+ comment passer une cinématique).
std::string waited_by_text(const std::vector<uint8_t>& waiting, const NameFn& name, bool skip_hint);
// Joueurs qui chargent (zone ou partie) sans qu'on les attende : ligne d'information, vide si aucun.
std::string loading_text(const std::vector<uint8_t>& players, double now, const NameFn& name);

} // namespace e33::presence
