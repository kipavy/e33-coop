#include "ui.hpp"

#include <deque>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "overlay.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

namespace e33::ui {

namespace {

struct Entry { const char* key; const char* fr; const char* en; };
// Table de traduction. Les textes restent courts : ils s'affichent en surimpression pendant le jeu.
constexpr Entry kTexts[] = {
    {"turn_of", "Tour de {0}", "{0}'s turn"},
    {"keys_host", "Coop : F7 inviter des amis · F8 prendre la main · hors combat : F5 choisir un personnage, F6 le donner au joueur suivant · F9 options",
                  "Coop: F7 invite friends · F8 take control · out of battle: F5 pick a character, F6 give it to the next player · F9 options"},
    {"owner_selected", "Choisi : {0} (joué par {1}). F6 pour le donner au joueur suivant, F5 pour un autre personnage",
                       "Selected: {0} (played by {1}). F6 gives it to the next player, F5 picks another character"},
    {"owners_summary", "Équipe : {0}", "Party: {0}"},
    {"keys_client", "Coop : F8 prendre la main · F9 options", "Coop: F8 take control · F9 options"},
    {"your_turn", "À vous de jouer", "Your turn"},
    {"turn_timer", "Tour de {0} : {1} s", "{0}'s turn: {1} s"},
    {"battle_close_menu", "Combat lancé : fermez le menu pour le rejoindre", "Battle started: close the menu to join it"},
    {"duo_parry", "Parade en duo ! +{1} PA ({0})", "Duo parry! +{1} AP ({0})"},
    {"turn_forced", "Temps écoulé : {0} attaque automatiquement", "Time is up: {0} attacks automatically"},
    {"pilot_at_merchant", "Le meneur est chez le marchand : ses achats arrivent chez vous", "The leader is at the merchant: their purchases reach you"},
    {"pilot_bought", "Le meneur a acheté un article", "The leader bought an item"},
    // Aperçu de l'action que prépare un autre joueur pendant son tour ({1} : action, {2} : cibles)
    {"intent_target", "{0} prépare {1} → {2}", "{0} is preparing {1} → {2}"},
    {"intent_action", "{0} prépare {1}", "{0} is preparing {1}"},
    {"intent_targets", "{0} vise {1}", "{0} is targeting {1}"},
    {"intent_attack", "une attaque", "an attack"},
    {"intent_skill_menu", "une compétence…", "a skill…"},
    {"intent_item_menu", "un objet…", "an item…"},
    {"intent_free_aim", "un tir en visée libre", "a free-aim shot"},
    {"paused_by", "Pause demandée par {0}", "Paused by {0}"},
    {"waiting_players", "En attente des autres joueurs…", "Waiting for the other players…"},
    {"waiting_battle_end", "En attente de la fin du combat de l'hôte…", "Waiting for the host's battle to end…"},
    {"joining", "Connexion à la partie de l'hôte…", "Joining the host's game…"},
    {"loading_host_game", "Chargement de la partie de l'hôte…", "Loading the host's game…"},
    {"resyncing", "Resynchronisation avec l'hôte…", "Resynchronizing with the host…"},
    // Défaite (D33) : seul l'hôte choisit « Réessayer » ou « Abandonner »
    {"defeat_host_choosing", "Défaite : {0} choisit (réessayer ou abandonner)…", "Defeat: {0} is choosing (retry or give up)…"},
    {"defeat_host_retry", "{0} réessaie le combat", "{0} retries the battle"},
    {"defeat_host_reserve", "{0} envoie l'équipe de réserve", "{0} sends the reserve team"},
    {"defeat_host_abandon", "{0} abandonne le combat : chargement de sa partie…", "{0} gave up the battle: loading their game…"},
    {"defeat_host_silent", "{0} ne choisit pas : à vous de choisir", "{0} is not choosing: your call"},
    {"player_joined", "{0} a rejoint la partie", "{0} joined the game"},
    {"player_left", "{0} a quitté la partie : l'hôte reprend ses personnages", "{0} left the game: the host takes over their characters"},
    {"player_lost", "Connexion perdue avec {0} : l'hôte reprend ses personnages", "Lost connection to {0}: the host takes over their characters"},
    {"host_left", "L'hôte a quitté la partie. Session terminée.", "The host left the game. Session over."},
    {"host_lost", "Connexion à l'hôte perdue. Session terminée.", "Lost connection to the host. Session over."},
    {"host_unreachable", "L'hôte ne répond pas. Session terminée.", "The host does not answer. Session over."},
    {"rejected", "Connexion refusée : {0}", "Connection refused: {0}"},
    {"reject_protocol", "versions du mod incompatibles (protocole hôte {0}, vous {1})", "incompatible mod versions (host protocol {0}, yours {1})"},
    {"reject_full", "la partie est complète", "the game is full"},
    {"pilot_is", "{0} mène l'exploration", "{0} is leading the exploration"},
    {"build_not_yours", "{0} est contrôlé par {1} : modification annulée", "{0} is controlled by {1}: change undone"},
    {"economy_rejected", "Achat ou amélioration impossible chez l'hôte : resynchronisation", "Purchase or upgrade not possible on the host: resynchronizing"},
    {"session_open", "Partie ouverte au coop", "Game open for coop"},
    {"owner_changed", "{0} est maintenant contrôlé par {1}", "{0} is now controlled by {1}"},
    {"steam_unavailable", "Steam indisponible pour le coop : {0}", "Steam is not available for coop: {0}"},
    {"steam_lobby_failed", "Lobby Steam impossible : {0}", "Steam lobby failed: {0}"},
    {"session_open_steam", "Partie ouverte au coop : F7 pour inviter un ami", "Game open for coop: press F7 to invite a friend"},
    {"steam_no_friend_lobby", "Aucun ami n'a ouvert de partie coop", "No friend has an open coop game"},
    {"coop_opened_menu", "Partie coop ouverte : invitez vos amis, puis lancez une nouvelle partie ou chargez la vôtre",
                         "Coop game open: invite your friends, then start a new game or load yours"},
    // Copie coop (D15)
    {"copy_host_hint", "Coop : F10 pour héberger la partie coop de {0} (copie du {1})", "Coop: press F10 to host {0}'s coop game (copy from {1})"},
    {"copy_client_hint", "Copie de la partie coop de {0} gardée (du {1}). Pour l'héberger : jeu lancé sans option -e33role, puis F10 au menu",
                         "Copy of {0}'s coop game kept (from {1}). To host it: start the game without -e33role, then F10 at the menu"},
    {"copy_hosting", "Partie coop de {0} (copie du {1}) : la progression est enregistrée dans la copie coop, pas dans vos sauvegardes",
                     "{0}'s coop game (copy from {1}): progress is saved in the coop copy, not in your saves"},
    {"copy_personal_save_blocked", "Coop : écriture dans vos sauvegardes perso bloquée", "Coop: write to your personal saves blocked"},
    {"copy_error", "Coop : copie de la partie impossible ({0})", "Coop: coop copy unavailable ({0})"},
    // Panneau « Coop » du menu pause (native/ui/owners_ui.cpp)
    {"coop_title", "Expédition coop", "Coop expedition"},
    {"coop_session", "Hôte : {0} · {1} joueur(s) · {2}", "Host: {0} · {1} player(s) · {2}"},
    {"coop_transport_steam", "Steam", "Steam"},
    {"coop_transport_udp", "IP directe", "Direct IP"},
    {"coop_you", "{0} (vous)", "{0} (you)"},
    {"coop_reserve", "réserve · {0}", "reserve · {0}"},
    {"coop_change_host", "Hors combat : F9 choisit une ligne, Maj+F9 change de joueur.",
                         "Out of battle: F9 picks a row, Shift+F9 changes the player."},
    {"coop_change_client", "La répartition est choisie par l'hôte.", "The host decides who plays whom."},
    {"hint_invite", "Inviter", "Invite"},
    {"hint_take", "Prendre la main", "Take control"},
    {"hint_pick", "Choisir", "Pick"},
    {"hint_give", "Donner", "Give"},
    {"you", "vous", "you"},
    {"host", "l'hôte", "the host"},
    // Attente détaillée : qui on attend et pourquoi ({1} : secondes écoulées, vide au début)
    {"wait_loading", "{0} charge la zone…{1}", "{0} is loading the area…{1}"},
    {"wait_joining", "{0} charge votre partie…{1}", "{0} is loading your game…{1}"},
    {"wait_cinematic", "En attente de {0}, qui regarde une cinématique", "Waiting for {0}, who is watching a cutscene"},
    {"wait_dialogue", "En attente de {0}, en pleine conversation", "Waiting for {0}, who is in a conversation"},
    {"wait_menu", "En attente de {0}, qui est dans un menu", "Waiting for {0}, who is in a menu"},
    {"wait_reconnecting", "En attente de {0}, qui se reconnecte…", "Waiting for {0} to reconnect…"},
    {"wait_battle_prep", "En attente de {0}, qui entre en combat…", "Waiting for {0} to enter the battle…"},
    {"wait_generic", "En attente de {0}…", "Waiting for {0}…"},
    // Attentes bornées : la partie ne reste jamais figée sur un joueur qui ne répond plus
    {"battle_started_without", "{0} n'entre pas dans le combat : il commence sans attendre", "{0} is not entering the battle: it starts without them"},
    {"turn_forced_stuck", "{0} ne répond plus : attaque automatique", "{0} is not responding: automatic attack"},
    {"seconds_suffix", " {0} s", " {0} s"},
    {"names_and", "{0} et {1}", "{0} and {1}"},
    // Joueur en retard (cinématique ou dialogue encore en cours ici)
    {"waited_by_one", "{0} a fini et vous attend", "{0} is done and waiting for you"},
    {"waited_by_many", "{0} ont fini et vous attendent", "{0} are done and waiting for you"},
    {"skip_hint", "Maintenir Échap pour passer", "Hold Esc to skip"},
    // Client : l'hôte n'est pas encore à un point sûr pour l'envoi de sa partie
    {"join_wait_narrative", "{0} regarde une cinématique : vous entrerez dans sa partie juste après", "{0} is watching a cutscene: you will join right after"},
    {"join_wait_loading", "{0} charge la zone : vous entrerez dans sa partie juste après", "{0} is loading the area: you will join right after"},
    // Menu principal : partie coop d'un ami
    {"join_button", "Rejoindre {0}", "Join {0}"},
    {"join_button_sub", "partie coop ouverte", "coop game open"},
    {"friend_invite", "{0} joue en coop · F7 Rejoindre sa partie", "{0} is playing coop · F7 Join their game"},
    // Mode neutre (D18) : rôle décidé en jeu
    {"a_player", "un joueur", "a player"},
    {"coop_ready_hint", "Coop : F7 pour ouvrir votre partie à vos amis Steam", "Coop: press F7 to open your game to your Steam friends"},
    {"invite_in_game", "Invitation coop reçue : revenez au menu principal pour rejoindre la partie de votre ami",
                       "Coop invite received: go back to the main menu to join your friend's game"},
    {"open_not_in_battle", "Coop : ouvrez votre partie hors combat (F7)", "Coop: open your game outside of battle (F7)"},
    {"coop_session_none", "Pas de session · F7 dans une partie chargée pour inviter un ami", "No session · F7 in a loaded game to invite a friend"},
    // Mises à jour (native/sync/update.cpp)
    {"update_available", "Nouvelle version {0} du mod coop disponible — U pour mettre à jour", "Coop mod version {0} is available — press U to update"},
    {"update_staged", "Mise à jour {0} du mod coop prête : installée à la fermeture du jeu", "Coop mod update {0} is ready: it installs when you close the game"},
    {"update_now", "Mise à jour du mod coop ({0})… le jeu va redémarrer", "Updating the coop mod ({0})… the game will restart"},
    {"update_need_ours", "L'hôte a la version {0} du mod coop, vous la {1} — U pour mettre à jour (le jeu redémarre)",
                         "The host has coop mod version {0}, you have {1} — press U to update (the game restarts)"},
    {"update_need_ours_off", "Version du mod différente ({0} chez l'autre joueur, {1} chez vous) : mettez à jour avec « Installer E33 Coop.exe »",
                             "Different mod version ({0} for the other player, {1} for you): update with \"Installer E33 Coop.exe\""},
    {"update_need_ours_host", "{0} a une version plus récente du mod coop ({1}, vous {2}) : U au menu principal pour mettre à jour",
                              "{0} has a newer coop mod ({1}, you {2}): press U at the main menu to update"},
    {"update_need_ours_host_auto", "{0} a une version plus récente du mod coop ({1}, vous {2}) : mise à jour installée à la fermeture du jeu",
                                   "{0} has a newer coop mod ({1}, you {2}): the update installs when you close the game"},
    {"update_need_other", "{0} doit mettre à jour le mod coop (sa version : {1}, la vôtre : {2})", "{0} must update the coop mod (their version: {1}, yours: {2})"},
    {"update_no_installer", "Installateur absent : téléchargez « Installer E33 Coop.exe » sur github.com/kipavy/e33-coop/releases",
                            "Installer missing: download \"Installer E33 Coop.exe\" from github.com/kipavy/e33-coop/releases"},
    // Options (native/ui/options.cpp)
    {"options_card", "Options coop {0}/{1} · {2} : {3} — F9 suivant · Maj+F9 changer", "Coop options {0}/{1} · {2}: {3} — F9 next · Shift+F9 change"},
    {"coop_options", "Options", "Options"},
    {"hint_row", "Ligne", "Row"},
    {"hint_change", "Changer", "Change"},
    {"opt_lang", "Langue du mod", "Mod language"},
    {"opt_updates", "Mises à jour", "Updates"},
    {"opt_aim_cam", "Visée de l'autre joueur", "Other player's aim view"},
    {"opt_turndep", "Attente sans figer", "Wait without freezing"},
    {"opt_duo_parry", "Parade en duo (hôte)", "Duo parry (host)"},
    {"opt_turn_timer", "Minuteur de tour (hôte)", "Turn timer (host)"},
    {"opt_auto", "auto (Windows)", "auto (Windows)"},
    {"opt_updates_auto", "auto (à la fermeture du jeu)", "auto (when the game closes)"},
    {"opt_updates_ask", "demander", "ask"},
    {"opt_updates_off", "désactivées", "off"},
    {"opt_on", "oui", "on"},
    {"opt_off", "non", "off"},
    {"opt_none", "aucun", "none"},
    {"opt_by_host", "choisi par l'hôte", "set by the host"},
    {"opt_cmdline", "(option de lancement)", "(launch option)"},
};

Lang g_lang = Lang::Fr;
std::map<std::string, std::string> g_status;
std::map<std::string, std::string> g_status_key;   // set_status_text : clé courante par emplacement
struct Toast { std::string text; double seconds; };
std::deque<Toast> g_pending;
int g_toasts = 0;
std::map<std::string, int> g_counts;

std::wstring arg(const wchar_t* key) { return cmdline::arg(key); }   // en test : options du scénario seulement

} // namespace

void init()
{
    const std::wstring l = arg(L"e33lang");
    if (l == L"en") g_lang = Lang::En;
    else if (l == L"fr") g_lang = Lang::Fr;
    else g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH ? Lang::Fr : Lang::En;
}

Lang lang() { return g_lang; }
void set_lang(Lang l) { g_lang = l; }

std::string text(const std::string& key, const std::vector<std::string>& args)
{
    std::string t = key;
    for (const auto& e : kTexts)
        if (key == e.key) { t = g_lang == Lang::Fr ? e.fr : e.en; break; }
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string ph = "{" + std::to_string(i) + "}";
        for (size_t p = t.find(ph); p != std::string::npos; p = t.find(ph, p + args[i].size())) t.replace(p, ph.size(), args[i]);
    }
    return t;
}

void set_status(const std::string& slot, const std::string& key, const std::vector<std::string>& args)
{
    if (key.empty()) { clear_status(slot); return; }
    const std::string t = text(key, args);
    auto it = g_status.find(slot);
    if (it != g_status.end() && it->second == t) return;
    g_status[slot] = t;
    g_status_key[slot] = key;
    ++g_counts[key];
    log().write("ui_status", JsonObj().add("slot", slot).add("key", key).add("text", t));
    overlay::set_line(slot, t);
}

void set_status_text(const std::string& slot, const std::string& key, const std::string& text)
{
    if (text.empty()) { clear_status(slot); return; }
    auto it = g_status.find(slot);
    if (it != g_status.end() && it->second == text) return;
    g_status[slot] = text;
    std::string& k = g_status_key[slot];
    if (k != key) {
        k = key;
        ++g_counts[key];
        log().write("ui_status", JsonObj().add("slot", slot).add("key", key).add("text", text));
    }
    overlay::set_line(slot, text);
}

void clear_status(const std::string& slot)
{
    g_status_key.erase(slot);
    if (!g_status.erase(slot)) return;
    log().write("ui_status", JsonObj().add("slot", slot).add("text", ""));
    overlay::set_line(slot, "");
}

void toast(const std::string& key, const std::vector<std::string>& args, double seconds)
{
    const std::string t = text(key, args);
    ++g_toasts;
    ++g_counts[key];
    log().write("ui_toast", JsonObj().add("key", key).add("text", t).add("s", seconds));
    overlay::toast(t, seconds);
}

void tick(double now) { overlay::tick(now); }

const std::map<std::string, std::string>& statuses() { return g_status; }
int toasts_shown() { return g_toasts; }
int count(const std::string& key) { auto it = g_counts.find(key); return it == g_counts.end() ? 0 : it->second; }

} // namespace e33::ui
