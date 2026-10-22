#include "intent.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FStrProperty.hpp>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"
#include "state.hpp"

using namespace RC::Unreal;

namespace e33::intent {

namespace {

constexpr double kPollS = 0.1;              // relevé de l'intention locale (10 fois/s au plus)
constexpr double kAimViewS = 0.05;          // caméra de visée envoyée ~20 fois/s
constexpr const char* kSlot = "turn_intent";   // après « Tour de X » dans la carte d'état (lignes triées par nom)

Net g_net;
bool g_aim_cam = true;                      // -e33aimcam=0 : visée rejouée avec la caméra du jeu (comportement d'avant)

void bump(const char* c, int n = 1) { if (g_net.bump) g_net.bump(c, n); }
BattleRef battle() { return g_net.battle ? g_net.battle() : BattleRef{}; }

// --- Textes du jeu --------------------------------------------------------------------------

// FText -> UTF-8 (KismetTextLibrary::Conv_TextToString). La copie brute du FText n'est jamais détruite (pas de
// compteur touché) ; la chaîne rendue n'est pas libérée : appels rares (noms mis en cache).
std::string ftext_utf8(const void* ftext)
{
    if (!ftext) return {};
    UObject* ktl = ue::find_object(STR("/Script/Engine.Default__KismetTextLibrary"));
    ue::Call c(ktl, STR("Conv_TextToString"));
    if (!c) return {};
    c.set_raw(STR("InText"), ftext);
    if (!c.invoke()) return {};
    const auto* s = static_cast<const FString*>(c.param_ptr(STR("ReturnValue")));
    const wchar_t* w = s ? **s : nullptr;
    return w ? ue::to_utf8(w) : std::string();
}

std::map<std::string, std::string> g_skill_names, g_item_names;
std::map<std::tuple<uint32_t, int, int>, std::string> g_unit_names;   // (combat, camp, emplacement) -> nom affiché

std::string skill_name(UObject* hero, const std::string& id)
{
    auto it = g_skill_names.find(id);
    if (it != g_skill_names.end()) return it->second;
    std::string name;
    ue::Call c(game::stats(hero), STR("FindEquippedSkillByID"));
    if (c) {
        const std::wstring w = ue::from_utf8(id);
        c.set_fname(STR("SkillID"), w.c_str()).invoke();
        if (UObject* asset = c.out_object(STR("Skill"))) name = ftext_utf8(ue::value_ptr<void>(asset, STR("Name")));
    }
    if (name.empty()) name = id;   // attaque gradient ou compétence absente de la liste équipée : identifiant brut
    g_skill_names[id] = name;
    return name;
}

// Nom d'un objet : GI.Inventory[i].ItemStaticData.Item_DisplayName de la ligne dont Item_HardcodedName vaut id.
std::string item_name(const std::string& id)
{
    auto it = g_item_names.find(id);
    if (it != g_item_names.end()) return it->second;
    std::string name;
    UObject* g = game::gi();
    if (auto* ap = CastField<FArrayProperty>(ue::prop(g, STR("Inventory")))) {
        auto* inner = CastField<FStructProperty>(ap->GetInner());
        auto* arr = ap->ContainerPtrToValuePtr<FScriptArray>(g);
        auto* stat = inner ? CastField<FStructProperty>(ue::struct_member(inner->GetStruct(), STR("ItemStaticData"))) : nullptr;
        FProperty* idp = stat ? ue::struct_member(stat->GetStruct(), STR("Item_HardcodedName")) : nullptr;
        FProperty* disp = stat ? ue::struct_member(stat->GetStruct(), STR("Item_DisplayName")) : nullptr;
        auto* data = arr ? static_cast<uint8_t*>(arr->GetData()) : nullptr;
        for (int32 i = 0; data && idp && disp && i < arr->Num(); ++i) {
            void* st = stat->ContainerPtrToValuePtr<void>(data + static_cast<size_t>(i) * inner->GetSize());
            if (ue::to_utf8(static_cast<FName*>(idp->ContainerPtrToValuePtr<void>(st))->ToString()) != id) continue;
            name = ftext_utf8(disp->ContainerPtrToValuePtr<void>(st));
            break;
        }
    }
    if (name.empty()) name = id;
    g_item_names[id] = name;
    return name;
}

std::string unit_name(uint32_t battle_id, const proto::Unit& u)
{
    const auto key = std::make_tuple(battle_id, static_cast<int>(u.kind), static_cast<int>(u.spot));
    auto it = g_unit_names.find(key);
    if (it != g_unit_names.end()) return it->second;
    UObject* o = game::find_unit(u.kind == proto::ActorKind::Hero, u.spot);
    if (!o) return "?";
    std::string name = ftext_utf8(ue::value_ptr<void>(game::stats(o), STR("CharacterDisplayName")));
    if (name.empty()) name = game::hardcoded_name(o);
    if (name.empty()) name = game::char_class(o);
    g_unit_names[key] = name;
    return name;
}

// --- Intention d'un héros d'ici (propriétaire) ----------------------------------------------

struct Local {
    uint32_t battle = 0, turn = 0;
    int spot = -1;
    uint8_t action = 0;                     // choisi pendant ce tour (jamais la valeur restée du tour d'avant)
    std::string secondary;
    bool done = false;                      // action lancée : plus d'aperçu
    bool sent = false;
    uint16_t seq = 0;
    proto::IntentPreview last;
    double next_poll = 0;
};
Local g_loc;

// Tour d'un héros d'ici en cours : état local (remis à zéro à chaque nouveau tour), nul sinon.
Local* local_turn()
{
    const BattleRef b = battle();
    if (!b.active) return nullptr;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur)) return nullptr;
    const int spot = game::spot(cur);
    if (!g_net.controls || !g_net.controls(spot)) return nullptr;
    if (g_loc.battle != b.id || g_loc.turn != b.turn || g_loc.spot != spot) {
        g_loc = Local{};
        g_loc.battle = b.id;
        g_loc.turn = b.turn;
        g_loc.spot = spot;
    }
    if (g_loc.done || (g_net.action_known && g_net.action_known(b.turn, spot))) return nullptr;
    return &g_loc;
}

bool same(const proto::IntentPreview& a, const proto::IntentPreview& b)
{
    if (a.action_type != b.action_type || a.aiming != b.aiming || a.secondary != b.secondary || a.targets.size() != b.targets.size()) return false;
    for (size_t i = 0; i < a.targets.size(); ++i)
        if (a.targets[i].kind != b.targets[i].kind || a.targets[i].spot != b.targets[i].spot) return false;
    return true;
}

void poll_local(double now)
{
    Local* l = local_turn();
    if (!l || now < l->next_poll) return;
    l->next_poll = now + kPollS;
    proto::IntentPreview p;
    p.battle_id = l->battle;
    p.turn = l->turn;
    p.spot = static_cast<uint8_t>(l->spot);
    p.action_type = l->action;
    p.secondary = l->secondary;
    p.aiming = g_net.local_aiming && g_net.local_aiming();
    // Cibles : celles du gestionnaire de ciblage tant qu'il cible (curseur du jeu, flèches gauche/droite ou souris).
    UObject* tm = ue::get_object(game::controller(), STR("AC_jRPG_TargetingManager"));
    if (tm && ue::get_bool(tm, STR("TargetingEnabled?")))
        for (UObject* t : ue::get_object_array(tm, STR("TargetedCharacters")))
            if (t && p.targets.size() < 16) p.targets.push_back(sync::unit_of(t));
    const bool empty = p.action_type == 0 && !p.aiming && p.targets.empty();
    if (same(p, l->last) && (l->sent || empty)) return;
    if (!l->sent && empty) return;   // début de tour : rien à montrer
    p.seq = ++l->seq;
    l->last = p;
    l->sent = true;
    if (g_net.publish) g_net.publish(p, true);
    bump("intent_previews_sent");
    log().write("intent_local", JsonObj().raw("msg", proto::to_json(p)), l->turn);
}

// --- Menus de combat : lus chez le propriétaire, rejoués chez les autres ---------------------

using proto::MenuPage;

constexpr double kMenuPollS = 1.0 / 15;    // état des menus d'ici relevé 15 fois/s au plus
constexpr double kMirrorStepS = 0.15;      // miroir : un appel au jeu par pas, le temps que l'interface suive
constexpr double kModWindowS = 0.8;        // changement des menus attribué au mod s'il suit un de ses appels de moins de 0,8 s
constexpr double kTurnGraceS = 1.2;        // début de tour : le jeu ouvre lui-même la roue

bool g_mirror = false;                     // -e33menumirror=1 : miroir des menus ; sinon ligne de texte seulement
bool g_guard = true;                       // -e33inputguard=0 : pas de coupure de la navigation ni des clics sur la roue

// Objets de l'interface de combat, relus à chaque usage (le HUD est recréé à chaque combat).
struct Hud {
    UObject* bm = nullptr;
    UObject* wheels = nullptr;             // WBP_HUD_WheelsContainer_C (roue 3D, pages)
    UObject* actions = nullptr, *skills = nullptr, *items = nullptr, *gradient = nullptr;
    UObject* tm = nullptr;                 // AC_jRPG_TargetingManager_C du contrôleur
};

Hud hud()
{
    Hud h;
    h.bm = game::battle_manager();
    if (!h.bm) return h;
    h.wheels = ue::get_object(ue::get_object(h.bm, STR("BattleScreenWidget")), STR("WBP_HUD_WheelsContainer"));
    if (!h.wheels) h.wheels = ue::get_object(ue::get_object(h.bm, STR("LGUI_Actor_BattleWheels")), STR("WheelsContainer"));
    h.actions = ue::get_object(h.wheels, STR("WBP_BattleWheel_Action"));
    h.skills = ue::get_object(h.wheels, STR("WBP_HUD_SkillWheel_Skills"));
    h.items = ue::get_object(h.wheels, STR("WBP_HUD_SkillWheel_Items"));
    h.gradient = ue::get_object(h.wheels, STR("WBP_HUD_SkillWheel_GradientAttacks"));
    h.tm = ue::get_object(game::controller(), STR("AC_jRPG_TargetingManager"));
    return h;
}

// Page active de la roue : IActiveWheel (TScriptInterface : objet puis pointeur d'interface).
MenuPage active_page(const Hud& h)
{
    auto** p = ue::value_ptr<UObject*>(h.wheels, STR("IActiveWheel"));
    UObject* o = p ? *p : nullptr;
    if (!o) return MenuPage::None;
    if (o == h.actions) return MenuPage::Actions;
    if (o == h.skills) return MenuPage::Skills;
    if (o == h.items) return MenuPage::Items;
    if (o == h.gradient) return MenuPage::Gradient;
    return MenuPage::None;
}

UObject* page_object(const Hud& h, MenuPage p)
{
    switch (p) {
    case MenuPage::Actions: return h.actions;
    case MenuPage::Skills: return h.skills;
    case MenuPage::Items: return h.items;
    case MenuPage::Gradient: return h.gradient;
    default: return nullptr;
    }
}

// Pages à liste (compétences, objets, attaques gradient : WBP_HUD_SkillWheel_C, deux colonnes de 3 boutons).
UObject* list_page(const Hud& h, MenuPage p) { return p == MenuPage::Actions ? nullptr : page_object(h, p); }
int column_of(UObject* wheel) { return ue::get_bool(wheel, STR("IsLeftColumnActive"), true) ? 0 : 1; }

std::vector<UObject*> column_buttons(UObject* wheel, int column)
{
    UObject* list = ue::get_object(wheel, column == 0 ? STR("WBP_HUD_SkillList-Left") : STR("WBP_HUD_SkillList-Right"));
    std::vector<UObject*> out;
    for (const wchar_t* n : {STR("WBP_HUD_SkillButton_Big_1"), STR("WBP_HUD_SkillButton_Big_2"), STR("WBP_HUD_SkillButton_Big_3")})
        out.push_back(ue::get_object(list, n));
    return out;
}

// Bouton survolé à la souris dans la colonne (UButton::IsHovered), -1 sinon.
int hovered(UObject* wheel, int column)
{
    const auto buttons = column_buttons(wheel, column);
    for (size_t i = 0; i < buttons.size(); ++i) {
        ue::Call c(ue::get_object(buttons[i], STR("Button")), STR("IsHovered"));
        if (c && c.invoke() && c.out_bool(STR("ReturnValue"))) return static_cast<int>(i);
    }
    return -1;
}

bool targeting_on(const Hud& h) { return ue::get_bool(h.tm, STR("TargetingEnabled?")); }
// Pré-ciblage : sur la roue des actions, le jeu garde le curseur sur un ennemi dès le début du tour (TargetingEnabled?
// vrai aussi). Ce n'est pas le ciblage d'une action choisie (vu le 2026-10-22, p7_menumirror_host : le miroir
// l'« annulait » en boucle et ne passait jamais aux pages ni aux colonnes).
bool pretargeting(const Hud& h) { return ue::get_bool(h.tm, STR("IsPretargeting")); }
std::vector<UObject*> targeted(const Hud& h) { return targeting_on(h) ? ue::get_object_array(h.tm, STR("TargetedCharacters")) : std::vector<UObject*>{}; }

bool same_set(std::vector<UObject*> a, std::vector<UObject*> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

bool same_menu(const proto::MenuState& a, const proto::MenuState& b)
{
    if (a.page != b.page || a.column != b.column || a.hover != b.hover || a.action_type != b.action_type || a.secondary != b.secondary ||
        a.targeting != b.targeting || a.targeting_type != b.targeting_type || a.targets.size() != b.targets.size()) return false;
    for (size_t i = 0; i < a.targets.size(); ++i)
        if (a.targets[i].kind != b.targets[i].kind || a.targets[i].spot != b.targets[i].spot) return false;
    return true;
}

// Propriétaire : état des menus pendant le tour d'un héros d'ici, envoyé à chaque changement.
struct LocalMenu { uint32_t battle = 0, turn = 0; int spot = -1; uint16_t seq = 0; proto::MenuState last; bool sent = false; double next = 0; };
LocalMenu g_lmenu;

void poll_menu(double now)
{
    Local* l = local_turn();
    if (!l || now < g_lmenu.next) return;
    if (g_lmenu.battle != l->battle || g_lmenu.turn != l->turn || g_lmenu.spot != l->spot) {
        g_lmenu = LocalMenu{};
        g_lmenu.battle = l->battle;
        g_lmenu.turn = l->turn;
        g_lmenu.spot = l->spot;
    }
    g_lmenu.next = now + kMenuPollS;
    const Hud h = hud();
    if (!h.wheels) return;
    proto::MenuState m;
    m.battle_id = l->battle;
    m.turn = l->turn;
    m.spot = static_cast<uint8_t>(l->spot);
    m.page = g_net.local_aiming && g_net.local_aiming() ? MenuPage::FreeAim : active_page(h);
    if (UObject* w = list_page(h, m.page)) {
        m.column = static_cast<uint8_t>(column_of(w));
        m.hover = static_cast<int8_t>(hovered(w, m.column));
    }
    m.action_type = l->action;
    m.secondary = l->secondary;
    m.targeting = targeting_on(h) && !pretargeting(h);   // ciblage d'une action choisie seulement (cibles : pré-ciblage compris)
    m.targeting_type = static_cast<uint8_t>(ue::get_int(h.tm, STR("SelectedTargetingType"), 0));
    for (UObject* t : targeted(h))
        if (t && m.targets.size() < 16) m.targets.push_back(sync::unit_of(t));
    if (g_lmenu.sent && same_menu(m, g_lmenu.last)) return;
    m.seq = ++g_lmenu.seq;
    g_lmenu.last = m;
    g_lmenu.sent = true;
    if (g_net.publish) g_net.publish(m, true);
    bump("menu_states_sent");
    log().write("menu_local", JsonObj().raw("msg", proto::to_json(m)), l->turn);
}

// Observateur : menus du jeu rejoués pendant le tour du héros d'un autre joueur.
std::map<std::tuple<uint32_t, uint32_t, int>, proto::MenuState> g_menus;   // (combat, tour, emplacement) -> dernier état reçu

struct Mirror {
    uint32_t battle = 0, turn = 0;
    int spot = -1;
    double t0 = 0;                          // début du tour vu ici
    bool have = false;
    proto::MenuState want;
    std::string selected;                   // "action|compétence" choisie par le miroir (ciblage ouvert par lui)
    double selected_at = -10;
    std::map<std::string, int> tries;       // essais par étape (le jeu peut refuser : pas de boucle sans fin)
    bool gave_up = false;                   // une partie n'a pas pu être rejouée : ligne de texte pour ce tour
    int cancel_game = 0;                    // retours en arrière sur un ciblage que le miroir n'a pas ouvert (ce tour)
    bool acted = false;                     // le miroir a changé les menus depuis la dernière remise à zéro
    bool released = false;
    double release_t0 = -1, released_at = -1;
    double next_step = 0, last_call = -10;
    UObject* hover_btn = nullptr;
    MenuPage base = MenuPage::None;         // page de la roue avant le premier pas du miroir (rendue avant l'exécution)
    bool base_set = false;
    // Suivi des changements de menus (miroir ou non) : ceux qui ne viennent pas du mod sont des « dérives ».
    bool seen = false;
    MenuPage page = MenuPage::None;
    bool targeting = false;
    std::vector<UObject*> targets;
};
Mirror g_mir;

// Ciblage d'une action choisie (attaque, compétence, objet) : pas le pré-ciblage de la roue des actions, sauf si le
// miroir vient lui-même d'ouvrir un ciblage (au cas où le jeu laisserait IsPretargeting levé).
bool selection_targeting(const Hud& h) { return targeting_on(h) && (!pretargeting(h) || !g_mir.selected.empty()); }

// Garde des entrées (plus bas) : entrées du ciblage du jeu retirées tout de suite (après un choix rejoué par le miroir).
bool block_targeting_input(double now, bool force);
bool can_block_targeting();

bool attempt(const std::string& step, int max = 3)
{
    int& n = g_mir.tries[step];
    if (n >= max) {
        if (n == max) {
            ++n;
            g_mir.gave_up = true;
            bump("menu_mirror_gave_up");
            log().write("menu_mirror_gave_up", JsonObj().add("step", step), g_mir.turn);
        }
        return false;
    }
    ++n;
    return true;
}

void mark_call(double now, const char* what, const char* fn, bool ok)
{
    g_mir.last_call = now;
    g_mir.acted = true;
    bump("menu_mirror_calls");
    log().write("menu_mirror_call", JsonObj().add("what", what).add("fn", fn).add("ok", ok), g_mir.turn);
}

// Appel sans paramètre fait par le mod (étiqueté : ce n'est pas une entrée du joueur d'ici).
bool mod_call(double now, UObject* o, const wchar_t* fn, const char* what)
{
    ue::Call c(o, fn);
    if (!c) { log().write("menu_mirror_missing", JsonObj().add("what", what).add("fn", ue::to_utf8(fn)).add("object", ue::short_name(o)), g_mir.turn); return false; }
    bool ok = false;
    { game::ModCall mod; ok = c.invoke(); }
    mark_call(now, what, ue::to_utf8(fn).c_str(), ok);
    return ok;
}

bool mod_call_object(double now, UObject* o, const wchar_t* fn, const wchar_t* param, UObject* v, const char* what)
{
    ue::Call c(o, fn);
    if (!c) { log().write("menu_mirror_missing", JsonObj().add("what", what).add("fn", ue::to_utf8(fn)), g_mir.turn); return false; }
    c.set_object(param, v);
    bool ok = false;
    { game::ModCall mod; ok = c.invoke(); }
    mark_call(now, what, ue::to_utf8(fn).c_str(), ok);
    return ok;
}

bool mod_call_bool(double now, UObject* o, const wchar_t* fn, const wchar_t* param, bool v, const char* what)
{
    ue::Call c(o, fn);
    if (!c) return false;
    c.set_bool(param, v);
    bool ok = false;
    { game::ModCall mod; ok = c.invoke(); }
    mark_call(now, what, ue::to_utf8(fn).c_str(), ok);
    return ok;
}

// ChangeWheelPage(TScriptInterface<WBPI_WheelPage>) : interface d'un BP, pointeur d'interface nul (comme le fait le BP).
bool change_page(double now, const Hud& h, UObject* page)
{
    ue::Call c(h.wheels, STR("ChangeWheelPage"));
    if (!c || !page || c.param_size(STR("IWheelPage")) != 2 * sizeof(void*)) return false;
    struct { UObject* object; void* iface; } v{page, nullptr};
    c.set_raw(STR("IWheelPage"), &v);
    bool ok = false;
    { game::ModCall mod; ok = c.invoke(); }
    mark_call(now, "change_page", "ChangeWheelPage", ok);
    return ok;
}

void clear_hover(double now)
{
    if (!g_mir.hover_btn) return;
    mod_call_bool(now, g_mir.hover_btn, STR("SetPromptHighlighted"), STR("Highlighted"), false, "hover_off");
    g_mir.hover_btn = nullptr;
}

// Page de la roue : chemins des boutons de la roue (contenu de la liste chargé avant), puis ChangeWheelPage seul.
bool open_page(double now, const Hud& h, MenuPage p)
{
    const std::string step = "page" + std::to_string(static_cast<int>(p));
    if (!attempt(step, 4)) return false;
    clear_hover(now);
    if (g_mir.tries[step] <= 2) {
        switch (p) {
        case MenuPage::Actions: mod_call(now, h.wheels, STR("BackToActionPage"), "page_actions"); break;
        case MenuPage::Skills:
            mod_call(now, h.wheels, STR("UpdateCharacterSkillWheels"), "skills_data");
            mod_call(now, h.wheels, STR("OnSkillsActionChosen"), "page_skills");
            break;
        case MenuPage::Items:
            mod_call(now, h.wheels, STR("UpdateItemsWheel"), "items_data");
            mod_call(now, h.wheels, STR("OnItemsActionChosen"), "page_items");
            break;
        case MenuPage::Gradient: mod_call(now, h.wheels, STR("OnUltimateActionChosen"), "page_gradient"); break;
        default: return false;
        }
    } else {
        change_page(now, h, page_object(h, p));
    }
    bump("menu_mirror_pages");
    g_mir.next_step = now + 0.45;   // animation d'ouverture de la page
    return true;
}

// Cibles voulues : groupe de ciblage du jeu qui les contient (TryFocusTargetGroup, puis les autres chemins).
bool focus_targets(double now, const Hud& h, const std::vector<UObject*>& want)
{
    std::string ids;
    for (UObject* o : want) ids += ue::short_name(o) + ",";
    const std::string step = "targets:" + ids;
    if (!attempt(step)) return false;
    UObject* group = nullptr;
    for (UObject* g : ue::get_object_array(h.tm, STR("TargetGroups")))
        if (g && same_set(ue::get_object_array(g, STR("Characters")), want)) { group = g; break; }
    if (!group)
        for (UObject* g : ue::get_object_array(h.tm, STR("TargetGroups"))) {
            const auto chars = ue::get_object_array(g, STR("Characters"));
            if (g && std::find(chars.begin(), chars.end(), want.front()) != chars.end()) { group = g; break; }
        }
    if (!group) {
        log().write("menu_mirror_no_group", JsonObj().add("targets", ids), g_mir.turn);
        return true;
    }
    const int n = g_mir.tries[step];
    if (n == 1) mod_call_object(now, h.tm, STR("TryFocusTargetGroup"), STR("TargetGroup"), group, "target_focus");
    else if (n == 2) mod_call(now, group, STR("FocusTargetGroup"), "target_group_focus");
    else mod_call_object(now, h.tm, STR("SetFocusedTargetingGroup"), STR("TargetingGroup"), group, "target_set_focused");
    bump("menu_mirror_targets");
    return true;
}

// Un pas du miroir : au plus un changement de l'interface. true si le jeu a été appelé.
bool mirror_step(double now, const Hud& h)
{
    if (!h.bm || !h.wheels || !h.tm) {
        if (attempt("hud", 1)) log().write("menu_mirror_no_hud", JsonObj().add("wheels", h.wheels != nullptr).add("tm", h.tm != nullptr), g_mir.turn);
        return false;
    }
    const auto& w = g_mir.want;
    const MenuPage page = active_page(h);
    // Ciblage voulu : attaque, ou compétence / objet choisi. Visée libre : rien dans les menus (le rejeu de la visée suit).
    const bool want_tgt = w.targeting && w.page != MenuPage::FreeAim &&
                          (w.action_type == game::ActAttack || ((w.action_type == game::ActSkill || w.action_type == game::ActItem) && !w.secondary.empty()));
    const std::string key = std::to_string(w.action_type) + "|" + w.secondary;
    if (!targeting_on(h) && now - g_mir.selected_at > 0.6) g_mir.selected.clear();   // ciblage refermé (par le jeu ou par nous)
    // Ciblage d'une action choisie ici (le pré-ciblage de la roue des actions ne compte pas : il n'empêche ni pages ni colonnes).
    bool tgt = selection_targeting(h);
    // Ciblage que le miroir n'a pas ouvert et qu'un retour en arrière ne referme pas (pré-ciblage mal reconnu) : celui du
    // jeu, laissé tel quel ; il ne bloque plus les pages ni les colonnes.
    if (tgt && g_mir.selected.empty() && g_mir.cancel_game >= 2) {
        if (g_mir.cancel_game++ == 2)
            log().write("menu_mirror_game_targeting", JsonObj().add("pretargeting", pretargeting(h)).add("page", static_cast<int>(page)), g_mir.turn);
        tgt = false;
    }
    // 1. Retour en arrière : ciblage plus voulu, ou ouvert pour une autre action / compétence.
    if (tgt && (!want_tgt || g_mir.selected != key)) {
        if (g_mir.selected.empty()) ++g_mir.cancel_game;
        else if (!attempt("cancel:" + g_mir.selected)) return false;
        mod_call(now, h.bm, STR("OnCancelTargeting"), "cancel_targeting");
        g_mir.selected.clear();
        g_mir.next_step = now + 0.4;   // animation de retour (pas de second retour en arrière avant)
        bump("menu_mirror_cancels");
        return true;
    }
    // 2. Page de la roue (rien ou visée libre : la page des actions, comme au début du tour).
    MenuPage want_page = w.page;
    if (want_page == MenuPage::None || want_page == MenuPage::FreeAim) want_page = g_mir.base != MenuPage::None ? g_mir.base : MenuPage::Actions;
    if (!tgt && page != want_page) return open_page(now, h, want_page);
    // 3. Colonne de la liste.
    UObject* lw = tgt ? nullptr : list_page(h, page);
    if (lw && column_of(lw) != w.column) {
        if (!attempt("column" + std::to_string(w.column))) return false;
        clear_hover(now);
        mod_call(now, lw, w.column == 0 ? STR("ResetColumnToLeft") : STR("ResetColumnToRight"), "column");
        bump("menu_mirror_columns");
        return true;
    }
    // 4. Choix : mêmes appels que l'exécution de la commande (SelectAction, SelectSecondaryAction), qui ouvrent le ciblage
    //    du jeu. Jamais RequestActionExecution ni ConfirmTarget : la confirmation vient seulement de la commande.
    if (want_tgt && !tgt) {
        if (g_mir.selected == key) return false;   // ciblage en cours d'ouverture
        // Un ciblage ouvert ici doit rester sourd aux entrées d'ici (une touche y confirmait l'action du héros d'un autre
        // joueur, p7_menumirror_host du 2026-10-22) : sans de quoi retirer ses entrées, pas de ciblage rejoué (ligne de texte).
        if (g_guard && !can_block_targeting()) {
            if (g_mir.tries["no_guard"]++ == 0) log().write("menu_mirror_no_guard", JsonObj().add("what", "UnregisterTargetingInput"), g_mir.turn);
            g_mir.gave_up = true;
            return false;
        }
        if (!attempt("select:" + key)) return false;
        clear_hover(now);
        bool ok = false;
        {
            game::ModCall mod;
            ue::Call sel(h.bm, STR("SelectAction"));
            if (sel) ok = sel.set_int(STR("ActionType"), w.action_type).invoke();
            if (ok && !w.secondary.empty()) {
                const std::wstring sec = ue::from_utf8(w.secondary);
                ue::Call c(h.bm, STR("SelectSecondaryAction"));
                ok = c && c.set_fname(STR("SelectedSecondaryAction"), sec.c_str()).invoke();
            }
        }
        if (g_guard) block_targeting_input(now, true);   // dans la même image que l'ouverture du ciblage
        mark_call(now, "select", "SelectAction/SelectSecondaryAction", ok);
        g_mir.selected = key;
        g_mir.selected_at = now;
        g_mir.next_step = now + 0.4;
        bump("menu_mirror_selects");
        return true;
    }
    // 5. Cibles (curseur du jeu).
    if (want_tgt && tgt && !w.targets.empty()) {
        std::vector<UObject*> want;
        for (auto& u : w.targets)
            if (UObject* o = game::find_unit(u.kind == proto::ActorKind::Hero, u.spot)) want.push_back(o);
        if (!want.empty() && !same_set(targeted(h), want)) return focus_targets(now, h, want);
    }
    // 6. Bouton survolé à la souris par le propriétaire (surbrillance de l'invite du bouton).
    if (lw) {
        const auto buttons = column_buttons(lw, column_of(lw));
        UObject* btn = w.hover >= 0 && w.hover < static_cast<int>(buttons.size()) ? buttons[static_cast<size_t>(w.hover)] : nullptr;
        if (btn != g_mir.hover_btn) {
            clear_hover(now);
            if (btn && mod_call_bool(now, btn, STR("SetPromptHighlighted"), STR("Highlighted"), true, "hover_on")) g_mir.hover_btn = btn;
            return true;
        }
    }
    return false;
}

// Tour d'un héros d'un autre joueur en cours ici : son emplacement, -1 sinon.
int remote_turn_spot()
{
    const BattleRef b = battle();
    if (!b.active) return -1;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur)) return -1;
    const int spot = game::spot(cur);
    if (g_net.controls && g_net.controls(spot)) return -1;
    return spot;
}

// Entrées d'ici pendant le tour d'un héros d'un autre joueur : en plus des contextes Enhanced Input retirés par le module
// coop, navigation d'interface du jeu (BP_UINavigationComponent : flèches, validation, retour, ciblage) et clics sur la
// roue 3D (LGUIWorldSpaceRaycaster) coupés. Rendus tels quels à la fin du tour, ou quand le menu pause est ouvert.
// En plus (2026-10-22, p7_menumirror_host : une touche ou un clic injecté confirmait le ciblage ouvert par le miroir et
// lançait l'action du héros de l'autre joueur ici) : entrées du ciblage du jeu retirées (WBP_HUD_BattleScreen :
// UnregisterTargetingInput, à chaque image où le jeu les a remises) et invites cliquables du tour (Confirmer / Retour du
// ciblage, Passer / Fuir) désactivées. Garde réarmée (et comptée) à chaque tour d'un héros d'un autre joueur.
struct Guard {
    bool on = false;
    uint32_t battle = 0, turn = 0;
    UObject* nav = nullptr; bool nav_prev = false; bool nav_set = false;
    UObject* rc = nullptr;
    std::vector<UObject*> disabled;         // invites désactivées par la garde (réactivées à la fin)
    double next = 0;
    double next_unreg = 0; int unreg_tries = 0; bool unreg_logged = false;
};
Guard g_guard_state;

UObject* battle_screen() { return ue::get_object(game::battle_manager(), STR("BattleScreenWidget")); }

// Entrées du ciblage du jeu retirées si le jeu les a enregistrées (IsTargetingRegistered). force : appel même sans
// le drapeau (juste après un choix rejoué par le miroir). false si l'écran de combat ou la fonction manque.
bool block_targeting_input(double now, bool force)
{
    auto& g = g_guard_state;
    UObject* bs = battle_screen();
    if (!bs) return false;
    const bool registered = ue::get_bool(bs, STR("IsTargetingRegistered"));
    // Chaque image : simple lecture du drapeau ; l'appel seulement s'il est levé (au plus 20 fois par tour s'il ne
    // baisse jamais : on n'insiste pas à chaque image).
    if (!force && (!registered || now < g.next_unreg || g.unreg_tries >= 20)) return true;
    ue::Call c(bs, STR("UnregisterTargetingInput"));
    if (!c) return false;
    g.next_unreg = now + 0.05;
    ++g.unreg_tries;
    { game::ModCall mod; c.invoke(); }
    bump("input_guard_targeting_unregistered");
    if (!g.unreg_logged) {
        g.unreg_logged = true;
        log().write("input_guard_targeting", JsonObj().add("registered", registered).add("after", ue::get_bool(bs, STR("IsTargetingRegistered"))).add("force", force),
                    battle().turn);
    }
    return true;
}

bool can_block_targeting()
{
    UObject* bs = battle_screen();
    return bs && static_cast<bool>(ue::Call(bs, STR("UnregisterTargetingInput")));
}

// Invites cliquables du tour (boutons CommonUI de l'écran de combat) : désactivées pendant la garde.
void disable_prompts(Guard& g)
{
    UObject* bs = battle_screen();
    for (const wchar_t* name : {STR("WBP_HUD_TargetingControls"), STR("WBP_BattleInput_PlayerTurn")}) {
        UObject* w = ue::get_object(bs, name);
        if (!w || std::find(g.disabled.begin(), g.disabled.end(), w) != g.disabled.end()) continue;
        ue::Call get(w, STR("GetIsEnabled"));
        ue::Call set(w, STR("SetIsEnabled"));
        // Paramètre introuvable : jamais désactivée (on ne pourrait pas la rendre). Déjà désactivée par le jeu : rien à rendre.
        if (!get || !set || !set.param_property(STR("bInIsEnabled")) || !get.invoke() || !get.out_bool(STR("ReturnValue"))) continue;
        { game::ModCall mod; set.set_bool(STR("bInIsEnabled"), false).invoke(); }
        g.disabled.push_back(w);
    }
}

void enable_prompts(Guard& g)
{
    UObject* bs = battle_screen();
    for (const wchar_t* name : {STR("WBP_HUD_TargetingControls"), STR("WBP_BattleInput_PlayerTurn")}) {
        UObject* w = ue::get_object(bs, name);
        if (!w || std::find(g.disabled.begin(), g.disabled.end(), w) == g.disabled.end()) continue;   // HUD recréé : rien à rendre
        ue::Call set(w, STR("SetIsEnabled"));
        if (set) { game::ModCall mod; set.set_bool(STR("bInIsEnabled"), true).invoke(); }
    }
    g.disabled.clear();
}

void guard_tick(double now, bool remote)
{
    auto& g = g_guard_state;
    const bool want = g_guard && remote && !game::pause_menu_visible();
    if (want) {
        const BattleRef b = battle();
        const bool rearm = g.on && (g.battle != b.id || g.turn != b.turn);
        if (rearm) { g.next = 0; g.unreg_tries = 0; g.unreg_logged = false; }   // nouveau tour : le jeu a pu tout remettre
        block_targeting_input(now, false);   // à chaque image : le ciblage peut s'ouvrir à tout moment
        if (g.on && !rearm && now < g.next) return;
        g.next = now + 0.5;
        disable_prompts(g);
        UObject* nav = ue::get_object(game::controller(), STR("BP_UINavigationComponent"));
        if (nav != g.nav) { g.nav = nav; g.nav_set = false; }
        if (nav && !ue::get_bool(nav, STR("DisableNavigation"))) {
            if (!g.nav_set) { g.nav_prev = false; g.nav_set = true; }
            ue::set_bool(nav, STR("DisableNavigation"), true);
        } else if (nav && !g.nav_set) {
            g.nav_prev = true;   // déjà coupée par le jeu : on la laissera ainsi
            g.nav_set = true;
        }
        UObject* rc = ue::get_object(ue::get_object(game::battle_manager(), STR("LGUI_Actor_BattleWheels")), STR("LGUIWorldSpaceRaycaster"));
        if (rc) {
            ue::Call c(rc, STR("DeactivateRaycaster"));
            if (c) { game::ModCall mod; c.invoke(); }
            g.rc = rc;
        }
        if (!g.on || rearm) {
            g.on = true;
            g.battle = b.id;
            g.turn = b.turn;
            bump("input_guards");
            log().write("input_guard", JsonObj().add("on", true).add("rearm", rearm).add("nav", nav != nullptr).add("raycaster", rc != nullptr)
                                           .add("prompts_disabled", static_cast<int64_t>(g.disabled.size())), b.turn);
        }
        return;
    }
    if (!g.on) return;
    enable_prompts(g);
    UObject* nav = ue::get_object(game::controller(), STR("BP_UINavigationComponent"));
    if (nav && nav == g.nav && g.nav_set) ue::set_bool(nav, STR("DisableNavigation"), g.nav_prev);
    UObject* rc = ue::get_object(ue::get_object(game::battle_manager(), STR("LGUI_Actor_BattleWheels")), STR("LGUIWorldSpaceRaycaster"));
    if (rc && rc == g.rc) {
        ue::Call c(rc, STR("ActivateRaycaster"));
        if (c) { game::ModCall mod; c.invoke(); }
    }
    log().write("input_guard", JsonObj().add("on", false), battle().turn);
    g = Guard{};
}

// Changements des menus ici pendant le tour d'un autre joueur : du mod (miroir, exécution) ou non (« dérive » : entrée
// d'ici qui a fuité, ou le jeu lui-même). Comptés pour le test des entrées bloquées.
void track_changes(double now, const Hud& h, bool known)
{
    const MenuPage page = h.wheels ? active_page(h) : MenuPage::None;
    const bool tgt = h.tm && targeting_on(h);
    const auto targets = h.tm ? targeted(h) : std::vector<UObject*>{};
    const bool changed = !g_mir.seen || page != g_mir.page || tgt != g_mir.targeting || targets != g_mir.targets;
    if (!changed) return;
    const bool first = !g_mir.seen;
    const MenuPage from = g_mir.page;
    const bool from_tgt = g_mir.targeting;
    g_mir.seen = true;
    g_mir.page = page;
    g_mir.targeting = tgt;
    g_mir.targets = targets;
    // Pas comptés : premier relevé, début du tour (le jeu ouvre la roue), exécution de la commande, visée rejouée.
    if (first || known || now - g_mir.t0 < kTurnGraceS || (g_net.remote_aim_active && g_net.remote_aim_active())) return;
    const bool by_mod = game::mod_call_depth() > 0 || now - g_mir.last_call < kModWindowS;
    bump(by_mod ? "menu_mirror_changes" : "menu_drifts");
    log().write(by_mod ? "menu_change" : "menu_drift", JsonObj().add("from_page", static_cast<int>(from)).add("page", static_cast<int>(page))
                                                           .add("from_targeting", from_tgt).add("targeting", tgt)
                                                           .add("targets", static_cast<int64_t>(targets.size())).add("since_mod_call", now - g_mir.last_call),
                g_mir.turn);
}

void mirror_tick(double now)
{
    const int spot = remote_turn_spot();
    guard_tick(now, spot >= 0);
    if (spot < 0) {
        if (g_mir.spot >= 0) g_mir = Mirror{};
        return;
    }
    const BattleRef b = battle();
    if (g_mir.battle != b.id || g_mir.turn != b.turn || g_mir.spot != spot) {
        g_mir = Mirror{};
        g_mir.battle = b.id;
        g_mir.turn = b.turn;
        g_mir.spot = spot;
        g_mir.t0 = now;
    }
    if (auto it = g_menus.find({b.id, b.turn, spot}); it != g_menus.end() && (!g_mir.have || it->second.seq != g_mir.want.seq)) {
        g_mir.want = it->second;
        g_mir.have = true;
        g_mir.tries.clear();   // nouvel état voulu : chaque étape a de nouveau droit à ses essais
    }
    const bool known = g_net.action_known && g_net.action_known(b.turn, spot);
    const Hud h = hud();
    track_changes(now, h, known || g_mir.released);
    if (!g_mirror || !g_mir.have) return;
    if (g_mir.released) {
        // Visée libre rejouée puis quittée sans action : les menus du propriétaire reprennent.
        const bool aim = g_net.remote_aim_active && g_net.remote_aim_active();
        if (known || aim || now - g_mir.released_at < 2.0) return;
        g_mir.released = false;
        log().write("menu_mirror_resume", JsonObj(), g_mir.turn);
    }
    if (known) return;   // la commande est là : mirror_release referme les menus juste avant son exécution
    if (g_net.paused && g_net.paused()) return;
    if (now < g_mir.next_step) return;
    if (!g_mir.base_set && h.wheels) { g_mir.base = active_page(h); g_mir.base_set = true; }
    if (mirror_step(now, h)) g_mir.next_step = std::max(g_mir.next_step, now + kMirrorStepS);
}

// --- Aperçu des autres (observateur) --------------------------------------------------------

std::map<std::tuple<uint32_t, uint32_t, int>, proto::IntentPreview> g_remote;   // (combat, tour, emplacement) -> dernier
double g_next_show = 0;
bool g_shown = false;

void show(double now)
{
    if (now < g_next_show) return;
    g_next_show = now + kPollS;
    const BattleRef b = battle();
    const proto::IntentPreview* p = nullptr;
    UObject* cur = b.active ? game::current_character() : nullptr;
    if (cur && game::is_player(cur)) {
        const int spot = game::spot(cur);
        auto it = g_remote.find({b.id, b.turn, spot});
        if (it != g_remote.end() && !(g_net.controls && g_net.controls(spot)) && !(g_net.action_known && g_net.action_known(b.turn, spot))) p = &it->second;
        // Menus rejoués dans l'interface du jeu : pas de ligne de texte, sauf pour la visée libre (rejouée à part) ou si
        // une partie des menus n'a pas pu être rejouée ici (pair sans MenuState, fonction du jeu introuvable ou refusée).
        const bool mirrored = g_mirror && g_menus.count({b.id, b.turn, spot}) && !(g_mir.spot == spot && g_mir.gave_up);
        if (p && mirrored && !p->aiming) p = nullptr;
    }
    std::string what;
    std::vector<std::string> names;
    if (p) {
        if (p->aiming) what = ui::text("intent_free_aim");
        else switch (p->action_type) {
            case game::ActAttack: what = ui::text("intent_attack"); break;
            case game::ActSkill: what = p->secondary.empty() ? ui::text("intent_skill_menu") : skill_name(cur, p->secondary); break;
            case game::ActItem: what = p->secondary.empty() ? ui::text("intent_item_menu") : item_name(p->secondary); break;
            default: break;   // passer / fuir : immédiats, rien à prévisualiser
        }
        for (auto& t : p->targets) names.push_back(unit_name(b.id, t));
    }
    if (what.empty() && names.empty()) {
        if (g_shown) ui::clear_status(kSlot);
        g_shown = false;
        return;
    }
    std::string targets;
    for (size_t i = 0; i < names.size() && i < 3; ++i) targets += (i ? ", " : "") + names[i];
    if (names.size() > 3) targets += " +" + std::to_string(names.size() - 3);
    const std::string player = g_net.name_of && g_net.owner_of_spot ? g_net.name_of(g_net.owner_of_spot(p->spot)) : "?";
    if (what.empty()) ui::set_status(kSlot, "intent_targets", {player, targets});
    else if (targets.empty()) ui::set_status(kSlot, "intent_action", {player, what});
    else ui::set_status(kSlot, "intent_target", {player, what, targets});
    g_shown = true;
}

// --- Caméra de la visée libre ------------------------------------------------------------------

// Caméra du propriétaire reçue, par (combat, tour, emplacement) ; une nouvelle visée (t_ms qui repart de 0) remplace l'ancienne.
std::map<std::tuple<uint32_t, uint32_t, int>, std::vector<proto::AimView>> g_views;
double g_next_view = 0;

struct Cam {
    UObject* actor = nullptr;               // CameraActor du mod
    UObject* pc = nullptr;                  // contrôleur au moment de la création (monde)
    UObject* prev = nullptr;                // cible de vue du jeu avant la visée
    bool engaged = false;
    double engaged_at = 0, next_reassert = 0, destroy_at = -1;
    bool placed = false;
    double pos[3]{}, pitch = 0, yaw = 0;
    bool from_owner = false;
};
Cam g_cam;

void set_float(ue::Call& c, const wchar_t* name, float v)
{
    if (c.param_size(name) == 4) if (auto* d = static_cast<float*>(c.param_ptr(name))) *d = v;
}

UObject* view_target(UObject* pc)
{
    ue::Call c(pc, STR("GetViewTarget"));
    if (!c || !c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

bool set_view_target(UObject* pc, UObject* target, float blend)
{
    ue::Call c(pc, STR("SetViewTargetWithBlend"));
    if (!c || !target) return false;
    c.set_object(STR("NewViewTarget"), target).set_int(STR("blendFunc"), 4 /* VTBlend_EaseInOut */).set_bool(STR("bLockOutgoing"), false);
    set_float(c, STR("BlendTime"), blend);
    set_float(c, STR("BlendExp"), 2.0f);
    return c.invoke();
}

// CameraActor du moteur, créé dans le monde du contrôleur (BeginDeferredActorSpawnFromClass + FinishSpawningActor).
UObject* spawn_camera(UObject* pc, const double pos[3])
{
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    UObject* cls = ue::find_object(STR("/Script/Engine.CameraActor"));
    ue::Call b(gps, STR("BeginDeferredActorSpawnFromClass"));
    if (!b || !cls) { log().write("aim_camera_error", JsonObj().add("what", "spawn functions or CameraActor class not found")); return nullptr; }
    // FTransform (doubles, alignés sur 16) : rotation (quaternion), translation, échelle.
    alignas(16) double t[12] = {0, 0, 0, 1, pos[0], pos[1], pos[2], 0, 1, 1, 1, 0};
    if (b.param_size(STR("SpawnTransform")) != sizeof t) {
        log().write("aim_camera_error", JsonObj().add("what", "unexpected FTransform size").add("size", static_cast<int64_t>(b.param_size(STR("SpawnTransform")))));
        return nullptr;
    }
    b.set_object(STR("WorldContextObject"), pc).set_object(STR("ActorClass"), cls).set_raw(STR("SpawnTransform"), t)
        .set_int(STR("CollisionHandlingOverride"), 1 /* AlwaysSpawn */).set_object(STR("Owner"), nullptr).set_int(STR("TransformScaleMethod"), 1);
    if (!b.invoke()) return nullptr;
    UObject* actor = b.out_object(STR("ReturnValue"));
    if (!actor) { log().write("aim_camera_error", JsonObj().add("what", "spawn returned null")); return nullptr; }
    ue::Call f(gps, STR("FinishSpawningActor"));
    if (f) f.set_object(STR("Actor"), actor).set_raw(STR("SpawnTransform"), t).set_int(STR("TransformScaleMethod"), 1).invoke();
    // Pas de bandes noires : l'image garde le format de l'écran.
    ue::set_bool(ue::get_object(actor, STR("CameraComponent")), STR("bConstrainAspectRatio"), false);
    return actor;
}

// Réglages d'image de la caméra d'épaule du héros (celle de la visée du jeu) : post-traitement (exposition…) et champ de vision.
void copy_camera_look(UObject* from, UObject* to)
{
    if (!from || !to) return;
    for (const wchar_t* name : {STR("PostProcessSettings"), STR("PostProcessBlendWeight"), STR("FieldOfView")}) {
        FProperty* dst = ue::prop(to, name);
        if (dst && dst == ue::prop(from, name)) dst->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(to), dst->ContainerPtrToValuePtr<void>(from));
    }
}

void place_camera(const double pos[3], double pitch, double yaw)
{
    ue::Call c(g_cam.actor, STR("K2_SetActorLocationAndRotation"));
    if (!c) return;
    if (auto* d = static_cast<double*>(c.param_ptr(STR("NewLocation")))) { d[0] = pos[0]; d[1] = pos[1]; d[2] = pos[2]; }
    if (auto* r = static_cast<double*>(c.param_ptr(STR("NewRotation")))) { r[0] = pitch; r[1] = yaw; r[2] = 0; }
    c.set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
}

double wrap180(double a)
{
    while (a > 180) a -= 360;
    while (a < -180) a += 360;
    return a;
}

// Pose de repli : derrière l'épaule droite du héros, regard vers le point visé.
bool fallback_pose(UObject* hero, const double aim[3], double pos[3], double& pitch, double& yaw)
{
    double h[3];
    if (!game::location(hero, h)) return false;
    double fx = aim[0] - h[0], fy = aim[1] - h[1];
    const double n = std::sqrt(fx * fx + fy * fy);
    if (n < 1) { fx = 1; fy = 0; } else { fx /= n; fy /= n; }
    const double rx = -fy, ry = fx;   // droite (UE : X avant, Y droite, Z haut)
    pos[0] = h[0] - fx * 260 + rx * 70;
    pos[1] = h[1] - fy * 260 + ry * 70;
    pos[2] = h[2] + 80;
    const double dx = aim[0] - pos[0], dy = aim[1] - pos[1], dz = aim[2] - pos[2];
    yaw = std::atan2(dy, dx) * 180.0 / 3.14159265358979;
    pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0 / 3.14159265358979;
    return true;
}

void release_camera(double now, const char* why)
{
    if (!g_cam.engaged) return;
    g_cam.engaged = false;
    UObject* pc = game::controller();
    if (pc && pc == g_cam.pc) {
        // Le jeu a déjà repris la vue (fin de visée) : on ne touche à rien ; sinon retour à sa vue d'avant.
        const UObject* cur = view_target(pc);
        if ((cur == g_cam.actor || now - g_cam.engaged_at < 1.0) && g_cam.prev) set_view_target(pc, g_cam.prev, 0.4f);
        g_cam.destroy_at = now + 1.5;   // après le fondu
    } else {
        g_cam = Cam{};   // autre monde : la caméra a disparu avec l'ancien
    }
    bump("aim_camera_released");
    log().write("aim_camera_released", JsonObj().add("why", why));
}

} // namespace

void init(const Net& net)
{
    g_net = net;
    g_aim_cam = cmdline::arg(L"e33aimcam") != L"0";
    // Désactivé par défaut (2026-10-22) : une touche d'ici confirmait la cible ouverte par le miroir (p7_menumirror_host).
    g_mirror = cmdline::arg(L"e33menumirror") == L"1";
    g_guard = cmdline::arg(L"e33inputguard") != L"0";
    log().write("intent_init", JsonObj().add("aim_cam", g_aim_cam).add("menu_mirror", g_mirror).add("input_guard", g_guard));
}

bool mirror_enabled() { return g_mirror; }

bool mirror_release(double now)
{
    const int spot = remote_turn_spot();
    const BattleRef b = battle();
    if (!g_mirror || spot < 0 || g_mir.battle != b.id || g_mir.turn != b.turn || g_mir.spot != spot) return true;
    if (!g_mir.released) {
        g_mir.released = true;
        g_mir.release_t0 = now;
        g_mir.tries.erase("rel_cancel");
        g_mir.tries.erase("rel_back");
        if (g_mir.acted) log().write("menu_mirror_release", JsonObj().add("selected", g_mir.selected), g_mir.turn);
    }
    g_mir.released_at = now;
    if (!g_mir.acted) return true;   // rien changé par le miroir depuis la dernière remise à zéro
    if (now - g_mir.release_t0 > 1.5) {
        bump("menu_mirror_release_timeouts");
        log().write("menu_mirror_release_timeout", JsonObj(), g_mir.turn);
        g_mir.acted = false;
        return true;
    }
    if (now < g_mir.next_step) return false;
    const Hud h = hud();
    clear_hover(now);
    // Ciblage refermé, puis roue revenue à la page des actions : l'état d'avant le miroir (celui du début du tour).
    // Le pré-ciblage de la roue des actions fait partie de cet état : pas d'annulation pour lui.
    if (h.tm && selection_targeting(h) && g_mir.tries["rel_cancel"]++ < 3) {
        mod_call(now, h.bm, STR("OnCancelTargeting"), "release_cancel");
        g_mir.selected.clear();
        g_mir.next_step = now + 0.3;
        return false;
    }
    const MenuPage home = g_mir.base != MenuPage::None ? g_mir.base : MenuPage::Actions;
    if (h.wheels && active_page(h) != home && g_mir.tries["rel_back"] < 4) {
        const int n = ++g_mir.tries["rel_back"];
        if (n <= 2 && home == MenuPage::Actions) mod_call(now, h.wheels, STR("BackToActionPage"), "release_back");
        else change_page(now, h, page_object(h, home));
        g_mir.next_step = now + 0.3;
        return false;
    }
    if (now - g_mir.last_call < 0.25) return false;   // fin de l'animation de retour
    g_mir.acted = false;
    g_mir.selected.clear();
    bump("menu_mirror_releases");
    log().write("menu_mirror_released", JsonObj().add("waited_s", now - g_mir.release_t0), g_mir.turn);
    return true;
}

void reload_options() { g_aim_cam = cmdline::arg(L"e33aimcam") != L"0"; }

// Garde des entrées, au plus près : pendant le tour du héros d'un autre joueur, tant que sa commande n'est pas arrivée,
// aucune confirmation de cible ne doit lancer d'action ici, qu'elle vienne d'une touche, d'un clic, d'un délai interne du
// jeu ou d'un appel du miroir. Un crochet UE4SS ne peut pas empêcher un appel, et un Blueprint qui appelle ConfirmTarget
// exécute son bytecode sans passer par UFunction::Func (essai du 2026-10-22). ConfirmTarget lance l'action par son
// délégué OnTargetConfirmed : pendant l'appel bloqué, la liste de ses abonnés (TArray de 16 octets) est mise de côté
// avant l'appel et remise après, la confirmation ne lance donc rien.
struct HeldDelegate
{
    UObject* tm = nullptr;
    std::array<uint8_t, 16> bytes{};
};
std::vector<HeldDelegate> g_confirm_held;
int g_confirm_logged = 0;

bool confirm_must_block()
{
    if (!g_guard) return false;
    const int spot = remote_turn_spot();
    if (spot < 0) return false;
    const BattleRef b = battle();
    if (g_net.action_known && g_net.action_known(b.turn, spot)) return false;   // commande arrivée : le rejeu confirme
    if (g_net.remote_aim_active && g_net.remote_aim_active()) return false;     // visée libre rejouée
    return true;
}

// Appelants (cadres de script) d'une fonction accrochée, pour les diagnostics de la garde.
std::string script_callers(FFrame& stack)
{
    std::string frames = "[";
    FFrame* f = stack.PreviousFrame();
    for (int depth = 0; f && depth < 5; ++depth) {
        if (frames.size() > 1) frames += ",";
        frames += JsonObj().add("o", ue::short_name(f->Object())).add("n", ue::short_name(f->Node())).str();
        FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    return frames + "]";
}

// État du gestionnaire de ciblage mis de côté pendant un appel bloqué : TargetingEnabled? à faux (si ConfirmTarget, la
// visée à la souris ou le clic sur un ennemi le testent, ils ne font rien).
struct HeldTargeting
{
    UObject* tm = nullptr;
    bool enabled = false;
};
std::vector<HeldTargeting> g_targeting_held;

UObject* targeting_manager() { return ue::get_object(game::controller(), STR("AC_jRPG_TargetingManager")); }

void hold_targeting(UObject* tm)
{
    if (!tm) return;
    g_targeting_held.push_back({tm, ue::get_bool(tm, STR("TargetingEnabled?"))});
    ue::set_bool(tm, STR("TargetingEnabled?"), false);
}

void release_targeting(UObject* tm)
{
    for (auto it = g_targeting_held.rbegin(); it != g_targeting_held.rend(); ++it) {
        if (it->tm != tm) continue;
        ue::set_bool(tm, STR("TargetingEnabled?"), it->enabled);
        g_targeting_held.erase(std::next(it).base());
        return;
    }
}

// Bouton « Confirmer » du bandeau de ciblage : l'écran de combat reçoit l'événement par un délégué natif (ProcessEvent),
// qui passe par UFunction::Func ; sa fonction est remplacée par confirm_click_gate (au tick, une fois l'écran chargé).
constexpr const wchar_t* kConfirmClickPath = L"/Game/UI/Widgets/HUD_Battle/WBP_HUD_BattleScreen.WBP_HUD_BattleScreen_C:"
    L"BndEvt__WBP_HUD_BattleScreen_WBP_HUD_TargetingControls_K2Node_ComponentBoundEvent_3_OnConfirmClicked__DelegateSignature";
UnrealScriptFunction g_confirm_click_orig = nullptr;
bool g_confirm_click_installed = false;
bool confirm_must_block();

void confirm_click_gate(UObject* context, FFrame& stack, void* result)
{
    if (!confirm_must_block()) { g_confirm_click_orig(context, stack, result); return; }
    bump("input_guard_confirm_clicks_blocked");
    log().write("input_guard_confirm_click_blocked", JsonObj().add("spot", remote_turn_spot()), battle().turn);
}

void install_confirm_click_gate()
{
    if (g_confirm_click_installed || !g_guard) return;
    UFunction* f = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kConfirmClickPath);
    if (!f || !f->GetFuncPtr()) return;
    g_confirm_click_orig = f->GetFuncPtr();
    f->SetFuncPtr(&confirm_click_gate);
    g_confirm_click_installed = true;
    log().write("input_guard_confirm_click_gate", JsonObj().add("installed", true));
}

void confirm_pre(HookCtx& ctx)
{
    if (!confirm_must_block()) return;
    void* d = ue::value_ptr<void>(ctx.Context, STR("OnTargetConfirmed"));
    if (!d) { log().write("input_guard_confirm_missing", JsonObj(), battle().turn); return; }
    HeldDelegate h;
    h.tm = ctx.Context;
    std::memcpy(h.bytes.data(), d, h.bytes.size());
    std::memset(d, 0, h.bytes.size());
    g_confirm_held.push_back(h);
    hold_targeting(ctx.Context);
    bump("input_guard_confirms_blocked");
    if (g_confirm_logged++ < 30)
        log().write("input_guard_confirm_blocked", JsonObj().add("spot", remote_turn_spot()).add("mod", game::mod_call_depth() > 0)
                                                       .add("pretargeting", ue::get_bool(ctx.Context, STR("IsPretargeting")))
                                                       .add("mirror_selected", g_mir.selected).raw("callers", script_callers(ctx.TheStack)), battle().turn);
}

void confirm_post(HookCtx& ctx)
{
    release_targeting(ctx.Context);
    for (auto it = g_confirm_held.rbegin(); it != g_confirm_held.rend(); ++it) {
        if (it->tm != ctx.Context) continue;
        if (void* d = ue::value_ptr<void>(ctx.Context, STR("OnTargetConfirmed"))) std::memcpy(d, it->bytes.data(), it->bytes.size());
        g_confirm_held.erase(std::next(it).base());
        return;
    }
}

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();
    const wchar_t* bm = game::path::BattleMgr;
    // Roue : action choisie (attaque, compétences, objets…) puis compétence / objet. Valeurs lues après l'appel.
    bus.post(fn(bm, L"SelectAction"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) {
            l->action = static_cast<uint8_t>(ue::get_int(game::battle_manager(), STR("SelectedAction"), 0));
            l->secondary.clear();
        }
    });
    bus.post(fn(bm, L"SelectSecondaryAction"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) l->secondary = ue::get_name_string(game::battle_manager(), STR("SelectedSecondaryAction"));
    });
    // Page des compétences ouverte (aucune choisie encore).
    bus.post(fn(bm, L"OnSelectSkillMenu"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) { l->action = 5; l->secondary.clear(); }
    });
    // Retour en arrière : ciblage annulé (retour à la liste, ou à la roue pour l'attaque), liste des compétences fermée.
    bus.post(fn(bm, L"OnCancelTargeting"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) {
            if (!l->secondary.empty()) l->secondary.clear();
            else l->action = 0;
        }
    });
    bus.post(fn(bm, L"OnSkillSelectionClosed"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) { l->action = 0; l->secondary.clear(); }
    });
    // Action lancée : l'aperçu s'arrête (l'action elle-même part par le module coop).
    for (const wchar_t* name : {L"RequestActionExecution", L"OnSelectSkip", L"OnSelectFlee"})
        bus.pre(fn(bm, name), [](HookCtx&, UFunction*) { if (Local* l = local_turn()) l->done = true; });

    // Confirmation de cible pendant le tour d'un autre joueur : abonnés de OnTargetConfirmed mis de côté pendant l'appel.
    {
        const std::wstring confirm = fn(L"/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_TargetingManager.AC_jRPG_TargetingManager_C:", L"ConfirmTarget");
        bus.pre(confirm, [](HookCtx& ctx, UFunction*) { confirm_pre(ctx); });
        bus.post(confirm, [](HookCtx& ctx, UFunction*) { confirm_post(ctx); });
    }
    // Visée à la souris du jeu (clic sur un ennemi -> OnClicked_Trace -> ConfirmTarget, vu le 2026-10-22) : ciblage
    // désactivé le temps de l'appel pendant le tour d'un autre joueur.
    for (const wchar_t* name : {L"UpdateMouseTargeting", L"UpdateFreeAimHover"}) {
        bus.pre(fn(bm, name), [](HookCtx&, UFunction*) { if (confirm_must_block()) hold_targeting(targeting_manager()); });
        bus.post(fn(bm, name), [](HookCtx&, UFunction*) { if (!g_targeting_held.empty()) release_targeting(targeting_manager()); });
    }
    // Diagnostic : d'où part une action lancée pendant le tour d'un autre joueur sans sa commande.
    bus.pre(fn(bm, L"RequestActionExecution"), [](HookCtx& ctx, UFunction*) {
        if (!confirm_must_block()) return;
        static int logged = 0;
        if (logged++ < 30) log().write("input_guard_execution_callers", JsonObj().raw("callers", script_callers(ctx.TheStack)), battle().turn);
    });
    bus.post(fn(bm, L"RequestActionExecution"), [](HookCtx&, UFunction*) {
        if (!g_guard || game::mod_call_depth() > 0 || remote_turn_spot() < 0) return;
        log().write("input_guard_execution_leak", JsonObj(), battle().turn);   // diagnostic : la confirmation a quand même lancé l'action
    });
    // Diagnostic : chemin pris par une entrée d'ici vers la confirmation ou le retour du ciblage pendant le tour d'un autre
    // joueur (navigation d'interface du jeu, invite cliquable, bouton CommonUI). Au plus 20 lignes par fonction et par lancement.
    const std::pair<const wchar_t*, const wchar_t*> routes[] = {
        {L"/Game/Gameplay/UINavigation/BP_UINavigationComponent.BP_UINavigationComponent_C:", L"InputAction_UI_Confirm"},
        {L"/Game/Gameplay/UINavigation/BP_UINavigationComponent.BP_UINavigationComponent_C:", L"InputAction_UI_Cancel"},
        {L"/Game/Gameplay/UINavigation/BP_UINavigationComponent.BP_UINavigationComponent_C:", L"OnElementAction"},
        {L"/Game/UI/Widgets/HUD_Battle/Buttons/WBP_HUD_Battle_TargetingControls.WBP_HUD_Battle_TargetingControls_C:", L"OnUIAction"},
        {L"/Game/UI/Widgets/HUD_Battle/Buttons/WBP_HUD_Battle_TargetingControls.WBP_HUD_Battle_TargetingControls_C:",
         L"BndEvt__WBP_HUD_CancelButton_WBP_GM_PromptGeneric_K2Node_ComponentBoundEvent_1_CommonButtonBaseClicked__DelegateSignature"},
        {L"/Game/UI/Widgets/HUD_Battle/WBP_HUD_BattleScreen.WBP_HUD_BattleScreen_C:",
         L"BndEvt__WBP_HUD_BattleScreen_WBP_HUD_TargetingControls_K2Node_ComponentBoundEvent_3_OnConfirmClicked__DelegateSignature"},
        {L"/Game/UI/Widgets/HUD_Battle/WBP_HUD_BattleScreen.WBP_HUD_BattleScreen_C:", L"RegisterTargetingInput"},
    };
    for (const auto& [cls, name] : routes) {
        const std::string n = ue::to_utf8(name).substr(0, 48);
        auto count = std::make_shared<int>(0);
        bus.pre(fn(cls, name), [n, count](HookCtx&, UFunction*) {
            if (*count >= 20 || remote_turn_spot() < 0) return;
            ++*count;
            log().write("input_route", JsonObj().add("fn", n).add("mod", game::mod_call_depth() > 0).add("guard", g_guard_state.on), battle().turn);
        });
    }
}

void receive(const proto::IntentPreview& m)
{
    const BattleRef b = battle();
    if (m.battle_id != b.id) return;
    auto& slot = g_remote[{m.battle_id, m.turn, m.spot}];
    if (slot.seq != 0 && m.seq <= slot.seq) return;   // en retard
    slot = m;
    bump("intent_previews_received");
    // Ménage : tours passés et combats précédents.
    for (auto it = g_remote.begin(); it != g_remote.end();) {
        if (std::get<0>(it->first) != b.id || std::get<1>(it->first) + 1 < b.turn) it = g_remote.erase(it);
        else ++it;
    }
}

void receive(const proto::AimView& m)
{
    const BattleRef b = battle();
    if (m.battle_id != b.id) return;
    auto& v = g_views[{m.battle_id, m.turn, m.spot}];
    if (!v.empty() && m.t_ms + 500 < v.back().t_ms) v.clear();   // nouvelle visée dans le même tour
    if (v.size() < 4000) v.push_back(m);
    bump("aim_views_received");
    for (auto it = g_views.begin(); it != g_views.end();) {
        if (std::get<0>(it->first) != b.id || std::get<1>(it->first) + 1 < b.turn) it = g_views.erase(it);
        else ++it;
    }
}

void receive(const proto::MenuState& m)
{
    const BattleRef b = battle();
    if (m.battle_id != b.id) return;
    auto& slot = g_menus[{m.battle_id, m.turn, m.spot}];
    if (slot.seq != 0 && m.seq <= slot.seq) return;   // en retard
    slot = m;
    bump("menu_states_received");
    log().write("menu_remote", JsonObj().raw("msg", proto::to_json(m)), m.turn);
    for (auto it = g_menus.begin(); it != g_menus.end();) {
        if (std::get<0>(it->first) != b.id || std::get<1>(it->first) + 1 < b.turn) it = g_menus.erase(it);
        else ++it;
    }
}

void tick(double now)
{
    if (!battle().active) {
        if (!g_remote.empty() || !g_views.empty()) { g_remote.clear(); g_views.clear(); g_unit_names.clear(); }
        g_menus.clear();
    }
    if (battle().active) install_confirm_click_gate();
    poll_local(now);
    poll_menu(now);
    mirror_tick(now);
    show(now);
}

void aim_owner_tick(double now, int spot, uint32_t t_ms)
{
    if (now < g_next_view) return;
    g_next_view = now + kAimViewS;
    const BattleRef b = battle();
    UObject* cm = ue::get_object(game::controller(), STR("PlayerCameraManager"));
    if (!cm || !b.active) return;
    proto::AimView v;
    v.battle_id = b.id;
    v.turn = b.turn;
    v.spot = static_cast<uint8_t>(spot);
    v.t_ms = t_ms;
    {
        ue::Call c(cm, STR("GetCameraLocation"));
        if (!c || !c.invoke()) return;
        const auto* d = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
        if (!d) return;
        v.x = d[0]; v.y = d[1]; v.z = d[2];
    }
    {
        ue::Call c(cm, STR("GetCameraRotation"));
        if (!c || !c.invoke()) return;
        const auto* r = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
        if (!r) return;
        v.pitch = r[0]; v.yaw = r[1];
    }
    {
        ue::Call c(cm, STR("GetFOVAngle"));
        if (c && c.invoke()) v.fov = c.out_double(STR("ReturnValue"), 0);
        if (!std::isfinite(v.fov) || v.fov < 0 || v.fov > 180) v.fov = 0;
    }
    if (g_net.publish) g_net.publish(v, false);
    bump("aim_views_sent");
}

void aim_camera_tick(double now, double dt, const AimReplay* r)
{
    // Caméra rendue : destruction après le fondu, dans le même monde seulement.
    if (!g_cam.engaged && g_cam.actor && g_cam.destroy_at > 0 && now >= g_cam.destroy_at) {
        if (game::controller() == g_cam.pc) {
            ue::Call d(g_cam.actor, STR("K2_DestroyActor"));
            if (d) d.invoke();
        }
        g_cam = Cam{};
    }
    if (!r || !r->hero || !g_aim_cam) { release_camera(now, "replay_over"); return; }
    UObject* pc = game::controller();
    if (!pc) return;
    if (g_cam.actor && pc != g_cam.pc) g_cam = Cam{};   // changement de monde : l'acteur n'existe plus
    // Pose voulue : caméra du propriétaire au même instant de sa visée, sinon derrière l'épaule vers le point visé.
    const BattleRef b = battle();
    double pos[3]{}, pitch = 0, yaw = 0, fov = 0;
    bool have = false, from_owner = false;
    auto it = g_views.find({b.id, b.turn, r->spot});
    if (it != g_views.end()) {
        const proto::AimView* best = nullptr;
        for (auto& v : it->second) if (v.t_ms <= r->elapsed_ms && (!best || v.t_ms >= best->t_ms)) best = &v;
        double h[3];
        // Garde-fou : la caméra du propriétaire doit être près du même héros ici (arène identique).
        if (best && game::location(r->hero, h) && std::hypot(best->x - h[0], best->y - h[1], best->z - h[2]) < 2000) {
            pos[0] = best->x; pos[1] = best->y; pos[2] = best->z;
            pitch = best->pitch; yaw = best->yaw; fov = best->fov;
            have = from_owner = true;
        }
    }
    if (!have && r->have_point) have = fallback_pose(r->hero, r->point, pos, pitch, yaw);
    if (!have) return;   // rien à montrer encore : la vue du jeu reste
    if (!g_cam.actor) {
        g_cam = Cam{};
        g_cam.actor = spawn_camera(pc, pos);
        g_cam.pc = pc;
        if (!g_cam.actor) { g_aim_cam = false; return; }   // échec : on n'insiste pas (comportement d'avant)
    }
    g_cam.destroy_at = -1;
    if (!g_cam.engaged || !g_cam.placed) {
        std::copy(pos, pos + 3, g_cam.pos);
        g_cam.pitch = pitch;
        g_cam.yaw = yaw;
        g_cam.placed = true;
    } else {
        // Lissage (paquets non fiables, ~20 par seconde).
        const double a = 1.0 - std::exp(-dt * 12.0);
        for (int i = 0; i < 3; ++i) g_cam.pos[i] += (pos[i] - g_cam.pos[i]) * a;
        g_cam.pitch += wrap180(pitch - g_cam.pitch) * a;
        g_cam.yaw += wrap180(yaw - g_cam.yaw) * a;
    }
    place_camera(g_cam.pos, g_cam.pitch, g_cam.yaw);
    if (fov > 1) if (auto* f = ue::value_ptr<float>(ue::get_object(g_cam.actor, STR("CameraComponent")), STR("FieldOfView"))) *f = static_cast<float>(fov);
    if (from_owner != g_cam.from_owner) {
        g_cam.from_owner = from_owner;
        log().write("aim_camera_source", JsonObj().add("from_owner", from_owner).add("spot", r->spot));
    }
    if (!g_cam.engaged) {
        copy_camera_look(ue::get_object(r->hero, STR("Camera_Shoulder")), ue::get_object(g_cam.actor, STR("CameraComponent")));
        if (fov > 1) if (auto* f = ue::value_ptr<float>(ue::get_object(g_cam.actor, STR("CameraComponent")), STR("FieldOfView"))) *f = static_cast<float>(fov);
        g_cam.prev = view_target(pc);
        set_view_target(pc, g_cam.actor, 0.35f);
        g_cam.engaged = true;
        g_cam.engaged_at = now;
        g_cam.next_reassert = now + 1.0;
        bump("aim_camera_engaged");
        log().write("aim_camera_engaged", JsonObj().add("spot", r->spot).add("from_owner", from_owner).add("prev", ue::short_name(g_cam.prev))
                                             .add("x", pos[0]).add("y", pos[1]).add("z", pos[2]).add("pitch", pitch).add("yaw", yaw));
        return;
    }
    // Le jeu a remis sa propre vue pendant la visée (caméra de visée, de combat…) : on reprend la main, sans à-coup.
    if (now >= g_cam.next_reassert) {
        g_cam.next_reassert = now + 0.5;
        if (UObject* cur = view_target(pc); cur != g_cam.actor) {
            set_view_target(pc, g_cam.actor, 0.2f);
            bump("aim_camera_reasserted");
            log().write("aim_camera_reasserted", JsonObj().add("game_view", ue::short_name(cur)));
        }
    }
}

} // namespace e33::intent
