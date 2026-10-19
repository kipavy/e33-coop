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
std::vector<std::wstring> g_last_party;

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

bool perform_action(int action_type, const std::wstring& secondary, const std::vector<UObject*>& targets, int combo_auto_success, std::string& err)
{
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

bool add_buff(UObject* st, const std::string& cls_name, int stacks, int turns, bool permanent, std::string& err)
{
    UClass* cls = ue::find_class_by_short_name(cls_name);
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

} // namespace e33::game
