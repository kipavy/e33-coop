#include "ui.hpp"

#include <deque>

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
    {"your_turn", "À vous de jouer", "Your turn"},
    {"turn_timer", "Tour de {0} : {1} s", "{0}'s turn: {1} s"},
    {"turn_forced", "Temps écoulé : {0} attaque automatiquement", "Time is up: {0} attacks automatically"},
    {"paused_by", "Pause demandée par {0}", "Paused by {0}"},
    {"waiting_players", "En attente des autres joueurs…", "Waiting for the other players…"},
    {"waiting_battle_end", "En attente de la fin du combat de l'hôte…", "Waiting for the host's battle to end…"},
    {"joining", "Connexion à la partie de l'hôte…", "Joining the host's game…"},
    {"loading_host_game", "Chargement de la partie de l'hôte…", "Loading the host's game…"},
    {"resyncing", "Resynchronisation avec l'hôte…", "Resynchronizing with the host…"},
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
    {"build_applied", "{1} a modifié {0}", "{1} changed {0}"},
    {"economy_rejected", "Achat ou amélioration impossible chez l'hôte : resynchronisation", "Purchase or upgrade not possible on the host: resynchronizing"},
    {"session_open", "Partie ouverte au coop", "Game open for coop"},
    {"owner_changed", "{0} est maintenant contrôlé par {1}", "{0} is now controlled by {1}"},
    {"you", "vous", "you"},
    {"host", "l'hôte", "the host"},
};

Lang g_lang = Lang::Fr;
std::map<std::string, std::string> g_status;
struct Toast { std::string text; double seconds; };
std::deque<Toast> g_pending;
int g_toasts = 0;
std::map<std::string, int> g_counts;

std::wstring arg(const wchar_t* key)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring out;
    const std::wstring k = std::wstring(L"-") + key + L"=";
    for (int i = 0; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() > k.size() && _wcsnicmp(a.c_str(), k.c_str(), k.size()) == 0) out = a.substr(k.size());
    }
    if (argv) LocalFree(argv);
    return out;
}

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
    ++g_counts[key];
    log().write("ui_status", JsonObj().add("slot", slot).add("key", key).add("text", t));
    overlay::set_line(slot, t);
}

void clear_status(const std::string& slot)
{
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
