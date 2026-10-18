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
inline const wchar_t* const BattleMgr = L"/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:";
inline const wchar_t* const Stats = L"/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_CharacterBattleStats.AC_jRPG_CharacterBattleStats_C:";
inline const wchar_t* const CtrlWorld = L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:";
inline std::wstring fn(const wchar_t* cls, const wchar_t* name) { return std::wstring(cls) + name; }
} // namespace path

UObject* gi();                // BP_jRPG_GI_Custom_C (persistant)
UObject* controller();        // BP_jRPG_Controller_World_C du niveau courant (nul au menu)
UObject* battle_manager();
UObject* save_manager();
UObject* main_menu();         // WBP_MM_MainMenu_C s'il existe
bool map_change_in_progress();
bool in_battle();

std::vector<UObject*> players();   // personnages joueurs du combat courant
std::vector<UObject*> enemies();
UObject* current_character();
UObject* stats(UObject* character);
bool is_player(UObject* character);
bool is_dead(UObject* character);
std::string char_name(UObject* character);   // nom d'objet court (ex. BP_Verso_Battle2_C_2147456957)
std::string char_class(UObject* character);  // nom de classe (ex. BP_Verso_Battle2_C)

// Charge une save depuis un slot relatif à Saved\SaveGames (ex. "e33coop-test/TEST_A"), autosave coupée.
bool load_save_slot(const std::wstring& slot, std::string& err);
// Déclenche un combat (recette E33 Versus) sur la première BP_BattleMap_C du niveau.
bool trigger_battle(const std::wstring& encounter, std::string& err);
// Fait exécuter une action au personnage courant (1 = attaque de base) sur des cibles.
bool perform_action(int action_type, const std::vector<UObject*>& targets, int combo_auto_success, std::string& err);
// Démarre une vraie parade (animation + fenêtre) pour un personnage ; renvoie le Success du jeu.
bool start_parry(UObject* character);

} // namespace e33::game
