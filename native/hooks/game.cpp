#include "game.hpp"

#include <map>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>

#include "log.hpp"
#include "reflect.hpp"
#include "ue.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace e33::game {

namespace {

UObject* g_gi = nullptr;
std::wstring g_last_encounter;
std::vector<std::wstring> g_last_party;

} // namespace

UObject* load_asset(const wchar_t* object_path)
{
    if (UObject* o = ue::find_object(object_path)) return o;
    auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
    if (!registry) return nullptr;
    FAssetData data = registry->GetAssetByObjectPath(FName(object_path, FNAME_Add));
    if (!data.PackageName().GetComparisonIndex()) return nullptr;
    return UAssetRegistryHelpers::GetAsset(data);
}

namespace {

bool set_member_object(UStruct* st, void* base, const wchar_t* name, UObject* v)
{
    FProperty* p = ue::struct_member(st, name);
    if (!p) return false;
    *static_cast<UObject**>(p->ContainerPtrToValuePtr<void>(base)) = v;
    return true;
}

bool set_member_byte(UStruct* st, void* base, const wchar_t* name, uint8_t v)
{
    FProperty* p = ue::struct_member(st, name);
    if (!p) return false;
    *static_cast<uint8_t*>(p->ContainerPtrToValuePtr<void>(base)) = v;
    return true;
}

bool set_member_bool(UStruct* st, void* base, const wchar_t* name, bool v)
{
    auto* bp = CastField<FBoolProperty>(ue::struct_member(st, name));
    if (!bp) return false;
    bp->SetPropertyValueInContainer(base, v);
    return true;
}

bool set_member_fname(UStruct* st, void* base, const wchar_t* name, const wchar_t* v)
{
    FProperty* p = ue::struct_member(st, name);
    if (!p) return false;
    *static_cast<FName*>(p->ContainerPtrToValuePtr<void>(base)) = FName(v, FNAME_Add);
    return true;
}

// Remplit un TArray<FS_jRPG_PartySlot> (membre OverrideParty des paramètres de combat).
bool set_party(UStruct* st, void* base, const std::vector<std::wstring>& party)
{
    auto* ap = CastField<FArrayProperty>(ue::struct_member(st, STR("OverrideParty")));
    if (!ap) return false;
    auto* inner = CastField<FStructProperty>(ap->GetInner());
    if (!inner) return false;
    UStruct* slot_struct = inner->GetStruct();
    const int32 elem = inner->GetSize();
    auto* arr = static_cast<FScriptArray*>(ap->ContainerPtrToValuePtr<void>(base));
    if (party.empty()) return true;
    if (arr->Num() != 0) return false;   // objet neuf : le tableau doit être vide
    arr->AddZeroed(static_cast<int32>(party.size()), elem, static_cast<uint32>(alignof(uint64_t)));
    auto* data = static_cast<uint8_t*>(arr->GetData());
    for (size_t i = 0; i < party.size(); ++i) {
        void* slot = data + i * static_cast<size_t>(elem);
        if (!set_member_fname(slot_struct, slot, STR("CharacterHardcodedName"), party[i].c_str())) return false;
        set_member_byte(slot_struct, slot, STR("Formation"), 0);
    }
    return true;
}

// Position monde d'un acteur (K2_GetActorLocation ; FVector = 3 doubles en UE 5.4).
std::optional<std::array<double, 3>> actor_location(UObject* actor)
{
    ue::Call c(actor, STR("K2_GetActorLocation"));
    if (!c || !c.invoke()) return std::nullopt;
    const auto* v = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
    if (!v) return std::nullopt;
    return std::array<double, 3>{v[0], v[1], v[2]};
}

double hp_ratio(UObject* ch)
{
    UObject* st = stats(ch);
    const double hp = ue::get_double(st, STR("CurrentHP"));
    ue::Call c(st, STR("GetMaxHP"));
    double max = 0;
    if (c && c.invoke()) max = c.out_double(STR("MaxHP"), c.out_double(STR("ReturnValue"), 0));
    return max > 0 ? hp / max : hp;
}

std::vector<UObject*> by_spot(std::vector<UObject*> v)
{
    std::sort(v.begin(), v.end(), [](UObject* a, UObject* b) { return spot(a) < spot(b); });
    return v;
}

} // namespace

UObject* gi()
{
    if (!g_gi) g_gi = UObjectGlobals::FindFirstOf(STR("BP_jRPG_GI_Custom_C"));
    return g_gi;
}

UObject* controller()
{
    ue::Call c(gi(), STR("GetControllerWorld"));
    if (!c || !c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

UObject* battle_manager() { return ue::get_object(controller(), STR("AC_jRPG_BattleManager")); }
UObject* save_manager() { return ue::get_object(gi(), STR("SaveManager")); }

UObject* main_menu() { return UObjectGlobals::FindFirstOf(STR("WBP_MM_MainMenu_C")); }
UObject* world_pawn() { return UObjectGlobals::FindFirstOf(STR("BP_jRPG_Character_World_C")); }
UObject* input_subsystem() { return UObjectGlobals::FindFirstOf(STR("EnhancedInputLocalPlayerSubsystem")); }

bool map_change_in_progress()
{
    ue::Call c(gi(), STR("IsMapChangeInProgress"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("InProgress"));
}

bool in_battle()
{
    ue::Call c(controller(), STR("IsInBattle"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("ReturnValue"));
}

// Un héros mort quitte PlayerCharacters pour DeadPlayerCharacters (vu le 2026-10-19) : les deux listes.
std::vector<UObject*> players()
{
    UObject* bm = battle_manager();
    auto out = ue::get_object_array(bm, STR("PlayerCharacters"));
    for (UObject* d : ue::get_object_array(bm, STR("DeadPlayerCharacters")))
        if (d && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    return out;
}
std::vector<UObject*> enemies() { return ue::get_object_array(battle_manager(), STR("Enemies")); }
UObject* current_character() { return ue::get_object(battle_manager(), STR("CurrentCharacter")); }
UObject* stats(UObject* ch) { return ue::get_object(ch, STR("AC_jRPG_CharacterStats")); }
bool is_player(UObject* ch) { return ue::get_bool(ch, STR("IsPlayerControlledCharacter")); }

bool is_dead(UObject* ch)
{
    ue::Call c(ch, STR("GetIsDead"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("Dead"));
}

// Dead? n'est pas posé par toutes les morts (SetDead des stats : vu le 2026-10-19) ; 0 PV = mort pour le jeu.
bool is_dead_fast(UObject* ch) { return ue::get_bool(ch, STR("Dead?")) || ue::get_double(stats(ch), STR("CurrentHP"), 1.0) <= 0.0; }
int spot(UObject* ch) { return ch ? static_cast<int>(ue::get_int(ch, STR("BattleSpotIndex"), -1)) : -1; }

UObject* find_unit(bool hero, int s)
{
    for (UObject* c : hero ? players() : enemies())
        if (spot(c) == s) return c;
    return nullptr;
}

std::string char_name(UObject* ch) { return ue::short_name(ch); }
std::string char_class(UObject* ch) { return ch ? ue::short_name(ch->GetClassPrivate()) : std::string("null"); }
std::string hardcoded_name(UObject* ch) { return ue::get_name_string(stats(ch), STR("CharacterHardcodedName")); }

bool load_save_slot(const std::wstring& slot, std::string& err)
{
    UObject* sm = save_manager();
    if (!sm) { err = "no save manager"; return false; }
    ue::set_bool(sm, STR("EnableAutoSave"), false);

    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    ue::Call load(gps, STR("LoadGameFromSlot"));
    if (!load) { err = "LoadGameFromSlot not found"; return false; }
    load.set_string(STR("SlotName"), slot.c_str()).set_int(STR("UserIndex"), 0).invoke();
    UObject* sg = load.out_object(STR("ReturnValue"));
    if (!sg) { err = "save not found: " + ue::to_utf8(slot); return false; }

    ue::Call apply(sm, STR("LoadGameFromSaveFile"));
    if (!apply) { err = "LoadGameFromSaveFile not found"; return false; }
    apply.set_object(STR("SaveGame"), sg).set_string(STR("SlotName"), slot.c_str()).invoke();
    log().write("save_load_requested", JsonObj().add("slot", ue::to_utf8(slot)).add("save_class", ue::short_name(sg->GetClassPrivate())));
    return true;
}

bool start_new_game(bool no_autosave, std::string& err)
{
    UObject* sm = save_manager();
    if (!sm) { err = "no save manager"; return false; }
    // Client (et tests) : la partie locale n'est qu'un support pour l'intro, l'instantané de l'hôte la remplace ensuite.
    // Aucune sauvegarde ne doit atterrir dans les emplacements du joueur.
    if (no_autosave) ue::set_bool(sm, STR("EnableAutoSave"), false);
    ue::Call c(sm, STR("StartNewGame"));
    if (!c) { err = "StartNewGame not found"; return false; }
    c.set_bool(STR("FromMainMenu"), true).set_fname(STR("OverrideStartLevelDataID"), STR("None")).invoke();
    log().write("new_game_requested", JsonObj().add("no_autosave", no_autosave));
    return true;
}

bool trigger_battle(const std::wstring& encounter, const std::vector<std::wstring>& party, std::string& err)
{
    UObject* ctrl = controller();
    if (!ctrl) { err = "no controller"; return false; }
    UObject* executor = ue::get_object(ctrl, STR("BP_GameActionExecutorComponent"));
    if (!executor) { err = "no game action executor"; return false; }

    UObject* dt = load_asset(STR("/Game/jRPGTemplate/Datatables/DT_jRPG_Encounters.DT_jRPG_Encounters"));
    auto* cls = static_cast<UClass*>(load_asset(STR("/Game/Gameplay/GameActionsSystem/TriggerBattle/BP_GameAction_TriggerBattle.BP_GameAction_TriggerBattle_C")));
    load_asset(STR("/Game/Gameplay/GameActionsSystem/TriggerBattle/BP_GameActionInstance_TriggerBattle.BP_GameActionInstance_TriggerBattle_C"));
    if (!dt || !cls) { err = "encounter table or trigger class not loaded"; return false; }

    std::vector<UObject*> maps;
    UObjectGlobals::FindAllOf(STR("BP_BattleMap_C"), maps);
    if (maps.empty()) { err = "no BP_BattleMap_C in level"; return false; }
    // Arène la plus proche du personnage du joueur. (Avant : la première par nom, qui pouvait être ailleurs
    // dans le niveau, hors du décor chargé : caméra dans la roche, écran noir pendant les attaques ennemies.)
    std::sort(maps.begin(), maps.end(), [](UObject* a, UObject* b) { return a->GetName() < b->GetName(); });
    double best_d2 = -1;
    if (UObject* pawn = UObjectGlobals::FindFirstOf(STR("BP_jRPG_Character_World_C")); pawn) {
        if (auto p = actor_location(pawn)) {
            std::stable_sort(maps.begin(), maps.end(), [&](UObject* a, UObject* b) {
                auto la = actor_location(a), lb = actor_location(b);
                auto d2 = [&](const std::optional<std::array<double, 3>>& l) {
                    if (!l) return 1e300;
                    const double dx = (*l)[0] - (*p)[0], dy = (*l)[1] - (*p)[1], dz = (*l)[2] - (*p)[2];
                    return dx * dx + dy * dy + dz * dz;
                };
                return d2(la) < d2(lb);
            });
            if (auto lm = actor_location(maps.front())) {
                const double dx = (*lm)[0] - (*p)[0], dy = (*lm)[1] - (*p)[1], dz = (*lm)[2] - (*p)[2];
                best_d2 = dx * dx + dy * dy + dz * dz;
            }
        }
    }

    static std::atomic<int> seq{0};
    const std::wstring obj_name = L"E33Coop_Trigger_" + std::to_wstring(++seq);
    FStaticConstructObjectParameters params{cls, executor};
    params.Name = FName(obj_name.c_str(), FNAME_Add);
    UObject* action = UObjectGlobals::StaticConstructObject(params);
    if (!action) { err = "StaticConstructObject failed"; return false; }

    auto* sp = CastField<FStructProperty>(ue::prop(action, STR("Battle Start Params")));
    if (!sp) { err = "Battle Start Params not found"; return false; }
    UStruct* st = sp->GetStruct();
    void* base = sp->ContainerPtrToValuePtr<void>(action);

    auto* row_prop = CastField<FStructProperty>(ue::struct_member(st, STR("EncounterRow")));
    if (!row_prop) { err = "EncounterRow not found"; return false; }
    void* row = row_prop->ContainerPtrToValuePtr<void>(base);
    bool ok = set_member_object(row_prop->GetStruct(), row, STR("DataTable"), dt) &&
              set_member_fname(row_prop->GetStruct(), row, STR("RowName"), encounter.c_str()) &&
              set_member_fname(st, base, STR("EnemyGlobalID"), STR("E33CoopTest")) &&
              set_member_byte(st, base, STR("EngagementType"), 0) &&
              set_member_object(st, base, STR("BattleMapBP"), maps.front()) &&
              set_member_byte(st, base, STR("TransitionType"), 0) &&
              set_member_bool(st, base, STR("PlayCameraIntro"), false) &&
              set_member_bool(st, base, STR("BackToExplorationOnLost"), true);
    if (!ok) { err = "could not fill Battle Start Params"; return false; }
    if (!party.empty() && !set_party(st, base, party)) { err = "could not fill OverrideParty"; return false; }

    ue::Call exec(executor, STR("ExecuteGameAction"));
    if (!exec) { err = "ExecuteGameAction not found"; return false; }
    exec.set_object(STR("GameAction"), action).invoke();
    g_last_encounter = encounter;
    g_last_party = party;
    std::string party_s;
    for (auto& p : party) party_s += (party_s.empty() ? "" : ",") + ue::to_utf8(p);
    log().write("battle_trigger", JsonObj().add("encounter", ue::to_utf8(encounter)).add("party", party_s).add("map", ue::short_name(maps.front()))
                                     .add("maps", static_cast<int64_t>(maps.size())).add("distance", best_d2 >= 0 ? std::sqrt(best_d2) : -1.0));
    return true;
}

const std::wstring& last_encounter() { return g_last_encounter; }
const std::vector<std::wstring>& last_party() { return g_last_party; }

namespace { int g_mod_calls = 0; }
ModCall::ModCall() { ++g_mod_calls; }
ModCall::~ModCall() { --g_mod_calls; }
int mod_call_depth() { return g_mod_calls; }

bool perform_action(int action_type, const std::wstring& secondary, const std::vector<UObject*>& targets, int combo_auto_success, std::string& err)
{
    ModCall mod;
    UObject* bm = battle_manager();
    if (!bm) { err = "no battle manager"; return false; }
    // Passer le tour / fuir : chemins de l'interface (pas de RequestActionExecution).
    if (action_type == ActSkip || action_type == ActFlee) {
        ue::Call c(bm, action_type == ActSkip ? STR("OnSelectSkip") : STR("OnSelectFlee"));
        if (!c) { err = "OnSelectSkip/OnSelectFlee not found"; return false; }
        c.invoke();
        return true;
    }
    if (combo_auto_success >= 0) ue::set_int(bm, STR("DEBUG_ComboAutoSuccessCount"), combo_auto_success);
    ue::Call sel(bm, STR("SelectAction"));
    if (!sel) { err = "SelectAction not found"; return false; }
    sel.set_int(STR("ActionType"), action_type).invoke();
    if (!secondary.empty()) {
        ue::Call sec(bm, STR("SelectSecondaryAction"));
        if (!sec) { err = "SelectSecondaryAction not found"; return false; }
        sec.set_fname(STR("SelectedSecondaryAction"), secondary.c_str()).invoke();
    }
    ue::Call req(bm, STR("RequestActionExecution"));
    if (!req) { err = "RequestActionExecution not found"; return false; }
    req.set_object_array(STR("TargetedCharacters"), targets).invoke();
    return true;
}

bool force_battle_end(int result, bool direct_flow, std::string& how)
{
    ModCall mod;
    const wchar_t* name = direct_flow ? STR("StartBattleEndFlow") : STR("ForceBattleEnd");
    how = direct_flow ? "StartBattleEndFlow" : "ForceBattleEnd";
    ue::Call c(battle_manager(), name);
    if (!c) { how += ": not found"; return false; }
    // Nom du paramètre connu pour StartBattleEndFlow ; ForceBattleEnd : seul paramètre (EBattleEndResult), nom pas dumpé.
    if (c.param_size(STR("BattleEndResult")) > 0) {
        c.set_int(STR("BattleEndResult"), result);
        how += "(BattleEndResult)";
    } else {
        const std::string p = c.set_first_int(result);
        if (p.empty()) { how += ": no parameter"; return false; }
        how += "(" + p + ")";
    }
    return c.invoke();
}

bool kill_unit(UObject* ch)
{
    ModCall mod;
    UObject* st = stats(ch);
    if (!st) return false;
    ue::set_double(st, STR("CurrentHP"), 0);
    ue::Call c(st, STR("SetDead"));
    return c && c.invoke();
}

bool skill_info(UObject* hero, const std::wstring& skill, int& targeting, int& ap_cost)
{
    ue::Call c(stats(hero), STR("FindEquippedSkillByID"));
    if (!c) return false;
    c.set_fname(STR("SkillID"), skill.c_str()).invoke();
    UObject* asset = c.out_object(STR("Skill"));
    if (!asset) return false;
    targeting = static_cast<int>(ue::get_int(asset, STR("TargetingType"), -1));
    ap_cost = static_cast<int>(ue::get_int(asset, STR("APCost"), 0));
    return true;
}

int battle_item_count(const std::wstring& item)
{
    ue::Call c(battle_manager(), STR("FindBattleItemCount"));
    if (!c) return -1;
    c.set_fname(STR("ItemHardcodedName"), item.c_str()).invoke();
    return static_cast<int>(c.out_int(STR("Quantity"), -1));
}

std::vector<UObject*> auto_targets(UObject* actor, int action_type, const std::wstring& secondary)
{
    std::vector<UObject*> alive_e, alive_h, dead_h;
    for (UObject* e : by_spot(enemies())) if (!is_dead_fast(e)) alive_e.push_back(e);
    for (UObject* h : by_spot(players())) (is_dead_fast(h) ? dead_h : alive_h).push_back(h);
    auto first_enemy = [&]() { return alive_e.empty() ? std::vector<UObject*>{} : std::vector<UObject*>{alive_e.front()}; };
    auto weakest_hero = [&]() {
        UObject* best = nullptr;
        double br = 1e300;
        for (UObject* h : alive_h) { const double r = hp_ratio(h); if (r < br) { br = r; best = h; } }
        return best ? std::vector<UObject*>{best} : std::vector<UObject*>{};
    };
    if (action_type == ActItem) {
        if (secondary.find(L"Revive") != std::wstring::npos) return dead_h.empty() ? std::vector<UObject*>{} : std::vector<UObject*>{dead_h.front()};
        if (secondary.find(L"Energy") != std::wstring::npos) return actor ? std::vector<UObject*>{actor} : std::vector<UObject*>{};
        return weakest_hero();
    }
    if (action_type == ActSkill) {
        int targeting = -1, cost = 0;
        skill_info(actor, secondary, targeting, cost);
        switch (targeting) {
        case 0: return actor ? std::vector<UObject*>{actor} : std::vector<UObject*>{};
        case 10: return alive_e;
        case 4: return weakest_hero();
        default: return first_enemy();
        }
    }
    return first_enemy();
}

std::string battle_inventory_json()
{
    std::string heroes = "[";
    for (UObject* h : by_spot(players())) {
        ue::Call c(battle_manager(), STR("GetEquippedCharacterSkills"));
        std::string skills = "[";
        if (c) {
            c.set_object(STR("Character"), h).invoke();
            for (UObject* sk : c.out_object_array(STR("EquippedSkills"))) {
                if (skills.size() > 1) skills += ",";
                skills += JsonObj().add("id", ue::get_name_string(sk, STR("NameID"))).add("ap", ue::get_int(sk, STR("APCost")))
                              .add("targeting", ue::get_int(sk, STR("TargetingType"), -1)).str();
            }
        }
        if (heroes.size() > 1) heroes += ",";
        heroes += JsonObj().add("name", hardcoded_name(h)).add("spot", spot(h)).add("class", char_class(h)).raw("skills", skills + "]").str();
    }
    std::string items = "{";
    for (const wchar_t* it : {L"Consumable_Health_Level2", L"Consumable_Energy_Level2", L"Consumable_Revive_Level2"}) {
        if (items.size() > 1) items += ",";
        items += "\"" + ue::to_utf8(it) + "\":" + std::to_string(battle_item_count(it));
    }
    return JsonObj().raw("heroes", heroes + "]").raw("items", items + "}").str();
}

bool start_parry(UObject* character)
{
    ue::Call c(character, STR("TryStartParry"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("Success"));
}

bool start_jump(UObject* character)
{
    if (ue::get_bool(character, STR("IsJumping"))) return true;
    ue::Call c(character, STR("TryStartJump"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("Success"));
}

bool start_dodge(UObject* character)
{
    ue::Call c(character, STR("TryStartDodge"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("Success"));
}

bool start_gradient_parry(UObject* character)
{
    ue::Call c(character, STR("TryStartGradientParry"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("Success"));
}

void clear_defense_lock(UObject* character)
{
    ue::Call c(character, STR("ClearDefensiveMoveLock"));
    if (c) c.set_string(STR("Reason"), STR("E33Coop")).invoke();
}

bool in_immunity(UObject* character)
{
    ue::Call c(character, STR("IsInImmunityPhase"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("ReturnValue"));
}

double parry_immune_duration(UObject* character)
{
    ue::Call c(character, STR("GetParryImmuneDuration"));
    if (!c || !c.invoke()) return 0.15;
    return c.out_double(STR("ReturnValue"), 0.15);
}

void cancel_defense(UObject* character)
{
    const double start = ue::get_double(character, STR("DefensiveMoveImmunityStartTime"));
    ue::set_double(character, STR("DefensiveMoveImmunityEndTime"), start - 1.0);
}

UObject* damage_source_character(UObject* damage_builder)
{
    if (!damage_builder) return nullptr;
    ue::Call src(damage_builder, STR("GetDamageSource"));
    if (!src || !src.invoke()) return nullptr;
    UObject* source = src.out_object(STR("DamageSource"));   // TScriptInterface : l'objet est le premier membre
    ue::Call chr(source, STR("GetDamageSourceCharacter"));
    if (!chr || !chr.invoke()) return nullptr;
    return chr.out_object(STR("CharacterStats"));
}

bool incoming_attack_jumpable(UObject* damage_builder)
{
    return ue::get_bool(damage_source_character(damage_builder), STR("IsCurrentAttackJumpable"));
}

std::vector<Buff> buffs(UObject* st)
{
    std::vector<Buff> out;
    UObject* comp = ue::get_object(st, STR("BattleBuffComponent"));
    for (UObject* w : ue::get_object_array(comp, STR("ActiveBattleBuffs"))) {
        UObject* inst = ue::get_object(w, STR("BattleBuffInstance"));
        if (!inst) continue;
        Buff b;
        b.wrapper = w;
        b.instance = inst;
        b.cls = char_class(inst);
        b.stacks = static_cast<int>(ue::get_int(inst, STR("StackCount")));
        b.turns = static_cast<int>(ue::get_int(inst, STR("TurnDuration")));
        b.permanent = ue::get_bool(inst, STR("IsPermanent"));
        b.type = static_cast<int>(ue::get_int(inst, STR("Type")));
        out.push_back(std::move(b));
    }
    return out;
}

bool is_helper_buff(const Buff& b) { return b.type == 3 || b.cls.find("Aggregator") != std::string::npos; }

bool remove_buff(UObject* st, UObject* instance)
{
    ue::Call c(ue::get_object(st, STR("BattleBuffComponent")), STR("RemoveBuffInstance"));
    if (!c) return false;
    c.set_object(STR("BuffInstance"), instance).invoke();
    return true;
}

std::map<std::string, std::string> g_class_paths;   // classe de buff -> chemin complet (envoyé par l'hôte)

void note_class_path(const std::string& cls, const std::string& path) { if (g_class_paths.size() < 500) g_class_paths[cls] = path; }

// Chargement d'une classe par son chemin (picto que ce joueur ne possède pas : jamais chargée ici, 2026-10-24) :
// KismetSystemLibrary MakeSoftClassPath -> Conv_SoftClassPathToSoftClassRef -> LoadClassAsset_Blocking.
UClass* load_class_path(const std::string& path)
{
    UObject* ksl = ue::find_object(STR("/Script/Engine.Default__KismetSystemLibrary"));
    ue::Call mk(ksl, STR("MakeSoftClassPath"));
    ue::Call cv(ksl, STR("Conv_SoftClassPathToSoftClassRef"));
    ue::Call ld(ksl, STR("LoadClassAsset_Blocking"));
    if (!mk || !cv || !ld) return nullptr;
    const std::wstring wp = ue::from_utf8(path);
    mk.set_string(STR("PathString"), wp.c_str());
    if (!mk.invoke() || mk.param_size(STR("ReturnValue")) != cv.param_size(STR("SoftClassPath"))) return nullptr;
    cv.set_raw(STR("SoftClassPath"), mk.param_ptr(STR("ReturnValue")));
    if (!cv.invoke() || cv.param_size(STR("ReturnValue")) != ld.param_size(STR("AssetClass"))) return nullptr;
    ld.set_raw(STR("AssetClass"), cv.param_ptr(STR("ReturnValue")));
    if (!ld.invoke()) return nullptr;
    return static_cast<UClass*>(ld.out_object(STR("ReturnValue")));
}

bool add_buff(UObject* st, const std::string& cls_name, int stacks, int turns, bool permanent, std::string& err)
{
    UClass* cls = ue::find_class_by_short_name(cls_name);
    if (!cls) if (auto it = g_class_paths.find(cls_name); it != g_class_paths.end()) {
        cls = load_class_path(it->second);
        log().write("coop_buff_class_loaded", JsonObj().add("class", cls_name).add("path", it->second).add("ok", cls != nullptr));
    }
    if (!cls) { err = "buff class not loaded: " + cls_name; return false; }
    FStaticConstructObjectParameters params{cls, st};
    UObject* inst = UObjectGlobals::StaticConstructObject(params);
    if (!inst) { err = "StaticConstructObject failed"; return false; }
    ue::Call act(st, STR("ActivateBuff"));
    if (!act) { err = "ActivateBuff not found"; return false; }
    act.set_object(STR("BuffInstance"), inst).set_int(STR("StatusEffectProbability"), 4).set_object(STR("SourceCharacter"), st).invoke();
    if (!act.out_bool(STR("Success"))) { err = "ActivateBuff refused"; return false; }
    ue::set_int(inst, STR("TurnDuration"), turns);
    ue::set_int(inst, STR("StackCount"), stacks);
    if (permanent) { ue::Call p(inst, STR("SetPermanent")); if (p) p.invoke(); }
    return true;
}

UObject* gradient_gauge() { return ue::get_object(battle_manager(), STR("GradientGaugeSystem")); }

namespace {

// Parcourt les champs entiers (bool, octet, énumération, int) et les tableaux de ces types des composants de
// mécanique propre d'un héros (BP_*Mechanic*Component, BP_StainHolderComponent). Champs propres à la classe BP
// seulement (pas ceux d'UActorComponent). visit(nom, adresse, taille, est_bool, bool_prop, conteneur).
template <class Visit>
void for_each_mech_field(UObject* hero, Visit visit)
{
    for (UObject* c : ue::get_object_array(hero, STR("BlueprintCreatedComponents"))) {
        if (!c) continue;
        const std::string cls = char_class(c);
        if (cls.find("Mechanic") == std::string::npos && cls.find("StainHolder") == std::string::npos) continue;
        for (FProperty* p : TFieldRange<FProperty>(c->GetClassPrivate(), EFieldIterationFlags::IncludeDeprecated)) {
            const std::string name = cls + "." + ue::to_utf8(p->GetName());
            if (auto* bp = CastField<FBoolProperty>(p)) { visit(name, nullptr, 0, bp, c); continue; }
            if (CastField<FIntProperty>(p) || CastField<FByteProperty>(p) || CastField<FEnumProperty>(p)) {
                visit(name, p->ContainerPtrToValuePtr<void>(c), p->GetSize(), nullptr, c);
                continue;
            }
            if (auto* ap = CastField<FArrayProperty>(p)) {
                FProperty* inner = ap->GetInner();
                if (!(CastField<FIntProperty>(inner) || CastField<FByteProperty>(inner) || CastField<FEnumProperty>(inner))) continue;
                auto* arr = static_cast<FScriptArray*>(ap->ContainerPtrToValuePtr<void>(c));
                int32 n = arr->Num();
                visit(name + "#", &n, -4, nullptr, c);   // taille du tableau (lecture seule)
                auto* data = static_cast<uint8_t*>(arr->GetData());
                for (int32 i = 0; i < n; ++i) visit(name + "[" + std::to_string(i) + "]", data + static_cast<size_t>(i) * inner->GetSize(), inner->GetSize(), nullptr, c);
            }
        }
    }
}

int64_t read_int(const void* p, int size)
{
    switch (size) {
    case 1: return *static_cast<const uint8_t*>(p);
    case 2: return *static_cast<const int16_t*>(p);
    case 4: case -4: return *static_cast<const int32_t*>(p);
    case 8: return *static_cast<const int64_t*>(p);
    default: return 0;
    }
}

} // namespace

std::vector<std::pair<std::string, int64_t>> mechanic_fields(UObject* hero)
{
    std::vector<std::pair<std::string, int64_t>> out;
    for_each_mech_field(hero, [&](const std::string& name, void* addr, int size, FBoolProperty* bp, UObject* comp) {
        out.emplace_back(name, bp ? (bp->GetPropertyValueInContainer(comp) ? 1 : 0) : read_int(addr, size));
    });
    return out;
}

int set_mechanic_fields(UObject* hero, const std::vector<std::pair<std::string, int64_t>>& wanted, int& failed)
{
    int fixed = 0;
    for_each_mech_field(hero, [&](const std::string& name, void* addr, int size, FBoolProperty* bp, UObject* comp) {
        auto it = std::find_if(wanted.begin(), wanted.end(), [&](const auto& w) { return w.first == name; });
        if (it == wanted.end()) return;
        const int64_t cur = bp ? (bp->GetPropertyValueInContainer(comp) ? 1 : 0) : read_int(addr, size);
        if (cur == it->second) return;
        if (bp) { bp->SetPropertyValueInContainer(comp, it->second != 0); ++fixed; return; }
        switch (size) {
        case 1: *static_cast<uint8_t*>(addr) = static_cast<uint8_t>(it->second); ++fixed; break;
        case 2: *static_cast<int16_t*>(addr) = static_cast<int16_t>(it->second); ++fixed; break;
        case 4: *static_cast<int32_t*>(addr) = static_cast<int32_t>(it->second); ++fixed; break;
        case 8: *static_cast<int64_t*>(addr) = it->second; ++fixed; break;
        default: ++failed; break;   // taille de tableau différente : non réparable champ par champ
        }
    });
    return fixed;
}

bool location(UObject* actor, double out[3])
{
    auto l = actor_location(actor);
    if (!l) return false;
    out[0] = (*l)[0]; out[1] = (*l)[1]; out[2] = (*l)[2];
    return true;
}

// --- Exploration et session (phase 4) ---------------------------------------------------------

std::wstring saves_dir()
{
    const wchar_t* lad = _wgetenv(L"LOCALAPPDATA");
    return std::wstring(lad ? lad : L"") + L"\\Sandfall\\Saved\\SaveGames";
}

bool make_snapshot(const std::wstring& slot, std::vector<uint8_t>& bytes, std::string& err)
{
    UObject* sm = save_manager();
    if (!sm) { err = "no save manager"; return false; }
    ue::Call create(sm, STR("CreateSaveGameDataFromGameState"));
    if (!create || !create.invoke()) { err = "CreateSaveGameDataFromGameState failed"; return false; }
    UObject* data = create.out_object(STR("SaveGameData"));
    if (!data) { err = "no save game data"; return false; }
    auto* cls = static_cast<UClass*>(load_asset(STR("/Game/Gameplay/Save/SaveObjects/BP_SaveGameObject_V8.BP_SaveGameObject_V8_C")));
    if (!cls) { err = "BP_SaveGameObject_V8_C not loaded"; return false; }
    static std::atomic<int> seq{0};
    const std::wstring name = L"E33Coop_Snapshot_" + std::to_wstring(++seq);
    FStaticConstructObjectParameters params{cls, gi()};
    params.Name = FName(name.c_str(), FNAME_Add);
    UObject* obj = UObjectGlobals::StaticConstructObject(params);
    if (!obj) { err = "StaticConstructObject failed"; return false; }
    ue::Call conv(obj, STR("ConvertFromSaveGameData"));
    if (!conv) { err = "ConvertFromSaveGameData not found"; return false; }
    conv.set_object(STR("SaveGameData"), data).invoke();
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    ue::Call save(gps, STR("SaveGameToSlot"));
    if (!save) { err = "SaveGameToSlot not found"; return false; }
    save.set_object(STR("SaveGameObject"), obj).set_string(STR("SlotName"), slot.c_str()).set_int(STR("UserIndex"), 0).invoke();
    if (!save.out_bool(STR("ReturnValue"))) { err = "SaveGameToSlot returned false"; return false; }
    std::wstring rel = slot;
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    const std::filesystem::path file = std::filesystem::path(saves_dir()) / (rel + L".sav");
    std::ifstream f(file, std::ios::binary);
    if (!f) { err = "cannot read " + ue::to_utf8(file.wstring()); return false; }
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (bytes.size() < 4) { err = "snapshot too small"; return false; }
    return true;
}

UObject* explore_pawn()
{
    UObject* ctrl = controller();
    if (!ctrl) return nullptr;
    ue::Call c(ctrl, STR("K2_GetPawn"));
    if (!c || !c.invoke()) return nullptr;
    UObject* p = c.out_object(STR("ReturnValue"));
    return p && ue::is_a(p, STR("BP_jRPG_Character_World_C")) ? p : nullptr;
}

std::string current_level()
{
    UObject* p = explore_pawn();
    if (!p) return {};
    // "/Game/Levels/WorldMap/Camps/Level_Camp_Main.Level_Camp_Main:PersistentLevel.BP_..." -> "Level_Camp_Main"
    const std::string full = ue::full_name(p);
    const auto colon = full.find(":PersistentLevel");
    if (colon == std::string::npos) return {};
    const auto dot = full.rfind('.', colon);
    return dot == std::string::npos ? std::string() : full.substr(dot + 1, colon - dot - 1);
}

bool pawn_state(UObject* pawn, PawnState& out)
{
    auto l = actor_location(pawn);
    if (!l) return false;
    out.pos[0] = (*l)[0]; out.pos[1] = (*l)[1]; out.pos[2] = (*l)[2];
    ue::Call r(pawn, STR("K2_GetActorRotation"));
    if (r && r.invoke())
        if (const auto* rot = static_cast<const double*>(r.param_ptr(STR("ReturnValue")))) out.yaw = rot[1];   // FRotator : Pitch, Yaw, Roll
    ue::Call v(pawn, STR("GetVelocity"));
    if (v && v.invoke())
        if (const auto* vel = static_cast<const double*>(v.param_ptr(STR("ReturnValue")))) { out.vel[0] = vel[0]; out.vel[1] = vel[1]; out.vel[2] = vel[2]; }
    return true;
}

bool move_pawn(UObject* pawn, double dir_x, double dir_y, double scale)
{
    ue::Call c(pawn, STR("AddMovementInput"));
    if (!c) return false;
    if (auto* d = static_cast<double*>(c.param_ptr(STR("WorldDirection")))) { d[0] = dir_x; d[1] = dir_y; d[2] = 0; }
    if (auto* s = static_cast<float*>(c.param_ptr(STR("ScaleValue")))) *s = static_cast<float>(scale);
    c.set_bool(STR("bForce"), true);
    return c.invoke();
}

bool teleport_pawn(UObject* pawn, const double pos[3], double yaw)
{
    ue::Call c(pawn, STR("K2_SetActorLocationAndRotation"));
    if (!c) return false;
    if (auto* d = static_cast<double*>(c.param_ptr(STR("NewLocation")))) { d[0] = pos[0]; d[1] = pos[1]; d[2] = pos[2]; }
    if (auto* r = static_cast<double*>(c.param_ptr(STR("NewRotation")))) { r[0] = 0; r[1] = yaw; r[2] = 0; }
    c.set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true);
    return c.invoke();
}

int pawn_gait(UObject* pawn, bool desired)
{
    ue::Call c(pawn, desired ? STR("GetDesiredGait") : STR("GetGait"));
    if (!c || !c.invoke()) return -1;
    return static_cast<int>(c.out_int(STR("ReturnValue"), -1));
}

bool pawn_sprint_action(UObject* pawn, bool on)
{
    ue::Call c(pawn, STR("SprintAction"));
    if (!c) return false;
    c.set_bool(STR("bValue"), on);
    return c.invoke();
}

bool set_pawn_desired_gait(UObject* pawn, int gait)
{
    ue::Call c(pawn, STR("SetDesiredGait"));
    if (!c) return false;
    c.set_int(STR("NewGait"), gait);
    return c.invoke();
}

bool pawn_gait_speeds(UObject* pawn, GaitSpeeds& out)
{
    // UALSCharacterMovementComponent::CurrentMovementSettings (FALSMovementSettings : WalkSpeed, RunSpeed, SprintSpeed
    // en tête, dump ALSV4_CPP).
    UObject* cmc = pawn ? ue::get_object(pawn, STR("CharacterMovement")) : nullptr;
    const auto* st = ue::value_ptr<float>(cmc, STR("CurrentMovementSettings"));
    if (!st) return false;
    out.walk = st[0]; out.run = st[1]; out.sprint = st[2];
    return out.run > 0 && out.sprint >= out.run && out.sprint < 5000;
}

float* pawn_max_walk_speed(UObject* pawn)
{
    UObject* cmc = pawn ? ue::get_object(pawn, STR("CharacterMovement")) : nullptr;
    return ue::value_ptr<float>(cmc, STR("MaxWalkSpeed"));
}

bool set_pawn_sprint_speed(UObject* pawn, float speed)
{
    // Vitesse de sprint des réglages ALS du pion (MaxWalkSpeed écrite directement est ignorée : mesuré le 2026-10-23,
    // le pion restait à 720 avec MaxWalkSpeed à 365 ou 864). bRequestMovementSettingsChange : l'ALS recalcule
    // MaxWalkSpeed depuis ces réglages à la fin du mouvement de l'image.
    UObject* cmc = pawn ? ue::get_object(pawn, STR("CharacterMovement")) : nullptr;
    auto* st = ue::value_ptr<float>(cmc, STR("CurrentMovementSettings"));
    if (!st) return false;
    st[2] = speed;
    if (!ue::set_bool(cmc, STR("bRequestMovementSettingsChange"), true)) ue::set_int(cmc, STR("bRequestMovementSettingsChange"), 1);
    return true;
}

bool move_info(UObject* pawn, MoveInfo& out)
{
    if (!pawn) return false;
    out.cls = ue::short_name(pawn->GetClassPrivate());
    // ACharacter::CharacterMovement -> MovementMode (TEnumAsByte<EMovementMode>), CustomMovementMode (Engine, dump)
    if (UObject* cmc = ue::get_object(pawn, STR("CharacterMovement"))) {
        out.move = static_cast<int>(ue::get_int(cmc, STR("MovementMode"), -1));
        out.custom = static_cast<int>(ue::get_int(cmc, STR("CustomMovementMode"), 0));
    }
    out.world_map = ue::get_bool(pawn, STR("bIsWorldMapPawn"));   // BP_jRPG_Character_World_C (dump)
    out.mount = ue::get_bool(gi(), STR("IsMountInUse"));           // BP_jRPG_GI_Custom_C, sauvegardé aussi (dump)
    ue::Call parent(pawn, STR("GetAttachParentActor"));
    out.attached = parent && parent.invoke() && parent.out_object(STR("ReturnValue")) != nullptr;
    return true;
}

bool set_movement_mode(UObject* pawn, int mode)
{
    UObject* cmc = ue::get_object(pawn, STR("CharacterMovement"));
    ue::Call c(cmc, STR("SetMovementMode"));
    if (!c) return false;
    c.set_int(STR("NewMovementMode"), mode).set_int(STR("NewCustomMode"), 0);
    return c.invoke();
}

bool set_pawn_visible(UObject* pawn, bool visible)
{
    ue::Call h(pawn, STR("SetActorHiddenInGame"));
    ue::Call col(pawn, STR("SetActorEnableCollision"));
    if (!h || !col) return false;
    h.set_bool(STR("bNewHidden"), !visible).invoke();
    col.set_bool(STR("bNewActorEnableCollision"), visible).invoke();
    return true;
}

std::vector<std::string> function_names(UObject* o, const std::vector<std::string>& needles, size_t max)
{
    std::vector<std::string> out;
    if (!o || !o->GetClassPrivate()) return out;
    auto lower = [](std::string s) { for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; };
    for (UFunction* f : TFieldRange<UFunction>(o->GetClassPrivate(), EFieldIterationFlags::IncludeSuper)) {
        if (!f) continue;
        const std::string n = ue::to_utf8(f->GetName());
        const std::string ln = lower(n);
        for (const auto& k : needles)
            if (ln.find(k) != std::string::npos) { out.push_back(n); break; }
        if (out.size() >= max) break;
    }
    return out;
}

bool map_destination(std::string& level, std::string& spawn_tag)
{
    UObject* g = gi();
    if (!g) return false;
    level = ue::get_name_string(g, STR("LevelDestinationAssetName"));
    auto* tag = ue::value_ptr<FName>(g, STR("SpawnPointTag"));   // FGameplayTag : TagName en tête
    spawn_tag = tag ? ue::to_utf8(tag->ToString()) : std::string();
    return !level.empty() && level != "None";
}

bool change_map(const std::wstring& level, const std::wstring& spawn_tag)
{
    UObject* lib = ue::find_object(STR("/Game/jRPGTemplate/Blueprints/Basics/FL_jRPG_CustomFunctionLibrary.Default__FL_jRPG_CustomFunctionLibrary_C"));
    UObject* ctx = explore_pawn();
    if (!ctx) ctx = controller();
    ue::Call c(lib, STR("ChangeMapByAssetName"));
    if (!c || !ctx) return false;
    c.set_fname(STR("LevelAssetName"), level.c_str())
        .set_fname(STR("SpawnPointTag"), spawn_tag.empty() ? STR("None") : spawn_tag.c_str())
        .set_object(STR("__WorldContext"), ctx);
    return c.invoke();
}

std::vector<UObject*> actor_instances(const std::wstring& cls)
{
    std::vector<UObject*> all, out;
    UObjectGlobals::FindAllOf(cls.c_str(), all);
    for (UObject* o : all)
        if (o && ue::to_utf8(o->GetName()).rfind("Default__", 0) != 0) out.push_back(o);   // objet par défaut de la classe
    return out;
}

std::vector<TeleportPoint> teleport_points()
{
    std::vector<TeleportPoint> out;
    for (UObject* o : actor_instances(STR("BP_jRPG_MapTeleportPoint_C"))) {
        TeleportPoint t;
        t.actor = o;
        t.name = ue::short_name(o);
        // LevelDestination : FDataTableRowHandle { DataTable, RowName }
        if (auto* sp = CastField<FStructProperty>(ue::prop(o, STR("LevelDestination")))) {
            void* base = sp->ContainerPtrToValuePtr<void>(o);
            if (FProperty* rn = ue::struct_member(sp->GetStruct(), L"RowName"))
                t.row = ue::to_utf8(static_cast<FName*>(rn->ContainerPtrToValuePtr<void>(base))->ToString());
        }
        if (auto* tag = ue::value_ptr<FName>(o, STR("DestinationSpawnPointTag"))) t.spawn = ue::to_utf8(tag->ToString());
        t.world_map = ue::get_bool(o, STR("IsInWorldMapLevel"));
        location(o, t.pos);
        for (int i = 0; i < 3; ++i) t.box[i] = t.pos[i];
        if (UObject* box = ue::get_object(o, STR("TeleportBox"))) {
            ue::Call c(box, STR("K2_GetComponentLocation"));
            if (c && c.invoke())
                if (const auto* v = static_cast<const double*>(c.param_ptr(STR("ReturnValue")))) { t.box[0] = v[0]; t.box[1] = v[1]; t.box[2] = v[2]; }
        }
        out.push_back(std::move(t));
    }
    return out;
}

int set_teleport_points_active(bool on)
{
    int n = 0;
    for (UObject* o : actor_instances(STR("BP_jRPG_MapTeleportPoint_C"))) {
        UObject* box = ue::get_object(o, STR("TeleportBox"));
        ue::Call c(box, STR("SetGenerateOverlapEvents"));
        if (!c) continue;
        c.set_bool(STR("bInGenerateOverlapEvents"), on);
        if (c.invoke()) ++n;
    }
    return n;
}

std::vector<std::string> loaded_bp_classes(const std::vector<std::string>& needles)
{
    // FindAllOf("BlueprintGeneratedClass") ne rend rien : parcours de tous les objets (lent, diagnostic seulement).
    auto lower = [](std::string s) { for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; };
    std::vector<std::string> out;
    UObjectGlobals::ForEachUObject([&](UObject* c, int32, int32) {
        if (c && c->GetClassPrivate() && c->GetClassPrivate()->GetName() == STR("BlueprintGeneratedClass")) {
            const std::string n = ue::to_utf8(c->GetName());
            const std::string l = lower(n);
            for (const auto& k : needles) if (l.find(k) != std::string::npos) { out.push_back(n); break; }
        }
        return LoopAction::Continue;
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool interact(UObject* target, bool instant)
{
    UObject* pawn = explore_pawn();
    ue::Call c(target, STR("OnInteractionRequested"));
    if (!c || !pawn) return false;
    c.set_object(STR("InteractingWorldCharacter"), pawn).set_bool(STR("Instant"), instant);
    return c.invoke();
}

UObject* dialogue_system() { return ue::get_object(controller(), STR("BP_DialogueSystemComponent")); }

bool in_dialogue()
{
    ue::Call c(dialogue_system(), STR("IsInDialogue"));
    if (!c || !c.invoke()) return false;
    return c.out_bool(STR("ReturnValue"));
}

int dialogue_choices()
{
    auto* a = ue::value_ptr<FScriptArray>(dialogue_system(), STR("CurrentChoices"));
    return a ? a->Num() : 0;
}

bool choose_dialogue(int index)
{
    ue::Call c(dialogue_system(), STR("OnChoiceMade"));
    if (!c) return false;
    c.set_int(STR("ChoiceIndex"), index);
    return c.invoke();
}

bool playing_cinematic() { return ue::get_bool(ue::get_object(controller(), STR("BP_CinematicSystem")), STR("IsPlayingCinematic")); }

bool battle_params(std::string& encounter, std::string& enemy_global_id, int& engage)
{
    UObject* bm = battle_manager();
    auto* sp = CastField<FStructProperty>(ue::prop(bm, STR("CurrentBattleStartParams")));
    if (!sp) return false;
    UStruct* st = sp->GetStruct();
    void* base = sp->ContainerPtrToValuePtr<void>(bm);
    if (auto* row = CastField<FStructProperty>(ue::struct_member(st, STR("EncounterRow")))) {
        void* rb = row->ContainerPtrToValuePtr<void>(base);
        if (FProperty* rn = ue::struct_member(row->GetStruct(), STR("RowName")))
            encounter = ue::to_utf8(static_cast<FName*>(rn->ContainerPtrToValuePtr<void>(rb))->ToString());
    }
    if (FProperty* id = ue::struct_member(st, STR("EnemyGlobalID")))
        enemy_global_id = ue::to_utf8(static_cast<FName*>(id->ContainerPtrToValuePtr<void>(base))->ToString());
    if (FProperty* e = ue::struct_member(st, STR("EngagementType"))) engage = *static_cast<uint8_t*>(e->ContainerPtrToValuePtr<void>(base));
    return !encounter.empty() && encounter != "None";
}

int passive_world_enemies()
{
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(STR("BP_jRPG_Enemy_World_Base_Seamless_C"), all);
    int n = 0;
    for (UObject* e : all) {
        if (!e || e->GetName().rfind(STR("Default__"), 0) == 0) continue;
        // Pas de combat au contact : la capsule de rencontre ne génère plus de chevauchements (le jeu peut la remettre :
        // appelé régulièrement). StartBattle appelé directement (par le mod) marche toujours.
        if (UObject* cap = ue::get_object(e, STR("EncounterCapsuleRange"))) {
            ue::Call c(cap, STR("SetGenerateOverlapEvents"));
            if (c) { c.set_bool(STR("bInGenerateOverlapEvents"), false).invoke(); ++n; }
        }
        // Pas de poursuite ni de détection du pion (verrou posé une seule fois : tant que la détection reste coupée).
        if (ue::get_bool(e, STR("EnableDetection"))) {
            ue::set_bool(e, STR("EnableDetection"), false);
            ue::Call lock(e, STR("AddAggroLock"));
            if (lock) lock.set_fname(STR("LockName"), STR("E33Coop")).invoke();
        }
    }
    return n;
}

UObject* find_world_enemy(const std::string& name)
{
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(STR("BP_jRPG_Enemy_World_Base_Seamless_C"), all);
    if (!name.empty()) {
        for (UObject* e : all) if (ue::short_name(e) == name) return e;
        return nullptr;
    }
    UObject* pawn = explore_pawn();
    auto p = pawn ? actor_location(pawn) : std::nullopt;
    if (!p) return nullptr;
    UObject* best = nullptr;
    double bd = 1e300;
    for (UObject* e : all) {
        auto l = actor_location(e);
        if (!l) continue;
        const double dx = (*l)[0] - (*p)[0], dy = (*l)[1] - (*p)[1];
        if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = e; }
    }
    return best;
}

bool start_world_battle(UObject* enemy, const std::wstring& encounter, int engage, std::string& err)
{
    if (!enemy) { err = "no world enemy"; return false; }
    if (!encounter.empty()) {
        auto* sp = CastField<FStructProperty>(ue::prop(enemy, STR("SelectedEncounter")));
        if (!sp) { err = "SelectedEncounter not found"; return false; }
        FProperty* rn = ue::struct_member(sp->GetStruct(), STR("RowName"));
        if (!rn) { err = "RowName not found"; return false; }
        *static_cast<FName*>(rn->ContainerPtrToValuePtr<void>(sp->ContainerPtrToValuePtr<void>(enemy))) = FName(encounter.c_str(), FNAME_Add);
    }
    ue::Call c(enemy, STR("StartBattle"));
    if (!c) { err = "StartBattle not found"; return false; }
    c.set_int(STR("EngagementType"), engage);
    return c.invoke();
}

bool set_game_paused(bool paused)
{
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    ue::Call c(gps, STR("SetGamePaused"));
    if (!c || !controller()) return false;
    c.set_object(STR("WorldContextObject"), controller()).set_bool(STR("bPaused"), paused);
    return c.invoke();
}

bool pause_menu_visible() { return ue::get_bool(controller(), STR("IsPauseMenuVisible")); }

UObject* defeat_screen()
{
    UObject* bm = battle_manager();
    return bm ? ue::get_object(bm, STR("GameOverScreenWidget")) : nullptr;
}

UObject* defeat_modal(UObject* screen, bool* visible)
{
    if (visible) *visible = false;
    if (!screen) return nullptr;
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(STR("WBP_ModalPopup_C"), all);
    for (UObject* m : all) {
        if (!m || ue::get_object(m, STR("ParentActivatableWidget")) != screen || !ue::get_bool(m, STR("bIsActive"))) continue;
        // ESlateVisibility : 1 réduite (Collapsed), 2 masquée (Hidden)
        const int64_t v = ue::get_int(m, STR("Visibility"), 0);
        if (visible) *visible = v != 1 && v != 2;
        return m;
    }
    return nullptr;
}

bool is_game_paused()
{
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    ue::Call c(gps, STR("IsGamePaused"));
    if (!c || !controller()) return false;
    c.set_object(STR("WorldContextObject"), controller());
    if (!c.invoke()) return false;
    return c.out_bool(STR("ReturnValue"));
}

bool back_to_title()
{
    ue::Call c(controller(), STR("BackToTitleMenu"));
    return c && c.invoke();
}

bool run_zone_action(const std::string& name_part, std::string& err)
{
    std::vector<UObject*> zones;
    UObjectGlobals::FindAllOf(STR("BP_GameActionTriggerZone_C"), zones);
    UObject* zone = nullptr;
    for (UObject* z : zones) if (ue::short_name(z).find(name_part) != std::string::npos) { zone = z; break; }
    if (!zone) { err = "zone not found: " + name_part; return false; }
    auto* wp = CastField<FStructProperty>(ue::prop(zone, STR("GameActionOnOverlap")));
    if (!wp) { err = "GameActionOnOverlap not found"; return false; }
    void* w = wp->ContainerPtrToValuePtr<void>(zone);
    UObject* action = nullptr;
    for (const wchar_t* m : {STR("GameActionReference"), STR("GameActionInline")})
        if (FProperty* p = ue::struct_member(wp->GetStruct(), m))
            if (!action) action = *static_cast<UObject**>(p->ContainerPtrToValuePtr<void>(w));
    if (!action) { err = "zone has no overlap action"; return false; }
    UObject* executor = ue::get_object(controller(), STR("BP_GameActionExecutorComponent"));
    ue::Call c(executor, STR("ExecuteGameAction"));
    if (!c) { err = "ExecuteGameAction not found"; return false; }
    c.set_object(STR("GameAction"), action).invoke();
    log().write("zone_action", JsonObj().add("zone", ue::short_name(zone)).add("action", ue::full_name(action)));
    return true;
}

bool skip_cinematic()
{
    // Comme le joueur : cinématique mise en pause (ses commandes s'affichent), puis « Passer ». Sans la pause,
    // SkipCinematic est ignoré (vu le 2026-10-23, scène de Sophie : 50 appels sans effet ; pause puis passage : fin en 1 s).
    UObject* cs = ue::get_object(controller(), STR("BP_CinematicSystem"));
    if (!ue::get_bool(cs, STR("CinematicPaused"))) { ue::Call p(cs, STR("PauseCinematic")); if (p) p.invoke(); }
    ue::Call c(cs, STR("SkipCinematic"));
    return c && c.invoke();
}

// --- Builds des personnages (phase 5, D13) ----------------------------------------------------

std::vector<std::string> character_names()
{
    std::vector<std::string> out;
    std::vector<std::pair<void*, void*>> pairs;
    if (!reflect::map_pairs(gi(), STR("CharactersCollection"), pairs, nullptr, nullptr)) return out;
    for (auto& [k, v] : pairs) out.push_back(ue::to_utf8(static_cast<FName*>(k)->ToString()));
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> party_names()
{
    std::vector<std::string> out;
    auto* ap = CastField<FArrayProperty>(ue::prop(gi(), STR("CurrentParty")));
    if (!ap) return out;
    auto* inner = CastField<FStructProperty>(ap->GetInner());
    auto* arr = ap->ContainerPtrToValuePtr<FScriptArray>(gi());
    if (!inner || !arr) return out;
    FProperty* name = ue::struct_member(inner->GetStruct(), STR("CharacterHardcodedName"));
    auto* data = static_cast<uint8_t*>(arr->GetData());
    for (int32 i = 0; i < arr->Num() && name; ++i)
        out.push_back(ue::to_utf8(static_cast<FName*>(name->ContainerPtrToValuePtr<void>(data + static_cast<size_t>(i) * inner->GetSize()))->ToString()));
    return out;
}

bool set_party_names(const std::vector<std::string>& names, std::string& err)
{
    UObject* mgr = ue::get_object(controller(), STR("AC_jRPG_CharactersManager"));
    if (!mgr) { err = "no characters manager"; return false; }
    ue::Call c(mgr, STR("SetParty"));
    if (!c) { err = "SetParty not found"; return false; }
    std::vector<std::wstring> wide;
    for (auto& n : names) wide.push_back(ue::from_utf8(n));
    c.set_fname_array(STR("InputPin"), wide).invoke();
    return true;
}

UObject* character_data(const std::string& wanted)
{
    std::vector<std::pair<void*, void*>> pairs;
    if (!reflect::map_pairs(gi(), STR("CharactersCollection"), pairs, nullptr, nullptr)) return nullptr;
    for (auto& [k, v] : pairs)
        if (ue::to_utf8(static_cast<FName*>(k)->ToString()) == wanted) return *static_cast<UObject**>(v);
    return nullptr;
}

bool character_build(const std::string& name, uint64_t& hash, std::string* summary)
{
    UObject* cd = character_data(name);
    ue::Call c(cd, STR("SaveCharacterState"));
    if (!c || !c.invoke()) return false;
    FProperty* p = c.param_property(STR("OutState"));
    void* v = c.param_ptr(STR("OutState"));
    if (!p || !v) return false;
    reflect::Leaves all, kept;
    reflect::flatten(p, v, "", all);
    // PV et PM changent hors des menus (dégâts d'exploration, soins) : ce n'est pas le build.
    for (auto& l : all)
        if (l.first.find("CurrentHP") == std::string::npos && l.first.find("CurrentMP") == std::string::npos) kept.push_back(l);
    hash = reflect::hash(kept);
    if (summary) *summary = std::to_string(kept.size()) + " leaves";
    // Les sorties sont des copies (tableaux, maps) : libérées ici, sinon fuite à chaque contrôle.
    p->DestroyValue(v);
    if (FProperty* p2 = c.param_property(STR("StructOut"))) p2->DestroyValue(c.param_ptr(STR("StructOut")));
    return true;
}

namespace {
UObject* new_save_object(const wchar_t* prefix, std::string& err)
{
    auto* cls = static_cast<UClass*>(load_asset(STR("/Game/Gameplay/Save/SaveObjects/BP_SaveGameObject_V8.BP_SaveGameObject_V8_C")));
    if (!cls) { err = "BP_SaveGameObject_V8_C not loaded"; return nullptr; }
    static std::atomic<int> seq{0};
    const std::wstring name = std::wstring(prefix) + std::to_wstring(++seq);
    FStaticConstructObjectParameters params{cls, gi()};
    params.Name = FName(name.c_str(), FNAME_Add);
    UObject* obj = UObjectGlobals::StaticConstructObject(params);
    if (!obj) err = "StaticConstructObject failed";
    return obj;
}

std::filesystem::path slot_file(const std::wstring& slot)
{
    std::wstring rel = slot;
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    return std::filesystem::path(saves_dir()) / (rel + L".sav");
}

bool write_slot_file(const std::wstring& slot, const std::vector<uint8_t>& bytes, std::string& err)
{
    const auto file = slot_file(slot);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) { err = "cannot write " + ue::to_utf8(file.wstring()); return false; }
    return true;
}

bool read_slot_file(const std::wstring& slot, std::vector<uint8_t>& bytes, std::string& err)
{
    const auto file = slot_file(slot);
    std::ifstream f(file, std::ios::binary);
    if (!f) { err = "cannot read " + ue::to_utf8(file.wstring()); return false; }
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return bytes.size() >= 4;
}
} // namespace

bool make_characters_blob(const std::wstring& slot, std::vector<uint8_t>& bytes, std::string& err)
{
    UObject* sm = save_manager();
    if (!sm) { err = "no save manager"; return false; }
    ue::Call create(sm, STR("CreateSaveGameDataFromGameState"));
    if (!create || !create.invoke()) { err = "CreateSaveGameDataFromGameState failed"; return false; }
    UObject* data = create.out_object(STR("SaveGameData"));
    if (!data) { err = "no save game data"; return false; }
    UObject* obj = new_save_object(L"E33Coop_Build_", err);
    if (!obj) return false;
    // Seulement les personnages : copie profonde de la map (même type des deux côtés), le reste de la save reste vide.
    FProperty* src = ue::prop(data, STR("CharactersCollection"));
    FProperty* dst = ue::prop(obj, STR("CharactersCollection"));
    if (!src || !dst) { err = "CharactersCollection not found"; return false; }
    dst->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(obj), src->ContainerPtrToValuePtr<void>(data));
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    ue::Call save(gps, STR("SaveGameToSlot"));
    if (!save) { err = "SaveGameToSlot not found"; return false; }
    save.set_object(STR("SaveGameObject"), obj).set_string(STR("SlotName"), slot.c_str()).set_int(STR("UserIndex"), 0).invoke();
    if (!save.out_bool(STR("ReturnValue"))) { err = "SaveGameToSlot returned false"; return false; }
    return read_slot_file(slot, bytes, err);
}

bool apply_characters_blob(const std::wstring& slot, const std::vector<uint8_t>& bytes, const std::vector<std::string>& names,
                           std::vector<std::string>& applied, std::string& err)
{
    applied.clear();
    if (!write_slot_file(slot, bytes, err)) return false;
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    ue::Call load(gps, STR("LoadGameFromSlot"));
    if (!load) { err = "LoadGameFromSlot not found"; return false; }
    load.set_string(STR("SlotName"), slot.c_str()).set_int(STR("UserIndex"), 0).invoke();
    UObject* obj = load.out_object(STR("ReturnValue"));
    if (!obj) { err = "LoadGameFromSlot returned null"; return false; }
    std::vector<std::pair<void*, void*>> pairs;
    if (!reflect::map_pairs(obj, STR("CharactersCollection"), pairs, nullptr, nullptr)) { err = "no CharactersCollection in blob"; return false; }
    for (auto& [k, v] : pairs) {
        const std::string name = ue::to_utf8(static_cast<FName*>(k)->ToString());
        if (std::find(names.begin(), names.end(), name) == names.end()) continue;
        UObject* cd = character_data(name);
        ue::Call c(cd, STR("LoadCharacterState"));
        FProperty* p = c ? c.param_property(STR("CharacterState")) : nullptr;
        if (!p) { err = "LoadCharacterState not found for " + name; continue; }
        void* dst = c.param_ptr(STR("CharacterState"));
        p->InitializeValue(dst);
        p->CopyCompleteValue(dst, v);
        c.invoke();
        p->DestroyValue(dst);
        ue::Call d(cd, STR("DispatchModificationsIfPossible"));
        if (d) d.set_string(STR("DebugReason"), STR("E33Coop build")).invoke();
        applied.push_back(name);
    }
    return true;
}

// --- Ressources communes (phase 5, D13 option 1) ---------------------------------------------------

UObject* inventory_manager() { return ue::get_object(controller(), STR("AC_jRPG_InventoryManager")); }

void economy_sample(std::map<std::string, int64_t>& out)
{
    out.clear();
    UObject* g = gi();
    if (!g) return;
    if (auto* gold = ue::value_ptr<int32_t>(g, STR("Gold"))) out["gold"] = *gold;
    // Objets : GI.Inventory (quantités par identifiant d'objet, toutes piles confondues).
    if (auto* ap = CastField<FArrayProperty>(ue::prop(g, STR("Inventory")))) {
        auto* inner = CastField<FStructProperty>(ap->GetInner());
        auto* arr = ap->ContainerPtrToValuePtr<FScriptArray>(g);
        if (inner && arr) {
            UStruct* st = inner->GetStruct();
            FProperty* stacks = ue::struct_member(st, STR("StacksAmount"));
            auto* stat = CastField<FStructProperty>(ue::struct_member(st, STR("ItemStaticData")));
            FProperty* id = stat ? ue::struct_member(stat->GetStruct(), STR("Item_HardcodedName")) : nullptr;
            auto* data = static_cast<uint8_t*>(arr->GetData());
            for (int32 i = 0; i < arr->Num() && stacks && id; ++i) {
                uint8_t* e = data + static_cast<size_t>(i) * inner->GetSize();
                const std::string name = ue::to_utf8(static_cast<FName*>(id->ContainerPtrToValuePtr<void>(stat->ContainerPtrToValuePtr<void>(e)))->ToString());
                out["item:" + name] += *static_cast<int32_t*>(stacks->ContainerPtrToValuePtr<void>(e));
            }
        }
    }
    // Niveaux des équipements (armes, pictos) : instances du système d'amélioration.
    std::vector<std::pair<void*, void*>> pairs;
    if (reflect::map_pairs(ue::get_object(g, STR("InventorySystem")), STR("ItemInstances"), pairs, nullptr, nullptr)) {
        for (auto& [k, v] : pairs) {
            UObject* inst = *static_cast<UObject**>(v);
            if (!inst || !ue::prop(inst, STR("CurrentLevel"))) continue;
            out["level:" + ue::to_utf8(static_cast<FName*>(k)->ToString())] = ue::get_int(inst, STR("CurrentLevel"));
        }
    }
}

bool economy_apply(const std::string& key, int64_t delta, std::string& err)
{
    if (delta == 0) return true;
    UObject* inv = inventory_manager();
    if (key == "gold") {
        if (!inv) { err = "no inventory manager"; return false; }
        if (delta > 0) {
            ue::Call c(inv, STR("ReceiveGold"));
            if (!c) { err = "ReceiveGold not found"; return false; }
            return c.set_int(STR("GoldAmount"), delta).set_string(STR("Reason"), STR("E33Coop")).invoke();
        }
        ue::Call c(inv, STR("TrySpendGold"));
        if (!c) { err = "TrySpendGold not found"; return false; }
        c.set_int(STR("GoldAmount"), -delta).set_string(STR("Reason"), STR("E33Coop")).invoke();
        if (!c.out_bool(STR("Success"))) { err = "not enough gold"; return false; }
        return true;
    }
    if (key.rfind("item:", 0) == 0) {
        if (!inv) { err = "no inventory manager"; return false; }
        const std::wstring id = ue::from_utf8(key.substr(5));
        if (delta > 0) {
            ue::Call c(inv, STR("AddItemToInventory"));
            if (!c) { err = "AddItemToInventory not found"; return false; }
            return c.set_fname(STR("ItemHardcodedName"), id.c_str()).set_int(STR("Amount"), delta).invoke();
        }
        ue::Call c(inv, STR("RemoveItemFromInventory"));
        if (!c) { err = "RemoveItemFromInventory not found"; return false; }
        return c.set_fname(STR("ItemHardcodedName"), id.c_str()).set_int(STR("Amount"), -delta).set_bool(STR("KeepEmptyStack"), false).invoke();
    }
    if (key.rfind("level:", 0) == 0) {
        const std::string id = key.substr(6);
        std::vector<std::pair<void*, void*>> pairs;
        if (!reflect::map_pairs(ue::get_object(gi(), STR("InventorySystem")), STR("ItemInstances"), pairs, nullptr, nullptr)) { err = "no item instances"; return false; }
        for (auto& [k, v] : pairs) {
            if (ue::to_utf8(static_cast<FName*>(k)->ToString()) != id) continue;
            UObject* inst = *static_cast<UObject**>(v);
            ue::Call c(inst, STR("ChangeLevel"));
            if (!c) { err = "ChangeLevel not found"; return false; }
            return c.set_int(STR("NewLevel"), ue::get_int(inst, STR("CurrentLevel")) + delta).set_string(STR("DebugReason"), STR("E33Coop")).invoke();
        }
        err = "item instance not found: " + id;
        return false;
    }
    err = "unknown key " + key;
    return false;
}

} // namespace e33::game

namespace e33::game {

// Diagnostic (parade du client à la manette) : liste « action=touche » d'un contexte Enhanced Input (membre Mappings).
std::string imc_mappings(UObject* imc, const char* only_key)
{
    if (!imc) return "";
    auto* ap = CastField<FArrayProperty>(ue::prop(imc, STR("Mappings")));
    if (!ap) return "?";
    auto* inner = CastField<FStructProperty>(ap->GetInner());
    if (!inner) return "?";
    UStruct* st = inner->GetStruct();
    FProperty* action = ue::struct_member(st, STR("Action"));
    auto* key = CastField<FStructProperty>(ue::struct_member(st, STR("Key")));
    FProperty* key_name = key ? ue::struct_member(key->GetStruct(), STR("KeyName")) : nullptr;
    if (!action || !key || !key_name) return "?";
    auto* arr = static_cast<FScriptArray*>(ap->ContainerPtrToValuePtr<void>(imc));
    auto* data = static_cast<uint8_t*>(arr->GetData());
    std::string out;
    for (int32 i = 0; i < arr->Num(); ++i) {
        uint8_t* m = data + static_cast<size_t>(i) * inner->GetSize();
        UObject* a = *static_cast<UObject**>(action->ContainerPtrToValuePtr<void>(m));
        void* k = key->ContainerPtrToValuePtr<void>(m);
        FName* kn = static_cast<FName*>(key_name->ContainerPtrToValuePtr<void>(k));
        const std::string kname = ue::to_utf8(kn->ToString());
        if (only_key) {
            if (kname != only_key) continue;
            if (!out.empty()) out += "+";
            out += a ? ue::short_name(a) : std::string("null");
            continue;
        }
        if (!out.empty()) out += ";";
        out += (a ? ue::short_name(a) : std::string("null")) + "=" + kname;
    }
    return out;
}

// Contextes actifs (tous ceux chargés) qui lient cette touche, avec leur priorité et l'action liée.
std::string active_contexts_with_key(const char* key)
{
    UObject* sub = input_subsystem();
    if (!sub) return "?";
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(STR("InputMappingContext"), all);
    std::string out;
    for (UObject* imc : all) {
        ue::Call has(sub, STR("HasMappingContext"));
        if (!imc || !has) continue;
        has.set_object(STR("MappingContext"), imc).invoke();
        if (!has.out_bool(STR("ReturnValue"))) continue;
        const std::string acts = imc_mappings(imc, key);
        if (acts.empty()) continue;
        if (!out.empty()) out += ",";
        out += ue::short_name(imc) + ":" + std::to_string(has.out_int(STR("OutFoundPriority"))) + "[" + acts + "]";
    }
    return out;
}

// Contexte actif de plus haute priorité qui lie cette touche : celui qui la reçoit (Enhanced Input).
std::string top_context_for_key(const char* key)
{
    UObject* sub = input_subsystem();
    if (!sub) return "?";
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(STR("InputMappingContext"), all);
    std::string best;
    int64_t best_prio = -1000000;
    for (UObject* imc : all) {
        ue::Call has(sub, STR("HasMappingContext"));
        if (!imc || !has) continue;
        has.set_object(STR("MappingContext"), imc).invoke();
        if (!has.out_bool(STR("ReturnValue"))) continue;
        const int64_t prio = has.out_int(STR("OutFoundPriority"));
        if (prio <= best_prio || imc_mappings(imc, key).empty()) continue;
        best_prio = prio;
        best = ue::short_name(imc);
    }
    return best;
}

// Widgets CommonUI activés (ils peuvent prendre des boutons de la manette avant le jeu).
std::string active_common_widgets()
{
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(STR("CommonActivatableWidget"), all);
    std::string out;
    int n = 0;
    for (UObject* w : all) {
        if (!w || n >= 30) continue;
        ue::Call c(w, STR("IsActivated"));
        if (!c || !c.invoke() || !c.out_bool(STR("ReturnValue"))) continue;
        ++n;
        if (!out.empty()) out += ",";
        out += ue::short_name(w->GetClassPrivate());
    }
    return out;
}

int common_input_type()
{
    UObject* sub = UObjectGlobals::FindFirstOf(STR("CommonInputSubsystem"));
    ue::Call c(sub, STR("GetCurrentInputType"));
    if (!c || !c.invoke()) return -1;
    return static_cast<int>(c.out_int(STR("ReturnValue"), -1));
}

} // namespace e33::game
