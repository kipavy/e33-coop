// Accès aux objets du jeu et actions de haut niveau (chemins vérifiés sur le build 23765773,
// voir docs/GAME-INTERNALS.md). Toutes ces fonctions doivent être appelées sur le game thread.
#pragma once

#include <string>
#include <vector>

#include <Unreal/UObject.hpp>

namespace e33::game {

using RC::Unreal::UObject;

// Chemins des fonctions hookées
namespace path {
inline const wchar_t* const BattleChar = L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C:";
inline const wchar_t* const EnemyChar = L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_Battle_Base.BP_jRPG_Enemy_Battle_Base_C:";
inline const wchar_t* const BattleMgr = L"/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:";
inline const wchar_t* const Stats = L"/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_CharacterBattleStats.AC_jRPG_CharacterBattleStats_C:";
inline const wchar_t* const CtrlWorld = L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:";
inline const wchar_t* const Timeline = L"/Game/Gameplay/Battle/SkillCombos/BP_BattleSequenceTimeline.BP_BattleSequenceTimeline_C:";
inline const wchar_t* const Targeting = L"/Game/Gameplay/Battle/Targeting/FL_TargetingHelpers.FL_TargetingHelpers_C:";
inline std::wstring fn(const wchar_t* cls, const wchar_t* name) { return std::wstring(cls) + name; }
} // namespace path

// E_jRPG_ActionType (libellés lus en jeu le 2026-10-19)
enum ActionType : int { ActNone = 0, ActAttack = 1, ActDefend = 2, ActItem = 3, ActFlee = 4, ActSkill = 5, ActSkip = 6 };
// EBattleEndResult
enum BattleEnd : int { EndInvalid = 0, EndVictory = 1, EndDefeat = 2, EndNeutral = 3 };

UObject* gi();                // BP_jRPG_GI_Custom_C (persistant)
UObject* controller();        // BP_jRPG_Controller_World_C du niveau courant (nul au menu)
UObject* battle_manager();
UObject* save_manager();
UObject* main_menu();         // WBP_MM_MainMenu_C s'il existe
bool map_change_in_progress();
bool in_battle();

std::vector<UObject*> players();   // héros du combat courant, morts compris (PlayerCharacters + DeadPlayerCharacters)
std::vector<UObject*> enemies();
UObject* current_character();
UObject* stats(UObject* character);
bool is_player(UObject* character);
bool is_dead(UObject* character);        // appelle GetIsDead (ProcessEvent)
bool is_dead_fast(UObject* character);   // lit Dead? (pas d'appel au jeu)
int spot(UObject* character);            // BattleSpotIndex (-1 si nul)
UObject* find_unit(bool hero, int spot); // combattant par camp et emplacement
std::string char_name(UObject* character);   // nom d'objet court (ex. BP_Verso_Battle2_C_2147456957)
std::string char_class(UObject* character);  // nom de classe (ex. BP_Verso_Battle2_C)
std::string hardcoded_name(UObject* character);   // nom interne du héros (ex. "Verso")

// Charge une save depuis un slot relatif à Saved\SaveGames (ex. "e33coop-test/TEST_A"), autosave coupée.
bool load_save_slot(const std::wstring& slot, std::string& err);
// Déclenche un combat (recette E33 Versus) sur l'arène la plus proche ; party non vide = équipe imposée.
bool trigger_battle(const std::wstring& encounter, const std::vector<std::wstring>& party, std::string& err);
// Dernière rencontre / équipe déclenchées par trigger_battle (vide sinon).
const std::wstring& last_encounter();
const std::vector<std::wstring>& last_party();
// Fait exécuter une action au personnage courant (type E_jRPG_ActionType, compétence ou objet, cibles).
// combo_auto_success >= 0 : DEBUG_ComboAutoSuccessCount (réussites automatiques des QTE offensifs).
bool perform_action(int action_type, const std::wstring& secondary, const std::vector<UObject*>& targets, int combo_auto_success, std::string& err);
// Cibles par défaut d'une action (même règle partout : test runner, hôte pour le faux client).
std::vector<UObject*> auto_targets(UObject* actor, int action_type, const std::wstring& secondary);
// Compétence équipée : type de ciblage (E_jRPG_TargetingType) et coût en PA ; false si absente.
bool skill_info(UObject* hero, const std::wstring& skill, int& targeting, int& ap_cost);
// Objets de combat restants (BattleItemsCount), -1 si inconnu.
int battle_item_count(const std::wstring& item);
// Journal : compétences équipées et objets disponibles (début de combat).
std::string battle_inventory_json();

// Défenses
bool start_parry(UObject* character);
bool start_jump(UObject* character);
bool start_dodge(UObject* character);
bool start_gradient_parry(UObject* character);
void clear_defense_lock(UObject* character);
bool in_immunity(UObject* character);
double parry_immune_duration(UObject* character);
// Annule la fenêtre de défense en cours (le prochain coup porte), sans toucher aux animations.
void cancel_defense(UObject* character);
// Dans un hook de AC_jRPG_CharacterBattleStats_C:ReceiveDamageFromObject : l'attaque entrante est-elle sautable ?
bool incoming_attack_jumpable(UObject* damage_builder);
// Personnage attaquant d'un DamageBuilder (nul si inconnu).
UObject* damage_source_character(UObject* damage_builder);

// Buffs actifs d'un combattant
struct Buff {
    UObject* wrapper = nullptr;
    UObject* instance = nullptr;
    std::string cls;
    int stacks = 0;
    int turns = 0;
    bool permanent = false;
    int type = 0;            // E_BuffAndStatusEffectTypes (3 = agrégateur)
};
std::vector<Buff> buffs(UObject* stats);
bool is_helper_buff(const Buff& b);   // agrégateurs : exclus du hash et de la resynchro
bool remove_buff(UObject* stats, UObject* instance);
bool add_buff(UObject* stats, const std::string& cls, int stacks, int turns, bool permanent, std::string& err);

// Jauge gradient (globale)
UObject* gradient_gauge();
// Position monde d'un acteur (K2_GetActorLocation).
bool location(UObject* actor, double out[3]);
// Visée libre
inline const wchar_t* const FreeAimComp = L"/Game/Gameplay/FreeAim/BP_FreeAimShooterBattleComponent.BP_FreeAimShooterBattleComponent_C:";
inline const wchar_t* const FreeAimProjectile = L"/Game/Gameplay/WorldSkills/BP_FreeAimProjectile_Base.BP_FreeAimProjectile_Base_C:";

} // namespace e33::game
