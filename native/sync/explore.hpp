// Session et exploration (phase 4, D12) : rejoindre une partie (instantané de l'hôte), pilote unique suivi par les
// autres instances, interactions et choix de dialogue rejoués, transitions de niveau, barrière à la fin des
// cinématiques et dialogues, pause partagée en combat. Le réseau reste dans coop.cpp (fonctions fournies ici).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../protocol/messages.hpp"

namespace e33::explore {

struct Net {
    std::function<void(const proto::Message&, bool reliable)> publish;            // hôte : à tous les clients ; client : à l'hôte
    std::function<void(uint8_t player, const proto::Message&)> send_to;           // hôte : à un client
    std::function<void(const proto::Message&, uint8_t origin, bool reliable)> relay;   // hôte : à tous sauf origin
    std::function<void(const char* counter, int n)> bump;
    std::function<std::string(uint8_t player)> name_of;                          // nom affiché d'un joueur
};

void init(bool host, bool want_join, const Net& net);   // want_join : client qui rejoint la partie de l'hôte
void reset();                                           // retour au mode neutre (D18) : inactif, état remis à zéro
void set_player_id(uint8_t id);
void on_peer_accepted(uint8_t player, bool wants_join);   // hôte
void on_peer_left(uint8_t player);                        // hôte
bool peer_in_game(uint8_t player);                        // hôte : client entré dans la partie (ou session sans « rejoindre »)
void on_session_end();                                    // client : session finie (entrées et pause rendues)
// Pause du jeu entier partagée entre plusieurs raisons (barrière narrative, pause d'un autre joueur, démarrage de
// combat coordonné) : le jeu est en pause tant qu'une raison la tient, et elle est réappliquée si le jeu l'enlève.
void hold_pause(const char* reason, bool on);
bool pause_held(const char* reason);
bool handle(uint8_t from, const proto::Message& m);       // true si le message est consommé ici
void tick(double now);
void subscribe();

bool join_requested();        // client : -e33join=1
bool joined();                // client : instantané chargé et monde prêt
int joined_players();         // hôte : clients entrés dans la partie
uint8_t pilot();
bool is_pilot();
void request_pilot();         // demande la main (touche F8, test runner)
bool narrative_active();      // dialogue ou cinématique en cours ici
bool choice_pending();        // question de dialogue posée ici, sans réponse
bool sequence_playing();      // séquence de cinématique en cours (OnSequenceStarted … OnSequenceFinished)
double last_event_age();     // secondes depuis le dernier événement d'exploration (interaction, choix, transition, combat, narration)
bool remote_paused();         // jeu mis en pause ici par la pause d'un autre joueur (combat)
double follow_error();        // suiveur : dernier écart au pilote (u), -1 si inconnu
bool local_moving();          // pion local (pilote ou suiveur) en mouvement (> 150 u/s) : tâches lourdes reportées
bool calm(double secs);       // point sûr sans événement d'exploration depuis secs (copie coop, coopsave)
// Dernier ennemi du monde qui a lancé un combat ici (nom d'acteur, engagement), pour BattleStart.
std::string take_world_enemy();   // vide si le combat n'a pas été lancé par un ennemi du monde à l'instant
int last_world_engage();

} // namespace e33::explore
