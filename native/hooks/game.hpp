// Accès aux objets du jeu et actions de haut niveau (chemins vérifiés sur le build 23765773,
// voir docs/GAME-INTERNALS.md). Toutes ces fonctions doivent être appelées sur le game thread.
#pragma once

#include <cstdint>
#include <map>
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
UObject* load_asset(const wchar_t* object_path);   // objet par chemin, chargé par le registre d'assets s'il ne l'est pas encore
UObject* controller();        // BP_jRPG_Controller_World_C du niveau courant (nul au menu)
UObject* battle_manager();
UObject* save_manager();
UObject* main_menu();         // WBP_MM_MainMenu_C s'il existe
UObject* world_pawn();        // pion d'exploration (BP_jRPG_Character_World_C) : nul au démarrage et au menu ; sans ProcessEvent
UObject* input_subsystem();   // sous-système Enhanced Input du joueur local (contextes d'entrée) ; FindFirstOf, pas à chaque image
// Diagnostic : touches d'un contexte d'entrée (« IA_x=Key;… ») ; widgets CommonUI activés ; type d'entrée courant (0 souris/clavier, 1 tactile, 2 manette).
std::string imc_mappings(UObject* imc, const char* only_key = nullptr);
std::string active_contexts_with_key(const char* key);
std::string top_context_for_key(const char* key);       // contexte actif de plus haute prioritÃ© qui lie cette touche ("" : aucun)   // « IMC:prio[action],… » des contextes actifs (tous) qui lient cette touche
std::string active_common_widgets();
int common_input_type();
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
// Nouvelle partie (SaveManager.StartNewGame depuis le menu principal) ; no_autosave : aucune sauvegarde écrite.
bool start_new_game(bool no_autosave, std::string& err);
// Déclenche un combat (recette E33 Versus) sur l'arène la plus proche ; party non vide = équipe imposée.
bool trigger_battle(const std::wstring& encounter, const std::vector<std::wstring>& party, std::string& err);
// Dernière rencontre / équipe déclenchées par trigger_battle (vide sinon).
const std::wstring& last_encounter();
const std::vector<std::wstring>& last_party();
// Appels faits par le mod (rejeu d'action, miroir des menus, visée rejouée) : pendant leur durée, mod_call_depth() > 0.
// Sert à distinguer, dans les hooks, ce que fait le mod de ce que fait le joueur d'ici (test des entrées bloquées).
struct ModCall { ModCall(); ~ModCall(); ModCall(const ModCall&) = delete; ModCall& operator=(const ModCall&) = delete; };
int mod_call_depth();

// Fait exécuter une action au personnage courant (type E_jRPG_ActionType, compétence ou objet, cibles).
// combo_auto_success >= 0 : DEBUG_ComboAutoSuccessCount (réussites automatiques des QTE offensifs).
bool perform_action(int action_type, const std::wstring& secondary, const std::vector<UObject*>& targets, int combo_auto_success, std::string& err);
// Fin de combat imposée (client : BattleEnd de l'hôte sans fin locale, combat qui continue ici). direct_flow : appel de
// BM.StartBattleEndFlow au lieu de BM.ForceBattleEnd. Non vérifié en jeu (noms du dump). how : fonction et paramètre
// utilisés, pour le journal. Ne pas appeler depuis un hook (tick seulement).
bool force_battle_end(int result, bool direct_flow, std::string& how);
// Écran de défaite (D33) : WBP_jRPG_GameOverScreen_C du gestionnaire de combat (nul hors défaite), et sa fenêtre
// « Réessayer / Abandonner » (WBP_ModalPopup_C ouverte par PromptBattleRetry, ParentActivatableWidget = l'écran), nulle
// si elle n'est pas ouverte. Parcourt les objets (FindAllOf) : pas à chaque image. visible : affichée (ni masquée ni réduite).
UObject* defeat_screen();
UObject* defeat_modal(UObject* screen, bool* visible = nullptr);
// Réponses de la fenêtre (EModalUIResponse, déduites des noms internes : à confirmer en jeu, coop_defeat_response).
enum ModalResponse : int { RespConfirm = 1, RespCancel = 2 };
inline constexpr const wchar_t* kGameOverScreenClass = L"/Game/UI/Widgets/HUD_Battle/SubWidget/WBP_jRPG_GameOverScreen.WBP_jRPG_GameOverScreen_C";
// Combattant mis à mort (PV à 0, puis Stats.SetDead, comme le réglage « dead » du test runner) : ennemi mort chez
// l'hôte seulement. false si les stats ou SetDead manquent.
bool kill_unit(UObject* character);
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
void note_class_path(const std::string& cls, const std::string& path);   // chemin d'une classe de buff envoyé par l'hôte

// Jauge gradient (globale)
UObject* gradient_gauge();
// Position monde d'un acteur (K2_GetActorLocation).
bool location(UObject* actor, double out[3]);
// Mécanique propre d'un héros (perfection de Verso, posture de Maelle, taches de Lune…) : champs entiers des composants
// BP_*Mechanic*Component / BP_StainHolderComponent ("Classe.Champ", "Classe.Tableau#" = taille, "Classe.Tableau[i]").
std::vector<std::pair<std::string, int64_t>> mechanic_fields(UObject* hero);
// Réécrit les champs qui diffèrent ; renvoie le nombre corrigé (failed : champs non réparables).
int set_mechanic_fields(UObject* hero, const std::vector<std::pair<std::string, int64_t>>& wanted, int& failed);
// --- Exploration et session (phase 4, D12) -----------------------------------------------------
// Dossier des saves (%LOCALAPPDATA%\Sandfall\Saved\SaveGames).
std::wstring saves_dir();
// Instantané de la partie en cours : CreateSaveGameDataFromGameState -> BP_SaveGameObject_V8_C -> SaveGameToSlot(slot),
// puis lecture du fichier. slot relatif à SaveGames (ex. "e33coop/snapshot").
bool make_snapshot(const std::wstring& slot, std::vector<uint8_t>& bytes, std::string& err);
// Pion d'exploration possédé par le contrôleur (K2_GetPawn, sans parcours d'objets) ; nul en combat ou au menu.
UObject* explore_pawn();
// Niveau courant (nom court du package du pion, ex. "Level_Camp_Main") ; vide sinon.
std::string current_level();
struct PawnState { double pos[3]{}; double yaw = 0; double vel[3]{}; };
bool pawn_state(UObject* pawn, PawnState& out);
bool move_pawn(UObject* pawn, double dir_x, double dir_y, double scale);   // AddMovementInput (monde)
bool teleport_pawn(UObject* pawn, const double pos[3], double yaw);
// Allure ALS du pion d'exploration (0 marche, 1 course, 2 sprint) : réelle (GetGait) ou voulue (GetDesiredGait) ; -1 si
// inconnue. Sprint : SprintAction(bValue), comme la touche du joueur ; SetDesiredGait en repli.
int pawn_gait(UObject* pawn, bool desired = false);
bool pawn_sprint_action(UObject* pawn, bool on);
bool set_pawn_desired_gait(UObject* pawn, int gait);
// Vitesses ALS du pion (CharacterMovement.CurrentMovementSettings : marche, course, sprint) et vitesse maximale au sol
// (CharacterMovement.MaxWalkSpeed, que l'ALS règle au changement d'allure permise ; le suiveur la règle lui-même en sprint).
struct GaitSpeeds { float walk = 0, run = 0, sprint = 0; };
bool pawn_gait_speeds(UObject* pawn, GaitSpeeds& out);
float* pawn_max_walk_speed(UObject* pawn);
bool set_pawn_sprint_speed(UObject* pawn, float speed);   // CurrentMovementSettings.SprintSpeed (rattrapage du suiveur)
// Mode de déplacement d'un pion (carte du monde : Esquie, vol, nage ; 2026-10-22). move = EMovementMode de
// CharacterMovement (-1 si illisible), mount = GI.IsMountInUse, world_map = bIsWorldMapPawn, attached = attaché à un acteur.
struct MoveInfo { int move = -1; int custom = 0; bool mount = false; bool world_map = false; bool attached = false; std::string cls; };
bool move_info(UObject* pawn, MoveInfo& out);
bool set_movement_mode(UObject* pawn, int mode);       // CharacterMovement.SetMovementMode(mode, 0)
bool set_pawn_visible(UObject* pawn, bool visible);    // SetActorHiddenInGame + SetActorEnableCollision
// Noms des fonctions de la classe de o (et de ses parents) contenant l'un des mots (insensible à la casse) : diagnostic.
std::vector<std::string> function_names(UObject* o, const std::vector<std::string>& needles, size_t max = 80);
// Destination d'un changement de niveau en cours (GI : LevelDestinationAssetName, SpawnPointTag).
bool map_destination(std::string& level, std::string& spawn_tag);
bool change_map(const std::wstring& level, const std::wstring& spawn_tag);   // FL_jRPG_CustomFunctionLibrary_C:ChangeMapByAssetName
// Points de passage du jeu entre niveaux (BP_jRPG_MapTeleportPoint_C : sorties de zone, entrées sur la carte du monde).
struct TeleportPoint {
    UObject* actor = nullptr;
    std::string name, row, spawn;   // acteur, ligne de niveau de destination (DT_jRPG_Levels), point d'apparition visé
    bool world_map = false;         // IsInWorldMapLevel
    double pos[3]{}, box[3]{};      // acteur, boîte de passage (TeleportBox)
};
std::vector<TeleportPoint> teleport_points();
// Boîtes de passage actives ou non (SetGenerateOverlapEvents) : le pion suiveur ne change pas de niveau de lui-même.
int set_teleport_points_active(bool on);
// Classes Blueprint chargées dont le nom contient l'un des mots (insensible à la casse) : diagnostic (parcours complet).
std::vector<std::string> loaded_bp_classes(const std::vector<std::string>& needles);
std::vector<UObject*> actor_instances(const std::wstring& cls);   // instances d'une classe, hors objet par défaut
// Interaction avec un objet du monde (BPI_Interaction:OnInteractionRequested) par le pion d'ici.
bool interact(UObject* target, bool instant);
// Dialogues
UObject* dialogue_system();
bool in_dialogue();
int dialogue_choices();                  // nombre de réponses proposées (0 = aucune attente de choix)
bool choose_dialogue(int index);
// Cinématique en cours (BP_CinematicSystem.IsPlayingCinematic).
bool playing_cinematic();
// Combat d'exploration : paramètres du combat courant (CurrentBattleStartParams du gestionnaire).
bool battle_params(std::string& encounter, std::string& enemy_global_id, int& engage);
// Ennemi du monde par nom d'acteur ; plus proche du pion si name est vide.
UObject* find_world_enemy(const std::string& name);
// Client : ennemis de la carte sans détection, poursuite ni combat au contact (le combat vient de l'hôte). Renvoie le nombre traité.
int passive_world_enemies();
// Rend au jeu les ennemis rendus passifs ici (capsule, détection, verrou d'aggro) ; renvoie le nombre rendu.
int restore_world_enemies();
// Lance le combat de cet ennemi du monde avec la rencontre imposée (SelectedEncounter) ; encounter vide = la sienne.
bool start_world_battle(UObject* enemy, const std::wstring& encounter, int engage, std::string& err);
// Pause du jeu entier (UGameplayStatics::SetGamePaused) et menu pause visible.
bool set_game_paused(bool paused);
bool pause_menu_visible();
bool is_game_paused();             // UGameplayStatics::IsGamePaused
bool back_to_title();              // contrôleur : BackToTitleMenu (retour au menu principal)
// Exécute l'action « au passage » d'une zone de déclenchement (BP_GameActionTriggerZone_C dont le nom contient name_part).
bool run_zone_action(const std::string& name_part, std::string& err);
bool skip_cinematic();

// --- Builds des personnages (phase 5, D13) ---
std::vector<std::string> character_names();   // personnages connus (GI.CharactersCollection), triés
std::vector<std::string> party_names();       // équipe courante, dans l'ordre (GI.CurrentParty)
// Remplace l'équipe courante (CharactersManager du contrôleur : SetParty).
bool set_party_names(const std::vector<std::string>& names, std::string& err);
UObject* character_data(const std::string& name);
// Empreinte du build (SaveCharacterState sans PV/PM).
bool character_build(const std::string& name, uint64_t& hash, std::string* summary = nullptr);
// États de tous les personnages, sérialisés par le jeu (save V8 ne contenant que CharactersCollection).
bool make_characters_blob(const std::wstring& slot, std::vector<uint8_t>& bytes, std::string& err);
// Applique (LoadCharacterState) les personnages nommés d'un tel blob.
bool apply_characters_blob(const std::wstring& slot, const std::vector<uint8_t>& bytes, const std::vector<std::string>& names,
                           std::vector<std::string>& applied, std::string& err);
// --- Ressources communes (D13) : or, quantités d'objets ("item:<id>"), niveaux d'équipement ("level:<id>") ---
UObject* inventory_manager();
// Rejoue ici l'utilisation d'un objet hors combat faite par un autre joueur : « heal_party » (élixir de chroma,
// contrôleur HealParty) ou « <objet>@<personnage> » (UseItemOnCharacter). L'objet est dépensé par le jeu.
bool replay_item_use(const std::string& what, std::string& err);
void economy_sample(std::map<std::string, int64_t>& out);
// Applique une variation avec les fonctions du jeu (ReceiveGold / TrySpendGold, AddItemToInventory / RemoveItemFromInventory, ChangeLevel).
bool economy_apply(const std::string& key, int64_t delta, std::string& err);

// --- Drapeaux persistants (GI.NamedIDsStates : TMap<UNamedID*, bool>, D38) ---
std::string named_id_name(UObject* id);                    // nom de l'objet (clé des feuilles « flags » de l'empreinte)
bool named_id_guid(UObject* id, uint32_t out[4]);          // UNamedID.Guid
// UNamedID par nom (clés de NamedIDsStates, puis GI.AllNamedIDs, puis GetAllNamedIDs), sinon par Guid (FindNamedIDByGuid) ;
// how : où il a été trouvé (journal). guid nul : pas de repli par Guid.
UObject* find_named_id(const std::string& name, const uint32_t* guid, std::string& how);
int persistent_flag(UObject* id);                          // -1 absent de NamedIDsStates, sinon 0 / 1 (lecture mémoire)
bool write_persistent_flag(UObject* id, bool value);       // GI.WritePersistentFlag (ModCall), vrai si la valeur est lue ensuite

// Visée libre
inline const wchar_t* const FreeAimComp = L"/Game/Gameplay/FreeAim/BP_FreeAimShooterBattleComponent.BP_FreeAimShooterBattleComponent_C:";
inline const wchar_t* const FreeAimProjectile = L"/Game/Gameplay/WorldSkills/BP_FreeAimProjectile_Base.BP_FreeAimProjectile_Base_C:";

} // namespace e33::game
