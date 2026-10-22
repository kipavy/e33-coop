// Sauvegarde côté client (D15) : chaque joueur garde une copie à jour de la progression coop, hors de ses emplacements
// perso (SaveGames\<SteamID>\ jamais touché), pour héberger plus tard la même partie (F10 au menu principal, rôle
// hôte) ou au moins ne pas la perdre. Copies : SaveGames\e33coop\copies\<hôte>.sav (+ .json, + .prev.sav).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace e33::coopsave {

void init(bool host, std::function<void(const char* counter, int n)> bump);
void subscribe();                 // garde-fous sur les sauvegardes du jeu (BP_SaveManager_C)
void tick(double now, bool can_host);   // game thread : menu principal (F10 si can_host), garde autosave, copie de l'hôte-copie

// La partie chargée vient de l'hôte (instantané, nouvelle partie suivie) : autosave coupée et tenue coupée.
void mark_coop_game();
// Client : instantané de l'hôte reçu et vérifié -> copie mise à jour avec ces octets.
void on_host_snapshot(const std::vector<uint8_t>& bytes, const std::string& host_name, const std::string& level);
// Client : empreinte d'exploration identique à celle de l'hôte, au calme -> copie rafraîchie localement si besoin.
void on_world_ok(uint64_t host_hash, const std::string& host_name);
// Hôte : instantané envoyé à un client (partie hébergée depuis une copie -> la copie reprend ces octets).
void on_snapshot_sent(const std::vector<uint8_t>& bytes);
// Retour au menu principal demandé (menu pause) : dernière copie si l'on héberge une copie.
void on_quit_to_title();

bool hosting_copy();
// Hôte : charge la copie la plus récente (autosave coupée) ; la progression ira dans cette copie.
bool host_latest_copy(std::string& err);
// Tests : installe une save (slot relatif à SaveGames, ex. "e33coop-test/TEST_B") comme copie coop de host_name.
bool install_copy(const std::wstring& slot, const std::string& host_name, std::string& err);

} // namespace e33::coopsave
