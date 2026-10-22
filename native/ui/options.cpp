#include "options.hpp"

#include <map>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/release.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../sync/coop.hpp"
#include "../sync/intent.hpp"
#include "owners_ui.hpp"
#include "ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

namespace e33::options {

namespace {

constexpr double kIdleClose = 20.0;   // sélection oubliée après 20 s sans touche

int g_sel = -1;
double g_now = 0, g_last_key = 0;
bool g_f9_down = false;
bool g_card = false;
std::map<std::string, int> g_counts;

struct Opt { const char* key; const char* label; bool host_only; };
constexpr Opt kOpts[] = {
    {"lang", "opt_lang", false},
    {"updates", "opt_updates", false},
    {"aim_cam", "opt_aim_cam", false},
    {"turndep", "opt_turndep", false},
    {"duo_parry", "opt_duo_parry", true},
    {"turn_timer_s", "opt_turn_timer", true},
};

std::string ue_free_narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Valeur effective (option de lancement ou settings.ini) dans le format du fichier.
std::string current(const char* key)
{
    for (const auto& d : release::kSettings) {
        if (std::string_view(d.key) != key) continue;
        std::string v = ue_free_narrow(cmdline::arg(cmdline::widen(d.arg).c_str()));
        if (std::string_view(key) == "lang" && v.empty()) v = "auto";
        const std::string n = release::normalize_setting(key, v);
        return n.empty() ? std::string(d.def) : n;
    }
    return {};
}

std::string value_text(const char* key, const std::string& v)
{
    const std::string_view k = key;
    if (k == "lang") return v == "fr" ? "Français" : v == "en" ? "English" : ui::text("opt_auto");
    if (k == "updates") return ui::text(v == "ask" ? "opt_updates_ask" : v == "off" ? "opt_updates_off" : "opt_updates_auto");
    if (k == "turn_timer_s") return v == "0" ? ui::text("opt_none") : v + " s";
    return ui::text(v == "1" ? "opt_on" : "opt_off");
}

bool is_client() { return coop::role() == coop::Role::Client; }

// Personnages modifiables : hôte, hors combat.
std::vector<std::string> heroes()
{
    if (coop::role() != coop::Role::Host || game::in_battle() || !game::controller()) return {};
    return game::party_names();
}

void apply(const char* key)
{
    const std::string_view k = key;
    if (k == "lang") ui::init();
    else if (k == "aim_cam") intent::reload_options();
    else coop::reload_options();
}

} // namespace

std::vector<Row> rows()
{
    std::vector<Row> out;
    for (const auto& h : heroes()) out.push_back({h, coop::name_of(coop::hero_owner(h)), true, true, false});
    for (const auto& o : kOpts) {
        Row r;
        r.label = ui::text(o.label);
        r.hero = false;
        if (o.host_only && is_client()) { r.value = ui::text("opt_by_host"); r.editable = false; }
        else {
            r.value = value_text(o.key, current(o.key));
            r.editable = !cmdline::from_command_line(o.key) || cmdline::test_mode();
            if (cmdline::from_command_line(o.key) && !cmdline::test_mode()) r.value += " " + ui::text("opt_cmdline");
        }
        out.push_back(r);
    }
    if (g_sel >= 0 && g_sel < static_cast<int>(out.size())) out[static_cast<size_t>(g_sel)].selected = true;
    return out;
}

int selected() { return g_sel; }

void select(int index)
{
    const int n = static_cast<int>(rows().size());
    g_sel = index >= 0 && index < n ? index : -1;
    g_last_key = g_now;
    log().write("options_select", JsonObj().add("row", g_sel));
}

bool change()
{
    if (g_sel < 0) return false;
    g_last_key = g_now;
    const auto hs = heroes();
    if (g_sel < static_cast<int>(hs.size())) {
        coop::reassign_character(static_cast<size_t>(g_sel));   // même chemin que F6 (Owners diffusé)
        ++g_counts["panel_changes"];
        log().write("options_owner_change", JsonObj().add("row", g_sel).add("character", hs[static_cast<size_t>(g_sel)]));
        return true;
    }
    const size_t oi = static_cast<size_t>(g_sel) - hs.size();
    if (oi >= std::size(kOpts)) return false;
    const Opt& o = kOpts[oi];
    if ((o.host_only && is_client()) || (cmdline::from_command_line(o.key) && !cmdline::test_mode())) { ++g_counts["option_refused"]; return false; }
    const std::string next = release::next_setting_value(o.key, current(o.key));
    if (!cmdline::test_mode() && !cmdline::save_setting(o.key, next)) {
        log().write("options_error", JsonObj().add("key", o.key).add("what", "settings.ini"));
        return false;
    }
    if (!cmdline::test_mode()) apply(o.key);
    ++g_counts["option_changes"];
    log().write("options_changed", JsonObj().add("key", o.key).add("value", next));
    return true;
}

int count(const std::string& name)
{
    auto it = g_counts.find(name);
    return it == g_counts.end() ? 0 : it->second;
}

void tick(double dt)
{
    g_now += dt;
    if (!cmdline::test_mode()) {
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !g_f9_down) {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid == GetCurrentProcessId()) {
                const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                if (shift) { if (g_sel < 0) select(0); else change(); }
                else {
                    const int n = static_cast<int>(rows().size());
                    select(g_sel + 1 >= n ? -1 : g_sel + 1);
                }
            }
        }
        g_f9_down = down;
    }
    if (g_sel >= 0 && g_now - g_last_key > kIdleClose) select(-1);
    // Hors du panneau du menu pause : carte avec la ligne choisie.
    const bool card = g_sel >= 0 && !owners_ui::pause_panel_shown();
    if (card) {
        const auto r = rows();
        if (g_sel < static_cast<int>(r.size())) {
            const Row& row = r[static_cast<size_t>(g_sel)];
            ui::set_status_text("options", "options_card", ui::text("options_card", {std::to_string(g_sel + 1), std::to_string(r.size()), row.label, row.value}));
        }
    } else if (g_card) ui::clear_status("options");
    g_card = card;
}

} // namespace e33::options
