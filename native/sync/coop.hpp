// Session coop dans le mod : réseau (UDP derrière net::Transport) + synchro du combat (D8, D10, D11).
// Activée par -e33role=host|client (+ -e33port=7777, -e33connect=ip:port).
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
// Compteurs pour le test runner (desyncs, raw_hash_diffs, gameplay_draws, defense_timeouts…).
const std::map<std::string, int>& counters();
int counter(const std::string& name);

} // namespace e33::coop
