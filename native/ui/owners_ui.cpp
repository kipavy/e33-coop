#include "owners_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/reflect.hpp"
#include "../hooks/ue.hpp"
#include "../sync/coop.hpp"
#include "options.hpp"
#include "ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

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
// Navigation (D24) : chaque ligne modifiable (personnage de l'équipe chez l'hôte hors combat, option) est un vrai bouton
// du jeu (WBP_BaseButton_C réglé comme « Paramètres » du menu pause, comme le bouton Rejoindre de l'écran titre) :
// souris (survol, clic), flèches / croix / stick, Entrée / A. Le bouton est en navigation contrainte horizontale
// (USFUIButtonBase, comme les lignes de l'écran Paramètres) : gauche / droite ne déplacent pas le focus, le jeu diffuse
// OnConstrainedNavigationTriggered(Direction), que le mod relie à un TextBlock caché de la ligne (SetJustification,
// paramètre d'un octet) pour le recevoir par un hook. Droite depuis un bouton du menu du jeu entre dans le panneau
// (règle explicite, retirée quand le panneau disparaît) ; Retour (Échap / B) y rend le focus au menu du jeu (porte sur
// l'événement Retour du menu et sur la touche pause du contrôleur, seulement quand une ligne du panneau a le focus).
// Les clics et flèches sont notés dans les hooks et traités au tick suivant (jamais de reconstruction pendant l'appel
// du jeu). F9 / Maj+F9 restent des raccourcis : la ligne choisie au clavier prend le focus. Boutons impossibles
// (classe, modèle introuvable) : panneau en texte comme avant, piloté par F9 / Maj+F9 seulement.

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
           kKeyBg = col(0x1c1b19), kKeyRim = col(0xcbb68c), kLine = col(0xece6da, 0.14f), kHighlight = col(0xe2cba0, 0.16f);
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

// Cellule qui remplit la largeur restante d'une HorizontalBox (FSlateChildSize { float Value; uint8 SizeRule }).
void fill_slot(UObject* s)
{
    ue::Call c(s, L"SetSize");
    if (auto* d = static_cast<uint8_t*>(c.param_ptr(L"InSize")); d && c.param_size(L"InSize") >= 5) {
        const float one = 1.f;
        std::memcpy(d, &one, 4);
        d[4] = 1;
        c.invoke();
    }
}

// Ligne « libellé ……… marque valeur », surlignée si choisie (F9) : fond doré léger et chevron.
void add_row(UObject* outer, UObject* vb, UObject* font, const std::string& label, const std::string& value, const Rgba& label_col,
             const Rgba& value_col, bool diamond_mark, bool selected, float size)
{
    UObject* hb = make(L"/Script/UMG.HorizontalBox", outer, L"Row");
    UObject* hero = text(outer, (selected ? "› " : "") + label, selected ? kGold : label_col, size, font);
    UObject* mark = diamond_mark ? diamond(outer, 10, value_col, "", nullptr, L"Mark") : nullptr;
    UObject* name = text(outer, value, value_col, size - 3, font);
    if (!hb || !hero || !name) return;
    if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", hero)) { fill_slot(s); align(s, HLeft, VCenter); }
    if (mark) if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", mark)) { align(s, HRight, VCenter); pad(s, 0, 0, 10, 0); }
    if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", name)) align(s, HRight, VCenter);
    UObject* row = hb;
    if (selected) {
        if (UObject* hl = border(outer, kHighlight, 0, HFill, VFill, L"Sel")) {
            pad(hl, 8, 2, 8, 2);
            add_child(hl, L"SetContent", hb);
            row = hl;
        }
    }
    add_vb(vb, row, 4, 4);
    UObject* line = sizebox(outer, 0, 1);
    UObject* fill = border(outer, kLine, 0, HFill, VFill);
    if (line && fill) { add_child(line, L"SetContent", fill); add_vb(vb, line, 0, 0); }
}

// --- Panneau navigable : lignes = boutons du jeu ------------------------------------------------

constexpr int kSelfHitTestInvisible = 4, kCollapsed = 1;   // ESlateVisibility
constexpr int64_t kNavLeft = 0, kNavRight = 1, kNavUp = 2, kNavDown = 3;   // EUINavigation
constexpr int64_t kRuleEscape = 0;                         // EUINavigationRule
constexpr float kRowPadX = 12.f, kRowPadY = 3.f;
constexpr float kRowWidth = kPanelWidth - 64.f;            // largeur intérieure du fond (marges 32 + 32)
constexpr const wchar_t* kNavFn = L"SetJustification";     // fonction du TextBlock caché reliée à la navigation contrainte

// Boutons de la liste du menu pause (dump) : modèle de style, règle « droite » vers le panneau, retour du focus.
const wchar_t* const kMenuButtons[] = {L"ResumeButton", L"SettingsButton", L"ExpeditionBasicsButton", L"LoadPreviousSavesButton",
                                       L"TeleportPlayerButton", L"AbandonBattleButton", L"PhotoModeButton",
                                       L"ReturnToTitleScreenButton", L"QuitGameButton"};
constexpr const wchar_t* kBackEvent =
    L"BndEvt__WBP_PauseMenu_WBP_CommonBoundActionButton_Back_K2Node_ComponentBoundEvent_5_CommonButtonBaseClicked__DelegateSignature";

struct NavRow {
    UObject* btn = nullptr;      // WBP_BaseButton_C
    UObject* label = nullptr;    // TextBlock du libellé (surlignage de la ligne choisie)
    UObject* helper = nullptr;   // TextBlock caché relié à OnConstrainedNavigationTriggered
    int index = -1;              // indice dans options::rows()
    std::string text;
    Rgba color{};
    bool shown_sel = false;
};
// Pointeurs gardés d'un passage à l'autre : seulement déréférencés après nav_alive() (notre racine encore dans le
// PausePanel du menu courant) ; les hooks ne font que comparer des pointeurs.
struct Nav {
    UObject* menu = nullptr;
    UObject* root = nullptr;
    std::vector<NavRow> rows;
    int focused = -1;            // ligne qui avait le focus au dernier passage (-1 : aucune)
    int last_sel = -1;           // options::selected() au dernier passage
    UObject* menu_focus = nullptr;   // dernier bouton du menu du jeu qui avait le focus
};
Nav g_nav;
bool g_nav_failed = false;       // boutons impossibles : panneau en texte (F9 / Maj+F9)
bool g_subscribed = false;
struct Pending { UObject* btn; int dir; bool click; };
std::vector<Pending> g_pending;  // clics / gauche-droite notés dans les hooks, traités au tick
double g_back_swallow_until = 0;

UObject* call_obj(UObject* o, const wchar_t* fn)
{
    ue::Call c(o, fn);
    if (!c || !c.invoke()) return nullptr;
    return c.out_object(L"ReturnValue");
}
bool call_bool(UObject* o, const wchar_t* fn)
{
    ue::Call c(o, fn);
    return c && c.invoke() && c.out_bool(L"ReturnValue");
}
// Focus clavier / manette sur le widget ou dans ses descendants (le bouton CommonUI peut le donner à son bouton interne).
bool has_focus(UObject* w) { return w && (call_bool(w, L"HasAnyUserFocus") || call_bool(w, L"HasFocusedDescendants")); }
bool set_focus(UObject* w)
{
    ue::Call c(w, L"SetFocus");
    return c && c.invoke();
}

UObject* create_widget(UObject* cls, UObject* pc)
{
    ue::Call c(ue::find_object(L"/Script/UMG.Default__WidgetBlueprintLibrary"), L"Create");
    if (!c || !cls) return nullptr;
    c.set_object(L"WorldContextObject", pc).set_object(L"WidgetType", cls).set_object(L"OwningPlayer", pc);
    if (!c.invoke()) return nullptr;
    return c.out_object(L"ReturnValue");
}

// Copie de réglages simples (nombres, booléens, pointeurs, structs sans chaîne), comme joinbutton.
void copy_props(UObject* dst, UObject* src, std::initializer_list<const wchar_t*> names)
{
    for (const wchar_t* n : names) {
        FProperty* ps = ue::prop(src, n);
        FProperty* pd = ue::prop(dst, n);
        if (!ps || !pd || ps->GetSize() != pd->GetSize()) continue;
        if (auto* bs = CastField<FBoolProperty>(ps)) {
            if (auto* bd = CastField<FBoolProperty>(pd)) bd->SetPropertyValueInContainer(dst, bs->GetPropertyValueInContainer(src));
            continue;
        }
        std::memcpy(pd->ContainerPtrToValuePtr<void>(dst), ps->ContainerPtrToValuePtr<void>(src), static_cast<size_t>(ps->GetSize()));
    }
}
// TSoftObjectPtr (0x28) copié sans sa chaîne de sous-chemin, seulement si elle est vide (texture), comme joinbutton.
void copy_soft(UObject* dst, UObject* src, const wchar_t* name)
{
    FProperty* ps = ue::prop(src, name);
    FProperty* pd = ue::prop(dst, name);
    if (!ps || !pd || ps->GetSize() != 0x28 || pd->GetSize() != 0x28) return;
    const auto* s = static_cast<const uint8_t*>(ps->ContainerPtrToValuePtr<void>(src));
    int32_t sub_num = 0;
    std::memcpy(&sub_num, s + 24 + 8, sizeof sub_num);
    if (sub_num != 0) return;
    std::memcpy(pd->ContainerPtrToValuePtr<void>(dst), s, 24);
}

FMulticastScriptDelegate* nav_delegate(UObject* btn, FMulticastDelegateProperty** prop_out = nullptr)
{
    auto* mp = CastField<FMulticastDelegateProperty>(ue::prop(btn, L"OnConstrainedNavigationTriggered"));
    if (!mp || mp->GetSize() != static_cast<int32>(sizeof(FMulticastScriptDelegate))) return nullptr;
    if (prop_out) *prop_out = mp;
    return static_cast<FMulticastScriptDelegate*>(mp->ContainerPtrToValuePtr<void>(btn));
}
bool bound_to(FMulticastScriptDelegate* md, UObject* helper)
{
    const FName fn(kNavFn, FNAME_Find);
    for (int32 i = 0; md && i < md->InvocationList.Num(); ++i)
        if (md->InvocationList[i].GetUObject() == helper && md->InvocationList[i].GetFunctionName() == fn) return true;
    return false;
}
// Gauche / droite du bouton (navigation contrainte) -> SetJustification(Direction) du TextBlock caché (ProcessEvent).
bool bind_nav(UObject* btn, UObject* helper)
{
    FMulticastDelegateProperty* mp = nullptr;
    FMulticastScriptDelegate* md = nav_delegate(btn, &mp);
    if (!md || !helper) return false;
    if (bound_to(md, helper)) return true;
    FScriptDelegate d;
    d.BindUFunction(helper, FName(kNavFn, FNAME_Add));
    mp->AddDelegate(d, btn, md);   // le moteur ajoute l'abonné (son allocateur)
    return bound_to(md, helper);
}

std::vector<UObject*> menu_buttons(UObject* menu)
{
    std::vector<UObject*> out;
    for (const wchar_t* n : kMenuButtons) if (UObject* b = ue::get_object(menu, n)) out.push_back(b);
    return out;
}

// Nos boutons sont-ils encore dans le menu pause courant ?
bool nav_alive(UObject* menu)
{
    if (!g_nav.root || !menu || g_nav.menu != menu) return false;
    for (UObject* w : ours(ue::get_object(menu, L"PausePanel"), "E33Coop_Pause")) if (w == g_nav.root) return true;
    return false;
}
NavRow* nav_row(int index)
{
    for (auto& r : g_nav.rows) if (r.index == index) return &r;
    return nullptr;
}
int nav_focused()
{
    for (auto& r : g_nav.rows) if (has_focus(r.btn)) return r.index;
    return -1;
}
UObject* current_menu() { return ue::get_object(game::controller(), L"PauseMenuWidget"); }
// Un bouton du menu du jeu a-t-il le focus (journal : le jeu focalise-t-il « Reprendre » à l'ouverture) ?
bool menu_has_focus_now(UObject* menu)
{
    for (UObject* b : menu_buttons(menu)) if (has_focus(b)) return true;
    return false;
}

void focus_menu(UObject* menu)
{
    UObject* to = nullptr;
    for (UObject* b : menu_buttons(menu)) if (b == g_nav.menu_focus) to = b;
    if (!to) to = ue::get_object(menu, L"ResumeButton");
    set_focus(to);
}

// Règle « droite » des boutons du menu du jeu : vers le panneau (target), ou par défaut (target nul).
void set_menu_rules(UObject* menu, UObject* target)
{
    for (UObject* b : menu_buttons(menu)) {
        if (target) {
            ue::Call c(b, L"SetNavigationRuleExplicit");
            if (c) c.set_int(L"Direction", kNavRight).set_object(L"InWidget", target).invoke();
        } else {
            ue::Call c(b, L"SetNavigationRuleBase");
            if (c) c.set_int(L"Direction", kNavRight).set_int(L"Rule", kRuleEscape).invoke();
        }
    }
}

void nav_forget(UObject* menu)
{
    if (g_nav.root && menu && g_nav.menu == menu) set_menu_rules(menu, nullptr);
    g_nav.root = nullptr;
    g_nav.menu = nullptr;
    g_nav.rows.clear();
    g_nav.focused = -1;
    g_nav.last_sel = options::selected();
    g_pending.clear();
}

// Ligne-bouton « libellé ……… marque valeur » : bouton du jeu, contenu à notre mise en page.
bool add_button_row(UObject* outer, UObject* vb, UObject* tmpl, UObject* pc, UObject* font, int index, const std::string& label,
                    const std::string& value, const Rgba& label_col, const Rgba& value_col, bool diamond_mark, bool selected, float size,
                    std::vector<NavRow>& out)
{
    UObject* box = sizebox(outer, kRowWidth - 2 * kRowPadX - 4, 0, L"RowBox");
    UObject* hb = make(L"/Script/UMG.HorizontalBox", outer, L"Row");
    UObject* lt = text(outer, (selected ? "› " : "") + label, selected ? kGold : label_col, size, font);
    UObject* mark = diamond_mark ? diamond(outer, 10, value_col, "", nullptr, L"Mark") : nullptr;
    UObject* vt = text(outer, value, value_col, size - 3, font);
    UObject* helper = make(L"/Script/UMG.TextBlock", outer, L"Nav");
    if (!box || !hb || !lt || !vt || !helper) return false;
    if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", lt)) { fill_slot(s); align(s, HLeft, VCenter); }
    if (mark) if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", mark)) { align(s, HRight, VCenter); pad(s, 0, 0, 10, 0); }
    if (UObject* s = add_child(hb, L"AddChildToHorizontalBox", vt)) align(s, HRight, VCenter);
    call_int(helper, L"SetVisibility", L"InVisibility", kCollapsed);
    add_child(hb, L"AddChildToHorizontalBox", helper);   // gardé vivant par l'arbre
    add_child(box, L"SetContent", hb);
    call_int(box, L"SetVisibility", L"InVisibility", kHitTestInvisible);   // la souris va au bouton

    UObject* btn = create_widget(tmpl->GetClassPrivate(), pc);
    if (!btn) return false;
    // Style du bouton du menu pause (jamais ses délégués), sans ses dimensions (lignes plus basses).
    copy_props(btn, tmpl, {L"Style", L"bHideInputAction", L"ClickMethod", L"TouchMethod", L"PressMethod", L"HasHoverStain",
                           L"BackgroundMaterialInstance", L"IsSelectedInstant", L"HoverStainTransform", L"HoverStainScalePercent",
                           L"SD_OnNavigated", L"HidePressedAnim", L"HideBackgroundWhenNotFocused", L"SD_OnClicked",
                           L"HoverStainMaterialInstance", L"MaxHoverScale"});
    copy_soft(btn, tmpl, L"HoverStainTexture");
    // Un focus ne doit jamais valoir un clic (sinon parcourir la liste changerait les valeurs).
    ue::set_bool(btn, L"bSelectable", false);
    ue::set_bool(btn, L"bToggleable", false);
    ue::set_bool(btn, L"bTriggerClickedAfterSelection", false);
    ue::set_bool(btn, L"bShouldSelectUponReceivingFocus", false);
    ue::set_bool(btn, L"ShouldCallFocusedHold", false);
    ue::set_int(btn, L"MinWidth", 0);
    ue::set_int(btn, L"MinHeight", 0);
    if (FProperty* cp = ue::prop(btn, L"ContentPadding"); cp && cp->GetSize() == 16) {
        const float m[4] = {kRowPadX, kRowPadY, kRowPadX, kRowPadY};
        std::memcpy(cp->ContainerPtrToValuePtr<void>(btn), m, sizeof m);
    }
    ue::Call setc(ue::get_object(btn, L"ButtonContent"), L"SetContent");
    if (!setc) return false;
    setc.set_object(L"Content", box).invoke();
    { ue::Call c(btn, L"SetIsNavigationConstrained"); if (c) c.set_bool(L"bInIsNavigationConstrained", true).invoke(); }
    { ue::Call c(btn, L"SetConstrainOrientation"); if (c) c.set_int(L"InConstrainOrientation", 0).invoke(); }   // EOrientation : horizontal
    if (!bind_nav(btn, helper)) {   // gauche / droite sans effet, le reste marche
        ++g_counts["nav_bind_errors"];
        log().write("owners_ui_error", JsonObj().add("what", "nav bind").add("row", index));
    }
    add_vb(vb, btn, 1, 1);
    NavRow r;
    r.btn = btn;
    r.label = lt;
    r.helper = helper;
    r.index = index;
    r.text = label;
    r.color = label_col;
    r.shown_sel = selected;
    out.push_back(r);
    return true;
}

// Ligne choisie (F9, focus) : chevron et libellé doré, sans reconstruire le panneau.
void update_highlight()
{
    const int sel = options::selected();
    bool shown = false;
    for (auto& r : g_nav.rows) {
        const bool want = r.index == sel;
        if (want == r.shown_sel) continue;
        set_text(r.label, (want ? "› " : "") + r.text);
        call_color(r.label, L"SetColorAndOpacity", L"InColorAndOpacity", want ? kGold : r.color);
        r.shown_sel = want;
        shown = shown || want;
    }
    if (shown) ++g_counts["pause_panels_selected"];
}

// Suivi du focus (4 fois par seconde) : focus -> ligne choisie (options), ligne choisie au clavier (F9) -> focus.
void tick_nav(UObject* menu)
{
    UObject* menu_btn = nullptr;
    for (UObject* b : menu_buttons(menu)) if (has_focus(b)) { g_nav.menu_focus = menu_btn = b; break; }
    const int focused = nav_focused();
    const int sel = options::selected();
    if (sel != g_nav.last_sel) {   // F9, testeur, oubli après 20 s
        NavRow* r = sel >= 0 ? nav_row(sel) : nullptr;
        if (r && focused != sel) { set_focus(r->btn); ++g_counts["panel_focus_follow"]; }
        else if (sel < 0 && focused >= 0) focus_menu(menu);
        g_nav.last_sel = sel;
        g_nav.focused = r ? sel : -1;
    } else if (focused != g_nav.focused) {   // souris, flèches, manette
        if (focused >= 0) { options::select(focused); ++g_counts["panel_focus_rows"]; }
        else if (g_nav.focused >= 0 && sel == g_nav.focused) options::select(-1);   // focus sorti du panneau
        // Diagnostic : où est parti le focus (bouton du menu, ailleurs dans le menu, hors du menu ; fenêtre au premier plan).
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (focused < 0) ++g_counts["panel_focus_lost"];
        log().write("owners_ui_panel_focus", JsonObj().add("row", focused).add("was", g_nav.focused)
                                                 .add("menu_btn", menu_btn ? ue::short_name(menu_btn) : std::string())
                                                 .add("in_menu", call_bool(menu, L"HasFocusedDescendants"))
                                                 .add("menu_self", call_bool(menu, L"HasAnyUserFocus"))
                                                 .add("fg", pid == GetCurrentProcessId()));
        g_nav.last_sel = options::selected();
        g_nav.focused = focused;
    }
    if (focused >= 0) options::keep_alive();
    update_highlight();
}

// Clics et gauche / droite notés dans les hooks : même chemin que Maj+F9 (options::change).
void process_pending()
{
    if (g_pending.empty()) return;
    const std::vector<Pending> todo = std::move(g_pending);
    g_pending.clear();
    UObject* menu = current_menu();
    if (!nav_alive(menu)) return;
    for (const Pending& p : todo) {
        NavRow* r = nullptr;
        for (auto& x : g_nav.rows) if (x.btn == p.btn) r = &x;
        if (!r) continue;
        const int index = r->index;
        if (!has_focus(r->btn)) set_focus(r->btn);   // clic : la ligne cliquée garde le focus
        options::select(index);
        const bool ok = options::change(p.dir);
        ++g_counts[p.click ? "panel_clicks" : "panel_navs"];
        if (!ok) ++g_counts["panel_refused"];
        log().write("owners_ui_panel_input", JsonObj().add("row", index).add("click", p.click).add("dir", p.dir).add("ok", ok));
        g_nav.focused = index;
        g_nav.last_sel = options::selected();
    }
    g_next = 0;   // valeur nouvelle affichée tout de suite
}

// Retour (Échap / B) quand une ligne du panneau a le focus : focus rendu au menu du jeu, le menu reste ouvert.
bool back_to_menu(const char* via)
{
    if (!game::pause_menu_visible()) return false;
    UObject* menu = current_menu();
    if (g_now < g_back_swallow_until) { ++g_counts["panel_back_dup"]; return true; }   // même appui vu par un autre chemin
    if (!nav_alive(menu) || nav_focused() < 0) return false;
    focus_menu(menu);
    options::select(-1);
    g_nav.focused = -1;
    g_nav.last_sel = -1;
    update_highlight();
    g_back_swallow_until = g_now + 0.3;
    ++g_counts["panel_back"];
    log().write("owners_ui_panel_back", JsonObj().add("via", via));
    return true;
}

// Portes (UFunction::Func remplacée, comme les gardes d'intent.cpp) : appelées par délégué / entrée (ProcessEvent).
struct Gate {
    const wchar_t* path;
    const char* via;
    UnrealScriptFunction orig = nullptr;
    bool installed = false;
};
Gate g_gates[] = {
    {L"/Game/UI/Widgets/InGame_Menu/PauseMenu/WBP_PauseMenu.WBP_PauseMenu_C:"
     L"BndEvt__WBP_PauseMenu_WBP_CommonBoundActionButton_Back_K2Node_ComponentBoundEvent_5_CommonButtonBaseClicked__DelegateSignature", "back_button"},
    {L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:"
     L"InpActEvt_IA_UI_Menu_Pause_K2Node_EnhancedInputActionEvent_7", "pause_key"},
    {L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C:"
     L"InpActEvt_IA_UI_Menu_Pause_K2Node_EnhancedInputActionEvent_0", "pause_key_battle"},
};

template <int I>
void back_gate(UObject* context, FFrame& stack, void* result)
{
    Gate& g = g_gates[I];
    bool swallow = false;
    try { swallow = back_to_menu(g.via); } catch (...) { swallow = false; }
    if (!swallow) g.orig(context, stack, result);
}

template <int... I>
void install_gates_impl(std::integer_sequence<int, I...>)
{
    const UnrealScriptFunction thunks[] = {&back_gate<I>...};
    for (int i = 0; i < static_cast<int>(sizeof...(I)); ++i) {
        Gate& g = g_gates[i];
        if (g.installed) continue;
        UFunction* f = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, g.path);
        if (!f || !f->GetFuncPtr()) continue;
        g.orig = f->GetFuncPtr();
        f->SetFuncPtr(thunks[i]);
        g.installed = true;
        log().write("owners_ui_back_gate", JsonObj().add("via", g.via));
    }
}
void install_gates() { install_gates_impl(std::make_integer_sequence<int, static_cast<int>(std::size(g_gates))>{}); }

void subscribe()
{
    if (g_subscribed) return;
    g_subscribed = true;
    auto& bus = HookBus::get();
    // Clic (souris, Entrée, A) : CommonButtonBase::HandleButtonClicked, délégué du bouton interne (ProcessEvent).
    bus.pre(L"/Script/CommonUI.CommonButtonBase:HandleButtonClicked", [](HookCtx& ctx, UFunction*) {
        for (auto& r : g_nav.rows) if (r.btn == ctx.Context) { g_pending.push_back({r.btn, 1, true}); return; }
    });
    // Gauche / droite : diffusion de OnConstrainedNavigationTriggered vers le TextBlock caché de la ligne.
    bus.pre(L"/Script/UMG.TextLayoutWidget:SetJustification", [](HookCtx& ctx, UFunction* f) {
        for (auto& r : g_nav.rows) {
            if (r.helper != ctx.Context) continue;
            const auto d = ue::read_param_int(ctx, f, L"InJustification");
            if (d && (*d == kNavLeft || *d == kNavRight)) g_pending.push_back({r.btn, *d == kNavLeft ? -1 : 1, false});
            return;
        }
    });
}

void tick_pause()
{
    UObject* menu = current_menu();
    UObject* host = ue::get_object(menu, L"PausePanel");
    const std::vector<UObject*> existing = host ? ours(host, "E33Coop_Pause") : std::vector<UObject*>{};
    const bool visible = game::pause_menu_visible();
    if (visible && !host && !g_panel_failed) {
        g_panel_failed = true;
        log().write("owners_ui_error", JsonObj().add("what", "no PausePanel").add("menu", menu != nullptr));
    }
    if (!visible || !host) {
        for (UObject* w : existing) remove_widget(w);
        if (g_nav.root) nav_forget(menu);
        g_panel_shown = false;
        g_panel_sig.clear();
        return;
    }

    // Contenu
    const bool session = coop::active();
    const bool battle = game::in_battle();
    const auto names = player_names();
    const uint8_t me = coop::my_player();
    std::vector<std::string> party;
    if (session) {
        if (battle) for (UObject* h : game::players()) { const std::string n = game::hardcoded_name(h); if (!n.empty()) party.push_back(n); }
        if (party.empty()) party = game::party_names();
    }
    std::vector<Row> rows;
    auto who = [&](uint8_t id) {
        auto n = names.find(id);
        const std::string name = n != names.end() ? n->second : coop::name_of(id);
        return id == me ? ui::text("coop_you", {name}) : name;
    };
    for (auto& h : party) { const uint8_t o = coop::hero_owner(h); rows.push_back({h, who(o), player_col(o), false}); }
    if (session)
        for (auto& h : game::character_names())
            if (std::find(party.begin(), party.end(), h) == party.end()) { const uint8_t o = coop::hero_owner(h); rows.push_back({h, ui::text("coop_reserve", {who(o)}), player_col(o), true}); }
    // Lignes choisissables (F9, focus) : personnages de l'équipe (hôte, hors combat), puis options.
    const auto sel_rows = options::rows();
    size_t hero_sel = SIZE_MAX, hero_rows = 0;
    std::vector<options::Row> opts;
    for (size_t i = 0; i < sel_rows.size(); ++i) {
        if (sel_rows[i].hero) { ++hero_rows; if (sel_rows[i].selected) hero_sel = i; }
        else opts.push_back(sel_rows[i]);
    }
    const std::string host_name = names.count(0) ? names.at(0) : coop::name_of(0);
    const std::string sub = session ? ui::text("coop_session", {host_name, std::to_string(names.size()), ui::text(coop::steam_transport() ? "coop_transport_steam" : "coop_transport_udp")})
                                    : ui::text("coop_session_none");
    const bool is_host = coop::role() == coop::Role::Host;
    const std::string foot = session ? ui::text(is_host ? "coop_change_host" : "coop_change_client") : std::string();
    std::vector<Hint> hints;
    if ((is_host || !session) && coop::steam_transport()) hints.push_back({"F7", "hint_invite", !session && battle});
    if (session) hints.push_back({"F8", "hint_take", false});
    if (is_host) { hints.push_back({"F5", "hint_pick", battle}); hints.push_back({"F6", "hint_give", battle}); }
    hints.push_back({"F9", "hint_row", false});
    hints.push_back({ui::lang() == ui::Lang::Fr ? "Maj+F9" : "Shift+F9", "hint_change", options::selected() < 0});

    // Avec les boutons, la ligne choisie se montre sans reconstruire (focus, surlignage) : hors de la signature.
    const bool want_buttons = !g_nav_failed;
    std::string sig = sub + "|" + foot + "|" + (battle ? "b" : "w") + "|" + std::to_string(static_cast<int>(ui::lang())) + "|" +
                      (want_buttons ? std::string("nav") : std::to_string(options::selected()));
    for (auto& r : rows) sig += "|" + r.hero + "=" + r.who + (r.reserve ? "*" : "") + std::to_string(r.color.r + r.color.g * 7 + r.color.b * 13);
    for (auto& o : opts) sig += "|" + o.label + "=" + o.value + (o.editable ? "" : "!");
    const bool alive = !g_nav.rows.empty() && nav_alive(menu);
    if (!existing.empty() && sig == g_panel_sig && (g_nav.rows.empty() || alive)) {
        g_panel_shown = true;
        if (alive) tick_nav(menu);
        return;
    }
    // Reconstruction : la ligne qui avait le focus le reprend dans le nouveau panneau.
    const int keep_focus = alive ? nav_focused() : -1;
    for (UObject* w : existing) remove_widget(w);
    if (g_nav.root) nav_forget(menu);
    g_panel_sig.clear();
    g_panel_shown = false;
    if (count("pause_errors") >= 3) return;   // échecs répétés : on n'insiste pas (carte des touches à la place)

    // Construction
    UObject* outer = tree_of(menu);
    UObject* wt = ue::get_object(menu, L"ReturnWrappingText");
    UObject* font = ue::get_object(wt, L"Text");
    UObject* tmpl = ue::get_object(menu, L"SettingsButton");
    UObject* pc = want_buttons ? call_obj(tmpl, L"GetOwningPlayer") : nullptr;
    bool buttons = want_buttons && tmpl && pc;
    if (want_buttons && !buttons) {
        g_nav_failed = true;
        log().write("owners_ui_error", JsonObj().add("what", "nav template").add("tmpl", tmpl != nullptr).add("pc", pc != nullptr));
    }
    UObject* root = sizebox(outer, kPanelWidth, 0, L"PausePanel");
    UObject* bg = border(outer, kPanel, 0, HFill, VFill);
    UObject* vb = make(L"/Script/UMG.VerticalBox", outer, L"List");
    if (!root || !bg || !vb) { ++g_counts["pause_errors"]; g_panel_failed = true; log().write("owners_ui_error", JsonObj().add("what", "panel")); return; }
    pad(bg, 32, 24, 32, 24);
    add_vb(vb, text(outer, ui::text("coop_title"), kGold, 26, font), 0, 0);
    add_vb(vb, text(outer, sub, kDim, 15, font), 0, 12);
    std::vector<NavRow> nav_rows;
    // Personnages de l'équipe en tête, dans l'ordre de game::party_names : l'indice de ligne est celui de options::rows.
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const bool editable = !r.reserve && i < hero_rows && sel_rows[i].label == r.hero;
        if (buttons && editable) {
            if (add_button_row(outer, vb, tmpl, pc, font, static_cast<int>(i), r.hero, r.who, kInk, r.color, true,
                               static_cast<int>(i) == options::selected(), 20, nav_rows)) continue;
            buttons = false;
            break;
        }
        add_row(outer, vb, font, r.hero, r.who, r.reserve ? kDim : kInk, r.reserve ? kDim : r.color, true, !want_buttons && !r.reserve && i == hero_sel, 20);
    }
    if (!foot.empty()) add_vb(vb, text(outer, foot, kDim, 14, font), 12, 0);
    add_vb(vb, text(outer, ui::text("coop_options"), kGold, 18, font), 16, 4);
    for (size_t k = 0; k < opts.size() && (buttons || !want_buttons); ++k) {
        const auto& o = opts[k];
        const int index = static_cast<int>(hero_rows + k);
        if (buttons) {
            if (add_button_row(outer, vb, tmpl, pc, font, index, o.label, o.value, o.editable ? kInk : kDim, o.editable ? kGold : kDim, false,
                               index == options::selected(), 16, nav_rows)) continue;
            buttons = false;
            break;
        }
        add_row(outer, vb, font, o.label, o.value, o.editable ? kInk : kDim, o.editable ? kGold : kDim, false, o.selected, 16);
    }
    if (want_buttons && !buttons) {   // un bouton n'a pas pu être fait : panneau en texte au prochain passage
        g_nav_failed = true;
        ++g_counts["nav_build_errors"];
        log().write("owners_ui_error", JsonObj().add("what", "nav buttons").add("built", static_cast<int64_t>(nav_rows.size())));
        return;
    }
    add_vb(vb, text(outer, std::string("E33 Coop ") + E33COOP_VERSION, kDim, 13, font), 14, 0);   // version installée
    add_child(bg, L"SetContent", vb);
    add_child(root, L"SetContent", bg);
    // Boutons : la souris doit atteindre les lignes (racine et fond « non testés eux-mêmes »).
    call_int(root, L"SetVisibility", L"InVisibility", buttons ? kSelfHitTestInvisible : kHitTestInvisible);
    if (buttons) call_int(bg, L"SetVisibility", L"InVisibility", kSelfHitTestInvisible);
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

    // Navigation : haut / bas bouclent dans le panneau, droite depuis le menu du jeu y entre.
    if (buttons && !nav_rows.empty()) {
        g_nav.menu = menu;
        g_nav.root = root;
        g_nav.rows = std::move(nav_rows);
        g_nav.last_sel = options::selected();
        UObject* first = g_nav.rows.front().btn;
        UObject* last = g_nav.rows.back().btn;
        { ue::Call c(first, L"SetNavigationRuleExplicit"); if (c) c.set_int(L"Direction", kNavUp).set_object(L"InWidget", last).invoke(); }
        { ue::Call c(last, L"SetNavigationRuleExplicit"); if (c) c.set_int(L"Direction", kNavDown).set_object(L"InWidget", first).invoke(); }
        NavRow* entry = keep_focus >= 0 ? nav_row(keep_focus) : nullptr;
        set_menu_rules(menu, entry ? entry->btn : first);
        install_gates();
        if (entry) { set_focus(entry->btn); ++g_counts["panel_focus_restored"]; }
        g_nav.focused = entry ? keep_focus : -1;
        ++g_counts["panel_buttons_built"];
    }

    g_panel_sig = sig;
    g_panel_failed = false;
    g_panel_shown = true;
    ++g_counts["pause_panels_built"];
    if (options::selected() >= 0) ++g_counts["pause_panels_selected"];
    log().write("owners_ui_pause_panel", JsonObj().add("rows", static_cast<int64_t>(rows.size())).add("hints", static_cast<int64_t>(hints.size()))
                                             .add("font", font != nullptr).add("sub", sub).add("selected", options::selected())
                                             .add("options", static_cast<int64_t>(opts.size())).add("buttons", static_cast<int64_t>(g_nav.rows.size()))
                                             .add("focus", keep_focus).add("menu_focus", menu_has_focus_now(menu)));
}

} // namespace

void tick(double dt)
{
    g_now += dt;
    process_pending();   // clics et flèches : à l'image suivante, pas au rythme du panneau
    if (g_now < g_next) return;
    g_next = g_now + 0.25;   // ProcessEvent : pas à chaque image
    if (!coop::ready()) return;
    subscribe();
    if (coop::active()) tick_badges();
    tick_pause();
}

bool pause_panel_shown() { return g_panel_shown; }
bool shows_key_hints() { return coop::ready() && !g_panel_failed; }

int panel_buttons() { return nav_alive(current_menu()) ? static_cast<int>(g_nav.rows.size()) : 0; }
int focused_row() { return nav_alive(current_menu()) ? nav_focused() : -1; }
bool menu_has_focus()
{
    for (UObject* b : menu_buttons(current_menu())) if (has_focus(b)) return true;
    return false;
}

bool test_focus(int row)
{
    UObject* menu = current_menu();
    if (!nav_alive(menu)) return false;
    if (row < 0) { focus_menu(menu); return menu_has_focus(); }
    NavRow* r = nav_row(row);
    return r && set_focus(r->btn) && has_focus(r->btn);
}

bool test_click(int row)
{
    if (!nav_alive(current_menu())) return false;
    NavRow* r = nav_row(row);
    if (!r) return false;
    ue::Call c(r->btn, L"HandleButtonClicked");   // comme le bouton interne (souris, Entrée, A)
    return c && c.invoke();
}

bool test_nav(int row, int dir)
{
    if (!nav_alive(current_menu())) return false;
    NavRow* r = nav_row(row);
    FMulticastScriptDelegate* md = r ? nav_delegate(r->btn) : nullptr;
    if (!md || !bound_to(md, r->helper)) return false;
    // Comme le moteur à la navigation contrainte : chaque abonné reçoit Direction (un octet) par ProcessEvent.
    std::vector<std::pair<UObject*, FName>> subs;
    for (int32 i = 0; i < md->InvocationList.Num(); ++i) subs.emplace_back(md->InvocationList[i].GetUObject(), md->InvocationList[i].GetFunctionName());
    for (auto& [o, n] : subs) {
        if (!o) continue;
        UFunction* f = o->GetFunctionByNameInChain(n);
        if (!f) continue;
        alignas(16) uint8_t parms[64] = {};
        parms[0] = static_cast<uint8_t>(dir < 0 ? kNavLeft : kNavRight);
        o->ProcessEvent(f, parms);
    }
    return true;
}

bool test_back()
{
    UObject* menu = current_menu();
    if (!g_gates[0].installed || !nav_alive(menu) || nav_focused() < 0) return false;   // sinon l'appel fermerait le menu
    ue::Call c(menu, kBackEvent);
    if (!c) return false;
    c.set_object(L"Button", ue::get_object(menu, L"WBP_CommonBoundActionButton_Back"));
    return c.invoke();
}

int count(const std::string& name)
{
    auto it = g_counts.find(name);
    return it == g_counts.end() ? 0 : it->second;
}

} // namespace e33::owners_ui
