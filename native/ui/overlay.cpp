#include "overlay.hpp"

#include <deque>
#include <map>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"

// Affichage avec les cartes « toast » du jeu (WBP_ToastCard : bandeau en haut de l'écran, texte riche, fondu), vu en
// jeu le 2026-10-20 : créées par WidgetBlueprintLibrary::Create puis AddToViewport, texte posé par Init(FText, thème)
// APRÈS l'ajout à l'écran (avant, le widget garde son texte par défaut).
//  - une carte persistante pour les indicateurs (tour de X, pause demandée par X, attente…), lignes jointes ;
//  - une carte par message temporaire, décalée sous la première, qui disparaît d'elle-même après sa durée.
// Les widgets meurent avec le monde (changement de niveau, retour au menu) : on les oublie quand le contrôleur change.

using RC::Unreal::UObject;

namespace e33::ui::overlay {

namespace {

constexpr const wchar_t* kCardClass = L"/Game/UI/Widgets/CommonElements/ToastNotification/WBP_ToastCard.WBP_ToastCard_C";
constexpr double kStatusDuration = 1.0e7;   // la carte d'état ne disparaît pas d'elle-même
constexpr double kLineHeight = 42.0;        // décalage vertical (unités d'écran) entre cartes

std::map<std::string, std::string> g_lines;
bool g_lines_dirty = false;
struct T { std::string text; double seconds; };
std::deque<T> g_pending;
double g_now = 0;
double g_next_try = 0;

UObject* g_owner = nullptr;      // contrôleur du joueur au moment de la création
UObject* g_status = nullptr;     // carte persistante
struct Active { UObject* w; double until; int slot; };
std::vector<Active> g_toasts;

UObject* player_controller()
{
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    ue::Call c(gps, STR("GetPlayerController"));
    if (!c || !game::gi()) return nullptr;
    c.set_object(STR("WorldContextObject"), game::gi()).set_int(STR("PlayerIndex"), 0);
    if (!c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

// FText d'une chaîne (KismetTextLibrary::Conv_StringToText) : octets bruts de la valeur de retour.
bool make_text(const std::string& s, std::vector<uint8_t>& out)
{
    UObject* ktl = ue::find_object(STR("/Script/Engine.Default__KismetTextLibrary"));
    ue::Call c(ktl, STR("Conv_StringToText"));
    if (!c) return false;
    const std::wstring w = ue::from_utf8(s);
    c.set_string(STR("InString"), w.c_str());
    if (!c.invoke()) return false;
    const size_t n = c.param_size(STR("ReturnValue"));
    const auto* p = static_cast<const uint8_t*>(c.param_ptr(STR("ReturnValue")));
    if (!p || n == 0) return false;
    out.assign(p, p + n);
    return true;
}

UObject* create_card(UObject* pc, const std::string& text, double duration, int slot)
{
    UObject* cls = game::load_asset(kCardClass);
    UObject* wbl = ue::find_object(STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
    if (!cls || !wbl) { log().write("ui_overlay_error", JsonObj().add("what", "class").add("cls", cls != nullptr).add("wbl", wbl != nullptr)); return nullptr; }
    ue::Call create(wbl, STR("Create"));
    if (!create) return nullptr;
    create.set_object(STR("WorldContextObject"), pc).set_object(STR("WidgetType"), cls).set_object(STR("OwningPlayer"), pc);
    if (!create.invoke()) return nullptr;
    UObject* w = create.out_object(STR("ReturnValue"));
    if (!w) { log().write("ui_overlay_error", JsonObj().add("what", "Create returned null")); return nullptr; }
    ue::set_double(w, STR("Duration"), duration);
    ue::Call add(w, STR("AddToViewport"));
    // Au-dessus des menus du jeu (inventaire, équipe : la bannière « fermez le menu » y restait cachée, 2026-10-24).
    if (add) add.set_int(STR("ZOrder"), 30000 + slot).invoke();
    if (slot > 0) {
        ue::Call tr(w, STR("SetRenderTranslation"));
        if (tr) {
            if (auto* v = static_cast<double*>(tr.param_ptr(STR("Translation")))) { v[0] = 0; v[1] = kLineHeight * slot; }
            tr.invoke();
        }
    }
    std::vector<uint8_t> ft;
    ue::Call init(w, STR("Init"));
    const bool text_ok = make_text(text, ft);
    if (init && text_ok) init.set_raw(STR("Text"), ft.data()).set_int(STR("Theme"), 1).invoke();
    log().write("ui_overlay_card", JsonObj().add("slot", slot).add("widget", ue::short_name(w)).add("init", static_cast<bool>(init)).add("text_ok", text_ok)
                                         .add("text_size", static_cast<int64_t>(ft.size())));
    return w;
}

void set_card_text(UObject* w, const std::string& text)
{
    std::vector<uint8_t> ft;
    ue::Call c(w, STR("SetText"));
    if (c && make_text(text, ft)) c.set_raw(STR("InText"), ft.data()).invoke();
}

void remove_card(UObject* w)
{
    ue::Call c(w, STR("RemoveFromParent"));
    if (c) c.invoke();
}

std::string status_text()
{
    std::string t;
    for (auto& [slot, line] : g_lines) { if (!t.empty()) t += "   ·   "; t += line; }
    return t;
}

} // namespace

void set_line(const std::string& slot, const std::string& text)
{
    if (text.empty()) g_lines.erase(slot);
    else g_lines[slot] = text;
    g_lines_dirty = true;
}

void toast(const std::string& text, double seconds) { g_pending.push_back({text, seconds}); }

void tick(double now)
{
    g_now = now;
    if (now < g_next_try) return;
    g_next_try = now + 0.25;   // ProcessEvent : pas à chaque image
    UObject* pc = game::controller() ? game::controller() : (game::main_menu() ? player_controller() : nullptr);
    if (!pc) return;
    if (pc != g_owner) {
        // Nouveau monde : les anciens widgets ont été détruits avec lui.
        g_owner = pc;
        g_status = nullptr;
        g_toasts.clear();
        g_lines_dirty = true;
    }
    // Indicateurs persistants
    if (g_lines_dirty) {
        g_lines_dirty = false;
        const std::string t = status_text();
        if (t.empty()) {
            if (g_status) remove_card(g_status);
            g_status = nullptr;
        } else if (!g_status) {
            g_status = create_card(pc, t, kStatusDuration, 0);
        } else {
            set_card_text(g_status, t);
        }
    }
    // Messages temporaires : sous la carte d'état, empilés.
    for (size_t i = 0; i < g_toasts.size();) {
        if (now > g_toasts[i].until) g_toasts.erase(g_toasts.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    while (!g_pending.empty() && g_toasts.size() < 3) {
        const T t = g_pending.front();
        g_pending.pop_front();
        int slot = 1;
        for (bool used = true; used;) {
            used = false;
            for (auto& a : g_toasts) if (a.slot == slot) { used = true; ++slot; }
        }
        if (UObject* w = create_card(pc, t.text, t.seconds, slot)) g_toasts.push_back({w, now + t.seconds + 1.0, slot});
        log().write("ui_overlay_toast", JsonObj().add("text", t.text).add("slot", slot));
    }
}

} // namespace e33::ui::overlay
