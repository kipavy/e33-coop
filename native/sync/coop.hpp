// Session coop dans le mod : réseau (UDP derrière net::Transport) + synchro du combat (D8, D10, D11) + robustesse
// (phase 5 : départs et retours de joueurs, fin de session, démarrage de combat coordonné, minuteur de tour).
// Activée par -e33role=host|client (+ -e33port=7777, -e33connect=ip:port, -e33name=, -e33turntimer=N, -e33lang=fr|en).
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace e33::coop {

enum class Role { Solo, Host, Client };

void init();                 // on_unreal_init : lit les arguments, ouvre le transport, abonne les hooks
void tick(double dt);        // game thread
Role role();
bool active();
// Le héros de cet emplacement est-il contrôlé par CETTE instance ? (solo : toujours vrai)
bool controls_spot(int spot);
int connected_players();     // hôte : clients acceptés ; client : 1 si accepté
bool session_ok();           // connecté et accepté (ou solo)
// Fin de combat traitée (récompenses comparées, derniers tirages vérifiés) : le test runner peut conclure.
bool battle_finalized();
// Hôte : premier tour d'un combat en attente des autres joueurs (démarrage coordonné, jeu en pause).
bool holding();
// Client : session terminée (hôte parti ou injoignable, refus de version) ; hôte : retour au menu principal.
bool session_ended();
// Action de ce héros pour le tour courant imposée par l'hôte (minuteur de tour écoulé) : le joueur n'agit plus.
bool action_forced(int spot);
// Nom affiché d'un joueur (« vous » pour soi).
std::string name_of(uint8_t player);
// Fin de session propre (fin de test, sortie) : Bye à tous les pairs, envoyé immédiatement.
void shutdown(const char* reason);
// Test runner : point visé par le « joueur » simulé pendant une visée libre d'un héros d'ici (nullptr = aucun).
void set_local_aim(const double* point);
// Compteurs pour le test runner (desyncs, raw_hash_diffs, gameplay_draws, defense_timeouts…).
const std::map<std::string, int>& counters();
int counter(const std::string& name);

} // namespace e33::coop
