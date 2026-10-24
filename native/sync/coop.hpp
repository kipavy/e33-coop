// Session coop dans le mod : réseau (UDP derrière net::Transport) + synchro du combat (D8, D10, D11) + robustesse
// (phase 5 : départs et retours de joueurs, fin de session, démarrage de combat coordonné, minuteur de tour).
// Sans option : mode neutre par Steam, rôle décidé en jeu (D18). -e33role=host|client (+ -e33transport=steam,
// -e33port=7777, -e33connect=ip:port, -e33name=…) garde l'ancien fonctionnement (tests, développement).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace e33::coop {

enum class Role { Solo, Host, Client };

void init();                 // on_unreal_init : lit les arguments, ouvre le transport, abonne les hooks
void tick(double dt);        // game thread
Role role();
bool active();
// Partie normale sans -e33role (D18) : « prêt au coop » par Steam, rôle décidé en jeu (F7 dans une partie chargée :
// hôte ; invitation, F7 ou « Rejoindre » au menu : client ; F10 au menu : hôte de la copie). Fin de session : neutre.
bool neutral();              // mode neutre, aucun rôle pour l'instant
bool ready();                // neutre ou session en cours (le module coop affiche les textes)
void reload_options();       // options changées en jeu : duo_parry, turn_timer_s, turndep relus
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
// Écran de défaite (D31) : fenêtre de choix ouverte en dernier, 0 « Réessayer / Abandonner », 1 équipe de réserve.
int defeat_prompt_kind();
// Tour d'un héros d'un autre joueur en cours ici, avant l'exécution de son action (test des entrées bloquées) : son
// emplacement, -1 sinon.
int remote_turn_open();
// Action de ce héros pour le tour courant imposée par l'hôte (minuteur de tour écoulé) : le joueur n'agit plus.
bool action_forced(int spot);
// Nom affiché d'un joueur (« vous » pour soi).
std::string name_of(uint8_t player);
// Répartition « qui joue qui » pour l'interface (native/ui/owners_ui) :
uint8_t my_player();                                      // hôte = 0
uint8_t hero_owner(const std::string& name);              // personnage (nom interne) : en combat par emplacement, sinon répartition hors combat
uint8_t spot_owner(int spot);                             // héros du combat en cours, par emplacement
std::vector<std::pair<uint8_t, std::string>> players();  // joueurs connus, noms bruts (hôte = 0 en tête)
bool steam_transport();
// Hôte : le personnage d'indice index dans l'équipe passe au joueur suivant (touche F6, test runner).
void reassign_character(size_t index);
// Fin de session propre (fin de test, sortie) : Bye à tous les pairs, envoyé immédiatement.
void shutdown(const char* reason);
// Test runner : point visé par le « joueur » simulé pendant une visée libre d'un héros d'ici (nullptr = aucun).
void set_local_aim(const double* point);
// Compteurs pour le test runner (desyncs, raw_hash_diffs, gameplay_draws, defense_timeouts…).
const std::map<std::string, int>& counters();
int counter(const std::string& name);
// Version du mod (Hello, test à distance).
const char* mod_version();
// Hôte Steam : id de son salon coop prêt (0 sinon) ; ce SteamID en est-il membre ? (test à distance : salon proposé
// au client par le canal à part, voir native/remote)
uint64_t host_lobby();
bool host_lobby_has(uint64_t steam_id);

} // namespace e33::coop
