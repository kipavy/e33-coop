// Session coop dans le mod : réseau (UDP derrière net::Transport) + synchro du combat (phase 2).
// Activée par -e33role=host|client (+ -e33port=7777, -e33connect=ip:port).
#pragma once

#include <cstdint>
#include <string>

namespace e33::coop {

enum class Role { Solo, Host, Client };

void init();                 // on_unreal_init : lit les arguments, ouvre le transport, abonne les hooks
void tick(double dt);        // game thread
Role role();
bool active();
// Le héros de cet emplacement est-il contrôlé par CETTE instance ? (solo : toujours vrai)
bool controls_spot(int spot);
// Désync détectées depuis le début (pour le test runner)
int desync_count();
int connected_players();   // hôte : clients acceptés ; client : 1 si accepté
int invalid_count();
bool session_ok();           // connecté et accepté (ou solo)

} // namespace e33::coop
