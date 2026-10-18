# État de la scène de modding E33 et internals connus (recherche du 2026-10-18)

[V] = vu dans du code, un dump ou la doc d'un mod ; [G] = supposition à vérifier en jeu.
Build Steam 23765773 : exe `SandFall-Win64-Shipping.exe` SHA256 `218a2478aad5b3a1674b5d7097ec1ac4f994abb95466f9da1407fcd39d639618` (UE 5.4.4, CL-68322, projet interne « ProjectW ») selon trois projets publics — à confirmer localement.

## Projets existants importants
- **Online Expedition (beta)**, Pipexlul, Nexus 820, v0.13.0 (2026-10-12) [V] : coop **en ligne** jusqu'à 5 joueurs, serveur propre (officiel ou auto-hébergé), Steam pour l'identité. Chaque joueur contrôle ses personnages, parades/esquives/sauts/contres locaux ; QTE, visée libre, buffs, barres de brise, mécaniques des 6 persos, cinématiques (skip par vote), histoire et inventaire (hôte) synchronisés. Livre son propre UE4SS. Code fermé. Logs `%LOCALAPPDATA%\OnlineExpedition\logs`.
- **E33 Versus**, `github.com/Sebastian0211-vs/ClairObscurExpedition33-VersusMod`, MIT, v0.5.0 (2026-10-08) [V] : mod Lua UE4SS + sidecar Python (`e33net.exe`) + relais TCP port 33033. Le PC du défenseur décide les attaques de monstres, celui de l'attaquant les combos des héros. Snapshot d'état (hash FNV) à chaque début de tour avec barrière ; ordre des tours corrigé depuis le PC décideur. À lire : `lib/sync.lua`, `lib/defense.lua`, `lib/state.lua`. Issue #7 ouverte : désync résiduelle.
- **E33-AutoParryAnim**, `github.com/uuuu790/E33-AutoParryAnim`, MIT, Nexus 917 : mod **C++** UE4SS, le plus proche de notre stack.
- Autres : Kouzukii coop local (voir `kouzukii-study.md`), `Demorck/ClairObscurArchipelagoRandomizer` (réseau Lua, save, drapeaux, combat), `Fro538/Reworked-e33-auto-parry`, `JaakkoLipp/COE33FastBattleEnd`, `mattmab/ClairObscurSDK` (MIT ; DLL native liée au hash exact de l'exe ; notes sur GObjects/name pool, console ; confirme Steamworks v157, Sentry, MsQuic), `IhorChornyi314/E33-Randomizer`, CheatTables ken1882, Nexus 907/797 (vitesse), 756 (skip mort), 434 (skip tour), 636 (Parry Trainer), 421 (QuickSave), 20 (No Intro).
- ⚠️ Dépôts « Expedition-33-Assistant », « Combat-Assistant », « Crash-Fix », « Hack Trainer » (ex. `danielpark-x98p6`, `john-b-io1990g9`, `rapidsnegotiatoroven`, `AngleClassMarvel`, `Vaultdreprepare`) : leurres malware probables. Ne rien exécuter.

## UE4SS
- v3.0.1 trop vieux pour UE 5.4 ; utiliser **experimental ≥ 1176** (actuel `UE4SS_v3.0.1-1181-g83330635`, 2026-10-17). Éviter 1135/1136 : lire une propriété soft-object depuis Lua plante E33 (issue #1433).
- Builds connus sur E33 : `716c1e4` (Versus, Archipelago), `848-g91b70e5` (Parry Trainer), `526-g0967ff5`, 1108/1152 (AutoParryAnim), 1125 (Nexus 630). Online Expedition prévient que d'autres builds, dont le dernier expérimental, « peuvent poser problème ».
- Pas de CustomGameConfig pour E33 ; réglages par défaut OK. Un échec d'AOB du constructeur `FText` est sans gravité (issue #855).
- Disposition : `Sandfall\Binaries\Win64\` (`SandFall-Win64-Shipping.exe`, `dwmapi.dll`) ; `Win64\ue4ss\` (`UE4SS.dll`, `UE4SS-settings.ini`, `UE4SS.log`) ; `ue4ss\Mods\<Mod>\` (`enabled.txt`, `scripts\main.lua` ou `dlls\main.dll`) ; `ue4ss\Mods\mods.txt`. Paks de mods : `Sandfall\Content\Paks\~mods` ; mods BP : `Paks\LogicMods`. Config du jeu : `%LOCALAPPDATA%\Sandfall\Saved\Config\Windows`.
- Pièges :
  - `FindFirstOf`/`FindAllOf` plantent pendant un voyage de niveau ou le spawn d'un combat → passer par les références : game instance → `GetPlayerController` → `.AC_jRPG_BattleManager`, `.AC_jRPG_CharactersManager`, `.BP_GameActionExecutorComponent` ; game instance → `.SaveManager`.
  - `LoopAsync`/`ExecuteWithDelay`/`ExecuteInGameThread` ont planté avec beaucoup de Lua sur le game thread ; Versus s'accroche au `Tick` BP d'un widget invisible `WBP_DPISizeBox_C`.
  - Un hook d'événement de dégâts a planté au premier coup ; Versus compare des snapshots de PV.
  - Assigner un `FText` Lua à une propriété est risqué ; le passer en argument est OK.
  - C++ : `UObjectGlobals::RegisterHook` (pas `UFunction::RegisterPreHook`), out-params via `FindOutParamValueAddress` dans le post, pas de callback nul, `GetPropertiesSize()`.

## Paks et outils
- IoStore (`.pak/.utoc/.ucas`), pas de `.sig`, **pas de chiffrement AES**. Montage « Sandfall », UE 5.4.
- `.usmap` : Kouzukii `Mappings.usmap` (1.5), `TheNaeem/Unreal-Mappings-Archive`, NomadWithoutAHome (CL 56289) ; ou régénérer via UE4SS `DumpUSMAP()` pour CL 68322.
- Dumps Dumper-7 (CL 56289, noms OK, offsets périmés) : `NomadWithoutAHome/Unreal-Engine-Game-SDKs/5.4.4-56289+l-Clair Obscur`, `Cocelillo/...ClairObscurFix.asi/src/SDK`.
- Outils : retoc v0.1.5 (`retoc to-legacy`, `retoc to-zen ... --version UE5_4`), FModel (GAME_UE5_4 + usmap), UAssetGUI v1.1.0, `trumank/kismet-analyzer`, Dumper-7.

## Carte des classes [V]
- `/Game/jRPGTemplate/Blueprints/Basics/` : `BP_jRPG_GI_Custom_C` (game instance : `.SaveManager`, `GetAllNamedIDs`, `WritePersistentFlag`), `BP_jRPG_Controller_World_C` (porte battle manager, characters manager, action executor ; `ReceiveTick`), `BP_jRPG_GM_Bootstrap_C`, `BP_jRPG_GM_World_C`, `BP_jRPG_Character_Battle_Base_C`, `BP_jRPG_Enemy_Battle_Base_C`, `FL_jRPG_CustomFunctionLibrary_C` (`ChangeMapByAssetName`, `Change Map Internal`).
- `/Game/jRPGTemplate/Blueprints/Components/` : `AC_jRPG_BattleManager_C`, `AC_jRPG_CharacterBattleStats_C` (accès `unit.AC_jRPG_CharacterStats`), `AC_jRPG_CharactersManager_C`, `AC_jRPG_InventoryManager_C`, `AC_jRPG_TargetingManager_C`, `AC_jRPG_CombatMovement_C`.
- `BP_CharacterData_C` (`/Game/Gameplay/CharacterData`), `BP_SaveManager_C` (`/Game/Gameplay/Save`), `BP_GameActionExecutorComponent_C` + `BP_GameAction_TriggerBattle_C` / `ReplaceCharacter` / `RequestSaveGame`, `BP_ExtendedCheatManager_C`.
- Data tables `/Game/jRPGTemplate/Datatables/` : `DT_jRPG_Encounters`, `DT_jRPG_Enemies`, `DT_jRPG_Levels`.
- IDs internes des héros : `Frey` (Gustave), `Lune`, `Maelle`, `Sciel`, `Verso`, `Monoco`.
- Plugins : CommonUI, LGUI (roue de combat `BP_LGUI_UIContainer_BattleWheelRoot_C`), OnlineSubsystemSteam (`SteamNetDriver`), OnlineSubsystemUtils (`IpNetDriver`), IrisCore. GameplayAbilities chargé mais inutilisé. Module natif `/Script/SandFall`.

## Sauvegardes
- GVAS ; éditeur `Infarctus/CO-E33_Save_editor` (crate Rust `uesave` 0.6.2). `%LOCALAPPDATA%\Sandfall\Saved\SaveGames\<SteamID64>\EXPEDITION_<n>.sav`, conteneur `BP_jRPG_SavesContainer_C`, backups. Steam Cloud actif : restaurer Steam fermé.
- Classes : `BP_SaveGameObject_V0/V1/V2_C` (+ upgraders), données runtime `BP_SaveGameData_C`.
- Propriétés : `CurrentParty`, `CharactersCollection`, `InventoryItems`, `Gold`, `QuestStatuses`, `ExplorationProgression`, `MapToLoad`, `SpawnPointTagToLoadAt`, `BattledEnemies`, `WeaponProgressions`, `PassiveEffectsProgressions`, `InteractedObjects`, `TimePlayed`, `FinishedGameCount` ; runtime : `GPE_States`, `UnlockedSpawnPoints`.
- `BP_SaveManager_C` : `RequestSaveInternal(bool processNow, reason)`, `RequestAutoSave`, `ForceAutoSave`, `RequestSaveAtSpawnPoint`, `SaveGameToFile(name)`, `RequestLoadGame(FS_LoadGameRequest)`, `LoadGameFromSaveFile`, `AsyncLoadGameFromSlot`, `CanSave`, `IsCurrentLevelValidForSave`, `EnableAutoSave`, `SetEnableAutoSave`, délégués `OnSaveGameToSlotStarted/Finished`, `OnLoadGameFromSlotStarted/Finished`.
- Usages : Archipelago appelle `RequestSaveInternal(true, …)` et hooke `SaveGameToFile` + `GetNewGameData` ; Versus met `EnableAutoSave=false` avant de charger un monde ; QuickSave (421) sauve hors points de sauvegarde, mais sauver en combat corrompt la save ; « Continuer » = `WBP_MM_MainMenu_C:OnContinueClicked()`.

## Combat
- `AC_jRPG_BattleManager_C` : `CurrentCharacter`, `PlayerCharacters[]`, `Enemies[]`, `NextCharacterTurns[]`, `InitiativeNeededToTakeTurn`, `SelectedAction` (`E_jRPG_ActionType`, 3 = objets), `SelectedSecondaryAction` (FName de compétence), `EncounterName`, `BattleEndState`, `HasActiveCounterAttack`, `FleeImpossible`, `DEBUG_ComboAutoSuccessCount`, piles de modificateurs de fenêtre parade/esquive, `GradientGaugeSystem`, `UseDiegeticQTE` ; fonctions `StartCharacterTurn`, `UpdateBattleTurns`, `GetCharacterWithMaxInitiative(out TimeToTurn)`, `End_Character_Turn(double UsedInitiative)`, `SelectAction`, `SelectSecondaryAction`, `RequestActionExecution(targets)`, `TriggerSkill`, `OnParryAction/OnDodgeAction/OnJumpAction/OnGradientCounterAction`, `TryCounterAttack`, `SpawnEnemyOnSpot`, `Spawn_Player_Character`, `CheckBattleEnd`, `StartBattleEndFlow(EBattleEndResult)`, `OnBattleEndVictory`, `OnAllHeroesKilled`, `RollBattleRewards`, `TriggerAbandonBattle`, `Sandfall_*` (debug).
- `AC_jRPG_CharacterBattleStats_C` : maps `CharacterBaseStats`/`CharacterCurrentStats` (`E_jRPG_StatType` : 1 PV, 2 PA, 3 Attaque, 8 Vitesse, 9 Crit), `CurrentHP`, `CurrentAP`, `CurrentInitiative`, `CurrentShieldPoints`, `StunBreakBarCurrent`, `TurnSkipRefCount`, `BattleBuffComponent` ; `ReceiveDamage`, `ReceiveDamageFromObject`, `TryParryHitFromCharacter(attacker, out Successful)`, `SetAP(n, reason)`, `SetCurrentShieldPoints`, `SetCurrentBreakBarDamage`, `ActivateBuff(inst, 4=Forced, …)`, `AdvanceInitiativeByTime`, `GetCurrentSpeed`, `DEBUG_Console_AutoParry`.
- `BP_jRPG_Character_Battle_Base_C` : `ControlledByBattleAI?`, `IsParrying`, `IsGradientParrying`, `IsDodging`, `IsJumping`, `ComboSuccessHistory/ComboPerfectHistory` ; `OnTurnStart`, `Acquire Targets`, `TryStart*`, `TryTriggerJumpCounterAttack`, `PerformStandardHitOnCharacter`, `OnDamageReceived(… Critical_ …)`.
- Ordre des tours : initiative/vitesse avec « time to turn » [V] ; simulation déterministe en temps virtuel [G]. Versus a quand même dû synchroniser `CurrentInitiative`. Un contre après parade complète déclenche `OnTurnStart` sans être un tour → tester `HasActiveCounterAttack`.
- QTE offensifs : `BP_BattleSequenceTimeline_C` `OnSkillComboWindowPreReached` → `OnSkillComboWindowStartReached` → `OnSkillComboWindowEndReached`, verdict dans `TryActivate` ; fenêtre parfaite = dernier quart ; temps réel (ignore la dilatation du temps, contrairement aux parades/esquives). CVars `Sandfall.QTE.ForcePerfect`, `Sandfall.AutoJump` désactivées en shipping.
- Défense : test du saut inline dans `ReceiveDamageFromObject` (verrou 0,9 s, immunité 0,65 s). Fenêtres de parade : Story 350 ms, Expeditioner/Expert 150 ms. Maintenir `IsParrying=true` sur la cible à l'impact pare le coup (Versus rejoue ainsi une parade distante).
- Visée libre : raycasts dans `BP_FreeAimShooterBattleComponent_C`, hors `RequestActionExecution`.
- Mécaniques : Maelle `BP_StanceMechanicComponent_C` + `BP_BattleBuff_Stance_*` ; Lune `BP_StainHolderComponent_C`, `BP_LuneStain_C` ; Verso `BP_PerfectionMechanicComponent_C` ; Gustave `BP_UniqueMechanic_Charge_Component_C` ; Sciel, Monoco : inconnus [G].
- Aléatoire : crits/procs tirés localement ; Versus diffuse le résultat de chaque coup.
- Recette Versus : hooks `OnTurnStart`, `Acquire Targets`, `SelectAction`, `SelectSecondaryAction`, `RequestActionExecution`, fin de combat, `ReceiveDamage(FromObject)`. Rejouer une action : `bm.DEBUG_ComboAutoSuccessCount = hits` → `SelectAction(t)` → `SelectSecondaryAction(FName)` → `RequestActionExecution(targets)`. Finir le tour d'une unité distante : `OnPlayerChoseAction` + `OnPlayerTurnEnd` sur le widget de combat, `ControlledByBattleAI?=true`, `bm["End Character Turn"](bm, 1000.0)`, `u["On Action Finished"](u, {})`. Bloquer l'input local pendant la défense adverse : `pc:DisableInput(pc)`. Lancer un combat : construire `BP_GameAction_TriggerBattle_C` + `ExecuteGameAction`.

## Ligne de commande, intros, console
- Steam lance `Expedition33_Steam.exe` (lanceur, section `.bind` SteamStub) ; `SandFall-Win64-Shipping.exe` sans `.bind`. Passage des arguments / lancement direct : [G].
- Flags UE standards (`-windowed -ResX -ResY -nosound -nullrhi -log`) : [G].
- Intros = widgets : ClairObscurFix appelle `BP_jRPG_GM_Bootstrap_C:OnEpilepsyWarningCompleted()` à l'apparition de `WBP_SplashScreen_Epilepsy_C` et `OnSaveWarningSplashScreenCompleted()` à celle de `WBP_SplashScreens_Logos_C`.
- Pas de console en shipping (ClairObscurFix et le SDK en construisent une ; ClairObscurFix spawn `BP_ExtendedCheatManager_C`).

## Réseau
- Steamworks v157 livré ; OnlineSubsystemSteam et IpNetDriver compilés. Aucun netcode de gameplay [G, très probable] : combat = composants BP locaux, aucune propriété répliquée dans le dump.
