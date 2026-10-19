#include "game.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <optional>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>

#include "log.hpp"
#include "ue.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace e33::game {

namespace {

UObject* g_gi = nullptr;
std::wstring g_last_encounter;

UObject* load_asset(const wchar_t* object_path)
{
    if (UObject* o = ue::find_object(object_path)) return o;
    auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
    if (!registry) return nullptr;
    FAssetData data = registry->GetAssetByObjectPath(FName(object_path, FNAME_Add));
    if (!data.PackageName().GetComparisonIndex()) return nullptr;
    return UAssetRegistryHelpers::GetAsset(data);
}

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

// Position monde d'un acteur (K2_GetActorLocation ; FVector = 3 doubles en UE 5.4).
std::optional<std::array<double, 3>> actor_location(UObject* actor)
{
    ue::Call c(actor, STR("K2_GetActorLocation"));
    if (!c || !c.invoke()) return std::nullopt;
    const auto* v = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
    if (!v) return std::nullopt;
    return std::array<double, 3>{v[0], v[1], v[2]};
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

std::vector<UObject*> players() { return ue::get_object_array(battle_manager(), STR("PlayerCharacters")); }
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

std::string char_name(UObject* ch) { return ue::short_name(ch); }
std::string char_class(UObject* ch) { return ch ? ue::short_name(ch->GetClassPrivate()) : std::string("null"); }

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

bool trigger_battle(const std::wstring& encounter, std::string& err)
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

    ue::Call exec(executor, STR("ExecuteGameAction"));
    if (!exec) { err = "ExecuteGameAction not found"; return false; }
    exec.set_object(STR("GameAction"), action).invoke();
    g_last_encounter = encounter;
    log().write("battle_trigger", JsonObj().add("encounter", ue::to_utf8(encounter)).add("map", ue::short_name(maps.front()))
                                     .add("maps", static_cast<int64_t>(maps.size())).add("distance", best_d2 >= 0 ? std::sqrt(best_d2) : -1.0));
    return true;
}

const std::wstring& last_encounter() { return g_last_encounter; }

bool perform_action(int action_type, const std::vector<UObject*>& targets, int combo_auto_success, std::string& err)
{
    UObject* bm = battle_manager();
    if (!bm) { err = "no battle manager"; return false; }
    if (combo_auto_success >= 0) ue::set_int(bm, STR("DEBUG_ComboAutoSuccessCount"), combo_auto_success);
    ue::Call sel(bm, STR("SelectAction"));
    if (!sel) { err = "SelectAction not found"; return false; }
    sel.set_int(STR("ActionType"), action_type).invoke();
    ue::Call req(bm, STR("RequestActionExecution"));
    if (!req) { err = "RequestActionExecution not found"; return false; }
    req.set_object_array(STR("TargetedCharacters"), targets).invoke();
    return true;
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

bool incoming_attack_jumpable(UObject* damage_builder)
{
    if (!damage_builder) return false;
    ue::Call src(damage_builder, STR("GetDamageSource"));
    if (!src || !src.invoke()) return false;
    UObject* source = src.out_object(STR("DamageSource"));   // TScriptInterface : l'objet est le premier membre
    ue::Call chr(source, STR("GetDamageSourceCharacter"));
    if (!chr || !chr.invoke()) return false;
    UObject* attacker_stats = chr.out_object(STR("CharacterStats"));
    return ue::get_bool(attacker_stats, STR("IsCurrentAttackJumpable"));
}

} // namespace e33::game
