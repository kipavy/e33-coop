// Transport Steam (phase 6) : Steam Networking Messages (P2P + relais Steam Datagram Relay) derrière net::Transport,
// et lobbies Steam pour les invitations. Utilise le steam_api64.dll déjà chargé par le jeu (Steamworks 1.57), par son
// API « flat » résolue à l'exécution : pas de SDK à la compilation, pas d'initialisation de Steam par le mod (c'est le
// jeu qui l'a faite), aucun contournement de Steam.
//
// Adresse d'un pair : "steam:<SteamID64>". Le Link (fiabilité, ordre, fragments) reste le même qu'en UDP : les
// messages Steam sont envoyés en non fiable.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "transport.hpp"

namespace e33::net::steam {

// Résout l'API Steam du jeu. false (et err) si le DLL ou une fonction manque, ou si Steam ne tourne pas.
bool load(std::string& err);
bool loaded();
uint64_t my_id();
std::string persona_name();
std::string peer_of(uint64_t id);       // "steam:<id>"
uint64_t id_of(const std::string& peer); // 0 si ce n'est pas une adresse Steam

std::unique_ptr<Transport> make_transport(std::string& err);

// « Ouvre » la session Steam avec un utilisateur : un premier paquet (4 octets "E33P", ignoré à la réception) et
// l'acceptation d'une session en attente. Sans cela, Steam garde en attente les messages d'un inconnu.
void punch(Transport& t, uint64_t id);
bool is_punch(const uint8_t* data, size_t len);

// --- Lobbies (invitations) ------------------------------------------------------------------------
enum class LobbyState { None, Creating, Joining, Ready, Failed };
struct Lobby {
    LobbyState state = LobbyState::None;
    uint64_t id = 0;
    uint64_t owner = 0;
    std::string error;
};
// Hôte : crée un lobby « amis seulement » de 3 places, marqué pour le coop (version du mod, protocole).
void create_lobby(Lobby& l, const std::string& mod_version, int protocol);
// Client : rejoint un lobby (invitation acceptée, ligne de commande +connect_lobby, lobby d'un ami).
void join_lobby(Lobby& l, uint64_t lobby_id);
// À appeler à chaque tick : fait avancer la création / l'entrée (résultats d'appels Steam, sans callback).
void poll_lobby(Lobby& l);
void leave_lobby(Lobby& l);
std::vector<uint64_t> lobby_members(const Lobby& l);
std::string lobby_data(uint64_t lobby_id, const char* key);
// Ouvre la fenêtre d'invitation de l'overlay Steam pour ce lobby.
void invite_dialog(const Lobby& l);
// Lobby passé par Steam sur la ligne de commande (« +connect_lobby <id> », invitation acceptée jeu fermé), 0 sinon.
uint64_t command_line_lobby();
// Invitation acceptée pendant que le jeu tourne (GameLobbyJoinRequested_t), 0 sinon ; consommée à la lecture.
uint64_t take_join_request();
// Lobbies coop ouverts par des amis qui jouent au jeu (données locales de Steam : appel peu coûteux, à espacer
// quand même de quelques secondes). Lobby d'une autre version du mod (quand elle est connue) : écarté.
struct FriendLobby {
    uint64_t lobby = 0;
    uint64_t friend_id = 0;
    std::string name;      // nom Steam de l'ami
    std::string mod;       // version du mod du lobby (vide si pas encore connue)
    bool compatible = true;
};
// any_version : aussi les lobbies d'une autre version du mod (compatible = false).
std::vector<FriendLobby> friend_lobbies(const std::string& mod_version, bool any_version = false);
// Premier ami qui joue au jeu dans un lobby coop (rejoindre depuis le menu principal), 0 sinon.
uint64_t find_friend_lobby(const std::string& mod_version);

// --- Test à distance (canal Steam à part, messages fiables de Steam ; voir native/remote) -------------------------------
// Lobby dans lequel se trouve cet ami s'il joue au jeu, 0 sinon.
uint64_t friend_lobby(uint64_t friend_id);
// --- Diagnostic : salons d'amis invisibles quand le jeu est relancé par un script (essai à deux PC du 2026-10-23) ---
// AppID vu par le jeu (ISteamUtils::GetAppID), 0 si Steam n'est pas chargé.
uint32_t app_id();
// Objet JSON (lecture seule) : AppID, connexion, nombre d'amis (flag immediate), amis qui jouent à ce jeu (au plus 10)
// et, pour chaque SteamID de focus (hôte de confiance, ami du rejoin.txt…), le retour brut de GetFriendGamePlayed
// (ok, game_id, lobby), l'état de persona et HasFriend.
std::string friends_diag_json(const std::vector<uint64_t>& focus);
// Objet JSON : processus du jeu (PID, parent et son exe, ligne de commande, dossier courant, variables Steam*).
std::string process_diag_json();
// Nom Steam d'un ami (vide si inconnu).
std::string friend_name(uint64_t id);
// Accepte la session P2P en attente de cet utilisateur (et seulement lui).
void accept_user(uint64_t id);
// Envoi brut sur un canal ; renvoie le code EResult de Steam (1 = OK, 25 = tampon d'envoi plein : réessayer).
int send_raw(uint64_t to, const void* data, size_t len, bool reliable, int channel);
struct RawMessage { uint64_t from = 0; std::vector<uint8_t> data; };
// Messages reçus sur un canal (au plus max) ; false si Steam n'est pas chargé.
bool receive_raw(int channel, std::vector<RawMessage>& out, int max = 64);

} // namespace e33::net::steam
