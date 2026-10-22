#include "owners_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <map>
#include <vector>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/reflect.hpp"
#include "../hooks/ue.hpp"
#include "../sync/coop.hpp"
#include "ui.hpp"

// Chemins vérifiés dans le dump (build 23765773) :
//  - ordre des tours : BM.BattleScreenWidget (WBP_HUD_BattleScreen_C) .WBP_HUD_DriveG_TimeContainer .WBP_HUD_TurnOrder
//    (WBP_HUD_TurnOrder_C : Portraits_List, PortraitsToCharacters = TMap<portrait, AC_jRPG_CharacterBattleStats_C*>) ;
//    portrait WBP_HUD_TurnOrder_Portrait_C : Ally_Box (UOverlay) contient l'image du héros ;
//  - menu pause : contrôleur.PauseMenuWidget (WBP_PauseMenu_C) .PausePanel (UOverlay, page principale du sélecteur :
//    caché avec elle quand les paramètres s'ouvrent) ; police copiée de ReturnWrappingText.Text (bouton « Écran titre »).
// Les widgets créés portent un nom « E33Coop_<Tag>_<n> » : on les retrouve parmi les enfants à chaque passage (aucun
// pointeur gardé d'un passage à l'autre, donc rien de périmé quand le jeu détruit son HUD).
// Bouton « Coop » dans la liste du menu pause : écarté (boutons CommonUI liés au graphe BP et à la navigation manette) ;
// le panneau s'affiche donc de lui-même à droite dès que le menu pause est ouvert.

using namespace RC;
using namespace RC::Unreal;

namespace e33::owners_ui {

namespace {

struct Rgba { float r, g, b, a; };
// Couleur sRGB 0xRRGGBB -> FLinearColor (UMG travaille en linéaire).
Rgba col(uint32_t hex, float a = 1.f)
{
    auto lin = [](uint32_t c) { return std::pow(static_cast<float>(c) / 255.f, 2.2f); };
    return {lin((hex >> 16) & 0xFF), lin((hex >> 8) & 0xFF), lin(hex & 0xFF), a};
}
const Rgba kInk = col(0xf1ece2), kDim = col(0xa29d93), kGold = col(0xe2cba0), kPanel = col(0x141312, 0.9f),
           kKeyBg = col(0x1c1b19), kKeyRim = col(0xcbb68c), kLine = col(0xece6da, 0.14f);
// Hôte : or ; puis azur, rose, vert (maquette).
Rgba player_col(uint8_t id)
{
    static const uint32_t pal[] = {0xe4c27e, 0x86c5e8, 0xe89a9a, 0x9fd39a};
    return col(pal[std::min<size_t>(id, 3)]);
}

// Énumérations UMG
enum : int { HFill = 0, HLeft = 1, HCenter = 2, HRight = 3 };
enum : int { VFill = 0, VTop = 1, VCenter = 2, VBottom = 3 };
constexpr int kHitTestInvisible = 3;   // ESlateVisibility
constexpr int kJustifyCenter = 1;      // ETextJustify

// Mise en page (unités Slate, référence 1920x1080)
constexpr float kBadge = 22.f;         // côté du losange (avant rotation)
constexpr float kBadgeOut = -5.f;      // marge droite : le losange déborde un peu du portrait
constexpr float kPanelWidth = 640.f;

std::map<std::string, int> g_counts;
double g_now = 0, g_next = 0;
bool g_panel_shown = false;
bool g_panel_failed = false;   // panneau impossible : le rappel des touches en carte (coop.cpp) reprend
std::string g_panel_sig;
std::map<UObject*, std::string> g_badge_sig;   // badge retrouvé vivant dans son portrait -> contenu affiché
bool g_logged_no_turn_order = false;

// --- Construction de widgets ------------------------------------------------------------------

UObject* make(const wchar_t* cls_path, UObject* outer, const wchar_t* tag)
{
    auto* cls = static_cast<UClass*>(ue::find_object(cls_path));
    if (!cls || !outer) return nullptr;
    static int seq = 0;
    const std::wstring name = std::wstring(L"E33Coop_") + tag + L"_" + std::to_wstring(++seq);   // unique dans l'outer
    FStaticConstructObjectParameters p{cls, outer};
    p.Name = FName(name.c_str(), FNAME_Add);
    return UObjectGlobals::StaticConstructObject(p);
}

void call_floats(UObject* o, const wchar_t* fn, const wchar_t* param, std::initializer_list<float> v)
{
    ue::Call c(o, fn);
    void* d = c.param_ptr(param);
    if (!d || c.param_size(param) < v.size() * sizeof(float)) return;
    std::memcpy(d, v.begin(), v.size() * sizeof(float));
    c.invoke();
}
void call_color(UObject* o, const wchar_t* fn, const wchar_t* param, const Rgba& c) { call_floats(o, fn, param, {c.r, c.g, c.b, c.a}); }
void call_int(UObject* o, const wchar_t* fn, const wchar_t* param, int64_t v)
{
    ue::Call c(o, fn);
    if (c) c.set_int(param, v).invoke();
}
// FVector2D : doubles en UE5 (16 o), floats sinon.
void call_vec2(UObject* o, const wchar_t* fn, const wchar_t* param, double x, double y)
{
    ue::Call c(o, fn);
    void* d = c.param_ptr(param);
    if (!d) return;
    if (c.param_size(param) == 16) { auto* v = static_cast<double*>(d); v[0] = x; v[1] = y; }
    else if (c.param_size(param) == 8) { auto* v = static_cast<float*>(d); v[0] = static_cast<float>(x); v[1] = static_cast<float>(y); }
    else return;
    c.invoke();
}
// Ajout d'un enfant (SetContent, AddChildToOverlay…) : renvoie le slot.
UObject* add_child(UObject* parent, const wchar_t* fn, UObject* child)
{
    ue::Call c(parent, fn);
    if (!c || !child) return nullptr;
    c.set_object(L"Content", child).invoke();
    return c.out_object(L"ReturnValue");
}
void align(UObject* w_or_slot, int h, int v)
{
    call_int(w_or_slot, L"SetHorizontalAlignment", L"InHorizontalAlignment", h);
    call_int(w_or_slot, L"SetVerticalAlignment", L"InVerticalAlignment", v);
}
void pad(UObject* w_or_slot, float l, float t, float r, float b) { call_floats(w_or_slot, L"SetPadding", L"InPadding", {l, t, r, b}); }
void remove_widget(UObject* w)
{
    ue::Call c(w, L"RemoveFromParent");
    if (c) c.invoke();
}

// FText d'une chaîne (KismetTextLibrary::Conv_StringToText), copié brut dans le paramètre (comme overlay.cpp).
void set_text(UObject* tb, const std::string& s)
{
    UObject* ktl = ue::find_object(L"/Script/Engine.Default__KismetTextLibrary");
    ue::Call conv(ktl, L"Conv_StringToText");
    if (!conv) return;
    const std::wstring w = ue::from_utf8(s);
    conv.set_string(L"InString", w.c_str());
    if (!conv.invoke()) return;
    const void* ft = conv.param_ptr(L"ReturnValue");
    ue::Call c(tb, L"SetText");
    if (c && ft && c.param_size(L"InText") == conv.param_size(L"ReturnValue")) c.set_raw(L"InText", ft).invoke();
}

// Police d'un texte du jeu (src), taille changée. Copie brute dans le tampon de SetFont : le jeu en fait sa propre
// copie, et le tampon n'est jamais détruit (compteurs de références inchangés). src nul : police par défaut du TextBlock.
void set_font(UObject* tb, UObject* src, float size)
{
    ue::Call c(tb, L"SetFont");
    auto* sp = CastField<FStructProperty>(c.param_property(L"InFontInfo"));
    void* dst = c.param_ptr(L"InFontInfo");
    UObject* from = src ? src : tb;
    FProperty* fp = ue::prop(from, L"Font");
    if (!sp || !dst || !fp || fp->GetSize() != sp->GetSize()) return;
    std::memcpy(dst, fp->ContainerPtrToValuePtr<void>(from), static_cast<size_t>(sp->GetSize()));
    if (FProperty* sz = ue::struct_member(sp->GetStruct(), L"Size"); sz && sz->GetSize() == 4)
        *static_cast<float*>(sz->ContainerPtrToValuePtr<void>(dst)) = size;
    c.invoke();
}

UObject* text(UObject* outer, const std::string& s, const Rgba& color, float size, UObject* font_src)
{
    UObject* tb = make(L"/Script/UMG.TextBlock", outer, L"Text");
    if (!tb) return nullptr;
    set_font(tb, font_src, size);
    set_text(tb, s);
    call_color(tb, L"SetColorAndOpacity", L"InColorAndOpacity", color);   // FSlateColor : couleur puis règle 0 (couleur donnée)
    call_vec2(tb, L"SetShadowOffset", L"InShadowOffset", 1, 1);
    call_color(tb, L"SetShadowColorAndOpacity", L"InShadowColorAndOpacity", {0, 0, 0, 0.85f});
    return tb;
}

UObject* border(UObject* outer, const Rgba& c, float p, int h, int v, const wchar_t* tag = L"Border")
{
    UObject* b = make(L"/Script/UMG.Border", outer, tag);
    if (!b) return nullptr;
    call_color(b, L"SetBrushColor", L"InBrushColor", c);   // pinceau par défaut : rectangle blanc, teinté
    pad(b, p, p, p, p);
    align(b, h, v);
    return b;
}

UObject* sizebox(UObject* outer, float w, float h, const wchar_t* tag = L"Size")
{
    UObject* sb = make(L"/Script/UMG.SizeBox", outer, tag);
    if (!sb) return nullptr;
    if (w > 0) call_floats(sb, L"SetWidthOverride", L"InWidthOverride", {w});
    if (h > 0) call_floats(sb, L"SetHeightOverride", L"InHeightOverride", {h});
    return sb;
}

// Losange : boîte tournée de 45°, liseré couleur du joueur, fond sombre, initiale redressée.
UObject* diamond(UObject* outer, float side, const Rgba& c, const std::string& initial, UObject* font_src, const wchar_t* tag)
{
    UObject* sb = sizebox(outer, side, side, tag);
    if (!sb) return nullptr;
    call_floats(sb, L"SetRenderTransformAngle", L"Angle", {45.f});
    if (initial.empty()) {   // petit losange plein (panneau)
        UObject* fill = border(outer, c, 0, HFill, VFill);
        if (!fill) return nullptr;
        add_child(sb, L"SetContent", fill);
        return sb;
    }
    UObject* rim = border(outer, c, 2, HFill, VFill);
    UObject* fill = border(outer, kPanel, 0, HCenter, VCenter);
    UObject* tb = text(outer, initial, c, side * 0.5f, font_src);
    if (!rim || !fill || !tb) return nullptr;
    call_floats(tb, L"SetRenderTransformAngle", L"Angle", {-45.f});
    call_int(tb, L"SetJustification", L"InJustification", kJustifyCenter);
    add_child(fill, L"SetContent", tb);
    add_child(rim, L"SetContent", fill);
    add_child(sb, L"SetContent", rim);
    return sb;
}

// Widgets du mod parmi les enfants d'un panneau (nom « E33Coop_<tag>_… »).
std::vector<UObject*> ours(UObject* panel, const std::string& prefix)
{
    std::vector<UObject*> out;
    for (UObject* slot : ue::get_object_array(panel, L"Slots")) {
        UObject* w = ue::get_object(slot, L"Content");
        if (w && ue::short_name(w).rfind(prefix, 0) == 0) out.push_back(w);
    }
    return out;
}

UObject* tree_of(UObject* user_widget)
{
    UObject* t = ue::get_object(user_widget, L"WidgetTree");
    return t ? t : user_widget;
}

std::string initial(const std::string& name)
{
    const std::wstring w = ue::from_utf8(name);
    if (w.empty()) return "?";
    return ue::to_utf8(std::wstring(1, static_cast<wchar_t>(std::towupper(w[0]))));
}

std::map<uint8_t, std::string> player_names()
{
    std::map<uint8_t, std::string> m;
    for (auto& [id, n] : coop::players()) m[id] = n;
    return m;
}

// --- Combat : losanges dans l'ordre des tours ---------------------------------------------------

void tick_badges()
{
    if (!game::in_battle()) { g_badge_sig.clear(); return; }
    UObject* screen = ue::get_object(game::battle_manager(), L"BattleScreenWidget");
    UObject* order = ue::get_object(ue::get_object(screen, L"WBP_HUD_DriveG_TimeContainer"), L"WBP_HUD_TurnOrder");
    if (!order) {
        if (!g_logged_no_turn_order) { g_logged_no_turn_order = true; log().write("owners_ui_no_turn_order", JsonObj().add("screen", screen != nullptr)); }
        return;
    }
    // Héros : composant de stats -> joueur (comparaisons de pointeurs seulement)
    std::map<UObject*, uint8_t> owner_of;
    for (UObject* h : game::players())
        if (UObject* st = game::stats(h)) owner_of[st] = coop::spot_owner(game::spot(h));
    std::map<UObject*, UObject*> stats_of;   // portrait -> stats
    std::vector<std::pair<void*, void*>> pairs;
    reflect::map_pairs(order, L"PortraitsToCharacters", pairs, nullptr, nullptr);
    for (auto& [k, v] : pairs) stats_of[*static_cast<UObject**>(k)] = *static_cast<UObject**>(v);
    std::vector<UObject*> portraits = ue::get_object_array(order, L"Portraits_List");
    for (auto& [p, st] : stats_of)
        if (p && std::find(portraits.begin(), portraits.end(), p) == portraits.end()) portraits.push_back(p);

    UObject* font = ue::get_object(ue::get_object(screen, L"WBP_HUD_BattleActionTooltip"), L"TitleText");
    const auto names = player_names();
    int shown = 0;
    for (UObject* p : portraits) {
        UObject* box = ue::get_object(p, L"Ally_Box");
        if (!box) continue;
        std::string want, ini;
        uint8_t owner = 0;
        if (auto s = stats_of.find(p); s != stats_of.end())
            if (auto o = owner_of.find(s->second); o != owner_of.end()) {
                owner = o->second;
                auto n = names.find(owner);
                ini = initial(n != names.end() ? n->second : coop::name_of(owner));
                want = std::to_string(owner) + ":" + ini;
            }
        bool keep = false;
        for (UObject* b : ours(box, "E33Coop_Badge")) {
            auto it = g_badge_sig.find(b);
            if (!keep && !want.empty() && it != g_badge_sig.end() && it->second == want) { keep = true; continue; }
            remove_widget(b);
            if (it != g_badge_sig.end()) g_badge_sig.erase(it);
        }
        if (!want.empty() && !keep && count("badge_errors") < 10) {
            UObject* b = diamond(tree_of(p), kBadge, player_col(owner), ini, font, L"Badge");
            UObject* slot = b ? add_child(box, L"AddChildToOverlay", b) : nullptr;
            if (slot) {
                call_int(b, L"SetVisibility", L"InVisibility", kHitTestInvisible);
                align(slot, HRight, VCenter);
                pad(slot, 0, 0, kBadgeOut, 0);
                g_badge_sig[b] = want;
                ++g_counts["badges_built"];
                ++shown;
                log().write("owners_ui_badge", JsonObj().add("portrait", ue::short_name(p)).add("player", static_cast<int>(owner)).add("initial", ini)
                                                   .add("font", font != nullptr));
            } else {
                ++g_counts["badge_errors"];
                log().write("owners_ui_error", JsonObj().add("what", "badge").add("built", b != nullptr));
            }
        }
        if (keep) ++shown;
    }
    g_counts["badges_shown"] = shown;
    g_counts["badges_shown_max"] = std::max(g_counts["badges_shown_max"], shown);
}

// --- Menu pause : panneau « Coop » et touches ----------------------------------------------------

struct Row { std::string hero, who; Rgba color; bool reserve; };
struct Hint { const char* key; const char* label; bool dim; };

void add_vb(UObject* vb, UObject* w, float t, float b)
{
    if (UObject* slot = add_child(vb, L"AddChildToVerticalBox", w)) pad(slot, 0, t, 0, b);
}

void tick_pause()
{
    UObject* menu = ue::get_object(game::controller(), L"PauseMenuWidget");
    UObject* host = ue::get_object(menu, L"PausePanel");
    const std::vector<UObject*> existing = host ? ours(host, "E33Coop_Pause") : std::vector<UObject*>{};
    const bool visible = game::pause_menu_visible();
    if (visible && !host && !g_panel_failed) {
        g_panel_failed = true;
        log().write("owners_ui_error", JsonObj().add("what", "no PausePanel").add("menu", menu != nullptr));
    }
    if (!visible || !host) {
        for (UObject* w : existing) remove_widget(w);
        g_panel_shown = false;
        g_panel_sig.clear();
        return;
    }

    // Contenu
    const bool battle = game::in_battle();
    const auto names = player_names();
    const uint8_t me = coop::my_player();
    std::vector<std::string> party;
    if (battle) for (UObject* h : game::players()) { const std::string n = game::hardcoded_name(h); if (!n.empty()) party.push_back(n); }
    if (party.empty()) party = game::party_names();
    std::vector<Row> rows;
    auto who = [&](uint8_t id) {
        auto n = names.find(id);
        const std::string name = n != names.end() ? n->second : coop::name_of(id);
        return id == me ? ui::text("coop_you", {name}) : name;
    };
    for (auto& h : party) { const uint8_t o = coop::hero_owner(h); rows.push_back({h, who(o), player_col(o), false}); }
    for (auto& h : game::character_names())
        if (std::find(party.begin(), party.end(), h) == party.end()) { const uint8_t o = coop::hero_owner(h); rows.push_back({h, ui::text("coop_reserve", {who(o)}), player_col(o), true}); }
    const std::string host_name = names.count(0) ? names.at(0) : coop::name_of(0);
    const std::string sub = ui::text("coop_session", {host_name, std::to_string(names.size()), ui::text(coop::steam_transport() ? "coop_transport_steam" : "coop_transport_udp")});
    const bool is_host = coop::role() == coop::Role::Host;
    const std::string foot = ui::text(is_host ? "coop_change_host" : "coop_change_client");
    std::vector<Hint> hints;
    if (is_host && coop::steam_transport()) hints.push_back({"F7", "hint_invite", false});
    hints.push_back({"F8", "hint_take", false});
    if (is_host) { hints.push_back({"F5", "hint_pick", battle}); hints.push_back({"F6", "hint_give", battle}); }

    std::string sig = sub + "|" + foot + "|" + (battle ? "b" : "w") + "|" + std::to_string(static_cast<int>(ui::lang()));
    for (auto& r : rows) sig += "|" + r.hero + "=" + r.who + (r.reserve ? "*" : "") + std::to_string(r.color.r + r.color.g * 7 + r.color.b * 13);
    if (!existing.empty() && sig == g_panel_sig) { g_panel_shown = true; return; }
    for (UObject* w : existing) remove_widget(w);
    g_panel_sig.clear();
    g_panel_shown = false;
    if (count("pause_errors") >= 3) return;   // échecs répétés : on n'insiste pas (carte des touches à la place)

    // Construction
    UObject* outer = tree_of(menu);
    UObject* wt = ue::get_object(menu, L"ReturnWrappingText");
    UObject* font = ue::get_object(wt, L"Text");
    UObject* root = sizebox(outer, kPanelWidth, 0, L"PausePanel");
    UObject* bg = border(outer, kPanel, 0, HFill, VFill);
    UObject* vb = make(L"/Script/UMG.VerticalBox", outer, L"List");
    if (!root || !bg || !vb) { ++g_counts["pause_errors"]; g_panel_failed = true; log().write("owners_ui_error", JsonObj().add("what", "panel")); return; }
    pad(bg, 32, 24, 32, 24);
    add_vb(vb, text(outer, ui::text("coop_title"), kGold, 26, font), 0, 0);
    add_vb(vb, text(outer, sub, kDim, 15, font), 0, 12);
    for (auto& r : rows) {
        UObject* hb = make(L"/Script/UMG.HorizontalBox", outer, L"Row");
        UObject* hero = text(outer, r.hero, r.reserve ? kDim : kInk, 20, font);
        UObject* mark = diamond(outer, 10, r.reserve ? kDim : r.color, "", nullptr, L"Mark");
        UObject* name = text(outer, r.who, r.reserve ? kDim : r.color, 17, font);
        if (!hb || !hero || !mark || !name) continue;
        if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", hero)) {
            ue::Call c(s, L"SetSize");   // FSlateChildSize { float Value; uint8 SizeRule } : remplir
            if (auto* d = static_cast<uint8_t*>(c.param_ptr(L"InSize")); d && c.param_size(L"InSize") >= 5) {
                const float one = 1.f;
                std::memcpy(d, &one, 4);
                d[4] = 1;
                c.invoke();
            }
            align(s, HLeft, VCenter);
        }
        if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", mark)) { align(s, HRight, VCenter); pad(s, 0, 0, 10, 0); }
        if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", name)) align(s, HRight, VCenter);
        add_vb(vb, hb, 4, 4);
        UObject* line = sizebox(outer, 0, 1);
        UObject* fill = border(outer, kLine, 0, HFill, VFill);
        if (line && fill) { add_child(line, L"SetContent", fill); add_vb(vb, line, 0, 0); }
    }
    add_vb(vb, text(outer, foot, kDim, 14, font), 12, 0);
    add_child(bg, L"SetContent", vb);
    add_child(root, L"SetContent", bg);
    call_int(root, L"SetVisibility", L"InVisibility", kHitTestInvisible);
    UObject* slot = add_child(host, L"AddChildToOverlay", root);
    if (!slot) { ++g_counts["pause_errors"]; g_panel_failed = true; log().write("owners_ui_error", JsonObj().add("what", "panel slot")); return; }
    align(slot, HRight, VTop);
    pad(slot, 0, 62, 105, 0);

    // Touches, en bas à droite à gauche de « Échap Retour », comme les indications du jeu.
    if (UObject* bar = make(L"/Script/UMG.HorizontalBox", outer, L"PauseHints")) {
        for (auto& h : hints) {
            UObject* item = make(L"/Script/UMG.HorizontalBox", outer, L"Hint");
            UObject* rim = border(outer, kKeyRim, 1, HFill, VFill);
            UObject* key = border(outer, kKeyBg, 0, HCenter, VCenter);
            UObject* kt = text(outer, h.key, kGold, 12, nullptr);
            UObject* lt = text(outer, ui::text(h.label), kInk, 16, font);
            if (!item || !rim || !key || !kt || !lt) continue;
            pad(key, 7, 1, 7, 1);
            add_child(key, L"SetContent", kt);
            add_child(rim, L"SetContent", key);
            if (UObject* s = add_child(item, L"AddChildToHorizontalBox", rim)) align(s, HLeft, VCenter);
            if (UObject* s = add_child(item, L"AddChildToHorizontalBox", lt)) { align(s, HLeft, VCenter); pad(s, 8, 0, 0, 0); }
            if (h.dim) call_floats(item, L"SetRenderOpacity", L"InOpacity", {0.45f});
            if (UObject* s = add_child(bar, L"AddChildToHorizontalBox", item)) pad(s, 26, 0, 0, 0);
        }
        call_int(bar, L"SetVisibility", L"InVisibility", kHitTestInvisible);
        if (UObject* s = add_child(host, L"AddChildToOverlay", bar)) { align(s, HRight, VBottom); pad(s, 0, 0, 300, 56); }
    }

    g_panel_sig = sig;
    g_panel_failed = false;
    g_panel_shown = true;
    ++g_counts["pause_panels_built"];
    log().write("owners_ui_pause_panel", JsonObj().add("rows", static_cast<int64_t>(rows.size())).add("hints", static_cast<int64_t>(hints.size()))
                                             .add("font", font != nullptr).add("sub", sub));
}

} // namespace

void tick(double dt)
{
    g_now += dt;
    if (g_now < g_next) return;
    g_next = g_now + 0.25;   // ProcessEvent : pas à chaque image
    if (!coop::active()) return;
    tick_badges();
    tick_pause();
}

bool pause_panel_shown() { return g_panel_shown; }
bool shows_key_hints() { return coop::active() && !g_panel_failed; }

int count(const std::string& name)
{
    auto it = g_counts.find(name);
    return it == g_counts.end() ? 0 : it->second;
}

} // namespace e33::owners_ui
