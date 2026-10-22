#include "presence.hpp"

#include <cmath>
#include <map>

#include "../ui/ui.hpp"

namespace e33::presence {

namespace {

constexpr double kBusyDebounce = 1.0;   // simple à-coup du thread de jeu (> 0,75 s) : pas encore un chargement
constexpr double kSilentReconnect = 3.0;   // lien muet (ni paquet, ni « occupé ») : connexion qui flanche
constexpr double kShowSeconds = 3.0;    // compteur de secondes affiché passé ce délai

struct P {
    bool busy = false;
    double busy_since = -1;
    double silent = 0;
    bool narr = false;
    bool cine = false;
    bool menu = false;
    bool joining = false;
    double joining_since = -1;
};
std::map<uint8_t, P> g;

std::string seconds(double since, double now)
{
    if (since < 0 || now - since < kShowSeconds) return "";
    return ui::text("seconds_suffix", {std::to_string(static_cast<int>(now - since))});
}

std::string clause(uint8_t player, double now, const NameFn& name, const char* idle_key)
{
    const std::string n = name ? name(player) : std::to_string(player);
    const P& p = g[player];
    switch (activity(player, now)) {
    case Activity::Reconnecting: return ui::text("wait_reconnecting", {n});
    case Activity::Joining: return ui::text("wait_joining", {n, seconds(p.joining_since, now)});
    case Activity::Loading: return ui::text("wait_loading", {n, seconds(p.busy_since, now)});
    case Activity::Cinematic: return ui::text("wait_cinematic", {n});
    case Activity::Dialogue: return ui::text("wait_dialogue", {n});
    case Activity::Menu: return ui::text("wait_menu", {n});
    case Activity::Ready: break;
    }
    return ui::text(idle_key ? idle_key : "wait_generic", {n});
}

std::string join(const std::vector<std::string>& parts, const char* sep)
{
    std::string t;
    for (auto& s : parts) { if (!t.empty()) t += sep; t += s; }
    return t;
}

} // namespace

void set_link(uint8_t player, bool busy, double silent_s, double now)
{
    P& p = g[player];
    if (busy && !p.busy) p.busy_since = now;
    if (!busy) p.busy_since = -1;
    p.busy = busy;
    p.silent = silent_s;
}

void set_narrative(uint8_t player, bool active, bool cinematic, double) { P& p = g[player]; p.narr = active; p.cine = cinematic; }
void set_menu(uint8_t player, bool open, double) { g[player].menu = open; }

void set_joining(uint8_t player, bool loading_snapshot, double now)
{
    P& p = g[player];
    if (loading_snapshot && !p.joining) p.joining_since = now;
    p.joining = loading_snapshot;
}

void forget(uint8_t player) { g.erase(player); }
void reset() { g.clear(); }

Activity activity(uint8_t player, double now)
{
    auto it = g.find(player);
    if (it == g.end()) return Activity::Ready;
    const P& p = it->second;
    if (!p.busy && p.silent > kSilentReconnect) return Activity::Reconnecting;
    if (p.joining) return Activity::Joining;
    if (p.busy && p.busy_since >= 0 && now - p.busy_since >= kBusyDebounce) return Activity::Loading;
    if (p.narr) return p.cine ? Activity::Cinematic : Activity::Dialogue;
    if (p.menu) return Activity::Menu;
    return Activity::Ready;
}

const char* activity_name(Activity a)
{
    switch (a) {
    case Activity::Ready: return "ready";
    case Activity::Loading: return "loading";
    case Activity::Joining: return "joining";
    case Activity::Cinematic: return "cinematic";
    case Activity::Dialogue: return "dialogue";
    case Activity::Menu: return "menu";
    case Activity::Reconnecting: return "reconnecting";
    }
    return "?";
}

std::string wait_text(const std::vector<uint8_t>& players, double now, const NameFn& name, const char* idle_key)
{
    std::vector<std::string> parts;
    for (uint8_t p : players) parts.push_back(clause(p, now, name, idle_key));
    return join(parts, " · ");
}

std::string waited_by_text(const std::vector<uint8_t>& waiting, const NameFn& name, bool skip_hint)
{
    if (waiting.empty()) return "";
    std::vector<std::string> names;
    for (uint8_t p : waiting) names.push_back(name ? name(p) : std::to_string(p));
    std::string t;
    if (names.size() == 1) t = ui::text("waited_by_one", {names[0]});
    else {
        const std::string last = names.back();
        names.pop_back();
        t = ui::text("waited_by_many", {ui::text("names_and", {join(names, ", "), last})});
    }
    if (skip_hint) t += " · " + ui::text("skip_hint");
    return t;
}

std::string loading_text(const std::vector<uint8_t>& players, double now, const NameFn& name)
{
    std::vector<std::string> parts;
    for (uint8_t p : players) {
        const Activity a = activity(p, now);
        if (a == Activity::Loading || a == Activity::Joining) parts.push_back(clause(p, now, name, nullptr));
    }
    return join(parts, " · ");
}

} // namespace e33::presence
