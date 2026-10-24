# GAME-INTERNALS — carte des internes de Clair Obscur: Expedition 33 pour le coop en ligne

## 1. En-tête et provenance

| Champ | Valeur |
|---|---|
| Jeu | Clair Obscur: Expedition 33, build Steam **23765773** |
| Exe | `SandFall-Win64-Shipping.exe`, SHA256 `218a2478aad5b3a1674b5d7097ec1ac4f994abb95466f9da1407fcd39d639618` |
| Moteur | UE **5.4.4**, CL-**68322**, branche `++streams+ProjectW-release` (nom du `.usmap` généré : `Sandfall-5.4.4-68322+++streams+ProjectW-release-83330635.usmap`) |
| Outil | UE4SS **3.0.1-1181-g83330635** (expérimental) |
| Date | 2026-10-18 |
| Dumps (locaux, jamais versionnés ni redistribués) | `.tools/dumps/23765773/` : `CXXHeaderDump/` (1888 `.hpp`), `UHTHeaderDump/` (modules natifs seulement), `UE4SS_ObjectDump.txt` (60 Mo, 278 647 lignes), `luatypes/`, `.usmap` |

Légende : **[dump]** = lu dans ces dumps ; **[prior]** = recherche antérieure (`docs/research/*.md`, AutoParry.cpp décompilé, sources Lua d'E33 Versus) ; **[inferred]** = déduction, à vérifier.

### 1.1 Limites des dumps, à connaître avant d'utiliser les signatures

- **Le CXXHeaderDump écrit toutes les fonctions BP en `void` et n'affiche pas le paramètre `ReturnValue`** [dump]. Les paramètres de sortie (`&`) sont présents, les valeurs de retour non. Les types de retour donnés ici viennent des `XxxProperty .../Classe:Fonction:ReturnValue` de l'ObjectDump [dump]. `luatypes/` a la même lacune.
- **Les enums BP (UserDefinedEnum) n'ont que des noms internes `NewEnumeratorN`** [dump]. Le libellé affiché (DisplayNameMap) n'est pas dumpé. Les **valeurs** (index) sont fiables, les noms non. Pour les lire : `UEnum::GetDisplayNameTextByValue` en jeu, ou FModel. Les rares correspondances connues viennent de [prior].
- **ObjectDump pris au menu principal** : seules les classes chargées à ce moment y figurent. **Absents** [dump] : `BP_jRPG_GM_World`, `BP_jRPG_GM_Bootstrap`, les widgets de splash (`WBP_SplashScreen_Epilepsy`, `WBP_SplashScreens_Logos`), les BP de combat propres à chaque héros, et les composants de mécanique unique sauf la Perfection de Verso (`BP_StanceMechanicComponent`, `BP_StainHolderComponent`, `BP_UniqueMechanic_Charge_Component`, ceux de Sciel et de Monoco). Pour eux il faut un second dump **en combat**, avec chaque héros dans l'équipe (§14).
- **Atout de l'ObjectDump** : il liste les **propriétés locales** des fonctions BP chargées (`local_*`, `CallFunc_<Fonction>_ReturnValue`, `K2Node_*`). On sait donc quelles fonctions un graphe appelle (par ex. `CallFunc_RandomFloatInRange` dans `RollCriticalHit`), sans en connaître l'ordre. C'est la base des sections 6 et 7. Limite : les fonctions sans valeur de retour ne laissent pas de `CallFunc_` (impossible de voir qui appelle `StartBattleNEW`, par exemple), et les événements de l'event graph (`End Character Turn`, `OnParryAction`…) n'ont pas de locales propres (tout est dans `ExecuteUbergraph_*`).
- Les offsets sont ceux du CXX dump pour ce build. Ils changent à chaque patch : il faut résoudre par nom (`GetPropertyByNameInChain`).
- Les membres de structs BP ont un suffixe GUID (`CharacterHardcodedName_2_2A63D4C5…`) [dump]. Ils sont abrégés ici (`CharacterHardcodedName`). En C++, résoudre par préfixe.

### 1.2 Raccourcis vers les objets clés (chemins complets)

| Rôle | Classe | Accès |
|---|---|---|
| Game instance | `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C` (parent `/Script/SandFall.ExtendedGameInstance`) | `UGameplayStatics::GetGameInstance` |
| Contrôleur monde | `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C` (parent `ALS_Player_Controller_C`) | `GI.GetControllerWorld() -> Object` ou PlayerController 0 |
| Battle manager | `/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C` | `Controller.AC_jRPG_BattleManager` (+0x920) ; `FL_jRPG_CustomFunctionLibrary_C::GetBattleManager(__WorldContext) -> Object` |
| Stats de combat | `/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_CharacterBattleStats.AC_jRPG_CharacterBattleStats_C` | `Character.AC_jRPG_CharacterStats` (+0x8F0) |
| Personnage de combat | `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C` (parent `ACharacter`, taille 0x1562) | `BM.PlayerCharacters`, `BM.Enemies` |
| Ennemi de combat | `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_Battle_Base.BP_jRPG_Enemy_Battle_Base_C` (hérite du précédent, taille 0x1638) | |
| Save manager | `/Game/Gameplay/Save/BP_SaveManager.BP_SaveManager_C` (parent `/Script/SandFall.ExtendedObject`) | `GI.SaveManager` (+0x1E0) |
| QTE manager | `/Game/Gameplay/QTE/BP_QTEManager.BP_QTEManager_C` | `Controller.AC_jRPG_QTEManager` (+0x910, type `UBP_QTEManager_C*`) ; `BM.GetQTEManager(out QTEManager)` |

Rappel [prior] : `FindFirstOf`/`FindAllOf` plantent pendant un voyage de niveau ou le spawn d'un combat. Passer par cette chaîne de références.

---

## 2. Cycle de vie d'un combat

### 2.1 Déclenchement depuis l'exploration

Trois chemins mènent à `AC_jRPG_BattleManager_C::StartBattleNEW(FFBattleStartParams BattleStartParams)` [dump pour les signatures, inferred pour l'ordre] :

1. **Ennemi du monde** `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C` [dump] :
   - Contact par `EncounterCapsuleRange` (`UCapsuleComponent*`, +0x780) et `BoxTriggered()` / `PPBattleTransitionZone` → `CanStartBattleWithCharacter(ABP_jRPG_Character_World_C* Character) -> Bool` → `StartBattle(TEnumAsByte<E_jRPG_EngageTypes::Type> EngagementType)` → `PlayBattleBeginTransition()`, `PlayBattleCameraTransition()`.
   - Données : `PossibleEncounters` (`TArray<FDataTableRowHandle>`), `SelectedEncounter` (`FDataTableRowHandle`, +0x7D0), `BattleMapDataLayers`, `GameActionPostBattle`, `BattleMusicParameters`, `EncounterLevelOffset`.
   - Retours : `OnBattleFinished(TEnumAsByte<EBattleEndResult::Type> BattleEndResult)`, `OnBattleFled()`, `OnEnemyBattledEvent(FName EnemyID)`.
   - `GetBattleCameraTransition(UCameraAnimationSequence*& CameraTransition)` tire au hasard (`CallFunc_Array_Random`) la caméra de transition [dump]. C'est purement cosmétique.
2. **Game action** `/Game/Gameplay/GameActionsSystem/TriggerBattle/BP_GameAction_TriggerBattle.BP_GameAction_TriggerBattle_C` (parent `BP_PDT_GameAction_C` ← `/Script/SandFall.GameAction`), membre `FFBattleStartParams Battle Start Params` (+0x30, 0x158). L'instance est `BP_GameActionInstance_TriggerBattle_C::ExecuteGameAction(const APlayerController* Controller)`, avec retour `OnBattleFinished(bool Victory)` [dump]. Exécution : `Controller.BP_GameActionExecutorComponent` (`/Game/Gameplay/GameActionsSystem/BP_GameActionExecutorComponent.BP_GameActionExecutorComponent_C`), puis `ExecuteGameAction(UGameAction* GameAction)` ou `ExecuteGameActionWrapper(FGameAction_Wrapper)`. Le contrôleur garde `CurrentTriggerBattleGameAction` (+0x9A8). Méthode de Versus pour lancer un combat [prior] : construire un `BP_GameAction_TriggerBattle_C` et appeler `ExecuteGameAction`.
3. **Dialogue / marchand** : `BP_DialogueSystemComponent_C::ActionStartBattle(FFBattleStartParams BattleStartParams)`, `OnMerchantFightRequested()`, `WaitForBattleToFinish()`, `OnBattleFinished(bool Victory)` [dump].

Côté contrôleur `BP_jRPG_Controller_World_C` [dump] :

- `CanStartBattle() -> Bool`, `IsInBattle() -> Bool`, `IsInBattleBeginTransition() -> Bool`
- `OnBattleRequested()`, `OnBattleBeginTransitionRequested()`, `OnBattleDependenciesLoaded()`, `OnBattleStart()`
- `PrepareSystemsForBattle()`, `PrepareCharacterForBattle()`, `PrepareWorldActorsForBattle()`, `HideWorldActorsAroundBattleMap()`, `PrepareLightingForBattle()`, `DisableExplorationControlsForBattle()`
- `RollEncounter(double InputScale)` : rencontres aléatoires, voir `GI.RandomEncountersEnabled` (+0x728) et `PossibleEncounters` (+0x438)
- état : `BattleFlowState` (`TEnumAsByte<E_BattleFlowState::Type>`, +0x9B0, 3 valeurs), `local_lastEncounterSettings` (`FFBattleStartParams`, +0xE00), `PreviousBattleInitialState` (`FS_BattleInitialState`, +0xF68), `IsTakingActionInBattle` (bool, +0xFA8)
- délégué `OnBattleFinished(bool Victory)` (+0xF58)

`FFBattleStartParams` (`/Game/Gameplay/Battle/FBattleStartParams`, 0x158) [dump] :

- `EncounterRow` (`FDataTableRowHandle`), `EnemyGlobalID` (FName), `EngagementType` (`E_jRPG_EngageTypes`, 3 valeurs : normal / first strike / ambush [inferred])
- `BattleMapBP` (`ABP_BattleMap_C*`), `BattleMapBPSoft`, `BattleMapDataLayers`
- `TransitionType` (`EBattleTransitionType`), `PlayCameraIntro`, `ForceFleeImpossible`
- `MusicParameters`, `AmbienceParameters`
- `GameActionPostBattleWrapper`, `GameActionPostBattle`, `BackToExplorationOnLost`
- `EncounterLevelOffset` (int32), `OverrideParty` (`TArray<FS_jRPG_PartySlot>`), `AllowReserveTeam`
- `PostBattleGameFlowTransitionRequest`, `BattleRewardParameters` (`FS_BattleRewardParameters`)

`FS_jRPG_Encounter` (ligne de `DT_jRPG_Encounters` [prior]) [dump] : `Enemies` (`TMap<int32, FDataTableRowHandle>` spot → ligne d'ennemi), `FleeImpossible`, `EncounterLevelOverride`, `DisableCameraEndMovement`, `DisableReactionBattleLines`, `IsNarrativeBattle`, `IsRetryableBattle`.

### 2.2 Démarrage, spawn des héros et des ennemis

Fonctions du `AC_jRPG_BattleManager_C` dans l'ordre probable [dump pour les noms, inferred pour l'ordre] :

```
StartBattleNEW(FFBattleStartParams)
  ├ LoadEncounterSettings()                         // lit EncounterRow -> CurrentBattleEnemies (TMap<int32,FName>, +0x758), FleeImpossible...
  ├ StartPreloadingBattleDependencies() / GetAssetsToLoadForBattle(TArray<TSoftClassPtr<UObject>>&)
  ├ OnDataLayersActivated() / OnBattleDependenciesFullyLoaded()
  ├ SetupBattleMap() / ChangeBattleMap(TSoftObjectPtr<ABP_BattleMap_C>) / OnBattleMapReady()   // SetupBattleMap tire un RandomFloatInRange (cosmétique, [inferred])
  ├ InitBattle(); LoadParty(); InitBattleItemCountsFromInventory()
  ├ SpawnAllCharacters() | SpawnOnlyRequestedTeam()
  │    ├ SpawnPlayerCharactersFromParty()
  │    │    └ Spawn Player Character(UBP_CharacterData_C* CharacterData, int32 BattleSpotIndex, TEnumAsByte<E_jRPG_FormationType::Type> FormationType, ABP_jRPG_Character_Battle_Base_C*& SpawnedCharacter)
  │    └ SpawnEnemies()
  │         └ SpawnEnemyOnSpot(int32 BattleSpotIndex, FName CharacterHardcodedName, bool NotifyBattleStart, ABP_jRPG_Character_Battle_Base_C*& SpawnedEnemy, FVector& EnemySpawnedLocation)
  ├ GetProperBattleSpawnPointTransform(bool EnemySpot?, int32 SpotIndex, E_jRPG_FormationType FormationType, FTransform& , E_jRPG_FormationType&)
  ├ InitPlayerCharactersGear(); CreateBattleInterface()
  ├ StartBattleTransition(USF_BlueprintLatentAction*) ; StartAudioForBattle(...)
  ├ TriggerBattleStartEvents(USF_BlueprintLatentAction*) -> ProcessNextCharacterBattleStartEvent() -> BattleStart_ProcessNextCharacter_SkillSetup() -> OnAllBattleStartEventsTriggered()
  ├ délégué OnBattleReadyToStart()
  └ StartBattle(TEnumAsByte<E_jRPG_EngageTypes::Type> EngageType) / EventBattleStart() -> UpdateBattleTurns()
```

- Autres spawns : `SpawnExtraPlayerCharacter(UBP_CharacterData_C*)`, `SpawnPuppetCharacter(TSubclassOf<ABP_jRPG_Character_Battle_Base_C>, UAC_jRPG_CharacterBattleStats_C* PuppetedBy, double Scale) -> Object`, `SpawnCharacterFromStoredState(FFBattleCharacterStoredState, …&)`, `SpawnBackupTeam()` / `OnBackupTeamLoaded()` (équipe de réserve).
- Côté ennemi : `ABP_jRPG_Character_Battle_Base_C::SummonMinions(FDataTableRowHandle EncounterRow, bool NotifyBattleStart, TArray<…>& SpawnedCharacters, TArray<FVector>& SpawnLocations)`, `SummonSingleMinion(FDataTableRowHandle EnemyRow, int32 BattleSpotIndex, bool NotifyBattleStart, …&, FVector&)`.
- Côté personnage : `PrepareForBattleInternal()`, `PerformBattleStartEvent(UBP_DependencyRefCounter_C*)`, `PerformDelayedBattleStart()`, `OnBattleStart()`, délégué `OnReadyForBattle()`. Côté stats : `Internal_BeginPrepareForBattle()`, `ProcessSkillComponentStartEvents(const USF_BlueprintLatentAction*, bool ForceInstant)`, `Internal_FinishPrepareForBattle()`, `InitSkillStates()`, `InitBattleGear()`, `SetCharacterDataFromHero(UBP_CharacterData_C*)`, `SetCharacterData(...)`.
- Index utiles du BM : `PlayerCharacters` (+0xB8), `Enemies` (+0xC8), `EnemiesBySpotIndex` (`TMap<int32, ABP_…*>`, +0x100), `AllPlayerCharactersBySpotIndex` (+0x4B0), `AllEnemyCharactersBySpotIndex` (+0x638), `BattleParty` (`TArray<FS_jRPG_PartySlot>`, +0x860). Recherche : `GetPlayerCharacterAtBattleSpot(int32, …&)`, `FindCharacterAtBattleSpot(ECharacterTeamSelector Team, int32 SpotIndex, …&)`, `FindCharacterStatsBattleActor(UAC_jRPG_CharacterBattleStats_C*, …&)`.
- Pour le coop : **chaque instance doit spawner les mêmes unités aux mêmes spots**. Clés d'identité stables : `(Team, BattleSpotIndex)` et `AC_jRPG_CharacterStats.CharacterHardcodedName` (FName, +0xA8). Versus a observé [prior] que les stats des unités diffèrent d'un PC à l'autre (scaling des ennemis selon les héros de la save locale). L'hôte doit donc envoyer une **fiche de stats complète** au début du combat (`CharacterBaseStats`/`CharacterCurrentStats`, voir §9).

### 2.3 Fin de combat : victoire, défaite, fuite

[dump] :

- Test : `CheckBattleEnd(bool& BattleHasEnded)` (appelle `IsBattleEnding`, `Array_IsEmpty` sur les listes [dump-locals]), `IsAFullTeamDead() -> Bool`, `CheckForGameOver()`, `IsBattleEnding() -> Bool`, `HasBattleStartedEndFlow` (+0x7A9), `TryProcessPendingBattleEnd()`, `BattleEndBlockerRefCount` (+0x6C8, `OnBattleEndBlockerLifted(bool)`).
- Flux : `StartBattleEndFlow(TEnumAsByte<EBattleEndResult::Type> BattleEndResult)`, `ForceBattleEnd(EBattleEndResult)`, `OnAllEnemiesKilled()` → `OnAllEnemiesKilledInternal()` → `OnBattleEndVictory()` → `ProcessVictoryEndFlow()`. Défaite : `OnAllHeroesKilled()` → `PerformLastStandFlow()` / `ProcessLastStandEvents(...)` → `ShowReserveTeamGameOver()` / `OnReserveTeamResponse(bool ShouldSendReserve)` → `OnRetryEncounterResponse(bool ShouldRetry)` / `OnGameOverScreenClosed()`.
- Fuite : `OnSelectFlee()`, `PerformFlee()`, `CanFleeCurrentFight(bool& FleeImpossible)`, délégué `OnBattleFlee()`, buff `/Game/Gameplay/Buffs/GenericBuff/BP_BattleBuff_Flee.BP_BattleBuff_Flee_C`. Abandon : `TriggerAbandonBattle()`, `CanAbandonBattle() -> Bool`, `AbandonRequested` (+0xB70). Retry : `CanRetryBattle() -> Bool`, `RequestRestartCurrentEncounter()`, `PrepareForEncounterRestart()`, `LoadRestartState(FS_BattleInitialState)`, `BattleRestartState` (+0xB30), et côté contrôleur `RestartCurrentEncounter(FS_BattleInitialState InitialState)`.
- `EBattleEndResult` (`/Game/Gameplay/Battle/EBattleEndResult`) : 4 valeurs 0..3, noms non dumpés. Victory/Defeat/Neutral(fuite ?)/Invalid selon [prior], **ordre à vérifier**. Le résultat est stocké dans `BM.BattleEndState` (+0x910) et lisible via `GetBattleEndState() -> Byte`.
- Last stand : `/Game/Gameplay/Battle/LastStand/BP_LastStandSystem.BP_LastStandSystem_C` (`BM.LastStandSystem` +0x738), `TryLastStand(ABP_…* , EDamageReason DeathReason, bool& HasLastStand)`, `IsLastStandActive() -> Bool`.
- Écran de game over : `/Game/UI/Widgets/HUD_Battle/SubWidget/WBP_jRPG_GameOverScreen.WBP_jRPG_GameOverScreen_C` (`PromptBattleRetry()`, `OnRetryEncounterResponse(bool)`, `OnReserveTeamResponse(bool)`).
- **Vu en jeu (2026-10-24, D31)** : `BM.GameOverScreenWidget` (+0x870) ; fenêtres de choix = `WBP_ModalPopup_C` (`ParentActivatableWidget` = l'écran, réponse par `NotifyUserResponse(EModalUIResponse)` : 1 oui, 2 non ; la fenêtre peut rester `bIsActive` après la réponse). D'abord « équipe de réserve » (`FadeInGameOverScreen` -> `OnReserveTeamModalResponse` -> `BM.OnReserveTeamResponse`), oui : la réserve entre et le combat continue (sans `StartBattleNEW`). Non : `PromptBattleRetry` si le combat est rejouable, sinon le jeu appelle seul `BM.OnRetryEncounterResponse(false)` -> `DisappearAndRemove(false)` -> `ResumeExplorationOnBattleEnd` (exploration sur place, aucun chargement ; combat lancé par une action de jeu). `RequestRestartCurrentEncounter` (réessayer) : `GetLastSavedGameData` + `RequestLoadGame` [dump-locals].

### 2.4 Récompenses

- `RollBattleRewards(FS_BattleRewards& BattleRewards)` [dump]. Locales : `local_lootContext`, `local_rolledLootEntries`, `local_currentEnemyScaling`, `local_GoldAcquired`, `local_ExperienceGained`, `CallFunc_RandomBoolWithWeight`, `RollLootTableReferences_RolledLootEntries`, `GetEnemyDataTableRow`, `GetArchetypeScalingDataTableForBattle`, `Was_Battle_No_Damage` [dump-locals]. **C'est aléatoire** : tirage à poids + `BP_LootSystemComponent_C::RollLootTable*`.
- `FS_BattleRewards` : `ExperienceGained` (int32), `GoldAcquired` (int32), `RolledLootEntries` (`TArray<FS_RolledLootEntry>`) [dump]. Stocké dans `BM.LastReceivedBattleRewards` (+0x7B0).
- Suite : `ComputeExtraLootItems()`, `AddExtraLootItem(FName ItemId, int32 Quantity, FString Reason)`, `CurrentBattleExtraLoot` (+0x7F0), `ReceiveBattleRewards(FS_BattleRewards&)`, `ReceiveBattleExperience(int32)`, `CreateBattleEndVictoryScreen(TArray<UBP_CharacterData_C*>&)`, `UpdateBattleEndVictoryScreen(...)`, `OnVictoryScreenClosedFlow()`, `TriggerPostBattleEvents()`.
- Loot : `/Game/Gameplay/Inventory/LootSystem/BP_LootSystemComponent.BP_LootSystemComponent_C` (`Controller.BP_LootSystemComponent` +0x8B0) : `RollLootTable(UDataTable*, FS_LootContext, double ChanceMultiplier, FString, TArray<FS_LootTableEntry>&)`, `RollLootTableSection(...)` (RandomFloatInRange + RandomIntegerInRange [dump-locals]).
- **Coop** : seul l'hôte appelle `RollBattleRewards` (pré-hook + remplacement de la sortie). Les clients reçoivent `FS_BattleRewards` et l'injectent par post-hook qui écrase l'out-param, ou appellent directement `ReceiveBattleRewards` [inferred].

### 2.5 Retour à l'exploration

- BM : `PreStopBattleSystems()`, `StoreAllCharacterFinalStates()` / `StoreCharacterFinalState(ABP_…*)` → `CharacterFinalStates` (`TMap<FName, FS_BattleCharacterFinalState{CurrentHP double, CurrentAP int32}>`, +0xA58), `TryFlushBattleStateToGameState()`, `ShouldUpdateCharacterDataWithFinalBattleState() -> Bool`, `StopBattleSystems()`, `UnspawnEnemies()`, `UnspawnPlayerCharacters()`, `CleanUpAfterBattle()`, `ResumeExplorationSystems(bool BeforeRestart)`, `GoBackToExploration()`, délégué `OnBattleFinished(bool Victory)` (+0x1E0), `GameActionsAfterBattle` (+0x4A0).
- Contrôleur : `ResumeExplorationOnBattleEnd(FName EnemyGlobalID, TEnumAsByte<EBattleEndResult::Type> BattleEndResult)` (s'appelait `OnBattleEnd` avant 1.4.0 [prior]), `OnBattleFlee(FName EnemyGlobalID)`, `ResumeWorldActorsAfterBattle()`, `ResetBattleState()`, `ResetBattleComponents()`.
- GI : `SetBattledEnemy(const FName& EnemyID)`, `BattledEnemies` (+0x6D8), `TransientBattledEnemies` (+0x598), `WorldTransformBeforeBattle` (+0x370), `WorldEnemiesTransformsBeforeBattle` (+0x3E8), `SpawningAfterBattle?` (+0x36C) ; spawn post-combat par `BP_SpawnPointManager_C::GetOnLoadMapTransformAfterBattle(...)`.

---

## 3. Ordre des tours

### 3.1 Modèle : initiative en temps virtuel, déterministe

[dump-locals + inferred] Le système est une **simulation d'initiative continue** :

- `AC_jRPG_CharacterBattleStats_C` :
  - `CurrentInitiative` (double, +0x180), `HasEngagementBonus` (bool, +0x498)
  - `GetCurrentSpeed() -> Double`, qui appelle `GetCurrentSpeedSimulate(bool HasEngagementBonus) -> Double`. Les locales montrent `Map_Find` dans `CharacterCurrentStats`, `Add`/`Multiply` (bonus d'engagement) et `GetBoolCVar` (cvar de debug).
  - `ComputeNewInitiativeAfterTimeSimulate(double CurrentInitiative, double Time, bool HasEngagementBonus, bool DEBUG_Verbose, double& NewInitiative)`, soit `FClamp(CurrentInitiative + Speed × Time)`.
  - `GetTimeToFullInitiativeSimulate(double CurrentInitiative, bool HasEngagementBonus, double& Time)`, soit `SafeDivide(Needed − CurrentInitiative, Speed)`. `GetTimeToFullInitiative(double& Time)` en est la version courante.
  - `AdvanceInitiativeByTime(double Time, bool DEBUG_Verbose)`, `AddInitiative(double Amount)` (`Map_Find` + `FClamp`), `RemoveInitiative(double Amount)`, `ReduceInitiative(double Amount)` (→ `BP_jRPG_Character_Battle_Base_C::OnInitiativeReduced(double)`).
- `AC_jRPG_BattleManager_C` :
  - `InitiativeNeededToTakeTurn` (double, +0xD8)
  - `GetCharacterWithMaxInitiative(bool DEBUG_Verbose, bool& Success?, ABP_jRPG_Character_Battle_Base_C*& Character, double& TimeToTurn)`. Il appelle `GetAllAliveCharacters`, `GetCharactersState(TArray<…>& Characters, TArray<double>& CharactersInitiative, TArray<bool>& CharactersEngagementBonus)` puis `GetCharacterWithMaxInitiativeSimulate(TArray<…>& Characters, TArray<double>& CharactersInitiative, TArray<bool>& HasEngagementBonus, int32 SimulationStep, bool DEBUG_Verbose, bool& Success?, …*& Character, double& TimeToTurn)`. On choisit le personnage au plus petit « time to turn ».
  - `UpdateCharacterNextTurns()` : simule N tours futurs (locales `local_SimulatedInitiativePerCharacter`, `local_EngagementBonusPerCharacter`, `TimeToFullInitiative`, `local_SortedCharacters`) et remplit `NextCharacterTurns` (`TArray<ABP_…*>`, +0x180), la file affichée par `/Game/UI/Widgets/HUD_Battle/SubWidget/TurnOrder/WBP_HUD_TurnOrder.WBP_HUD_TurnOrder_C::SetCharacters(const TArray<UAC_jRPG_CharacterBattleStats_C*>&, const UAC_jRPG_BattleManager_C*)`.
  - `UpdateBattleTurns()` (événement), `AddInitativeToAllCharacters()` (sic, événement)
  - `ForcedNextCharactersQueue` (`TArray<UAC_jRPG_CharacterBattleStats_C*>`, +0x9A8) avec `QueueForcedNextCharacter(const UAC_…*&)`, `GetForcedNextCharacters() -> Array`, `TryRemoveQueuedForcedNextCharacter(...)`. Ces tours forcés (« jouer à nouveau » / `Force Play Again(bool ShouldPlayTooltip, bool PreventChainedPlayAgain, bool& HasAppliedForcePlayAgain)` des stats) **passent avant** la simulation [inferred].
  - `TurnCount` (int32, +0x3D8, `GetTurnCount(int32&)`)

**Conséquence** : à état égal (`CurrentInitiative`, vitesse effective = `CharacterCurrentStats[vitesse]` + multiplicateurs de buffs, `HasEngagementBonus`, `ForcedNextCharactersQueue`, `TurnSkipRefCount`), l'ordre est identique sur toutes les instances. Il n'y a **pas d'aléatoire dans le choix du tour** [dump-locals : aucun `Random` dans GetCharacterWithMaxInitiative/UpdateCharacterNextTurns]. Versus a quand même dû forcer `CurrentInitiative` [prior], ce qui confirme que la dérive vient de l'état (stats, buffs), pas du tirage. Dans notre architecture, l'hôte diffuse l'identité du prochain acteur (`(team, spot)`) et les clients le **vérifient** au lieu de le recalculer.

### 3.2 Début et fin de tour

- `StartCharacterTurn(ABP_jRPG_Character_Battle_Base_C* Character)` : `local_CharacterHasChanged`, `GetCurrentHPPercentage`, `GetMaxHP`, `GetIntCVar`, **`RandomIntegerInRange`**, `GetCharacterReactionSystem`, `GetQTEManager`, cast `BP_jRPG_Enemy_Battle_Base` [dump-locals]. Le tirage alimente probablement les répliques de combat (`BP_CharacterReactionSystem`, cosmétique) [inferred, à vérifier].
- Puis `StartCharacterTurn_Phase2()` (`IsBattleEnding`, `IsTurnSkipActive`, `On Action Finished`, `AreAllDependenciesSolved`), `ProcessNextTurnStartListener()`, `OnAllTurnStartEventsProcessed()` (délégué +0xB78), `TurnStartNotifyDependencies` (+0x318), `TurnStartNextPhase` (+0x320). Le délégué `OnTurnStart(ABP_…* Character)` est en +0x338.
- Côté joueur : `OnPlayerTurnPreStart()`, `OnPlayerCharacterTurnStart()` (`TryProcessForcedSkill`, `HasForcedAutoAttack`, **`Array_Random` sur les héros vivants et les ennemis vivants** : choix de cible aléatoire en attaque automatique forcée [dump-locals]), `StartPlayerTurnChoice()`, `CurrentTurn_CanChooseAction` (+0x4C0).
- Côté personnage : `OnTurnStart()`, `OnTurnEnded()`, `OnSelfTurnStart`/`OnSelfTurnEnd` (délégués), `IsCurrentTurn(bool&)`, `IsTurnActive` (+0x135C), `OnCharacterTurnEnd(ABP_…*)`, `OnPuppetTurnEnded`. Côté stats : `OnCharacterTurnStart(UAC_jRPG_CharacterBattleStats_C* CharacterTurn)`. Côté buffs : `BP_BattleBuffComponent_C::OnCharacterTurnStart()` / `OnCharacterTurnEnd()`.
- Fin : `End Character Turn(double UsedInitiative)` (événement, nom avec espaces). Versus le termine avec `bm["End Character Turn"](bm, 1000.0)` [prior]. Puis `OnCharacterActionFinished(ABP_…*)`, `ProcessNextCharacterTurnEnd()`, `OnTurnEndEventBegin(ABP_…*)` / `OnTurnEndEventEnd()`, `ProcessAdditionalTurnEndEvent()`, `HasActiveTurnEndEvent` (+0x2C2), `CanAddTurnEndEvent` (+0x311), `RequestAdditionalTurnEnd()` / `FlushAdditionalTurnEndEvent(bool&)` côté perso, `MoveCharactersBackToTheirSpots(...)`, `WaitingForCharactersToDashBack` (+0x668).
- Passer son tour : `OnSelectSkip()`, `OnTurnSkipped()`, `AC_jRPG_ActionsManager_C::SkipTurn()`, `OnSkipTurnChosen()` (perso), `bIsSkipImpossible` (+0x9C1). Tours sautés (gel…) : `TurnSkipRefCount` (int32, +0x2EC), `AddTurnSkip()` / `RemoveTurnSkip()` / `IsTurnSkipActive() -> Bool`.

### 3.3 Contre-attaques : pseudo-tours

- BM :
  - `HasActiveCounterAttack` (+0x2C1), `CurrentlyCounteringCharacters` (+0x950), `CurrentCounteredCharacter` (+0x960)
  - `TryCounterAttack(UBP_BattleFeedbackState_C* ParryState)` (appelle `CanCharactersCounterAttackEnemySkill(ABP_…* EnemyCharacter, TArray<…>& TargetedCharacters, bool& CanCounter)`)
  - `TryGradientCounterAttack(UBP_BattleFeedbackState_C* ParryState)`, `StartCounterAttackInternal(ABP_…* Source, ABP_…* Target)`, `OnCounterAttackFinished()`
  - contre AoE : `BeginAOECounterAttack()` / `EndAOECounterAttack()` / `ComputeCharactersForAOECounterAttack() -> Array`
  - `TriggerSkillCounter(UAC_… TargetCharacterStats, UAC_… CounteringCharacterStats, TSubclassOf<ABP_Battle_SkillScript_C> Skill)`, `OnGradientCounterSuccess()`
- Personnage :
  - `ExecuteSoloCounterAttack(bool IsGradientCounter, USF_BlueprintLatentAction*)`, `ExecuteGradientCounterAttack(ABP_…* CounteredCharacter)`, `CounterAttackActionEffect()`, `GradientCounterAttackActionEffect()`, `LeadGroupCounter(...)`
  - `SetIsCounterAttacking(ABP_…*, bool IsGradient)`, `IsPerformingCounterAttack` (+0xC9C), `IsCurrentCounterAttackGradient` (+0xC9D), `CurrentCounterAttackTarget/Source` (+0xCA0/+0xCA8)
  - `CanBeCountered` (+0xCB0), `IsBeingCountered` (+0xCB1), `CounterAttackRequiresPerfect` (+0xCB2), `CurrentCounterType` (`ECounterType`, +0x1390, 4 valeurs), `StartCounterableMove(ECounterType)`, `OnCounteredStart()`, `BeginStaggerOnCountered()`, délégué `OnCounterAttacked(ABP_…*)`
- Stats : `AutomaticCounterSkill` (`TSubclassOf<ABP_Battle_SkillScript_C>`, +0x570), `CanCounterAttackCurrentEnemySkill() -> Bool`.
- **Piège** [prior] : un contre après parade complète déclenche `OnTurnStart` sans être un tour. Il faut tester `BM.HasActiveCounterAttack` avant de compter un tour (et avant de hasher l'état, §9).

---

## 4. Choix d'action

### 4.1 Types d'action

`E_jRPG_ActionType` (`/Game/jRPGTemplate/Enumerations/E_jRPG_ActionType`) : 7 valeurs **0..6**. Noms internes dans l'ordre des valeurs : `NewEnumerator6=0, NewEnumerator0=1, NewEnumerator1=2, NewEnumerator2=3, NewEnumerator5=4, NewEnumerator3=5, NewEnumerator18=6` [dump]. Libellés non dumpés. Seule valeur sûre : **3 = objets** (Versus écrit `bm.SelectedAction = 3` directement pour les objets [prior]). Les autres (attaque, compétence/magie, défense, fuite, déplacement, passer…) se déduisent des branches `ExecuteAttackAction` / `ExecuteMagicAction` / `ExecuteDefendAction` / `ExecuteItemAction` / `ExecuteFleeAction` / `ExecuteMoveAction`. **À lire en jeu** via `FL_jRPG_CustomFunctionLibrary_C::ActionTypeToText(E_jRPG_ActionType, __WorldContext, FText&)` [§14].

### 4.2 Chaîne joueur : roue → battle manager → personnage

[dump pour les signatures, prior pour l'enchaînement observé par Versus]

1. Roue LGUI : `BM.LGUI_Actor_BattleWheels` (`/Game/Gameplay/Battle/3D_UI/LGUI/BP_LGUI_UIContainer_BattleWheelRoot.BP_LGUI_UIContainer_BattleWheelRoot_C`, +0x890) et widget `WBP_HUD_BattleScreen_C` (`BM.BattleScreenWidget`, +0x2B8).
2. `AC_jRPG_BattleManager_C::SelectAction(TEnumAsByte<E_jRPG_ActionType::Type> ActionType)` écrit `SelectedAction` (+0x170). Locales : `SwitchEnum`, `GetComponentByClass`, `GetAllItemsUsableInBattle` [dump-locals].
3. `SelectSecondaryAction(FName SelectedSecondaryAction)` écrit `SelectedSecondaryAction` (FName, +0x174). C'est l'ID de compétence ou d'objet.
4. Ciblage : `OnStartTargeting()`, puis `AC_jRPG_TargetingManager_C::EnableTargeting(E_jRPG_TargetingType)` / `EnablePreTargeting()`, `ComputeTargetGroups(...)`, `ConfirmTarget()` (délégué `OnTargetConfirmed()`), `TargetedCharacters` (+0xB0), `SelectedTargetingType` (+0xAA), `CancelTargeting()`. BM : `OnTargetChanged()`, `OnStopTargeting(bool WasPreTargeting)`, `OnCancelTargeting()`, délégué `OnStartedTargeting()`. `E_jRPG_TargetingType` a 11 valeurs.
5. `RequestActionExecution(TArray<ABP_jRPG_Character_Battle_Base_C*>& TargetedCharacters)`, puis côté perso `ExecuteAction(TEnumAsByte<E_jRPG_ActionType::Type> ActionType, FName SecondaryActionName)` → `ExecuteAttackAction()` | `ExecuteMagicAction(FName MagicName)` | `ExecuteItemAction(FName ItemName)` | `ExecuteDefendAction()` | `ExecuteFleeAction()` | `ExecuteMoveAction(FName MoveName)` → `ExecuteSkillAction(FName SkillID)` → `ExecuteSkillScript(ABP_Battle_SkillScript_C*)` / `ExecuteSkillScriptFromState(UBP_SkillState_C*)` → `BP_Battle_SkillScript_C::OnExecuteSkill()` → `ExecuteSkill_Internal()`.
6. Stats : `SelectedActionType` (+0x1A0), `SelectedSecondaryActionName` (+0x1A4), `TargetedCharacters` (+0x1B0), `TriggerSkill(TArray<…>&, TSubclassOf<ABP_Battle_SkillScript_C>)`. Perso : `CurrentBattleSkill` (+0x10B8), `BattleActionQueue` (`TArray<FName>`, +0xAE0), `Acquire Targets(TArray<…>& Targets, TArray<…>& AcquiredTargets)` (Versus le hooke pour détecter le vrai départ d'une attaque [prior]), `TargetsAcquisitionOverride` (+0x12B8), `On Action Finished(bool& HasMoreActions)`.

**Widget HUD** `/Game/UI/Widgets/HUD_Battle/WBP_HUD_BattleScreen.WBP_HUD_BattleScreen_C` [dump] : `OnPlayerTurnPreStart()`, `OnPlayerTurnStart()`, `OnCharacterTurnStart(UAC_jRPG_CharacterBattleStats_C*)`, **`OnPlayerChoseAction()`**, **`OnPlayerTurnEnd()`**, `OnEnemiesTurnStart()`, `OnEnemiesTurnEnd()`, `OnInput_SkipTurnChosen()`, `OnFleeSelected()`, `OnStartTargeting()`, `OnCancelTargeting()`, `EnableWheel()`/`DisableWheel()`, `RegisterWheelInput()`/`UnregisterWheelInput()`, `RegisterTargetingInput()`/`UnregisterTargetingInput()`, `OnFreeAimStart()`/`OnFreeAimEnd()`, `ShowTooltip(FText, bool AutoFadeout)`, `OnBattleEnd()`, `AddUniqueMechanicWidget(UUserWidget*)` (`WBP_UniqueMechanic_CurrentCharacter` +0x5B8).

**Recette Versus pour jouer l'action d'une unité distante** [prior] : `bm.DEBUG_ComboAutoSuccessCount = hits` (+0x278), puis `SelectAction(t)` (ou `SelectedAction = 3`), `SelectSecondaryAction(FName)`, `RequestActionExecution(targets)`. Pour finir le tour d'une unité distante sans jouer : `BattleScreenWidget:OnPlayerChoseAction()`, `OnPlayerTurnEnd()`, `u["ControlledByBattleAI?"] = true`, `bm["End Character Turn"](bm, 1000.0)`, `u["On Action Finished"](u, {})`.

### 4.3 Compétences et objets

- Données : `/Game/Gameplay/SkillTree/BP_DataAsset_Skill.BP_DataAsset_Skill_C`, état runtime `/Game/Gameplay/Battle/SkillStateSystem/BP_SkillState.BP_SkillState_C` [dump] : `IsOvercharged` (+0x30), `SkillDefinition` (+0x38), `SkillScriptInstance` (+0xA8), `GetSkillCost() -> Int`, `AddSkillCostModifier(int32 DeltaCost, …&, FString)`, `SetOvercharge(bool, bool ForcedOverchargeFeedback)`, `UpdateSkillState()`. Map par perso : `Stats.SkillStates` (`TMap<UBP_DataAsset_Skill_C*, UBP_SkillState_C*>`, +0x588), `Stats.Skills` (+0x578), `FindEquippedSkillByID(FName SkillID, …&)`, `BM.GetEquippedCharacterSkills(...)`, `GetEquippedCharacterGradientAttacks(...)`.
- Script : `/Game/Gameplay/Battle/Skills/BP_Battle_SkillScript.BP_Battle_SkillScript_C` (AActor) [dump] : `CurrentCharacter`, `SkillState`, `TargetCharactersOverride`, `IsCostCanceled`, `ExecutionCount`, `AllowPlayerChoice`, `ValidateSkillTargets(...)`, `ValidateSkillAvailability(bool&, FS_BlockedActionFeedback&)`, `AcquireTargets(TArray<…>& Desired, TArray<…>& Acquired)`, `ApplyBuff(TSubclassOf<UBP_BattleBuffInstance_C>, UAC_… Target, int32 TurnDuration, UAC_… SourceIfNotSelf, EBuffApplicationProbability, UBP_BattleBuffInstance_C*&)`, `OnActionEffect()`, `ForceSkillOnNextTurn(bool AllowPlayerChoice)`, délégué `OnSkillExecutionFinished()`. Les scripts de chaque compétence (sous-classes) ne sont pas dans ce dump.
- Compétence forcée : `Stats.ForcedSkillOnNextTurn` (+0x708), `ForceSkillOnNextTurn(ABP_Battle_SkillScript_C*)`, `TryProcessForcedSkill(bool& PreventPlayerChoice)`. Attaque auto forcée : `ForceAutoAttackRefCount` (+0x6D8), `HasForcedAutoAttack() -> Bool`.
- Objets : `BM.SelectedAction = 3`, `GetAllItemsUsableInBattle(TArray<FS_jRPG_Item_DynamicData>&)`, `ConsumeBattleItem(FName ItemHardcodedName, int32 Amount)`, `FindBattleItemCount(FName, int32&)`, `BattleItemsCount` (+0xA08), `GetBattleItemSkills(...)`. Côté stats : délégué `OnConsumableActionEffect(UAC_… Source, TArray<…>& Targets, FName ShardID, FName ConsumableID)`. Vol : `TryStealConsumableFromPlayer(UAC_… Thief, FDataTableRowHandle, int32 Quantity, int32& RemovedQuantity)`.
- `AC_jRPG_ActionsManager_C` (`Controller.AC_jRPG_ActionsManager` +0x918) : `SelectAction(E_jRPG_ActionType, FName SecondaryActionName)`, `GetActionData(FName, FS_jRPG_Action&, bool&)`, `GetActionCost(FName, double& CostHP, double& CostMP)`, `AttackStatCalculation(...)`. C'est un vestige du template jRPG, encore utilisé pour fuite et passage de tour [inferred].

### 4.4 IA

- `BP_jRPG_Character_Battle_Base_C::PerformActionByAI()` (événement) et `PerformActionByAI_Internal()` (seule locale : `IsPackagedForDistribution`, d'où une logique surtout surchargée dans les BP d'ennemis) [dump]. Drapeau `ControlledByBattleAI?` (+0x978). Événement de routage : `BP_EventRoutingSystemComponent_AISkills` (+0x6C8, `/Game/Gameplay/Battle/EventRoutingSystem/BP_EventRoutingSystemComponent.BP_EventRoutingSystemComponent_C`), `DeclareSkillEvent(...)`, `ExecuteSkillEvent(FFRoutedEventHandle&)`, `DeclareCharacterSkillEvents()`.
- Ciblage IA (`BP_jRPG_Enemy_Battle_Base_C`) [dump] :
  - `AcquireTargetRandomHero(…*& Output)`, `GetRandomEnemy(…*&)` (**RandomIntegerInRange**)
  - `AcquireSpecificTarget(int32 DesiredTargetIndex, bool AcquireRandomTargetIfWantedTargetDead, …*&, bool& IsTargetDead)`, `AcquireTargetMiddleHero(bool RandomIfDead, …*&)`
  - `AcquireTargetAllHeroes(bool OnlyAlive, TArray<…>&)`, `AcquireTargetWithLowerHealth/HigherHealth(…*&)`, `AcquireTargetAllEnemies(...)`
  - `DEBUG_RandomHeroAcquireOverride` (+0x15A8 ennemi, +0x6F8 BM), override de cible exploitable pour forcer la cible de l'hôte [inferred]
  - helpers `/Game/Gameplay/Battle/Targeting/FL_TargetingHelpers.FL_TargetingHelpers_C` : `GetRandomAliveHero(__WorldContext, …*&)` (**Array_Random**), `GetRandomCharacterByBuffCritera(...)` (**Array_Random**), `GetSpecificHero(int32, bool RandomIfDead, …)`, `GetMiddleHero(bool RandomIfDead, …)`
- **Coop** : l'IA tourne **uniquement chez l'hôte**. Chez les clients, on bloque `PerformActionByAI` (pré-hook qui n'exécute pas) et on rejoue l'action reçue (compétence + cibles) via la même chaîne que §4.2. Point délicat : les skill scripts d'ennemis choisissent leurs cibles à l'intérieur du script (`AcquireTargets`) [inferred]. Il faut donc imposer les cibles avec `TargetsAcquisitionOverride` (perso) ou `TargetCharactersOverride` (script), ou bien hooker les fonctions `AcquireTarget*` / `GetRandom*` pour renvoyer le résultat de l'hôte.

---

## 5. Calcul des dégâts

### 5.1 Chaîne

```
[attaquant] PerformStandardHit(bool CanBeCountered, bool CanBeJumped, bool RequiresGradientCounter, double DamageMultiplier, EAttackType DamageElement, bool& HasHit, TArray<UBP_BattleDamages_C*>& BattleDamages)
         └ PerformStandardHitOnCharacter(ABP_… TargetCharacter, ...same..., bool& HasHit, UBP_BattleDamages_C*& BattleDamages)
              └ [cible] AC_jRPG_CharacterBattleStats_C::ReceiveDamage(
                    double FinalDamageMultiplier, TScriptInterface<IIBattleDamageSource_C> DamageSourceCharacter,
                    TEnumAsByte<EAttackType::Type> AttackType, bool Simulated Damage (No Damage will be dealt),
                    double Straight Damage (ignores calculation), bool IsCounterDamage,
                    TEnumAsByte<EDamageReason::Type> DamageReason, FVector DamageLocation, FString DebugReason,
                    double& DamageDealt, bool& HasHit, bool& DodgeSuccess, bool& ParrySuccess, bool& QTESuccess?,
                    UBP_BattleDamages_C*& BattleDamages)
                    └ FL_BattleHelpers_C::MakeDamageBuilder(UAC_… TargetCharacterStats, EAttackType AttackElement, double FinalDamageMultiplier,
                                                            double BreakBarDamageMultiplier, TScriptInterface<IIBattleDamageSource_C> DamageSource,
                                                            EDamageReason DamageReason, FString DebugReason, __WorldContext) -> Object (UBP_BattleDamageBuilder_C)
                    └ ReceiveDamageFromObject(double Straight Damage (ignores calculation), UBP_BattleDamageBuilder_C* DamageBuilder,
                                              double& DamageDealt, bool& HasHit, bool& DodgeSuccess, bool& ParrySuccess,
                                              UBP_BattleDamages_C*& BattleDamages)
```

[dump pour les signatures. Chaînage PerformStandardHitOnCharacter → ReceiveDamage → MakeDamageBuilder → ReceiveDamageFromObject déduit des locales `CallFunc_ReceiveDamage_*`, `CallFunc_MakeDamageBuilder`, `CallFunc_ReceiveDamageFromObject_*`]. Le constructeur peut aussi être utilisé directement : `UBP_BattleDamageBuilder_C::DealDamages(bool& HasHit, UBP_BattleDamages_C*& BattleDamages)`.

**Corps de `ReceiveDamageFromObject`** [dump-locals + prior décompilé] :

- Locales : `local_CriticalRoll`, `local_isParrySuccessful`, `local_isDodgeSuccess`, `local_hasHitReached`, `local_ActualDamageDealtOnShield`, `local_shieldDamageMultiplier`, `local_newHPValue`, `local_CounterSkillToTrigger` (ClassProperty), `local_LockedAttackType`, `local_Critical?`.
- Appels : `TryParryHitFromCharacter`, `TryDodgeHitFromCharacter`, **`TryRollEnemyDodgeFromStats`**, **`RollCriticalHit`**, **`RandomFloatInRange`**, `ApplyCriticalHitModifiers`, `GetCurrentShieldPoints`, `TryConsumeShieldPoints`, `GetIgnoreShields`, `ReduceHP`, `IsInImmunityPhase`, `GetBoolCVar` ×2 + `IsDebugModeEnabled` (cvars `Sandfall.AutoJump`…), `TryGetTargetedAllies`, `GetDistanceTo` + `GetScaledCapsuleRadius` (test de portée/AoE [inferred]), `GetCharacterReactionSystem`.
- Branche jump [prior, AutoParry.cpp] : `if (DamageSourceCharacter->IsCurrentAttackJumpable)` alors (cvar AutoJump) ou (`self->IsJumping && self->IsInImmunityPhase()`) donne `BeginJumpCounterAttackWindow(attacker); BattleDamages->Cancel(3 /*EMissReason*/, "Jumped"); DodgeSuccess = true;`. Sinon `StopAnimMontage(AnimJump)` et le coup porte. Hors jump : `TryParryHitFromCharacter(...)` puis `TryDodgeHitFromCharacter(...)`.

### 5.2 `UBP_BattleDamageBuilder_C` (`/Game/Gameplay/Battle/BP_BattleDamageBuilder.BP_BattleDamageBuilder_C`, UObject, 0xEC) [dump]

Entrées :

- `TargetCharacterStats`, `INPUT_CriticalHitDamageMultiplier`, `INPUT_CriticalHitChanceModifier`, `INPUT_HitLocationOverride`, `INPUT_BreakBarMultiplier`
- `AttackElement` (`EAttackType`), `AttackPowerMultiplier`, `FinalDamageMultiplier`, `DamageSource` (interface), `DamageReason`
- `INPUT_RegularHitChanceModifier`, `INPUT_UseDefenseAsAttackPower`, `StraightDamage`, `INPUT_Deathtouch`
- `IgnoreMarkedRemoval`, `IgnoreShields`, `ForcedCriticalHit`, `BypassModifiers`

Méthodes `Add*` / `Set*` / `Get*` correspondantes : `AddCriticalHitChanceModifier(double, FString)`, `AddFinalDamageMultiplier(double, FString)`, `AddRegularHitChanceModifier(double, FString)`, `AddStraightDamageFlat(double, bool NullifyBaseDamage, bool KeepModifiersOnNullify, FString)`, `AddStraightDamageAsMaxHPPercent(...)`, `SetForcedCriticalHit()` / `GetForcedCriticalHit() -> Bool`, `SetIgnoreShields()`, `SetAsDeathtouch()`, `NullifyBaseDamage(FString)`, `UseDefenseAsAttackPower()`, `LoadFromData()`.

**`SetForcedCriticalHit()` est le point d'injection naturel pour imposer un crit décidé par l'hôte** [inferred].

### 5.3 `UBP_BattleDamages_C` (`/Game/Gameplay/Battle/BP_BattleDamages.BP_BattleDamages_C`) [dump]

Parent déclaré : `USF_ExtendedUserWidget` (curieux mais tel quel dans le dump), taille 0x390. Champs :

- `CurrentInputDamages_Base`, `SourceCharacter` / `TargetCharacter` (`UAC_jRPG_CharacterBattleStats_C*`)
- `IgnoreTargetDefense`, `InputAttackElement` / `FinalAttackElement`, **`IsCriticalHit`** (+0x304), `DamageReason` (+0x305), `DamageWorldLocation`
- `DamageBaseBeforeTargetHPClamp`, `BaseTypeAffinity` (`EElementalAffinity`), `InputDamageMultiplier`, **`CriticalRoll`** (double, +0x340)
- `ActiveModifiers` (`TArray<FFDamageModifier{Type,Value,AffectsBaseDamages,Reason}>`), `ValueState_AttackElement` / `ValueState_CriticalHitRoll` (`EDamageValueState`), `FinalLock`
- **`IsAbsorbedByShield`**, `ExtraCriticalHitChance`, `ExtraCriticalHitDamageMultiplier`, `BreakBarDamageMultiplier`, `SourceDamageBuilder`, **`MissReason`** (`EMissReason`, 8 valeurs, +0x380), `FinalDamageLimit`

Pipeline (noms) : `Init()` → `LoadFromBuilder(UBP_BattleDamageBuilder_C*)` → `ProcessBaseElement()` → `InitAffinities()` / `SetAffinities(...)` / `ApplyAffinitiesModifiers()` → **`RollCriticalHit(bool& IsCriticalHit)`** → `ApplyCriticalHitModifiers(bool& IsCriticalHit)` → `ApplyDefense()` / `CalculateDefense(double AttackPower, double Defense) -> Double` → `ApplyActiveModifiers()` / `ComputeDamageModifiers(...)` → `ApplyCommonDamageMultipliers()` → `ApplyStraightDamages(double)` → `ApplyDebugOverrides()` → `ApplyClamp()` → `ApplyTargetClampAndRounding()` → `InitFinalDamages()` → `NotifyListeners()` → `Finish()`. Annulation : `Cancel(TEnumAsByte<EMissReason::Type> MissReason, FString DebugReason)`. Lecture : `GetFinalDamages(double&)`, `GetIsCriticalHit(bool&)`, `GetIsWeakness(bool&)`, `GetIsResistant(bool&)`, `GetFinalBreakBarDamageAmount()`, `GetMissReason() -> Byte`, `SetIsCriticalHit(bool, FString)`, `OverrideWithEffectiveDamage(double FinalDamage)`.

Les buffs interviennent via le `BP_BattleBuffComponent_C` de la source et de la cible : `ProcessDealtRawDamages`, `ProcessReceivedRawDamages`, `ProcessFinalDamages`, `ProcessDealtDamageCriticalHitChance(UBP_BattleDamages_C*)`, `ProcessAttackElement`, `ProcessPermeate`.

### 5.4 Critiques : où ils se décident

[dump-locals] `RollCriticalHit` : `local_blockCritical`, `local_forceCritical`, `GetForcedCriticalHit`, `GetConsoleVariableBoolValue` + `IsPackagedForDistribution` (cvar de debug inactive en shipping), `GetCriticalRollToBeat_Value`, **`RandomFloatInRange`**, `LessEqual`. Soit `crit = forced || (!blocked && Random[0,1] <= RollToBeat)` [inferred]. `GetCriticalRollToBeat(double& Value)` vaut `Map_Find(stat crit)` × … + `ExtraCriticalHitChance`. `ReceiveDamageFromObject` a en plus sa propre `local_CriticalRoll` + `RandomFloatInRange`. Les deux tirages possibles sont à capturer. Blocage global : `BM.DisableCriticalHit` (bool, +0x421).

### 5.5 Esquive « de stats » (ennemis)

`TryRollEnemyDodgeFromStats() -> Bool` [dump-locals] : `IsDeathTouch`, **`RandomFloat`**, `IsFlyingCharacter`, `Greater`, `EqualEqual_ByteByte`. C'est un tirage d'esquive aléatoire (personnages volants / stat d'esquive) qui donne un raté (`MissReason`).

### 5.6 Boucliers

Stats : `CurrentShieldPoints` (int32, +0x170), `GetCurrentShieldPoints(int32&)`, `SetCurrentShieldPoints(int32 NewValue, FString DebugReason)`, `AddShieldPoints(int32 Amount, FString)`, `TryConsumeShieldPoints(int32 Amount, FString, bool& Success)`, délégués `OnShieldPointsChanged(int32 Current, int32 Previous)` (stats) et `OnShieldPointsIncreased(UAC_…, int32 New, int32 Prev)` (BM). `FFCharacterStats.ShieldPointsAtSpawn`. Un coup absorbé met `BattleDamages.IsAbsorbedByShield` et déclenche `OnDamageAbsorbed(int64 Amount, UBP_BattleDamages_C*)`. Perso : `Shield_Explosion()`, `ShieldAdded()`.

### 5.7 Barre de brise (break) et étourdissement

Stats : `StunBreakBarCurrent` (int64, +0x468), `StunBreakBarMax` (int64, +0x470), `BreakDamageBlockerRefCount` (+0x478), `IsStun` (+0x414). Méthodes :

- `InitBreakBar(int32 BreakBarHPPercentage)`, `GetCurrentBreakBarPercent() -> Double`
- `AddBreakBarDamage(int64 DamageValue, UAC_… Source, FString Reason)`, `AddBreakBarDamagePercentage(double, …)`, `SetBreakBarDamagePercentage(double, …)`, **`SetCurrentBreakBarDamage(int64 NewValue, UAC_… SourceCharacter, FString Reason)`**
- `CanPerformBreakStun() -> Bool`, `TryPerformBreakStun(UAC_… Source, E_BreakStunReason Reason, bool& Success)`, `PerformBreakStun(...)`, `GetStunBuffClass() -> Class`
- délégués `OnBreakDamageReceived(...)`, `OnBreakBarStun(UAC_…)`, et BM `OnCharacterBreakStunned(UAC_… Stunned, UAC_… Source)`

L'étourdissement est un buff : `/Game/Gameplay/Buffs/GenericBuff/BP_BattleBuff_Stun.BP_BattleBuff_Stun_C`. Le composant de compétence `/Game/Gameplay/SkillTree/SkillDataComponents/Shared/PerformBreakStun/BP_SkillDataComponent_PerformBreakStun.BP_SkillDataComponent_PerformBreakStun_C` existe.

### 5.8 PV, mort, réanimation

- PV :
  - `CurrentHP` (double, +0x168), `GetMaxHP(double&)`, `SetHP(double HP Value)`
  - `ReduceHP(double Amount, double& NewHPValue, double& EffectiveDamage)`, `RecoverHP(double RecoveryAmount, UAC_… Source, bool PlayVFX, EHealReason Reason, FString)` (objet `/Game/Gameplay/Battle/BP_BattleHeal.BP_BattleHeal_C`, `ModifyAmount(double, FString)`, délégué BM `OnCharacterHealSuccessful(UBP_BattleHeal_C*)`)
  - délégué `OnHPChanged(double NewHP, double PreviousHP)`
  - `SimulatedDamage?` (+0x2E8), `DEBUG_IgnoreDamages` perso (+0xCF8), `Sandfall_HeroesIgnoreDamage(bool)` BM
- Mort :
  - stats : `CheckForDeath(UBP_BattleDamages_C*)`, `OnKilledByDamage(UBP_BattleDamages_C*)`, **`SetDead()`** (`SpawnObject` [dump-locals], donc un événement), `SetDead_Internal()`, `NotifyDeath(bool DeathCounter)`, `IsDead() -> Bool`, `bIsDeathQueuedForTurnEnd` (+0x762) / `IsDeathQueuedForTurnEnd() -> Bool`, `PreventDeathRefCount` (+0x548), délégués `OnDeath()` et `OnDeathDamagePrevented(UObject*)`
  - perso : `Dead?` (bool, +0x999), `GetIsDead(bool&)`, `MakeDead_Internal()`, `OnDeath(bool DeathByCounter)`, `OnDeathPublic(bool)`
  - BM : `OnEnemyKilled(ABP_…*)`, `OnPlayerCharacterKilled(ABP_…*)`, `Internal_MakePlayerCharacterDead(...)`, `DeadPlayerCharacters` (+0x150), `DeadEnemies` (+0x160), `LastFatalDamageReceivedReason` (+0x898), délégués `OnCharacterKilled`, `OnEnemyKilledEvent`, `IsProcessingKilledCharacter` (+0x7E0)
- Réanimation :
  - stats : **`Resurrect(double HP Amount, UAC_… SourceCharacter)`** (`ShouldQueueRevive`, construit `S_ResurrectParameters{TargetCharacter, SourceCharacter, HPPercentage}`), `ResurrectInternal()`, `HasPendingResurrect(bool&)`, `NextResurrectParameters` (+0x678)
  - BM : `RegisterPendingResurrect(...)`, `ShouldQueueRevive() -> Bool`, `ProcessNextPendingResurrect(...)`, `PendingResurrects` (+0x7D0), `OnPlayerCharacterResurrected(ABP_…*)`, délégué `OnResurected(ABP_…*)`
  - perso : `OnResurrected(double)`, `OnResurrectedPublic(double)`, `ResurectAnimation`
- Expulsion / réintégration : BM `KickCharacterFromBattle_Internal(...)`, `KickedCharacters` (`TMap<FGuid, FFBattleCharacterStoredState>`, +0x678), `ReintegrateCharacterToBattle(FGuid&, …*&)`.

### 5.9 Buffs et statuts

- Composant `/Game/Gameplay/Buffs/BP_BattleBuffComponent.BP_BattleBuffComponent_C` (UObject, pas un ActorComponent ; `Stats.BattleBuffComponent` +0x228) :
  - `ActiveBattleBuffs` (`TArray<UBP_BattleBuffInstanceWrapper_C*>`, +0x30), `StandbyBattleBuffs` (+0x48)
  - `ActivateBuff(UBP_BattleBuffInstance_C*& BuffInstance, UAC_…*& SourceCharacter, UBP_BattleBuffInstanceWrapper_C*& Wrapper, bool& Success)`, `AddBuff(...)`, `RemoveBuffInstance(UBP_BattleBuffInstance_C*)`, `RemoveBuffWrapper(...)`, `RemoveAllBuffsOfType(E_BuffAndStatusEffectTypes)`, `RemoveAllBuffsFromFamily(...)`
  - `CreateBuffStack(TSubclassOf<…>, int32 Count, int32 TurnDuration, UAC_… Source, TArray<…>&)`, `TryConsumeBuffs(...)`, `RemoveBuffStacks(...)`, `TryRemoveBuffStacks(...)`
  - `GetActiveBuffsByClass(...)`, `GetActiveBuffCounts(TSubclassOf<…>) -> Int`, `HasBuffOfClass(...) -> Bool`, `GetBuffInstancesByType(E_BuffAndStatusEffectTypes, TArray<…>&)`, `GetStatusEffectBuffInstance(...)`, `FindActiveBuffsByInstigator(...)`
  - `OnCharacterTurnStart()`, `OnCharacterTurnEnd()`, `ProcessRemovedBuffs()`, `On Stance Change(...)`, `On Perfection Changed(...)`, `OnGradientGaugeChange(...)`
- Wrapper `/Game/Gameplay/Buffs/BP_BattleBuffInstanceWrapper.BP_BattleBuffInstanceWrapper_C` : `BattleBuffInstance` (+0x28), `StatMultipliers` (`TArray<FGuid>`), `CurrentTurnHitCount`, `IsActive(bool&)`, `HasExpired(bool&)`, `ApplyBuffOnTurnEnd()`, `DeactivateBuff()`.
- Instance `/Game/Gameplay/Buffs/BP_BattleBuffInstance.BP_BattleBuffInstance_C` (0x151) :
  - **`TurnDuration`** (int32, +0x30), `TargetCharacterStats`, **`IsPermanent`** (+0x40), `CharacterStatMultipliers` (`TArray<FFStatMultiplierData{StatType, Multiplier}>`)
  - `StackingType` (`E_jRPG_BuffStackingType`), `BuffFamily`, `IsMarkedForRemoval`, **`Type`** (`E_BuffAndStatusEffectTypes`, 6 valeurs, +0xA1), `SourceCharacterStats`, **`StackCount`** (+0xD0)
  - `AggregatorBuffInstance` / `AggregatorBuffClass`, `BuffInstigator`
  - `GetStackCount(int32&)`, `ChangeStackCount(int32)`, `SetTurnDuration(int32)`, `SetPermanent()`, `MarkBuffForRemoval()`, `ActivateBuff(const UAC_…*&)`, `DeactivateBuff()`, `ApplyOnTurnEnd()`
  - hooks d'effet `ProcessReceivedRawDamages` / `ProcessDealtRawDamages` / `ProcessReceivedFinalDamages(…, int32 CurrentTurnHitCount, bool&)` / `ProcessAPChange` / `OnPerfectionChanged` / `OnStanceChange` / `ProcessHeal` / `OnSuccessfulParry` / `OnSuccessfulDodge` (interface `/Game/Gameplay/BPI_BattleListener.BPI_BattleListener_C`)
- **Point d'entrée d'application** : `AC_jRPG_CharacterBattleStats_C::ActivateBuff(UBP_BattleBuffInstance_C* BuffInstance, TEnumAsByte<EBuffApplicationProbability::Type> StatusEffectProbability, UAC_jRPG_CharacterBattleStats_C* SourceCharacter, bool& Success)`. Locales : `local_buffRoll`, `local_maxValidRoll`, `GetProbabilityToReceiveBuff(UAC_… Source, EBuffApplicationProbability, UBP_BattleBuffInstance_C*) -> Double`, **`RandomFloatInRange`**, `GreaterEqual`, `WasPrevented` (objet `/Game/Gameplay/Battle/Action_AddBuff.Action_AddBuff_C` avec `PreventAddBuff(FText)`). L'application d'un statut est donc **aléatoire** sauf en probabilité 4 = « Forced » [prior Versus]. Délégués stats `OnBuffAdded(UBP_BattleBuffInstance_C*)` / `OnBuffRemoved(...)`.
- Raccourcis : `BP_jRPG_Character_Battle_Base_C::ApplyStandardStatusEffect(E_StatusEffects, int32 TurnDuration, EBuffApplicationProbability, bool Condition, bool& Success)` / `ApplyStandardStatusEffectOnTarget(...)`, `BP_Battle_SkillScript_C::ApplyBuff(...)`. Buffs globaux de combat : `BM.BattleModifiersBuffComponent` (+0x858), `AddBattleModifierBuff(UBP_BattleBuffInstance_C*, bool ForcePermanent)`. Brûlure : `BP_BattleBuff_Burn_C` + agrégateur `BP_BattleBuff_BurnAggregator_C`, dégâts natifs `/Script/SandFall.SF_BattleHelpers::ComputeBurnDamage(int32 StackCount, double BaseDamage, double FinalMultiplier, int32 PreviousCumulatedStackCount, double PreviousTotal)`.

---

## 6. Aléatoire

### 6.1 Constat global

- **Aucun `FRandomStream` dans les classes de combat** [dump]. Les seules propriétés `FRandomStream` du dump sont `ABP_Humanoid_NPC_C::Stream` (anim de PNJ). Les `MakeRandomStream` / `*FromStream` se trouvent seulement dans l'interactif d'environnement, les prises d'escalade (`BP_Holds_C:Seed`) et les anims faciales. **Tout l'aléatoire de combat passe par les nœuds Kismet non seedés** (`UKismetMathLibrary::RandomFloat`, `RandomFloatInRange`, `RandomInteger`, `RandomIntegerInRange`, `RandomBool`, `RandomBoolWithWeight`, `UKismetArrayLibrary::Array_Random`), donc par le RNG global `FMath::Rand`/`FRand`.
- Ce RNG est seedable globalement (`FMath::RandInit(seed)` / `SRandInit`) [inferred, moteur]. Mais il est **partagé** avec les tirages cosmétiques (répliques, audio, caméras, transitions), qui ne se produisent pas dans le même ordre ni le même nombre de fois sur chaque machine. **Seeder ne suffit pas à synchroniser.** Conclusion : l'hôte calcule, les clients forcent le résultat (hook post sur la fonction de tirage ou sur son consommateur).
- Le module natif `/Script/SandFall` n'expose **aucune** fonction de tirage de combat [dump : `USandfallBlueprintHelpers`, `USF_BattleHelpers` (seulement `ComputeBurnDamage`), `UMathHelpers` sans Random].

### 6.2 Inventaire des tirages en combat (fonction → nœud aléatoire) [dump-locals]

| Fonction (classe) | Nœud | Effet | Coop |
|---|---|---|---|
| `BP_BattleDamages_C::RollCriticalHit(bool& IsCriticalHit)` | `RandomFloatInRange` | **crit** | hôte décide → client : post-hook forçant `IsCriticalHit` (ou `SetForcedCriticalHit` sur le builder / `SetIsCriticalHit`) |
| `AC_jRPG_CharacterBattleStats_C::ReceiveDamageFromObject(...)` | `RandomFloatInRange` (+ `local_CriticalRoll`) | crit et/ou dispersion des dégâts [inferred] | forcer le résultat final (`OverrideWithEffectiveDamage` / écriture PV) |
| `BP_BattleDamages_C::TryRollEnemyDodgeFromStats() -> Bool` | `RandomFloat` | **esquive « de stats » / raté** | post-hook forçant le Bool de retour (RESULT_DECL) |
| `AC_jRPG_CharacterBattleStats_C::ActivateBuff(...)` | `RandomFloatInRange` vs `GetProbabilityToReceiveBuff` | **application de statut** | hôte décide, client appelle avec probabilité 4 (Forced) ou force `Success` |
| `AC_jRPG_BattleManager_C::OnPlayerCharacterTurnStart()` | `Array_Random` ×2 (héros/ennemis vivants) | cible d'une attaque automatique forcée | rejouer la cible de l'hôte |
| `AC_jRPG_BattleManager_C::StartCharacterTurn(...)` | `RandomIntegerInRange` (+ `GetIntCVar`) | probablement réplique de début de tour [inferred] | à vérifier : si gameplay, synchroniser |
| `AC_jRPG_BattleManager_C::RollBattleRewards(FS_BattleRewards&)` | `RandomBoolWithWeight` + `BP_LootSystemComponent_C::RollLootTable*` (`RandomFloatInRange`, `RandomIntegerInRange`) | **loot** | hôte seulement |
| `AC_jRPG_BattleManager_C::SetupBattleMap()` | `RandomFloatInRange` | placement/caméra [inferred] | cosmétique |
| `AC_jRPG_BattleManager_C` ubergraph | `Array_Random` | inconnu (ubergraph) [§14] | à identifier |
| `BP_jRPG_Character_Battle_Base_C` ubergraph | `Array_Random` ×4 | choix d'anim / séquence d'attaque (`Attackx1Sequence` est un `TArray<ULevelSequence*>`, `AnimDodge`, `CounterSequence`, `AnimAOECounter`) [inferred] | cosmétique mais **durée de séquence ≠ ⇒ timing QTE ≠** : à imposer (§8) |
| `BP_jRPG_Enemy_Battle_Base_C::GetRandomEnemy(...)`, `AcquireTargetRandomHero(...)` | `RandomIntegerInRange` | **cible IA** | hôte |
| `FL_TargetingHelpers_C::GetRandomAliveHero`, `GetRandomCharacterByBuffCritera` | `Array_Random` | **cible** (IA / compétences) | hôte |
| `/Game/Gameplay/EpicBattleSequence/SkillSequences/Common/Skill_AOECounter_V1.Skill_AOECounter_V1_DirectorBP_C` | `Array_Random` | séquence du contre AoE | cosmétique/timing |
| `BP_jRPG_Enemy_World_Base_Seamless_C::GetBattleCameraTransition` | `Array_Random` | caméra de transition | cosmétique |
| `GenericFunctionLibrary_C::GetRandomOtherObjectFromArray` (`/Game/Gameplay/GenericFunctionLibrary`) | `Array_Random` | utilitaire, appelants inconnus | à surveiller |
| `BP_CharacterReactionSystem_C` (`/Game/Gameplay/CharacterReactions/...`), `BP_BattleLineGatherer_C::RollChancePercent/RollFromPool/SelectBattleLineCandidate`, `BP_BattleLineSettings_C::RollChanceValue`, `BP_BattleLinesModel_Base_C::TryRegisterPool`, `AN_Ono_C`, `AN_ENM_IsBattle_C`, `BP_World_Audio_Component_C`, `WBP_BattleTransition_C`, `AC_jRPG_DynamicCameraManager_C::SetNewDynamicPath` | divers | **cosmétique** (répliques, sons, caméra) | ignorer, mais ils consomment le RNG global |
| `BP_Chest_Regular_C::ComputeEncounterLevelOverride`, `RollChestItems` | `RandomIntegerInRange` | loot de coffre (exploration) | hôte |

Non visibles dans ce dump (classes non chargées) : les skill scripts propres à chaque compétence (sous-classes de `BP_Battle_SkillScript_C`), les BP de héros et d'ennemis spécifiques, les buffs spécifiques. **Second dump en combat requis** pour compléter (même requête grep `CallFunc_Random*|Array_Random|RandomStream` sur les propriétés de l'ObjectDump).

### 6.3 Stratégie de hook recommandée [inferred]

- Ne pas essayer de rejouer les tirages. Hooker les **consommateurs** (`RollCriticalHit`, `TryRollEnemyDodgeFromStats`, `ActivateBuff`, `GetRandom*` / `AcquireTarget*`, `RollBattleRewards`) : chez l'hôte en post (journaliser le résultat), chez le client en post (écraser `IsCriticalHit` / RESULT_DECL / out-param par la valeur reçue).
- Garde-fou indépendant : après chaque coup, l'hôte envoie le **résultat net** (PV, bouclier, brise, buffs de la cible), comme `DEF.resultOf`/`writeResult` de Versus [prior], et le client l'applique.

---

## 7. QTE défensifs : parade, esquive, saut, contre gradient

### 7.1 Entrées et routage

- Actions d'entrée [dump] : `/Game/Gameplay/Inputs/InputMappingProfiles/Battle/InputActions/IA_Battle_Defense_Parry`, `…/IA_Battle_Defense_Dodge`, `…/IA_Battle_Defense_Jump`, `…/IA_Battle_Defense_JumpCounter`, `…/IA_Battle_Defense_GradientCounter` (contexte `/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_BattleDefense`).
- Liées dans **`BP_jRPG_Controller_World_C`** (`InpActEvt_IA_Battle_Defense_{Parry,Dodge,Jump,JumpCounter,GradientCounter}_K2Node_EnhancedInputActionEvent_N(FInputActionValue, float ElapsedTime, float TriggeredTime, const UInputAction*)`). `TryBindDefendInput()` / `UnbindDefendInput()`, `IsDefenseInputBound` (+0x9B1), `OnDefendAction()` / `OnDefendReleased()` [dump].
- Le contrôleur relaie à **`AC_jRPG_BattleManager_C`** : `OnParryAction()`, `OnDodgeAction()`, `OnJumpAction()`, `OnGradientCounterAction()`, `OnJumpShot()`, `OnDefendStart()` / `OnDefendStop()`, `SetDefenseState(bool WantedDefense)`, `UpdateDefend(FText DebugReason)`, `UpdateDefenseControls()`, `WantedDefense` (+0x2C0), `DefendingHeroes` (+0x1F8), `PreviousDefendingHeroes` (+0x208), `BlockAllDefensiveMoves` (+0x420). Ce sont des événements [dump]. Le relais BM → `TryStart*` sur le(s) héros visé(s) est [inferred / prior Kouzukii : `CurrentCharacter` → `Is Attacking Heroes(ABP_jRPG_Enemy_Battle_Base_C*, bool& IsAttacking, TArray<…>& TargetedHeroes)`].

### 7.2 Démarrer un mouvement défensif (personnage, `BP_jRPG_Character_Battle_Base_C`)

[dump, locales entre parenthèses]

- `TryStartParry(bool& Success)` (`CanDoDefensiveMove`, `IsParryLocked`, `GetGameTimeInSeconds`, `GreaterEqual`, `Divide`, `PlayAnimMontage`)
- `TryStartDodge(bool& Success)` (`local_dodgeDuration`, `local_dodgeWindowMultiplier`, `GetDodgeImmuneDuration`, `IsDodgeLockActive`, `Percent_IntInt` sur `AnimDodge` → `DodgeAnimIndex` +0xF64, `FMax`, `PlayAnimMontage`)
- `TryStartJump(bool& Success)` (`HasUnlockedJump() -> Bool`, `CanDoDefensiveMove`, `PlayAnimMontage`)
- `TryStartGradientParry(bool& Success)` (`HasUnlockedGradientParry`, `CanDoDefensiveMove`, `PlayAnimMontage`)
- `StartDefensiveMove(double LockDuration, double ImmuneDuration, double WarmUpDuration, FString Reason)` : `GetGameTimeInSeconds` + `Add`. Il écrit `DefensiveMoveImmunityStartTime` (+0x1208) = now + WarmUp, `DefensiveMoveImmunityEndTime` (+0xB08) et `DefensiveMoveLockEndTime` (+0xAF8), et arme `DefensiveMoveLockTimer` (+0xB00).
- `IsInImmunityPhase() -> Bool` : `GetGameTimeInSeconds`, `GreaterEqual`, `LessEqual`, soit `Start <= now <= End`. **Temps de jeu dilaté** (`GetGameTimeInSeconds`), contrairement aux QTE offensifs (§8).
- `CanDoDefensiveMove(bool&)` (`GetTeam`, `IsLockedByDefensiveMoveTimer`, `IsDefenseLockActive`), `IsLockedByDefensiveMoveTimer(bool&)` (`K2_GetTimerRemainingTimeHandle`), `IsImmunityByDefensiveMoveFinished(bool&)`, `EndDefensiveMove()`, `ClearDefensiveMoveLock(FString)`, `AdjustDefensiveMoveLockTimer(double NewDuration, FString)`, `StartDefenseLock(double Duration, FString)`, `ResetDefensiveLockAfterTime(double)`, `OnDefenseLockTimerExpired()`.
- Drapeaux d'état : `IsParrying` (+0xB10), `IsDodging` (+0xB11), `IsJumping` (+0xB12), `IsStunLocked` (+0xB13), `IsGradientParrying` (+0x1160), `GetIsDefending(bool&)`.
- Délégués : `OnParryStart`, `OnGradientParryStart`, `OnDodgeStart`, `OnJumpStart(ABP_…*)`, `OnDefensiveMoveEnd`, `OnDefenseLockStart/End`, `OnDefenseStateChange(bool)`.

### 7.3 Paramètres de timing

[dump, valeurs par défaut non dumpées sauf mention]

| Paramètre | Offset | Note |
|---|---|---|
| `ParryLockDuration` / `ParryImmuneDuration` | +0xB30 / +0xB38 | fenêtre de parade (Story 350 ms, Expeditioner/Expert 150 ms [prior]) |
| `DodgeLockDuration` / `DodgeSuccessLockDuration` / `DodgeImmuneDuration` | +0xB18 / +0xB20 / +0xB28 | |
| `GradientParryLockDuration` / `GradientParryImmuneDuration` | +0x1150 / +0x1158 | |
| `JumpLockDuration` / `JumpWarmUpDuration` / `JumpImmuneDuration` | +0x11F0 / +0x11F8 / +0x1200 | 0.9 / 0.0 / 0.65 [prior] |
| `JumpCounterAttackWindowDuration` / `…StartDelay` / `…Slomo` | +0x1220 / +0x1228 / +0x1230 | |
| `MinimumDodgeLockDiff` | +0x1488 | |
| `StunLockDuration` | +0xD60 | |

- Durée effective : `GetParryImmuneDuration() -> Double` et `GetDodgeImmuneDuration() -> Double` valent base × (1 + modificateurs), d'après les locales `GetParryWindowModifier_Stack`, `GetGlobalParryWindowModifier_Stack`, `GetCurrentValue`, `Add`, `Multiply` [dump-locals].
  - Piles : `Stats.ParryWindowModifierStack` (+0x748) / `DodgeWindowModifierStack` (+0x740), `BM.GlobalParryWindowModifierStack` (+0x908) / `GlobalDodgeWindowModifierStack` (+0x900) (`FSF_ModifierStackWrapper{UModifierStack_Float* Stack}`).
  - API : `AddParryWindowModifier(double Percent, FSF_ModifierStackEntryHandle&, FString)` (stats), `AddGlobalParryWindowModifier(float Multiplier, …&, FString)` (BM), et leurs équivalents dodge.
  - **C'est l'outil propre pour compenser la latence réseau** (au lieu de multiplier les durées dans `StartDefensiveMove` comme Kouzukii [prior]).
- Difficulté : `ApplyDifficultySettings()` lit une ligne de `/Game/Gameplay/Battle/ScalingSystem/BalancingTables/Content/DT_Balancing_BattleDifficultySettings` selon `BM.GetCurrentBattleDifficulty() -> Byte` / `BM.BattleDifficulty` (+0x998) [dump-locals].
- Verrous côté stats : `ParryLock` (+0x710) / `AddParryLock` / `RemoveParryLock` / `IsParryLocked() -> Bool`, `DodgeLock` (+0x600) / `AddDodgeLock` / `IsDodgeLockActive() -> Bool`, `DefenseLockRefCount` (+0x410) / `AddDefenseLock()` / `IsDefenseLockActive() -> Bool`.

### 7.4 Où se décide le verdict touché / pas touché

C'est sur **l'instance qui exécute `ReceiveDamageFromObject` de la cible**, au moment de l'impact (`OnBattleSequenceActionEffect` de la séquence de l'attaquant) [dump + prior].

- **Parade** : `AC_jRPG_CharacterBattleStats_C::TryParryHitFromCharacter(UAC_jRPG_CharacterBattleStats_C* AttackingCharacter, bool& Successful)`, sur le composant de la **cible**. Locales : `IsPackagedForDistribution` ×2, `DEBUG_Console_AutoParry() -> Bool`, `DEBUG_Console_AutoGradientParry() -> Bool`, `TryStartParry_Success`, `TryStartGradientParry_Success`. Le verdict lit l'état parade / parade gradient de la cible (`IsParrying` / `IsGradientParrying` dans la fenêtre d'immunité) [inferred + prior : « tenir `IsParrying=true` sur la cible à l'impact pare le coup »]. AutoParry force `Successful=true` en post-hook [prior].
- **Esquive** : `TryDodgeHitFromCharacter(UAC_… AttackingCharacter, bool& Successful, bool& IsPerfectDodge)`. Locales : `TryStartDodge_Success`, `DEBUG_Console_AutoDodge`, `GetGameTimeInSeconds`, **`GetParryImmuneDuration`**, `Subtract`, `LessEqual`. Donc **esquive parfaite = (now − début d'esquive) ≤ durée d'immunité de parade** [inferred]. Délégué stats `OnPerfectDodge()`.
- **Saut** : test inline dans `ReceiveDamageFromObject` (`IsCurrentAttackJumpable` de l'attaquant, `IsJumping && IsInImmunityPhase()` de la cible), puis `BeginJumpCounterAttackWindow(ABP_… Attacker)`, `BattleDamages->Cancel(3, "Jumped")` et `DodgeSuccess=true`. Contre en saut : `CanDoJumpCounterAttack` (+0x11C8), `TryTriggerJumpCounterAttack()`, `JumpAttacker` (+0x1238), `OnJumpCounter` (délégué), widget `/Game/UI/Widgets/HUD_Battle/Buttons/WBP_Battle_JumpCounterHint.WBP_Battle_JumpCounterHint_C` [prior + dump].
- **Type d'attaque** (posé par l'attaquant) sur `AC_jRPG_CharacterBattleStats_C` : `IsCurrentAttackUndodgeable` (+0x568), `IsCurrentAttackUnblockable` (+0x569), `IsCurrentAttackGradient` (+0x56A), `IsCurrentAttackJumpable` (+0x56B), `ResetCurrentAttackTypeFlags()`. Perso : `StartUnblockableAttack()`, `StartUndodgeableAttack()`, `StartGradientAttack()`. Flags par compétence : `HasReceivedHitDuringCurrentSkill` (+0x348), `HasParriedAllHitsDuringCurrentSkill` (+0x349), `ResetCurrentParryFlags()`, `OnParryPerfectFail(FText Reason)`.

### 7.5 Après le verdict : PA, feedback, contre

- PA (stats) : `APGainParry` (+0x3B0), `APParryCost` (+0x3A8), `APDodgeCost` (+0x3A4), `APDefendCost` (+0x3A0), `APNewTurnGain` (+0x3AC), `APGainAttack` (+0x3B4), `APTurnStartBonus` (+0x3E8). `GainAP(int32 Amount, EAPChangeReason)`, `SetAP(int32 NewAP, EAPChangeReason)` (`ProcessAPChange` des buffs, `FClamp` sur `GetMaxAP`, `BP_BattleEvent_APChange_C`), `ReduceAP(double, EAPChangeReason)`, `DrainAP(int32, UAC_… Source)`, `CanSpendAP(int32) -> Bool`. `EAPChangeReason` a 14 valeurs.
- Feedback (BM) : `OnParrySuccess(ABP_… Character, ABP_… Enemy)`, `OnDodgeSuccess(...)`, `OnDefendSuccess(...)`, `FeedbackSuccessfulParry/Dodge/Defend(UBP_BattleFeedbackState_C*)`, `AddParrySuccessFeedback()` etc., `ProcessNextFeedback()`. Délégués `OnParrySuccessful`, `OnDodgeSuccessful`, `OnDefendSuccessful(Character, Enemy)`. États `SuccessfulParryFeedback` / `SuccessfulDodgeFeedback` / `SuccessfulDefendFeedback` (`/Game/Gameplay/Battle/BattleFeedback/BP_BattleFeedbackState.BP_BattleFeedbackState_C{Heroes, Enemies, DefenseMoveType: E_DefenseMoveType}`). Perso : `OnParrySuccess(ABP_… ParriedEnemy)`, `OnDodgeSuccess(ABP_…, bool IsPerfectDodge)`, `FeedbackSuccessfulParry()`.
- Contre : quand **toutes** les attaques d'une compétence sont parées, `TryCounterAttack(ParryState)` / `TryGradientCounterAttack(ParryState)` lancent le pseudo-tour (§3.3).
- Perfection de Verso : `BP_PerfectionMechanicComponent_C::OnParrySuccessful(...)` / `On DodgeSuccessful(...)` (§8.4).

### 7.6 Coop : schéma [inferred]

- **Propriétaire du héros ciblé** : laisser le jeu vanilla évaluer ses `TryStart*`. Post-hook sur `TryParryHitFromCharacter` / `TryDodgeHitFromCharacter` / `ReceiveDamageFromObject` de **ses** héros pour capturer `(Successful, IsPerfectDodge, kind)` et l'envoyer.
- **Hôte et autres clients** pour ce héros : bloquer les `TryStart*` locaux (pré-hook qui n'exécute pas), puis injecter le verdict distant. Soit en tenant le drapeau (`IsParrying` / `IsGradientParrying` / `IsDodging` / `IsJumping`) et en jouant l'anim avant l'impact (méthode Versus [prior]), soit en écrasant les out-params `Successful` / `IsPerfectDodge` dans le post-hook (méthode AutoParry [prior]). Le saut n'a pas d'out-param : tenir `IsJumping=true` + `DefensiveMoveImmunityStartTime/EndTime` encadrant `now`.
- **Ordonnancement** : l'hôte doit retarder sa timeline d'attaque ennemie (ou bloquer à l'impact) jusqu'à recevoir le verdict du propriétaire. Versus démarre l'attaque chez l'attaquant ~0.25 s après le « began » du défenseur [prior].

---

## 8. QTE offensifs et mécaniques propres aux personnages

### 8.1 Timeline des séquences de combat : `/Game/Gameplay/Battle/SkillCombos/BP_BattleSequenceTimeline.BP_BattleSequenceTimeline_C` (UObject, 0x140) [dump]

- Création : une instance par piste d'attaque. Le perso appelle `DeclareBattleSequenceTimingsAsList(ABP_EpicBattleSequenceActor_C* BattleSequence, TArray<FBattleSequenceKeyframe>& Keyframes, TMap<double, BattleSequenceCameraType> CameraKeyframes, int32 TrackIndex)` et stocke dans `BattleSequenceTimelines` (`TMap<int32, UBP_BattleSequenceTimeline_C*>`, +0xC48). `FBattleSequenceKeyframe` est natif (`/Script/SandFall`) : `{BattleSequenceKeyframeType m_keyframeType; float m_time}`. Types : `ACTIONEFFECT_AUTOCOMBO=1, ACTIONEFFECT=2, COMBOWINDOW_END_AUTO_STARTPRE=3, COMBO_EXIT_POINT=4, COMBOWINDOW_PRE=5, COMBOWINDOW_START=6, COMBOWINDOW_END=7, PRESET_FAST=50, PRESET_MEDIUM=51, PRESET_SLOW=52, SNAP_TO_TARGET=100, SNAP_TO_SOURCE=101` [dump, enum natif avec vrais noms].
- Champs :
  - `CurrentBattleSequence`, `CurrentBattleSequenceTimeline` (`TMap<double, FFBattleSequenceTimelineKeyframe>`), `NextBattleSequenceActionTime`, `NextBattleKeyframeRealTime`, `LastBattleSequenceAction`, `BattleCharacter`
  - **`IsInSkillComboWindow`** (+0xA9), **`IsSkillComboSuccessful`** (+0xAA), **`ComboSuccessHistory`** (`TArray<bool>`, +0xB0), `ComboPreTimes` / `ComboWindowStartTimes` / `ComboWindowEndTimes` (`TArray<double>`)
  - **`CurrentWindowStartRealTime`** (+0xF0), **`CurrentWindowEndRealTime`** (+0xF8), `TrackIndex`, `CanGetInput`, `IsQTEInterrupted` (+0x105), `AnticipationDuration` (+0x108), `ValidityDuration` (+0x110), `LastSkillComboWindowStartTime`, `ActionEffectCount`
  - **`IsSkillComboPerfect`** (+0x134), **`TriggerAutoQTEAtTime`** (+0x138)
- Flux [prior décompilé + dump-locals] : `OnSkillComboWindowPreReached()` (`GetUndilatedTimeSeconds`, `QTEManager.StartSkillCombo`) → `OnSkillComboWindowStartReached()` (`ShouldDoAutoQTE`, `StartAllowSkillCombo()`) → `TickCustomTimers()` → `OnSkillComboWindowEndReached()` (`Array_Add` sur l'historique, `QTEManager`). Autres : `OnSkillEffectReached()`, `OnSkillComboExitPointReached()`, `StartPrePhase()` / `StartValidityPhase()` / `StartEndPhase()`, `InterruptTimeline()`, `StopSequence()` / `ForceStopSequence()`, `TickBattleSequenceTimeline()`, `QueueNextBattleSequenceKeyframe()`, `OnBattleSequenceActionReached(EBattleActionEvent)`.
- **Verdict** : `TryActivate(bool& Success, bool& Perfect)` (logique complète dans AutoParry.cpp [prior]). `IsInPerfectWindow() -> Bool` : `GetUndilatedTimeSeconds`, `Subtract`, `Divide`, `InRange_FloatFloat`, `GetBoolCVar` (`Sandfall.QTE.ForcePerfect`, tuée par `IsPackagedForDistribution`). La fenêtre parfaite est le **dernier quart** de `[CurrentWindowStartRealTime, CurrentWindowEndRealTime]`, mesuré en **temps réel non dilaté** [dump-locals + prior].
- Appui : `IA_Battle_QTE` (`/Game/Gameplay/Inputs/InputMappingProfiles/Battle/InputActions/IA_Battle_QTE`, contexte `IMC_Battle_QTE`), puis `Controller.InpActEvt_IA_Battle_QTE_…`, puis `BM.OnComboPressed()`, puis le perso `Try Activate Skill Combo()` (nom avec espaces), puis `TryActivate` sur la timeline qui expire le plus tôt (`GetBattleTimelineExpiringSoonest(TArray<…>&)`, `ExpiresSoonerThan(...)`) [inferred pour le relais BM → perso].
- Auto : `BP_FL_GameUserSettingsHelpers_C::ShouldDoAutoQTE(__WorldContext) -> Bool` (option d'accessibilité). Debug : `BM.DEBUG_ComboAutoSuccessCount` (int32, +0x278) et perso `DEBUG_CurrentComboAutoSuccess` (+0xBC8). Versus s'en sert pour rejouer N réussites [prior].
- Résultat côté perso : `ComboSuccessHistory` (+0xBD0), `ComboPerfectHistory` (+0xBE0), `IsLastSkillComboSuccessful` (+0xB48), `GetLastSkillComboInfo(bool& OnlySuccess, bool& OnlyFail, bool& OnlyPerfect, int32& SuccessCount, int32& FailCount, int32& PerfectCount)`, `OnSkillComboFailed()`, `CurrentSkillNextComboAction` (+0xB40), `QTE_Keyframes` (`QTEKeyframeType` natif : INACTIVE/IMPACT). Le calcul des dégâts lit `ComboPerfectHistory` [prior].

### 8.2 `BP_QTEManager_C` (`/Game/Gameplay/QTE/BP_QTEManager.BP_QTEManager_C`, ActorComponent du contrôleur) [dump]

- Champs : `hasActiveQTE`, `qteResultPhase` (`EQTEPhaseType`, 4 valeurs), `hasDoneAction`, `slowMoTimeDilation`, `qteStartRealTime`, `qteExpectedEndRealTime`, `comboPreTime` / `comboStartTime` / `comboEndTime`, `skillComboPromptWidget(s)`, `skillComboQTEWidgets` (`TArray<UWBP_HUD_QTE_Base_C*>`), `CurrentTimeSource` (`ULevelSequencePlayer*`), `IsSkillComboFailed`, `show_QTE_As_UI`, `QTEContainer`, délégué `OnQTEStarted(UWBP_HUD_QTE_Base_C*)`.
- Méthodes : `StartSkillCombo(double NowTime, double PreDurationRealTime, double ValidityDurationRealTime, ULevelSequencePlayer* sequenceTimeSource, int32 TrackIndex, bool showQTE)`, `StopSkillCombo(int32 TrackIndex, bool IsSuccess)`, `OnSkillComboAction(bool IsSuccess, int32 TrackIndex, bool IsPerfect)`, `On Timeline Ended(int32 TrackIndex, bool Success)`, `OnAction(bool IsSuccess)`, `GetCurrentQTEState(EQTEPhaseType&)`, `FlushActiveQTE(...)`, `OnQTENotifyStateStart/Tick/End(...)`, `GetRealTimeProgress(double&, bool&)`, `ComputeRealTimeDuration(...)`.
- C'est l'UI et le slow-mo. Le verdict reste dans la timeline. Widgets : `/Game/Gameplay/Battle/SkillCombos/WBP_SkillComboPrompt`, `WBP_QTEContainer`, `/Game/UI/Widgets/HUD_Battle/Buttons/WBP_HUD_QTE(_Base)`. QTE diégétique : `BM.UseDiegeticQTE` (+0x448), cvar `DEBUG_CVarChange_DiegeticQTE`.

### 8.3 Visée libre

- **Combat** : `/Game/Gameplay/FreeAim/BP_FreeAimShooterBattleComponent.BP_FreeAimShooterBattleComponent_C` (`Character.BP_FreeAimShooterBattleComponent` +0x890) [dump].
  - Champs : `ShotsLeft` (+0xA8), `ShotsFired` (+0xAC), `TimeLeft` (+0xB0), `IsAiming`, `CurrentFreeAimType` (`EFreeAimType`, 4 valeurs), **`AimingRotation`** (FRotator, +0x168), `NextProjectileActor`, `ProjectileClass`, `IsShootingProjectile`, `PreBlocked`.
  - Méthodes : `EnableFreeAim(EFreeAimType, int32 AmmoCount, double TimeLimit, TSubclassOf<ABP_FreeAimProjectile_Base_C>)`, `OnFreeAimShootAction()`, `FireFreeAimProjectile()`, `PrepareNextFreeAimProjectile(…*&)`, `BindFreeAimProjectile(...)`, **`OnFreeAimProjectileHit(ABP_… Character, EFreeAimBodyPartType BodyPart, FHitResult HitResult, UBP_FreeAimTargetPart_C* TargetBodyPartBP)`**, `UpdateAimingLocation(FVector WorldAimLocation)`, `EndFreeAim()`, `CancelRegularFreeAim()`, `CanCancelFreeAim() -> Bool`, `HasShotsLeft() -> Bool`, `HasFreeAimTimeExpired() -> Bool`.
  - Délégués : `OnFreeAimEnabled`, `OnFreeAimEnded(TArray<ABP_…*>& AllHitCharacters, EFreeAimType)`, `OnFreeAimCancelled`, `OnFreeAimHit_Character_Regular(Character, BodyPart, FHitResult, TargetPart)`, `OnFreeAimHit_Character_Skill(HitCharacter, BodyPartType, TargetPart)`, `ShootFreeAimProjectile`.
- Côté perso : `EnableFreeAim(...)`, `StartFreeAim_Regular()`, `ShootFreeAimProjectile()`, `OnFreeAimHit(EFreeAimBodyPartType BodyPart, ABP_FreeAimProjectile_Base_C* Projectile)`, `TryCancelFreeAim()`, `FreeAimEndTurnInternal(EFreeAimType)`, `OnFreeAimEnded(TArray<…>&, EFreeAimType)`, `OnFreeAimCancelled()`, `LookX` / `LookY` (+0x1348/+0x1350), `FreeAimLookSpeed`, `FreeAimTargetParts` (+0x1140), `HasAimingSkill` (+0xBF1), entrées `InpActEvt_IA_Battle_FreeAim_Shoot_…` / `…_Cancel_…`. BM : `EnableFreeAimControls(EFreeAimType)`, `DisableFreeAimControls()`, `StartFreeAimFromUI()`, `CancelFreeAimFromUI()`, `ComputeFreeAim3rdPersonTargetLocation(bool DEBUG_ForShoot, bool& HasHitCharacter, …*&, FVector& HitLocation, FVector& StartLocation)`, `UpdateFreeAimHover()`, `CurrentHoveredCharacterFreeAim` (+0x3F0). Contexte d'entrée `/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_FreeAim`.
- Projectiles : `/Game/Gameplay/WorldSkills/BP_FreeAimProjectile_Base.BP_FreeAimProjectile_Base_C` (`Caster`, `IsConsumed`, `Shoot()`, `TriggerRaycast()`, `OnCollision(UObject*, UPrimitiveComponent*, FHitResult)`, délégué `OnBattleCharacterHit(...)`) et `/Game/Gameplay/WorldSkills/BP_FreeAimProjectile_Battle.BP_FreeAimProjectile_Battle_C` (`DesiredElementToShoot`). Zones : `/Game/Gameplay/FreeAim/BP_FreeAimTargetPart.BP_FreeAimTargetPart_C` (`BodyPartType`, `BaseDamage`, `DamageMultiplier`, `GetFinalDamageMultiplier()`, `OnPartHit(...)`). `EFreeAimBodyPartType` a 5 valeurs.
- Exploration : `/Game/Gameplay/FreeAim/BP_FreeAim.BP_FreeAim_C` (`PerformFreeAimRaycast(...)`, `PerformShoot()`), `BP_ExplorationCapacityComponent_FreeAimDamage/Teleport`. Hors combat.
- **Coop** : les tirs ne passent pas par `RequestActionExecution` [prior]. Il faut transmettre, chez le propriétaire, chaque `OnFreeAimProjectileHit` (cible `(team, spot)`, `BodyPart`, et le `BattleDamages` résultant) plus la fin (`OnFreeAimEnded` avec `AllHitCharacters`, `ShotsFired`). Sur les autres instances, on n'exécute pas la balistique : on applique les dégâts reçus. `AimingRotation` / `LookX/Y` ne sert qu'à l'affichage.

### 8.4 Mécaniques propres aux personnages

| Perso (ID interne) | Mécanique | Classes / données trouvées [dump] | Classes attendues (absentes du dump) |
|---|---|---|---|
| Gustave (`Frey`) | **Surcharge** (charge) | SFX `UI_Battle_UniqueMecha_Gustave_OverchargeFull_*`, icône `/Game/UI/Resources/Textures/Icons/UniqueMechaniques/Gustave/T_UI_GustaveCharge_Charge`. `BP_SkillState_C.IsOvercharged` / `SetOvercharge(bool, bool)` / `ShouldShowAsOvercharged()`, `Stats.IsSkillOvercharged(UBP_DataAsset_Skill_C*) -> Bool`, `Stats.CurrentOverdrive` (+0x2D0) / `AddOverdrive(double, FText)` / `OnOverrideFull()`, `OverdriveSources` | `BP_UniqueMechanic_Charge_Component_C` [prior] |
| Lune | **Taches** (Stains : FIRE/WATER/EARTH/LIGHTNING/LIGHT) | icônes `/Game/UI/Resources/Textures/Icons/UniqueMechaniques/Lune/T_UI_LuneStain_{EARTH,FIRE,LIGHT,LIGHTNING,WATER}`, SFX `…Lune_Stain_TurnStart`, dossier `/Game/Gameplay/Battle/Stains/` (seulement `EHealReason` chargé) | `BP_StainHolderComponent_C`, `BP_LuneStain_C` [prior] |
| Maelle | **Postures** (sans / Offensive / Défensive / Virtuose) | enum `/Game/Gameplay/Battle/StanceMaelle/E_StanceType` (4 valeurs), `BP_BattleBuffComponent_C::On Stance Change(E_StanceType NewStance, E_StanceType previousStance, …&, …&)`, `BP_BattleBuffInstance_C::OnStanceChange(...)`, `FL_jRPG_CustomFunctionLibrary_C::GetStanceDisplayName(E_StanceType, …)` / `GetStanceIcon(...)`, perso `OnStanceBroken()`, `SD_StanceBreak`, `AC_jRPG_ActionsManager_C.StanceDamageMultiply` / `StanceDamageMultiplier()` | `BP_StanceMechanicComponent_C`, `BP_BattleBuff_Stance_*` [prior] |
| Sciel | **Soleil / Lune / Crépuscule** (cartes) + **Présage** (Foretell) | icônes `/Game/UI/Resources/Textures/Icons/UniqueMechaniques/Sciel/T_UI_ScielCard_{Sun,Moon,Twilight}`, SFX `…Sciel_{Sun,Moon,Twilight}_{Switch,TurnStart}`, icône de statut `/Game/UI/Resources/Textures/StatModifier/T_HUD_Icon_ForetellBlack`, une propriété `ForetellSlot` (widget) | composant Sciel inconnu (à dumper) |
| Verso | **Rangs de perfection** D/C/B/A/S | **`/Game/Gameplay/Battle/Perfection/BP_PerfectionMechanicComponent.BP_PerfectionMechanicComponent_C`** (détail ci-dessous), buff `/Game/Gameplay/Buffs/UniqueMechanics/Perfection/BP_BattleBuff_PerfectionManager.BP_BattleBuff_PerfectionManager_C`, `BP_BattleDamagePerfectionChangeEvent_C`, enums `E_Perfection_Ranks` (6), `E_PerfectionUpdateType` (3), `EPerfectionReason` (19), widgets `WBP_UniqueMechanic_Perfection`, `WBP_Perfection_Letter_Gauge`, debug `BM.Sandfall_ChangePerfection(FString CharacterId, int32)` | — |
| Monoco | **Masques** (Agile / Tout-puissant / Équilibré / Lanceur / Lourd ; SFX bleu/or/vert/violet/rouge) + **Capture de pied** (transformations) | icônes `/Game/UI/Resources/Textures/Icons/UniqueMechaniques/Monoco/T_UI_MonocoMask_{AGILE,ALMIGHTY,BALANCED,CASTER,HEAVY}`, `T_UI_Icon_MonocoFootCapture`, widget `/Game/Gameplay/Battle/UniqueMechanics/TransformMasks/WBP_HealthBarDecorator_FootCaptureStatus.WBP_HealthBarDecorator_FootCaptureStatus_C` | composant Monoco inconnu (dossier `/Game/Gameplay/Battle/UniqueMechanics/TransformMasks/`) |

Commun : interface `/Game/Gameplay/Battle/UniqueMechanics/BPI_UniqueMechanicComponent.BPI_UniqueMechanicComponent_C::CreatePortraitWidget(UUserWidget*&, int32& SlotIndex)`, état « surpuissant » `Internal_SetUniqueMechanicOverpoweredState(bool)` / `CurrentOverpoweredFeedbackState` (+0x14F8), `Feedback_OnUniqueMechanicOverpowerEnable/Disable()`, widget HUD `WBP_UniqueMechanic_CurrentCharacter`, table de textes `/Game/StringTables/Gameplay/ST_UniqueMechanics`. **Il faudra énumérer les composants qui implémentent `BPI_UniqueMechanicComponent` sur chaque héros dans le dump de combat (§14).**

`BP_PerfectionMechanicComponent_C` (Verso) [dump] :

- `Perfection` (int32, +0xA8), `CurrentRank` (`E_Perfection_Ranks`, +0x168), `MaximumPerfection` (+0x1A0), `PerfectionMultiplier` (+0x268), `RankMapping` / `RankMapping_Inverted`, `RankOrder`
- `PerfectionFinalDamageMultipliers` (`TMap<Rank, double>`), `PerfectionToAdd` (`TMap<EPerfectionReason, int32>`), `PerfectionGainReasons` / `PerfectionLossReasons`, `LoseOneRankAtATime`
- `AddPerfection(int32, EPerfectionReason)`, `RemoveXPerfection(int32, EPerfectionReason)`, `RemoveAllPerfection()`, `Set Perfection Clamped(int32, E_PerfectionUpdateType, EPerfectionReason)`, `SetSpecificRank(E_Perfection_Ranks, EPerfectionReason)`, `IncreaseOrDecreaseRank(int32, EPerfectionReason, E_Perfection_Ranks&)`, `UpdateRank()`, `ResetMechaState(EPerfectionReason)`, `OnParrySuccessful(...)`, `On DodgeSuccessful(...)`, `OnTurnStart()` / `OnTurnEnd()`
- délégués `OnPerfectionChange(E_PerfectionUpdateType, int32, EPerfectionReason)`, `OnRankChangedUp/Down(E_Perfection_Ranks)`, `OnBattleDamagePerfectionChange(UBP_BattleDamagePerfectionChangeEvent_C*)`

### 8.5 Mécaniques demandant timing ou visée chez le propriétaire : point de hook et résultat à transmettre

| Mécanique | Décision locale (propriétaire) | Point de hook [inferred sauf mention] | Résultat à transmettre |
|---|---|---|---|
| Combo de compétence / attaque de base (QTE de rythme) | appui `IA_Battle_QTE` dans la fenêtre | post `BP_BattleSequenceTimeline_C:TryActivate(bool& Success, bool& Perfect)` + post `OnSkillComboWindowEndReached` [prior AutoParry] | par `(TrackIndex, index de fenêtre)` : `{Success, Perfect}`, ou en fin de compétence `ComboSuccessHistory` + `ComboPerfectHistory`. Rejeu : forcer `IsSkillComboSuccessful` / `IsSkillComboPerfect` avant `OnSkillComboWindowEndReached`, ou appuyer via `TriggerAutoQTEAtTime` + forcer `IsInPerfectWindow` (RESULT_DECL), comme AutoParry |
| Interruption de QTE | — | `InterruptTimeline()`, `IsQTEInterrupted` | drapeau d'interruption |
| Visée libre (tir) | rotation + raycast | post `BP_FreeAimShooterBattleComponent_C:OnFreeAimProjectileHit` ; post `OnFreeAimEnded` ; `FireFreeAimProjectile` (compter les tirs) | liste `{cible (team,spot), BodyPart, dégâts finaux, crit}` + `ShotsFired` ; rejeu par application des dégâts (pas de balistique) |
| Visée en compétence (`OnFreeAimHit_Character_Skill`) | idem | post sur ce délégué / `OnFreeAimHit` du perso | idem |
| Contre en saut | appui `IA_Battle_Defense_JumpCounter` pendant la fenêtre | post `TryTriggerJumpCounterAttack()` (le propriétaire est le défenseur) | `{déclenché}` + résultat du contre |
| Contre gradient | appui `IA_Battle_Defense_GradientCounter` | `BM.OnGradientCounterAction` / `TryGradientCounterAttack`, verdict via `TryParryHitFromCharacter` (gradient) | `{gparry réussi par coup}` |
| Gradient (attaque gradient du joueur) | choix + charges | `BP_GradientGaugeSystem_C::ConsumeCharge(int32)`, `ChangeGradientGauge(double, FString)` | jauge après action (§9) |
| Gustave : surcharge | pas de timing propre [inferred] | `BP_SkillState_C:SetOvercharge` | état surcharge |
| Lune : taches | pas de timing propre [inferred] (gain par sorts, consommation) | composant Stain (à dumper) | liste de taches |
| Maelle : posture | pas de timing (changement par compétence/coup) [inferred] | `BP_BattleBuffComponent_C:On Stance Change` | posture courante |
| Sciel : soleil/lune, présage | pas de timing propre [inferred] | composant Sciel (à dumper) ; buffs de présage | état carte + piles de présage par cible |
| Verso : perfection | gains liés à QTE / parade réussis | post `BP_PerfectionMechanicComponent_C:Set Perfection Clamped` / `OnPerfectionChange` | `Perfection` + `CurrentRank` |
| Monoco : roue bestiale / masques | **à vérifier** : la roue « Bestial Wheel » pourrait avoir un élément de timing ou de choix [inferred] | composant Monoco (à dumper), widgets `TransformMasks` | masque courant + compétences capturées |

Règle générale : la **décision de timing** (Success/Perfect) vient du propriétaire, la **conséquence numérique** (dégâts, crits, procs) vient de l'hôte. L'hôte rejoue avec les verdicts du propriétaire forcés, puis diffuse les résultats.

---

## 9. État à hasher pour la détection de désync

Par unité `ABP_jRPG_Character_Battle_Base_C` → `AC_jRPG_CharacterStats` (`UAC_jRPG_CharacterBattleStats_C`) [dump, offsets de ce build] :

| Champ | Accès | Type | Remarque |
|---|---|---|---|
| identité | `Stats.CharacterHardcodedName` (+0xA8), `Char.BattleSpotIndex` (+0xD20), `Char.Enemy?` (+0x988) | FName / int32 / bool | clé de tri |
| PV | `Stats.CurrentHP` (+0x168) | double | arrondir à 0,1 (bruit flottant) [prior] |
| PV max et stats | `Stats.CharacterCurrentStats` (+0x118, `TMap<E_jRPG_StatType, double>`), éventuellement `CharacterBaseStats` (+0xC8) | map | trier par clé. `E_jRPG_StatType` a 11 valeurs ; 1=PV, 2=PA, 3=Attaque, 8=Vitesse, 9=Crit [prior Versus] |
| multiplicateurs de stats | `Stats.CharacterStatMultipliers` (+0x230, `TMap<FGuid, FStatMultiplier>`) | map | à éviter dans le hash (GUID locaux) ; les buffs couvrent |
| PA | `Stats.CurrentAP` (+0x178) | double | arrondir à l'entier |
| bouclier | `Stats.CurrentShieldPoints` (+0x170) | int32 | |
| brise | `Stats.StunBreakBarCurrent` (+0x468), `StunBreakBarMax` (+0x470) | int64 | |
| étourdi | `Stats.IsStun` (+0x414) | bool | |
| initiative | `Stats.CurrentInitiative` (+0x180), `HasEngagementBonus` (+0x498) | double / bool | arrondir |
| tours sautés | `Stats.TurnSkipRefCount` (+0x2EC) | int32 | |
| mort | `Char.Dead?` (+0x999), `Stats.bIsDeathQueuedForTurnEnd` (+0x762) | bool | |
| compétence forcée | `Stats.ForcedSkillOnNextTurn` (+0x708) → nom de classe | objet | |
| fuite | `Stats.IsFleeing` (+0x5D9) | bool | |
| surcharge | `Stats.SkillStates` (+0x588) → `UBP_SkillState_C.IsOvercharged` (+0x30) | map | Gustave |
| overdrive | `Stats.CurrentOverdrive` (+0x2D0) | double | |
| **buffs** | `Stats.BattleBuffComponent` (+0x228) → `ActiveBattleBuffs` (+0x30) → `Wrapper.BattleBuffInstance` (+0x28) → `{GetClass().GetFName(), StackCount (+0xD0), TurnDuration (+0x30), IsPermanent (+0x40), Type (+0xA1)}` | liste | **exclure `Type == 3` et les classes `*Aggregator*`** (assistants permanents créés par certains buffs [prior Versus]). Trier par nom de classe. Ignorer `StandbyBattleBuffs` ou les hasher à part |
| mécaniques | Verso : `BP_PerfectionMechanicComponent_C.Perfection` (+0xA8) / `CurrentRank` (+0x168). Maelle : posture (composant à dumper). Lune : taches. Sciel : carte / présage. Monoco : masque | | trouver le composant par `GetComponentByClass` ou via l'interface `BPI_UniqueMechanicComponent` |
| phase (boss) | `u.CurrentPhase` (propriété de BP d'ennemi spécifique) [prior Versus] | | optionnel |

Globaux (`AC_jRPG_BattleManager_C`) :

- `TurnCount` (+0x3D8)
- `CurrentCharacter` (+0xB0) / `CurrentTurnCharacter` (+0x750) → `(team, spot)`
- `NextCharacterTurns` (+0x180) → liste `(team, spot)`, ordre significatif
- `ForcedNextCharactersQueue` (+0x9A8)
- **jauge gradient** : `GradientGaugeSystem` (+0x7E8, `/Game/Gameplay/Battle/GradientGaugeSystem/BP_GradientGaugeSystem.BP_GradientGaugeSystem_C`) → `CurrentProgress` (double, +0x28), `AvailableCharges` (int32, +0x30), `MaxCharges` (+0x4C) ; écriture `SetCurrentGradientGauge(int32 NewCharges, double NewProgress, FString Reason)`
- `TeamMotivationValue` (+0x1B0)
- `BattleItemsCount` (+0xA08, objets restants)
- `DeadPlayerCharacters` / `DeadEnemies` (tailles)
- `HasActiveCounterAttack` (+0x2C1) : **ne pas hasher pendant un contre** (pseudo-tour)

Moment du hash : à `OnTurnStart`, avec `HasActiveCounterAttack == false`, après `OnAllTurnStartEventsProcessed` (ticks de brûlure, régén et expiration de buffs appliqués). Ou bien à `End Character Turn` côté hôte, avant `UpdateBattleTurns` [inferred].

Réparation (setters du jeu, testés par Versus [prior]) :

- `SetCurrentShieldPoints(int32, FString)`, `SetCurrentBreakBarDamage(int64, UAC_*, FString)`, écriture directe `CurrentInitiative` et `CurrentHP`, `SetAP(int32, EAPChangeReason)`, `AddTurnSkip()` / `RemoveTurnSkip()`
- buffs : `StaticConstructObject(cls, stats)` puis `stats.ActivateBuff(inst, 4 /*Forced*/, src, out)` puis réécrire `TurnDuration` (l'activation ajoute un tour) ; `ChangeStackCount(int32)` ; `BattleBuffComponent.RemoveBuffInstance(inst)`
- gradient : `SetCurrentGradientGauge`. Verso : `Set Perfection Clamped`.

---

## 10. Sauvegardes

### 10.1 API du `BP_SaveManager_C`

`/Game/Gameplay/Save/BP_SaveManager.BP_SaveManager_C`, `GI.SaveManager` (+0x1E0), taille 0x260 [dump].

- État :
  - `SaveSlots` (`TArray<FS_SaveSlotData>`, +0x30), `SaveGameObjectClasses`, `SaveRequested` (+0x50), `SaveIsInProgress` (+0x51), `LoadIsInProgress` (+0xA9)
  - `EnableAutoSave` (+0xA8), `AutoSaveEnableReferenceCounter` (+0x188)
  - `MinimumTimeToWaitBetweenSaves` (+0x90), `MinimumTimeToWaitBetweenBackupSaves` (+0x98), `MinimumTimeToWaitToValidateSave` (+0xD8), **`MinimumDistanceToClosestEnemy`** (+0xE0)
  - `SpawnPointTagRequested` (+0xA0), `LoadGameRequest` (+0xB0), `LoadGameOverrideParameters` (+0x1E0), `SelectedSlot` (+0x120), `MaxNumberOfBackupSaves` (+0x1C8), `LastSaveGameObject` (+0x1D0), `SaveBlocker_NewGame`
  - délégués `OnSaveGameToSlotStarted/Finished`, `OnLoadGameFromSlotStarted`, `OnLoadGameFromSlotFinished(bool Success)`, `OnSelectedSlotChanged(int32 Prev, int32 Cur)`
- Sauvegarder :
  - `RequestSaveInternal(bool ProcessImmediately, const FString DebugReason)`, `RequestAutoSave(const FString)`, `ForceAutoSave(const FString)`
  - `RequestSaveAtSpawnPoint(FGameplayTag SpawnPointTag, bool ForceProcessImmediately, const FString)`
  - `ProcessSaveRequest()` / `ProcessSaveRequestUnsafe()`, `FlushAllSaveRequests()`
  - `SaveGameToFile(FString SaveToOverwrite)`, `SaveGameToObject(UBP_SaveGameBase_C* SaveObject, FString SaveName)`, `CreateSaveGameDataFromGameState(UBP_SaveGameData_C*& SaveGameData)`
  - `AsyncSaveGameToSlot(USaveGame*, FString SlotName)`, `TryAsyncSaveBackupToSlot(...)`, `AsyncSaveContainer()`, `ComputeSaveMetaData(FS_SaveSlotMetaData&)`
- Conditions :
  - `CanSave(bool& CanSave)` (out-param, pas de ReturnValue)
  - **`IsCurrentLevelValidForSave() -> Bool`**, `IsAutoSaveEnabled() -> Bool`, `SetEnableAutoSave(const bool Enable, FSF_ReferenceCounterHandle&, FString)`
  - `HasProcessSaveRecently() -> Bool`, `GetRemainingTimeToValidateSave() -> Double`, `IsSaveOrLoadInProgress() -> Bool`, `IsSaveInProgress(bool&)`, `IsLoadInProgress(bool&)`
  - côté monde : `Controller.SaveBlocker_Debug` (+0xC48), `BP_GameActionExecutorComponent_C.SaveBlockerReferenceHandle` + `ShouldBlockAutoSave()`, `UGameActionInstance::ShouldBlockAutoSaveDuringExecution()`
- Charger :
  - `RequestLoadGame(FS_LoadGameRequest LoadGameRequest, FS_LoadGameOverrideParameters LoadGameOverrideParameters)`, `ProcessLoadRequest()`, `CanLoadSave(bool&)`, `PrepareToLoadSaveGame()`
  - `AsyncLoadGameFromSlot(FString SlotName)`, `RetrieveSaveDataFromSlotName(FString) -> Object`, `LoadGameFromSaveFile(USaveGame*, FString SlotName)`, `LoadGameFromSaveData(UBP_SaveGameData_C*)`, `LoadGameSaveData(UBP_SaveGameData_C*, bool& IsLoaded)`, `ApplySaveDataToGameInstance(UBP_SaveGameData_C*)`
  - `ApplyLoadGameOverrideParameters(...)`, `ApplyLoadOverrideForSpawnPoint(...)`, `ApplyLoadOverrideForTimePlayed(...)`, `OnLoadOperationsDone()`, `OnLoadOperationsFailed(E_SaveManagerLoadingStatus)`, `IsSaveLoadable(FString) -> Bool`
  - mise à niveau : `UpgradeSaveIfNeeded(USaveGame*) -> Object`, `ProcessUpgradeSave(UBP_SaveGameBase_C*) -> Object`, `UpgradeLegacySave(UBP_jRPG_GameSave_C*, UBP_SaveGameObject_V0_C*&)`, `GetCurrentSaveVersion() -> Int`
- Slots et noms :
  - `GetSaveNameForSlot(int32 Slot) -> Str`, `GetSaveNameForSelectedSlot() -> Str`, `GetSaveContainerName() -> Str`, `GetSaveContainerBackupName() -> Str`, `GetBackupSaveFolderName() -> Str`
  - `ComputeSaveFilePath(FString SaveName) -> Str`, `ComputeAbsoluteBackupSaveFileFolderPath() -> Str`, `ComputeBackupSaveRegexPattern(int32) -> Str`, `ComputeBackupSaveListForSlot(int32) -> Array`
  - `GetMaxNumberOfSlots() -> Int`, `SetSelectedSlot(int32)` / `GetSelectedSlot(int32&)`, `GetSlots(TArray<FS_SaveSlotData>&)`, `GetSelectedSlotData(FString& MainSave, TArray<FString>& BackupSaves)`
  - `AddBackupSaveToSelectedSlot(...)`, `UpdateMainSaveToSelectedSlot(FString)`, `DeleteSaveSlot(int32)`, `DeleteBackupSaveInSlot(int32, FString)`, `DeleteSaveFile(FString)`, `LoadSavesContainer()`, `HasAnySaveInAnySlot() -> Bool`
- Nouvelle partie : `StartNewGame(bool FromMainMenu, FName OverrideStartLevelDataID)`, `InitializeLevelForNewGame(bool, FName)`, `GetNewGameData(FName OverrideStartLevelDataID, FName& LevelAssetName, FGameplayTag& SpawnPointTag)` (hooké par Archipelago [prior]), `StartNewGamePlus()`, `StartMainQuests()`.
- Helpers `/Game/Gameplay/Save/BP_FL_SaveGameHelpers.BP_FL_SaveGameHelpers_C` : `GetSaveManager(__WorldContext, UBP_SaveManager_C*&)`, `RequestAutoSave(FString, …)`, `ForceAutoSave(FString, …)`, **`ForceLoadLastSave(__WorldContext)`**, `ConvertItemsToSave/FromSave(...)`. Latent : `/Game/Gameplay/Save/BP_LatentAction_LoadGame.BP_LatentAction_LoadGame_C`.

### 10.2 Nommage des slots

- Fichiers : `%LOCALAPPDATA%\Sandfall\Saved\SaveGames\<SteamID64>\EXPEDITION_<n>.sav` + backups, conteneur `BP_jRPG_SavesContainer_C` [prior].
- `/Game/jRPGTemplate/Blueprints/SaveObjects/BP_jRPG_SavesContainer.BP_jRPG_SavesContainer_C` (`USaveGame`) : `SaveSlots` (`TArray<FS_SaveSlotData>`) [dump].
- `FS_SaveSlotData{MainSave: FString, BackupSaves: TArray<FString>, MetaData: FS_SaveSlotMetaData{SaveDateTime, TimePlayed, LastMap: FName, CurrentParty, FinishedGameCount}}` [dump].
- Les chaînes exactes renvoyées par `GetSaveNameForSlot` et `GetSaveContainerName` sont **à lire en jeu** (§14).

### 10.3 Objets de sauvegarde

- Chaîne : `/Game/Gameplay/Save/SaveObjects/BP_SaveGameBase.BP_SaveGameBase_C` (`USaveGame`, `SaveVersion` int32, `ConvertFromSaveGameData(UBP_SaveGameData_C*)`, `ConvertToSaveGameData()`, `GetSaveVersion(int32&)`) ← `BP_SaveGameObject_V0_C` … **`BP_SaveGameObject_V8_C`** (actuel, 0xBD0). Mises à niveau : `/Game/Gameplay/Save/Upgrade/UpgradeObjects/BP_SaveGameObjectUpgrader_V{n}_To_V{n+1}_C` jusqu'à V7→V8 [dump]. Héritage legacy : `/Game/jRPGTemplate/Blueprints/SaveObjects/BP_jRPG_GameSave.BP_jRPG_GameSave_C`.
- Champs V0 [dump] :
  - équipe et inventaire : `CurrentParty`, `CharactersCollection` (`TMap<FName, FS_jRPG_CharacterSaveState>`), `MapToLoad`, `Inventory`, `AreaName`, `TimePlayed`, `FinishedGameCount`, `InventoryItems` (`TMap<FName,int32>`), `Gold`, `EquippedConsumableShards`, `WeaponProgressions`, `PassiveEffectsProgressions`, `ExternalCompanions`
  - monde : `GPE_States`, `TransientBattledEnemies`, `TransientEnemyTransforms`, `TransientEnemyRoamingPointIndexes`, `TransientEnemyLastFleePoints`, `BattledEnemies`, `MovingPlatformsStates`, `TransientInteractedObjects`, `InteractedObjects`, `LiftsFloor`, `ManorGestrals`, `99Gestrals`, `PreviousManorDoorInfo`, `MerchantStates`
  - progression narrative : `InteractedCinematics`, `InteractedTutorials`, `SoundNotif`, `InteractedDialogs`, `InteractedDialogues`, `NamedIDsStates` (`TMap<FGuid,bool>`, les drapeaux persistants), `QuestStatuses`, `Relationship_Level`, `ExplorationProgression`, `EncounteredEnemies`, `BattleTowerSaveData`
  - position et rendu : `CurrentCharacterWorld`, **`SpawnPointTagToLoadAt`**, `ReturnSpawnPointTag`, `MapToLoadAfterCamp`, `LastWorldMapTransform`, **`LastTransform`**, `LastLightingScenario`, `LastAudioParameters`, `UnlockedSpawnPoints`, `UnlockedMapSections`, `IsMountInUse`
  - divers : `SaveDateTime`, `GameDifficultyData`, `EntitlementData`
- Ajouts V1 : `NotificationPools`, `FavoritePools`, `LastUsedSavePoint` (`FS_LevelSpawnPointData`), `VisitedLevelRowNames`. V4 : `ActivatedChallengeIDs`. V5 : `PassiveEffectsSetCollection`.
- Données runtime : `/Game/Gameplay/Save/BP_SaveGameData.BP_SaveGameData_C` (mêmes champs, agrégés).
- Personnage : `S_jRPG_CharacterSaveState{CharacterHardcodedName, CurrentLevel, CurrentExperience, AvailableActionPoints, CurrentHP, CurrentMP, CharacterActions, CharacterActionsOrder, PassiveEffectProgressions, EquippedPassiveEffects, EquippedItemsPerSlot, AssignedAttributePoints, UnlockedSkills, EquippedSkills, CharacterCustomization, IsExcluded, LuminaFromConsumables}` [dump].

### 10.4 En mémoire et « sauvegarder n'importe où »

- `UGameplayStatics::SaveGameToMemory` / `LoadGameFromMemory` **n'apparaissent pas** dans le dump (non réfléchis en 5.4) [dump : seulement `SaveGameToSlot`, `LoadGameFromSlot`, `AsyncSaveGameToSlot`, `DoesSaveGameExist`]. Ils sont accessibles en C++ seulement si le symbole est résolu (statiquement lié, non exporté) [inferred].
- Alternatives propres [dump] :
  - natif `/Script/SandFall.SaveGameHelpers` : `SaveGameToPath(USaveGame* SaveGameObject, FString SaveFilePath)` / `LoadGameFromPath(FString SaveFilePath)` / `CopyIdenticalProperties(UObject* From, UObject* To)`. Écrire dans un fichier temporaire puis lire ses octets suffit pour transférer une save (ou la sérialiser nous-mêmes)
  - `BP_SaveManager_C::CreateSaveGameDataFromGameState(...)` donne un `UBP_SaveGameData_C` en mémoire, sans disque. On peut le copier et le sérialiser champ par champ, puis le réappliquer avec `ApplySaveDataToGameInstance` / `LoadGameFromSaveData` [inferred]
  - `BP_GameActionExecutorComponent_C.LastGameStateSnapshot` (`UBP_SaveGameData_C*`, +0x150), `GetLastSavedGameData()`, `CreateSaveGameDataIfNeeded()`
- **Sauvegarder n'importe où** : le jeu passe par `CanSave(bool&)` et `IsCurrentLevelValidForSave()`, plus les bloqueurs (game actions en cours, cinématique, menu) et `MinimumDistanceToClosestEnemy`. QuickSave (Nexus 421) le fait hors point de sauvegarde [prior]. **Jamais en combat** : save corrompue [prior], et `CreateSaveGameDataFromGameState` capture l'état d'exploration du GI, pas l'état de combat. Pour le coop : seul l'hôte sauvegarde. On force avec `RequestSaveInternal(true, reason)` (Archipelago [prior]) en dehors des combats et des transitions. On met `EnableAutoSave=false` / `SetEnableAutoSave(false, handle, …)` chez les clients pour qu'ils n'écrasent pas leur propre save.

---

## 11. Transitions de niveau, points de spawn, cinématiques, dialogues, pause

### 11.1 Changer de carte

- `FL_jRPG_CustomFunctionLibrary_C` [dump] :
  - `ChangeMapByName(const FName LevelDestination, FGameplayTag SpawnPointTag, __WorldContext)`, `ChangeMapByAssetName(const FName LevelAssetName, FGameplayTag SpawnPointTag, __WorldContext)`, `Change Map Internal(FFLevelDataEntry& LevelData, FGameplayTag SpawnPointTag, __WorldContext)`
  - `GetLevelData(FName, …, bool& Found, FFLevelDataEntry&)`, `FindLevelDataByID(FName RowName, …)`, `FindLevelDataByLevelFile(FName LevelAssetName, …, bool&, FName& RowName, FFLevelDataEntry&)`, `GetCurrentLevelData(…, bool&, FFLevelDataEntry&, FName& RowName)`, `GetLevelDataDT() -> Object`, `GetAllLevelDataDTs() -> Array`
  - `IsCurrentLevelMainMenu() -> Bool`, `IsCurrentLevelProductionLevel() -> Bool`
- GI : `ChangeMap()` (événement), `PrepareToChangeMap()`, `IsMapChangeInProgress(bool&)`, `IsChangingMap` (+0x1D0), `LevelDestinationAssetName` (+0x1F8), `SpawnPointTag` (+0x278), `ReturnSpawnPointTag` (+0x2E0), `PreviousLevelRowName` (+0x270), `ResetDataOnChangeMapIfNeeded(const FFLevelDataEntry&)`, `ProcessLevelParameters(FFLevelDataEntry)`, `GetPreviousLevelData(bool&, FFLevelDataEntry&)`, délégués `OnChangeMapProcessStarted(FName)` / `OnChangeMapProcessFinished(FName)`.
- Contrôleur : **`PrepareToChangeMap(FFLevelDataEntry& LevelData)`** (Kouzukii y supprime ses joueurs supplémentaires [prior]), `IsAreaChangeInProgress(bool&)`, `IsChangingArea` (+0xDE8), `PrepareWorldForTransition()` / `ResumeWorldAfterTransition()`, `TeleportCharacterToSpawnPoint()`, `ProcessAreaChanged()`, `OnAreaChanged(ABP_jRPG_Character_World_C*, ABP_LightingScenarioReference_C*, FS_AreaAudioParameters, FS_ChangeLevelParameters)`, `GetCurrentLevelData(FFLevelDataEntry&)`, `ChangingMapInputLockBlockerReference` (+0xD48).
- Chargement natif : `/Script/SandFall.SF_LoadingScreenGISubsystem::OpenLevelWithLoadingScreen(const UObject* WorldContextObject, FName LevelName, FLatentActionInfo, bool bAbsolute, FString Options, TSubclassOf<USF_LoadingScreenWidget>)` / `OpenLevelBySoftPtrWithLoadingScreen(...)`, `OverrideNextLoadingScreenData(FLoadingScreenData)`, `OnLoadingScreenFinished()`, `IsLoadingScreenSubSystemActive()` [dump]. C'est le chemin final probable de `Change Map Internal` [inferred].
- `FFLevelDataEntry` (`/Game/Gameplay/LevelData/FLevelDataEntry`, 0x1B0) : `LevelAssetName`, `LevelType` (`ELevelType`, 5 valeurs), `LevelParameters`, `DisplayName`, `LevelIcon`, `LevelMinimapIcon`, **`MainSpawnPoint`** (FGameplayTag), `SubAreas` (`TMap<FGameplayTag, FText>`), `ScalingMinLevel` / `ScalingMaxLevel`, `MainSpawnPointOnWorldMap`, `LevelCompassYawOffset`, `LevelStartGameAction` (`UGameAction*`), `DEBUG_StartSpawnPointTag`, `DEBUG_ReferenceProgressionSave`… [dump]. Tables : `DT_jRPG_Levels` [prior] (non chargée au menu).

### 11.2 Points de spawn

- `/Game/LevelTools/BP_jRPG_MapSpawnPoint.BP_jRPG_MapSpawnPoint_C` (AActor) : `SpawnPointName` (FName), **`SpawnPointTag`** (FGameplayTag, +0x2C0), `SpawnPointMusic`, `LightingScenario`, `GetSpawnTransfrom(FTransform&)` (sic) [dump].
- `/Game/Gameplay/GameFlow/Spawning/BP_SpawnPointManager.BP_SpawnPointManager_C` (`GI.SpawnPointManager` +0x1C8 ; `BP_FL_SpawnPointHelpers_C::GetSpawnPointManager`) :
  - `LoadedSpawnPoints` (`TMap<FGameplayTag, ABP_jRPG_MapSpawnPoint_C*>`)
  - `RegisterSpawnPoint(...)`, `GetSpawnPointByTag(FGameplayTag, …*&, bool& Found)`, `IsSpawnPointLoaded(FGameplayTag, bool&)`
  - `GetOnLoadMapTransform(FTransform&, FRotator&, …*&, TSoftObjectPtr<ABP_LightingScenarioReference_C>&, FS_AreaAudioParameters&)` et ses variantes `…Regular`, `…ForWorldMap`, `…WithStaticSpawnPoint(FGameplayTag, …)`, `…AfterBattle`
  - `GetStaticSpawnPointTag(FGameplayTag&)`, `IsUsingDynamicSpawnPoint()`, `ComputeChangeLevelParameters(FS_ChangeLevelParameters&)`, `PrepareToChangeMap()`
- Points débloqués : `GI.UnlockedSpawnPoints` (`TMap<FName, FS_LevelSpawnPointsData{LevelAssetName, SpawnPointTags[]}>`, +0xF48), `UnlockSpawnPointForCurrentLevel(const FGameplayTag&)`, `HasUnlockedSpawnPointInCurrentLevel(...) -> Bool`, `LastUsedSavePoint` (+0x1108). Téléport de secours : `WBP_PauseMenu_C::ComputeSafeLocation(FName& LevelAssetName, FGameplayTag& SpawnPointTag)` / `TeleportToSafeLocation()` [dump]. Autre type de point : `/Game/LevelTools/BP_jRPG_MapTeleportPoint.BP_jRPG_MapTeleportPoint_C`.
- Transition (fondu) : `/Game/Gameplay/GameFlow/Transition/BP_GameFlowTransitionSystem.BP_GameFlowTransitionSystem_C` (sur `BP_jRPG_GM_MainMenu_C`, et sur le GM monde [inferred]) : `RequestTransition(const FS_GameFlowTransitionRequest&)`, `HoldTransition()`, `ReleaseTransition(double)`, `GetTransitionPhase(...)`. Helpers `BP_FL_GameFlowHelpers_C::RequestGameFlowTransition(...)`, `HoldGameFlowTransition`, `ReleaseGameFlowTransition(double)`, `ReleaseAndWaitGameFlowTransition(...)`, `IsInWorldMap(…, bool&)`.

### 11.3 Cinématiques

- `/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C` (`Controller.BP_CinematicSystem` +0x8A8 ; `FL_jRPG_CustomFunctionLibrary_C::GetCinematicsSystem() -> Object`) [dump] :
  - lancer : `TriggerCinematic(FS_TriggerCinematicVariables, USF_BlueprintLatentAction*)`, `StartLevelSequence(ULevelSequence*, FMovieSceneSequencePlaybackSettings)`, `PlayCinematicWithLevelSequence()`, `PlayCinematicWithMediaPlayer()`
  - contrôler : **`SkipCinematic()`**, `PauseCinematic()`, `CanPause(bool&)`, `SetSequencePlayRate(double)`
  - suivre : `OnSequenceStarted()` / `OnSequenceFinished()`, `IsPlayingSequence(bool&)`, `IsPlayingCinematic` (+0x238), `IsSkippingCinematic` (+0x248), `CinematicPaused` (+0x239), `IsInTransition` (+0x350)
  - transitions : `ExecutePreCinematicTransition()` / `ExecutePostCinematicTransition()` / `ExecuteExitPointTransition()`, `CleanUpAfterCinematic()`, `ResumeGameAfterCinematic()`, `DEBUG_AutoSkipCinematicIfNeeded()`
  - délégués `OnCinematicRequested`, `EventWhenTriggeringCinematic`, `EventAfterPostCinematicEvents`, `EventBeforePostCinematicTransitionStarted`
  - widget de contrôle `/Game/UI/Widgets/InGame_Menu/Cinematics/WBP_CinematicControls.WBP_CinematicControls_C`
- `FS_TriggerCinematicVariables` (0x150) : `Cinematic_LevelSequence_SequenceToPlay` (soft), **`Cinematic_LevelSequence_ID`** (FName, l'identifiant à synchroniser), `…PlayerSettings`, `Cinematic_DisableEnableInput`, `Cinematic_IsMediaPlayerCinematic`, `CanBePaused`, `AllowRepeat`, `PreCinematicParameters`, `PostCinematicParameters{BPCineExitPoint, …, GameFlowTransitionRequest, GameActionToExecute}`… [dump]. Vidéos : `/Game/Cinematics/IntroStroySequence/WBP_MediaPlayerCinematic.WBP_MediaPlayerCinematic_C`, `/Game/Gameplay/CinematicSystem/Media/BP_MediaPlayer.BP_MediaPlayer_C`. Déjà vues : `GI.InteractedCinematics` / `SetPlayedCinematic(const FName&)`.
- Séquences de combat (attaques) : `/Game/Gameplay/EpicBattleSequence/BP_EpicBattleSequenceActor.BP_EpicBattleSequenceActor_C` (`PlayBattleSequence(...)`, `StartSequence(double& SequenceDuration)`, `TriggerActionEffect(int32)`, délégués `OnActionEffect(int32 ActionEffectIndex)` / `OnSequenceFinished`, `InterruptSequence()`, `WasInterrupted`). Lancement : `BM.SpawnBattleSequence(ULevelSequence*, ABP_… Source, TArray<…>& Targets, bool SnapOnTarget, bool UseTargetAsOrigin, bool UseTargetOrientation, bool LookAtTarget, bool LookAtAttacker, bool DisableSequenceSnapping, ABP_EpicBattleSequenceActor_C*&, FTransform&)` [dump].

### 11.4 Dialogues

- `/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C` (`Controller.BP_DialogueSystemComponent` +0x8F8 ; `FL_…::GetDialogueSystemComponent(...)`) [dump] :
  - lancer : `QueueDialogue(TSoftObjectPtr<ABP_jRPG_NPC_Dialog_C>, FS_DialogueParameters)`, `ActivateDialogue(ABP_Dialogue_C*)`, `StartRichDialogueLines(...)`, `PlayNextDialogueLine()`
  - avancer et choisir : **`OnNextClicked(UWBP_GM_PromptGeneric_C*)`** (ligne suivante), `OnDialogueInterruptInput(...)`, `StartChoice(ABP_Dialogue_C*, TArray<FText>& Choices, EDialogueCameraType, FS_Dialogue_CustomOptions)`, **`OnChoiceMade(int32 ChoiceIndex)`**, `LastChoice` (+0xB0), `CurrentChoices`
  - auto-défilement : `CanAutoSkipCurrentDialogueLine()`, `TryStartTimerToWaitToAutoSkipLines(float)`, `CanAutoSkipDialogueLine` (+0x290)
  - suivre : `IsInDialogue()`, `HasActiveDialogue()`, `ActiveDialogue` (+0xA8), `EndDialogue(ABP_Dialogue_C*, FS_Dialogue_EndParameters)`, `IsWaitingForDialogInput` (+0xE0), délégués `OnDialogueStarted/Finished(ABP_Dialogue_C*)`, `OnAnyDialogueStarted/Finished`, `OnDialogueBattleStarted/Finished`
  - dialogues dans le monde : `PlayInWorldDialogue(UBP_DA_IWD_data_C*, bool&)`, `InterruptInWorldDialogue()`
  - sauvegarde en dialogue : `SaveAtSpawnPoint(FGameplayTag)`
- `/Game/Gameplay/DialogueSystem/BP_Dialogue.BP_Dialogue_C` (`DialogueGUID` FGuid, `NPC`, `ForcedDialogueStage`, `GetDialogueStage(int32&)` / `SetDialogueStage(int32)`, `Action_EndDialogue(int32 NextDialogueStage, FS_Dialogue_EndParameters)`, `SetBattleParty(...)` / `RestoreBattleParty()`). PNJ : `/Game/LevelTools/NPCs/BP_jRPG_NPC_Dialog.BP_jRPG_NPC_Dialog_C` (`TriggerDialogInternal(FS_DialogueParameters)`, `TeleportAndTriggerDialogue(...)`). Progression : `GI.InteractedDialogues` (`TMap<FGuid,int32>`), `AddInteractedDialogues(const FGuid&, const int32&)`, `CheckDialogueIsAtLeastAtStage(...)`. UI : `/Game/UI/Widgets/DialogueSystem/WBP_DialogueOverlay.WBP_DialogueOverlay_C`, `WBP_Dialogue_ChoiceOption`, entrée `/Game/Gameplay/Inputs/InputMappingProfiles/Dialogue/IA_UI_Dialogue_AutoSkip`.
- Ancien système encore chargé : `/Game/AnonsDialogSystem/Blueprints/BP_DialogSystem.BP_DialogSystem_C` (`W_DialogBox`, `W_DialogEntry`) [dump]. Probablement inutilisé [inferred].

### 11.5 Pause et menus

- Contrôleur :
  - `OpenPauseMenu()`, `MakePauseMenuVisible(bool)`, `CanOpenPauseMenu() -> Bool`, `IsPauseMenuVisible` (+0xBC8), `PauseMenuWidget` (+0xBD0, `/Game/UI/Widgets/InGame_Menu/PauseMenu/WBP_PauseMenu.WBP_PauseMenu_C`)
  - `OpenMainMenu()` / `MakeGameMenuVisible(bool, FS_GameMenuParameters)` / `CanOpenGameMenu() -> Bool` (menu de jeu), `IsAnyMenuVisible() -> Bool`
  - `PauseGameplay(FS_GameplayPauseParameters)`, `UnpauseGameplay()`, **`SetGameplayPaused(FS_GameplayPauseParameters, FSF_ReferenceCounterHandle&, FString)`**, `IsGameplayPaused(bool&)`, `GameplayIsPausedReferenceCounter` (+0x9F0)
  - `HandlePauseWidget(bool)`, `HandleInputsOnPause(bool)`, `OnPlatformPaused(bool)`, `OnAppLostFocus(bool)`, `BackToTitleMenu()`
  - délégués `OnPauseMenuOpened/Closed`, `OnMainMenuOpened/Closed`, `OnSavePointMenuOpened/Closed`
  - points de sauvegarde : `OpenSavePointMenu(bool)` / `CloseSavePointMenu(bool)`
- `FS_GameplayPauseParameters{WantToPause, IncludeControlledPawn, IgnoredActors, IgnoredClasses, PreparingToChangeMap, ForceUpdatePausedActors}` [dump]. GameMode natif : `/Script/SandFall.SandfallGameMode` (`OnGameModePauseStateChanged(bool)`). GI : `SetGameplayPaused(bool)`.
- `WBP_PauseMenu_C` : `OnResumeClicked()`, `OnBackToExploration()`, `OnRequestQuit()`, `OnAbandonBattleUserResponse(EModalUIResponse)`, `OnReturnToTitleScreenResponse(...)`, `TeleportToSafeLocation()`, `RetrievePreviousSave(FString)` / `OnUserResponseToLoadBackupSave(bool)`. En combat : BM `RegisterPauseIMC(bool)` (`IMC_Battle_PauseMenu`). **Coop** : la pause d'un client ne doit pas geler l'hôte. Il faut intercepter `SetGameplayPaused` chez les clients, ou n'autoriser qu'une pause « UI » [inferred].

---

## 12. Démarrage et menu principal

### 12.1 Démarrage et splashes

- `BP_jRPG_GM_Bootstrap_C` et les widgets de splash ne sont **pas dans ce dump** (déchargés une fois au menu) [dump]. Selon [prior, ClairObscurFix] : à l'apparition de `WBP_SplashScreen_Epilepsy_C`, appeler `BP_jRPG_GM_Bootstrap_C:OnEpilepsyWarningCompleted()` ; à celle de `WBP_SplashScreens_Logos_C`, appeler `BP_jRPG_GM_Bootstrap_C:OnSaveWarningSplashScreenCompleted()`. Mise en œuvre UE4SS : `NotifyOnNewObject` sur la classe du widget, puis appel sur le GameMode courant. **Chemins `/Game/...` exacts à relever avec un dump fait pendant le boot** (§14).
- Vidéo d'intro (nouvelle partie) : `/Game/Cinematics/IntroStroySequence/VID_IntroStory`, `VID_IntroStoryVideo`, `WBP_MediaPlayerCinematic_C`. GI : `OnInitialCinematicCompleted()`, `1stCinematicSeen` (+0x860) [dump].

### 12.2 Menu principal

- Niveau `/Game/Levels/UI/MainMenu/Level_MainMenu.Level_MainMenu_C`. GameMode `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GM_MainMenu.BP_jRPG_GM_MainMenu_C` (parent `ASandfallGameMode`, composant `BP_GameFlowTransitionSystem`). Contrôleur `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_MainMenu.BP_jRPG_Controller_MainMenu_C` (`MainMenuWidgetReference` +0x878, `SequenceData`, `SequenceActorPlayer`) [dump].
- `/Game/UI/Widgets/MainMenu/WBP_MM_MainMenu.WBP_MM_MainMenu_C` : **`OnContinueClicked()`** (« Continuer », utilisé par Versus [prior]), `OnNewGameClicked()`, `OnSettingsClicked()`, `OnQuitClicked()`, `BackToTitle()`, `BackToNewGame()`, `GetBuildText()` (hooké par Kouzukii pour afficher sa version [prior]), `InpActEvt_AnyKey_K2Node_InputKeyEvent_0(FKey)` (« appuyez sur une touche »), `OnFadeToBlackFinished()`, `OnStartGameSettingsApplied()`, `SetActiveWidgetInSwitcher(UCommonActivatableWidget*)`, sous-widgets `TitleScreen`, `LoadScreen`, `SettingsScreen`, `Button_BattleTestMap` (UButton, carte de test de combat) [dump].
- `/Game/UI/Widgets/MainMenu/WBP_MM_TitleScreen.WBP_MM_TitleScreen_C` : boutons `ContinueButton` / `NewGameButton` / `SettingsButton` / `QuitButton`, délégués `OnContinueClicked`, `OnNewGameClicked`, `OnSettingsClicked`, `OnQuitClicked`, `SetIsAnySave(bool)`, `TransitionFromStart()` [dump].
- `/Game/UI/Widgets/MainMenu/WBP_MM_LoadScreen.WBP_MM_LoadScreen_C` : `LoadAvailableSaves()`, `SetFocusedSaveSlot(UBP_SaveSlotViewItem_C*, FVector2D)`, `OnSaveSlotClicked(UCommonButtonBase*)`, **`LoadSave()`**, `OnLoadResponse(bool IsAccepted)`, `OnNewSaveResponse(bool)`, `OnDeleteResponse(bool)`, `SetIsNewGameState(bool)`, `RetrieveBackupSaveDatasIfNeeded()`, `FocusedSaveSlotItem`, `BackupSaveDatas` [dump]. Entrées de slot : `/Game/UI/Widgets/MainMenu/BP_SaveSlotViewItem.BP_SaveSlotViewItem_C`, `WBP_MM_SaveSlot`, `WBP_MM_BackupSaveEntry`, `WBP_MM_BackupSavesBox`.
- Chargement direct sans UI [inferred] : `GI.SaveManager.SetSelectedSlot(n)` puis `RequestLoadGame(FS_LoadGameRequest{SlotName = GetSaveNameForSlot(n), …}, overrides)`. Ou bien `BP_FL_SaveGameHelpers_C::ForceLoadLastSave`. Pour forcer le point d'apparition (rejoindre l'hôte) : `FS_LoadGameOverrideParameters{SpawnPointTag, SpawnTransform, TimePlayed}`. Nouvelle partie : `SaveManager.StartNewGame(true, OverrideStartLevelDataID)`.

### 12.3 Écran de chargement

`/Game/UI/Widgets/CommonElements/LoadingScreen/WBP_LoadingScreen.WBP_LoadingScreen_C` (`Appear()`, `Disappear()`, `ShowBlack()`, `HasAppeared`), `/Game/UI/Widgets/LoadingScreen/WBP_LoadingScreen_Expedition33.WBP_LoadingScreen_Expedition33_C` (`SelectTip` tire au hasard, cosmétique), `GI.LoadingScreenWidget` (+0xB08), natif `USF_LoadingScreenGISubsystem` / `USF_LoadingScreenWidget::StartLoadingScreen(FName LevelLoaded)` (§11.1). Fin de chargement d'une save : délégué GI `OnGameLoaded()` (+0xA68), `SaveLoaded` (+0xA78), `BindOnSaveLoaded(const FBindOnSaveLoadedEvent&, bool& AlreadyLoaded)`, `GetIsSaveLoaded(bool&)` [dump].

---

## 13. Entrées

- **Contextes de combat** (`/Game/Gameplay/Inputs/InputMappingProfiles/Battle/`) : `IMC_BattleDefense`, `IMC_Battle_FreeAim`, `IMC_Battle_InputBlocker`, `IMC_Battle_MainWheel`, `IMC_Battle_PauseMenu`, `IMC_Battle_PlayerTurn`, `IMC_Battle_QTE`, `IMC_Battle_Targeting` [dump].
- **Actions** (`…/Battle/InputActions/`) : `IA_Battle_Defense_{Parry,Dodge,Jump,JumpCounter,GradientCounter}`, `IA_Battle_QTE`, `IA_Battle_FreeAim_{Shoot,Cancel}`, `IA_Battle_Main_{Attack,FreeAim,Gradient,Items,Skills}`, `IA_Battle_{FirstSkill,SecondSkill,ThirdSkill}`, `IA_Battle_SkillToggleColumn{Left,Right}`, `IA_Battle_SkipTurn`, `IA_Battle_Flee`, `IA_Battle_Targeting_{Left,Right}`. UI : `/Game/Gameplay/Inputs/InputMappingProfiles/UI_Menu/IA_UI_Menu_{Game,Pause,ScrollEntry}`, `IA_UI_Exploration_ShowTeamStatus`, contexte `IMC_UI_Menu` [dump].
- **Routage** [dump] :
  - défense et QTE de combo : `BP_jRPG_Controller_World_C` (événements `InpActEvt_IA_Battle_Defense_*`, `InpActEvt_IA_Battle_QTE_*`, `TryBindDefendInput()` / `UnbindDefendInput()`), puis BM (`OnParryAction`, `OnDodgeAction`, `OnJumpAction`, `OnGradientCounterAction`, `OnComboPressed`), puis `TryStart*` / `Try Activate Skill Combo` du personnage concerné (§7, §8)
  - visée libre : directement sur le personnage (`InpActEvt_IA_Battle_FreeAim_{Shoot,Cancel}`), aussi `IA_CameraRightAction_*` / `IA_CameraUpAction_*` et `IA_UI_Menu_Pause`
  - roue et ciblage : widgets `WBP_HUD_BattleScreen_C` (`RegisterWheelInput`, `RegisterTargetingInput`) et `WBP_BattleInput_PlayerTurn/EnemyTurn/SelectTarget`
  - BM : `SetBattleInputEnabled(bool)`, `SetInputMappingContextEnabled(bool bIsEnabled, const UInputMappingContext*, int32 Priority)`, `Disable Targeting and Input()`, `RegisterPauseIMC(bool)`
  - contrôleur : `SetInputModeBattle(bool IsTakingAction)`, `SetInputModeUI(bool, bool)`
- **Verrous d'entrée** [dump] :
  - `/Game/Gameplay/Inputs/InputLock/BP_InputLockComponent.BP_InputLockComponent_C` (`Controller.BP_InputLockComponent` +0x888) : `AddInputLock(FS_InputLockParameters{AffectMovement, AffectCamera}, FSF_ReferenceCounterHandle&, FString)`, `RemoveInputLock(...)`, `IsMovementInputLocked()`, `IsCameraInputLocked()`. Il ne couvre que déplacement et caméra.
  - `/Game/Gameplay/Inputs/BP_InputManager.BP_InputManager_C` (`Controller.BP_InputManager` +0x8F0) : `SetAllInputBlocked(bool bLocked)`, `bLocked` (+0x120), `SetInputActionsRestriction(TArray<EInputAction>&, bool AllowNavigation)`, `SetInputActionsRestriction_NavigationOnly(bool)`, `ResetInputActionRestriction()`, couches `CreateInputLayer(bool IsModal, FGuid&)` / `DeleteInputLayerByGUID`, `BindToActionPressed(EInputAction, UObject*, …)`. `EInputAction` a 34 valeurs.
  - `/Game/Gameplay/Inputs/InputContext/BP_InputContextComponent.BP_InputContextComponent_C` : `SetInputContext(FS_InputContextParameters)`, `RemovePushedInputContexts()`
  - bloqueurs du contrôleur : `UIInputLockBlockerReference`, `BattleInputLockBlockerReference` (+0xCE8), `TransitionInputLockBlockerReference`, `SavePointInputLockBlockerReference`, `ChangingMapInputLockBlockerReference`
  - natif : `/Script/SandFall.SandfallBlueprintHelpers::FlushInputs(APlayerController*)`, `IsPawnInputEnabled(const APawn*)`
  - méthode brutale de Versus : `pc:DisableInput(pc)` / `EnableInput` pendant la défense adverse [prior]
- **Vu en jeu le 2026-10-20** : touches clavier (AZERTY affiché) roue `F` attaque, `E` compétences, `Z` objets, `A` gradient ; QTE `Espace` / A manette. Priorités : `IMC_Battle_PlayerTurn` 99, `MainWheel` 100, `Targeting` 1000, `QTE` 2 ; `IMC_Battle_PauseMenu` reste actif tout le combat. `SetBattleInputEnabled(false)` et `Disable Targeting and Input()` **ne retirent aucun contexte** (le gradient s'ouvre encore). `EnhancedInputLocalPlayerSubsystem:RemoveMappingContext` les coupe ; le jeu remet tour/roue/ciblage à chaque début de tour d'un héros, `QTE` à chaque compétence. L'événement `OnSkillComboWindowStartReached` arrive 70 à 310 ms après `CurrentWindowStartRealTime` (même sans mod) ; latence appui → `TryActivate` ≈ 50 ms.
- **Coop** [inferred] : pour empêcher un joueur d'agir sur un héros qui n'est pas le sien, le plus propre est de ne pas lier `IMC_BattleDefense` (ou de `UnbindDefendInput`) quand aucun de ses héros n'est visé, et de couper `IMC_Battle_PlayerTurn` / `IMC_Battle_MainWheel` (`SetInputMappingContextEnabled(false, …)`) pendant les tours des autres. À défaut, pré-hooker les `TryStart*` et `OnComboPressed`.

---

## 14. Questions ouvertes et vérifications en jeu

1. **Second dump UE4SS en combat**, avec successivement les 6 héros dans l'équipe et un boss, pour récupérer :
   - les composants de mécanique (`BP_StanceMechanicComponent`, `BP_StainHolderComponent`, `BP_UniqueMechanic_Charge_Component`, Sciel, Monoco) et leurs champs à hasher
   - les BP de héros et d'ennemis spécifiques (`PerformActionByAI` réel, `CurrentPhase`)
   - les skill scripts : refaire l'inventaire `CallFunc_Random*` / `Array_Random` (§6)
   - `BP_jRPG_GM_World` et `DT_jRPG_Encounters` / `DT_jRPG_Levels`
2. **Dump au démarrage** (avant le menu) : chemins exacts de `BP_jRPG_GM_Bootstrap_C`, `WBP_SplashScreen_Epilepsy_C`, `WBP_SplashScreens_Logos_C`, et signatures de `OnEpilepsyWarningCompleted` / `OnSaveWarningSplashScreenCompleted`.
3. **Libellés d'enum** : lire en jeu les DisplayName de `E_jRPG_ActionType`, `EBattleEndResult`, `E_jRPG_StatType`, `EDamageReason`, `EMissReason`, `EBuffApplicationProbability` (4 = Forced ?), `E_BuffAndStatusEffectTypes` (3 = agrégateur ?), `E_StanceType`, `E_Perfection_Ranks`, `ECounterType`, `E_BattleFlowState`, `E_jRPG_EngageTypes`. En C++ : `UEnum::GetDisplayNameTextByValue`.
4. **`StartCharacterTurn`** : le `RandomIntegerInRange` est-il cosmétique (répliques) ou gameplay ? Hooker `RandomIntegerInRange` pendant un tour et regarder l'appelant.
5. **Ubergraphs** du BM (`Array_Random`) et du perso (`Array_Random` ×4) : identifier ce qui est tiré (variantes d'anim/séquence ?). Si la séquence diffère, les fenêtres de QTE diffèrent → imposer l'index choisi par l'hôte.
6. **`ReceiveDamageFromObject`** : à quoi sert son propre `RandomFloatInRange` (crit bis, variance de dégâts ?) ; ordre exact parade → esquive → stats-dodge → crit. Lire le bytecode avec `kismet-analyzer` sur l'asset extrait (retoc) ou par traçage.
7. **Versus dit que les dégâts des attaques de héros ne passent pas par les hooks `ReceiveDamage(FromObject)`** [prior] alors que `PerformStandardHitOnCharacter` appelle `ReceiveDamage` [dump-locals]. Vérifier en C++ (`RegisterHook` sur `ReceiveDamage`, `ReceiveDamageFromObject`, `DealDamages`) pendant une attaque de héros. C'est peut-être une limite des hooks Lua sur les appels BP→BP.
8. **Retard de verdict défensif** : l'hôte peut-il suspendre la timeline d'une attaque ennemie à l'impact (time dilation locale de l'attaquant, `InterruptTimeline`, ou pré-hook qui retient `OnBattleSequenceActionEffect`) le temps d'un aller-retour réseau, sans casser les anims ? Sinon il faut le décalage de départ à la Versus.
9. **Rejeu des QTE offensifs côté hôte** : vérifier que forcer `IsInPerfectWindow` (RESULT_DECL) + armer `TriggerAutoQTEAtTime` reproduit exactement `ComboSuccessHistory` / `ComboPerfectHistory` envoyés par le propriétaire, échec compris (ne pas appuyer). Alternative : `DEBUG_ComboAutoSuccessCount` (succès seulement, pas de « parfait »).
10. **Déterminisme de l'ordre des tours** : comparer `NextCharacterTurns` hôte / client sur plusieurs combats avec `CurrentInitiative` synchronisé. Vérifier l'effet de `HasEngagementBonus` et des buffs de vitesse.
11. **Valeurs par défaut** des durées de parade/esquive/saut par difficulté (ligne de `DT_Balancing_BattleDifficultySettings`) et effet réel de `AddGlobalParryWindowModifier` (pourcentage ou multiplicateur ? le BM prend un `float Multiplier`, les stats un `double Percent`).
12. **Sauvegarde** : chaînes exactes de `GetSaveNameForSlot(n)` et `GetSaveContainerName()` ; comportement de `CanSave` / `IsCurrentLevelValidForSave` en camp, en carte du monde et en donjon ; vérifier qu'un `CreateSaveGameDataFromGameState` + `SaveGameToPath` produit une save valide chargeable par `LoadGameFromPath` + `LoadGameFromSaveData`.
13. **`SaveGameToMemory`** : le symbole est-il présent dans l'exe (signature/AOB) ? Sinon `SaveGameToPath` + lecture du fichier.
14. **Pause** : `SetGameplayPaused` chez un client affecte-t-il seulement son monde local ? (attendu oui, aucun netcode) Quelles dépendances (cinématiques, dialogues) attendent une pause ?
15. **Monoco** : la roue bestiale / capture de pied comporte-t-elle un QTE ou un choix temporisé à traiter comme mécanique locale ?
16. **Sciel / Présage** : où sont stockées les piles de présage (buff sur la cible ? composant ?) pour les hasher.
17. **Fiabilité de `GetUndilatedTimeSeconds`** (QTE offensifs) vs `GetGameTimeInSeconds` (défense) : toute pause ou ralenti local (slow-mo de `BP_QTEManager.slowMoTimeDilation`, freeze frames `BM.StartFreezeFrame(E_FreezeFrame, …)`) décale les deux horloges différemment entre machines. Mesurer.
18. **ABI UE4SS** : rester sur le commit `83330635` pour le build du mod (cf. `ue4ss-build.md`) ; surveiller l'issue #1463 sur Windows 11 26200.

---

## 15. Vérifié en jeu (build 23765773, 2026-10-18)

Session d'exploration avec UE4SS 1181 + mod Lua `E33Explore` (télécommande `runs/explore/cmd.lua`).

- **UE4SS 1181 fonctionne** sur ce build (Windows 11 26200) ; menu principal atteint ~49 s après le lancement ; aucune erreur bloquante dans `UE4SS.log`. L'issue UE4SS #1463 ne se reproduit pas : les hooks BP (ProcessLocalScriptFunction) se déclenchent.
- **Classes de combat chargées dès le démarrage** : `RegisterHook` réussit ~11 s après l'init pour `BP_jRPG_Character_Battle_Base_C:{OnTurnStart,OnTurnEnded,TryStartParry,TryStartDodge,TryStartJump,OnParrySuccess,OnDodgeSuccess}`, `AC_jRPG_BattleManager_C:{StartCharacterTurn,SelectAction,SelectSecondaryAction,RequestActionExecution,CheckBattleEnd,StartBattleEndFlow}`, `AC_jRPG_CharacterBattleStats_C:{ReceiveDamage,TryParryHitFromCharacter}`, `BP_jRPG_Controller_World_C:{ResumeExplorationOnBattleEnd,PrepareToChangeMap}`. `StartBattleNEW` : introuvable sous ce nom.
- **Menu principal** : `NotifyOnNewObject("/Game/UI/Widgets/MainMenu/WBP_MM_MainMenu.WBP_MM_MainMenu_C")` se déclenche ; le widget appartient au GI (`BP_jRPG_GI_Custom_C`).
- **Slots de save** : 10 slots `EXPEDITION_0..9` (`GetSaveNameForSlot`), `SelectedSlot=0`. Les noms de slot de `UGameplayStatics` sont relatifs à `Saved\SaveGames\` : `DoesSaveGameExist("76561198355797267/EXPEDITION_0")` = vrai, `"EXPEDITION_0"` = faux, `"e33coop-test/TEST_A"` = vrai, `"../e33coop-test/TEST_A"` = faux.
- **Charger une save de test** : `GameplayStatics:LoadGameFromSlot("e33coop-test/TEST_A", 0)` renvoie un `BP_SaveGameObject_V7_C` (save de mars 2026, mise à niveau à la volée), puis `SaveManager:LoadGameFromSaveFile(sg, slot)` charge la partie (écran de chargement, puis `Level_Lumiere_Main_V2`, pion `BP_jRPG_Character_World_C`). `EnableAutoSave=false` posé avant : aucune écriture dans les saves perso (hash vérifiés).
- **Déclencher un combat** : recette Versus (`StaticConstructObject(BP_GameAction_TriggerBattle_C, executor)`, remplir `Battle Start Params` (membres à suffixe GUID), `ExecuteGameAction`) avec `L_LumiereCitizenFleuret_Male*1` et la première `BP_BattleMap_C` (tri par nom) → combat lancé en ~3 s. `DT_jRPG_Encounters` : 797 lignes (`runs/explore/encounters.txt`, local).
- **Faire jouer un héros** : `BM.DEBUG_ComboAutoSuccessCount=10; BM:SelectAction(1); BM:RequestActionExecution({ennemi})` → attaque de base exécutée (Perfection de Verso montée de rang, +1 charge gradient). Donc `E_jRPG_ActionType` 1 = attaque de base.
- **Séquence observée d'une attaque de héros** : `SelectAction` → `RequestActionExecution` → `TryParryHitFromCharacter` sur les stats de l'**ennemi** (une fois par coup, 3-4 coups) → `OnTurnEnded(héros)` → `CheckBattleEnd` → `OnTurnStart(suivant)` → `StartCharacterTurn`.
- **Séquence observée d'une attaque ennemie (sans défense)** : `OnTurnStart(ennemi)` → `StartCharacterTurn` → (≈3 s) `TryParryHitFromCharacter(stats de Verso)` → `ReceiveDamage(stats de Verso)` → (≈1,6 s) 2e coup idem → `OnTurnEnded(ennemi)` → `CheckBattleEnd` → `OnTurnStart(Verso)`. PV de Verso 13965 → 6113.
- **Arguments de lancement** : `steam.exe -applaunch 1903340 <args>` transmet les arguments au lanceur puis à `SandFall-Win64-Shipping.exe` (ligne de commande : `… Sandfall -windowed -ResX=1280 -ResY=720 -log`). `-ResX/-ResY` non appliqués (rendu 1920x1080). `-log` ouvre une console.
- **Parade sur commande (C++, test runner)** : appeler `BP_jRPG_Character_Battle_Base_C:TryStartParry` sur le défenseur dans le **pré-hook** de `AC_jRPG_CharacterBattleStats_C:TryParryHitFromCharacter` suffit : le verdict `Successful` lu dans le post-hook vaut `true` sans le forcer (2/2 sur le premier run). Ne rien faire → `false`, le coup porte (`ReceiveDamage`).
- **Fin de combat** : `AC_jRPG_BattleManager_C:StartBattleEndFlow(BattleEndResult)` reçoit `1` sur une victoire (ennemi unique tué, `Enemies` vide ensuite). Libellés des autres valeurs : à confirmer.
- **Temps de cycle d'un test** : lancement Steam → menu ~23 s → save chargée et monde stable ~85 s → combat de 6 tours ~70 s → `result.json` ; le processus se termine avec le code du verdict.
- **Piège** : générer le SDK UE4SS (`GenerateSDK`) charge puis décharge de force des milliers d'assets ; un combat déclenché juste après a fait planter le jeu. Les dumps sont désormais à la demande uniquement.
- **IA ennemie, observée en jeu** (`/Game/Characters/Enemies/Lumiere/BP_EnemyBattle_LumiereCitizen_Fleuret_Male.BP_EnemyBattle_LumiereCitizen_Fleuret_Male_C`, parent `…_Fleuret_Female_C` → `BP_EnemyBattle_LumiereCitizen_C` → `BP_jRPG_Enemy_Battle_Base_C`) :
  - les compétences ennemies sont des **événements BP** de la classe d'ennemi : `Skill_Combo1/2/3`, `Skill_Combo4_Earth`, chacun avec ses `Hit_ComboN` (un par coup) ; `PerformActionByAI` (≈0,8 s après `OnTurnStart`) en choisit un ;
  - chaîne observée : `OnTurnStart(ennemi)` → `PerformActionByAI` → `Skill_Combo4_Earth` → 3 × (`ReceiveDamage` sur le héros + `Hit_Combo4_Earth`) → `OnTurnEnded` ;
  - le choix vient d'un `RandomIntegerInRange` dans l'ubergraph de `…_Fleuret_Female_C` ; la cible d'un `AcquireTargetRandomHero` → `FL_TargetingHelpers_C:GetRandomAliveHero` (`Array_Random`) ;
  - `Skill_Combo4_Earth` est une attaque **sautable** : aucun `TryParryHitFromCharacter`, le verdict passe par `ReceiveDamageFromObject` (`IsCurrentAttackJumpable` sur les stats de l'attaquant, via `DamageBuilder → GetDamageSource → GetDamageSourceCharacter`). Les autres combos sont parables.
- **Conséquence pour les tests** : l'IA tire au hasard entre attaques parables et sautables, avec des nombres de coups différents. Les compteurs d'un run (tours, coups) varient donc d'un run à l'autre tant que l'IA n'est pas pilotée (voir D9).

### Ajouts du 2026-10-19 (build 23765773)
- **Hooks UE4SS sur fonctions natives** (`KismetMathLibrary:Random*`) : ils se déclenchent bien, mais `ctx.TheStack` est le cadre de la fonction native elle-même (`Object` = `Default__KismetMathLibrary`, `Node` = la fonction, paramètres dans `Locals`). Le vrai appelant BP est dans `TheStack.PreviousFrame()`.
- **Ne jamais appeler le jeu (ProcessEvent) depuis un hook appelé très souvent** : un `GetControllerWorld` fait dans le hook de tirage pendant un chargement de save a fait planter le jeu (lecture de l'adresse nulle, thread principal). Les fonctions de tirage sont aussi appelées hors du thread de jeu.
- **Appelants des tirages en combat** : voir D10 (IA ennemie, `RollCriticalHit`, `BP_LU_*`, `ActivateBuff` ; le reste est cosmétique).
- **Crashs** : le build Shipping n'écrit pas de log ; UE4SS écrit `ue4ss/crash_*.dmp`, et le jeu affiche une boîte « Fatal error » qui attend un clic (fermée automatiquement par `run-scenario`).
- **Chargement de save** : le thread de jeu ne pompe plus le réseau pendant ~30–60 s (aucun tick).
- **IA du Fleuret (`L_LumiereCitizenFleuret_Male*1`, save TEST_A)** : `PerformActionByAI` tire `RandomIntegerInRange(0, 3)`. Valeur 3 → `Skill_Combo4_Earth` (3 coups sautables). Valeurs 0–2 → action de ~7 s qui ne touche pas le héros, **avec ou sans D10** (vérifié : valeur naturelle 2, et 0 imposé seul, runs du 2026-10-19 11:36–11:39). Les attaques parables à 2 coups vues à 00:35 et 09:25 dépendent donc d'autre chose que ce tirage (état de l'ennemi ?) : à étudier en phase 3. Les tests de phase 2 utilisent la graine 1865 (valeur 3 aux tours 3, 6 et 9).
- **Écran noir pendant les attaques ennemies (environnement de test)** : signalé par Killian le 2026-10-19 et confirmé par captures. L'interface est visible, mais le rendu de la scène est noir pendant les caméras d'attaque ennemie ; pendant les tours du héros, la caméra est dans des rochers bleus. Choisir l'arène (`BP_BattleMap_C`) la plus proche du joueur (19 arènes, la plus proche à ~50 m) **ne corrige pas**. Hypothèse : un combat forcé dans le hub de Lumière (prologue, save TEST_A), où le jeu ne fait pas de combat, ne trouve pas l'arène, l'éclairage ou les caméras d'un vrai combat. Piste : une save de test dans une zone de combat réelle (Prairies, Eaux volantes…), avec une rencontre de la zone. N'affecte pas la synchro.
- **Ne jamais hooker `KismetArrayLibrary:Array_Random`** (custom thunk à paramètres génériques) : avec le hook UE4SS, `FL_TargetingHelpers_C:GetRandomAliveHero` ne renvoie plus de cible. L'ennemi attaque alors dans le vide : caméra sous la carte, aucun `TryParryHitFromCharacter` (vu par Killian, puis isolé le 2026-10-19 : 0 coup en mode hôte même avec héros local, coups normaux en solo). Cela explique aussi l'« IA du Fleuret : choix 0–2 sans coup » relevé plus haut : ce n'était pas le tirage, c'était le ciblage cassé. Le ciblage sera synchronisé en phase 3 au niveau de `GetRandomAliveHero`.
- **Save de test `TEST_B`** (NG+ de fin de partie de Killian, 2026-10-19) : `Level_SpringMeadows_Main_V2`, point d'apparition `Level.SpawnPoint.SpringMeadows.Eveque` (Vallons fleuris), équipe = Verso seul. Les rencontres de la zone (`SM_*`) y ont leur vraie arène (`BP_BattleMap_Eveque_SM_C`, ~18 m) et un rendu correct. `SM_Lancelier*1` meurt en 2 tours ; `SM_Lancelier_Alpha` (≈ 7 tours, attaques parables) sert aux tests courants ; `SM_Eveque*1` (boss + 2 sbires invoqués, 15 tours, ~3 min) aux tests longs.
- **IA du Lancelier (`BP_EnemyBattle_Lancelier_Unique_V2_C`)** : deux `RandomBool` (rangs 0 et 1) dans son ubergraph.
- **Client coop** : le module coop peut lancer le combat (monde stable 5 s) avant la fin de l'attente du runner (8 s). Le runner passe donc en combat sur tout début de tour pendant ses attentes.

### Ajouts du 2026-10-19 après-midi (phase 3, build 23765773)
- **Équipe imposée** : `FFBattleStartParams.OverrideParty` (`TArray<FS_jRPG_PartySlot{CharacterHardcodedName FName, Formation byte}>`) fonctionne pour un combat déclenché par `BP_GameAction_TriggerBattle` : combat à 3 héros avec une save où Verso est seul. Noms internes dans TEST_B : `Lune`, `Maelle`, `Sciel`, `Monoco`, `Verso` (`GI.CharactersCollection`). Emplacements observés (Verso, Maelle, Lune) : Maelle 0, Lune 1, Verso 2.
- **Libellés d'énumérations** (DisplayNameMap, lus en jeu) : `E_jRPG_ActionType` 0 N/A, 1 Attack, 2 Defend, 3 Items, 4 Flee, 5 Magic (compétences), 6 SkipTurn. `EBattleEndResult` 0 Invalid, 1 Victory, 2 Defeat, 3 Neutral.
- **Compétence** : `BM.SelectAction(5)`, `BM.SelectSecondaryAction(FName NameID)`, `BM.RequestActionExecution(cibles)`. `UBP_DataAsset_Skill_C` : `NameID`, `APCost`, `TargetingType` (0 soi, 1 un ennemi, 4 un allié, 10 tous les ennemis). Trouver une compétence équipée : `Stats.FindEquippedSkillByID(SkillID, out Skill)` ; liste : `BM.GetEquippedCharacterSkills(Character, out EquippedSkills)`.
- **Objet** : `SelectAction(3)` + `SelectSecondaryAction("Consumable_Health_Level2")` (ou `_Energy_`, `_Revive_`) + `RequestActionExecution({allié})`. Stock : `BM.FindBattleItemCount(FName, out Quantity)`. Passer / fuir : `BM.OnSelectSkip()` / `BM.OnSelectFlee()`.
- **QTE offensifs** : une fenêtre par piste de `BP_BattleSequenceTimeline_C` (`TrackIndex`), séquentielles (Combo1 : 2 fenêtres de 0,62 s). Un vrai appui = `BM.OnComboPressed()` (ou `Character."Try Activate Skill Combo"`) → `TryActivate(out Success, out Perfect)` ; parfait = appui dans le dernier quart. **Écrire `IsSkillComboSuccessful`/`IsSkillComboPerfect` avant `OnSkillComboWindowEndReached` ne reproduit pas un vrai appui** : avec les mêmes drapeaux, un coup de plus que l'appui réel (test du 2026-10-19). Les drapeaux restent lisibles après `OnSkillComboWindowEndReached`. Le mode auto (`DEBUG_ComboAutoSuccessCount`) réussit sans « parfait » et sans appeler `TryActivate`.
- **Ciblage ennemi** : `AcquireTargetRandomHero` → `FL_TargetingHelpers_C:GetRandomAliveHero(__WorldContext, out Output)` ; l'out-param `Output` est réécrit dans un post-hook et l'ennemi frappe bien le héros choisi.
- **Mort** : un héros mort passe de `BM.PlayerCharacters` à `BM.DeadPlayerCharacters`. `Stats.SetDead()` met le héros mort pour le jeu (`GetIsDead` vrai) sans poser `Char.Dead?` : tester aussi `CurrentHP <= 0`. Réanimation par teinte : objet ciblé sur le héros mort. Maelle (TEST_B) se relève seule après sa mort (pictos) et encaisse les coups du Lancelier à 1 PV (boucliers / effets) : ne pas compter sur elle pour mourir.
- **Récompenses** : `BM.RollBattleRewards(out FS_BattleRewards{ExperienceGained, GoldAcquired, RolledLootEntries[{ItemID FName, Quantity, LootContextLevelOffset}]})`, appelée après `StartBattleEndFlow(1)` ; tirages dans `AC_jRPG_BattleManager_C:RollBattleRewards` (RandomBoolWithWeight) et `BP_LootSystemComponent_C:RollLootTableSection`. Fermer l'écran de victoire : `BM.OnVictoryScreenClosedFlow()`.
- **Entrées** : `BM.SetBattleInputEnabled(false)` pendant le tour d'un héros d'un autre joueur n'empêche ni `SelectAction`/`RequestActionExecution` ni `Try Activate Skill Combo` appelés par le mod.
- **Luminas de Verso (TEST_B)** : `BP_LU_FreeAim*`, `BP_LU_LightDamageBurn_C`, `BP_LU_Roulette_C` tirent à chaque coup (`ProcessDealtRawDamages`).

## 16. Session et exploration (vérifié en jeu, build 23765773, 2026-10-20, phase 4)

- **Instantané de la partie** : `BP_SaveManager_C:CreateSaveGameDataFromGameState(out SaveGameData)` → objet `BP_SaveGameObject_V8_C` construit (`StaticConstructObject`, outer = GI) → `ConvertFromSaveGameData(SaveGameData)` → `GameplayStatics:SaveGameToSlot(obj, "e33coop/snapshot", 0)` : fichier GVAS de ~438 Ko dans `SaveGames\e33coop\` (hors dossier SteamID). `SpawnPointTagToLoadAt = Level.SpawnPoint.Generic.Dynamic`, `LastTransform` = position du pion. Rechargé par `LoadGameFromSlot` + `LoadGameFromSaveFile` (depuis le menu ou en partie) : le pion réapparaît exactement à cette position (≈ 6 s de chargement depuis le menu).
- **Pion d'exploration** : `BP_jRPG_Character_World_C`, mouvement `ALSCharacterMovementComponent`. Accès sans parcours d'objets : `Controller:K2_GetPawn()`. `AddMovementInput(WorldDirection, ScaleValue (float), bForce)` le fait marcher avec l'animation ; `K2_SetActorLocationAndRotation(..., bTeleport)` le téléporte.
- **Niveau courant** : nom du package dans le nom complet du pion (`/Game/Levels/WorldMap/Camps/Level_Camp_Main.Level_Camp_Main:PersistentLevel.…`).
- **Transitions** : `FL_jRPG_CustomFunctionLibrary_C:ChangeMapByAssetName(LevelAssetName, SpawnPointTag, __WorldContext)` ; chaîne : `PrepareToChangeMap` (contrôleur, GI) → `GI:ChangeMap` → `Change Map Internal` → `ChangeMapByAssetName`, puis `OnAreaChanged`. Destination lisible sur le GI au `ChangeMap` : `LevelDestinationAssetName`, `SpawnPointTag`. Le chargement d'une save passe aussi par `ChangeMapByAssetName`. Vallons fleuris → camp : ~4 s.
- **Combat d'exploration** : `BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle(EngagementType)` lance le vrai combat de l'ennemi (sa rencontre : `SelectedEncounter`, `FDataTableRowHandle`, modifiable avant l'appel). `AC_jRPG_BattleManager_C:StartBattleNEW(FFBattleStartParams)` (hookable) puis `CurrentBattleStartParams` : `EncounterRow.RowName`, `EnemyGlobalID` (`ObjectID_Enemy_<niveau>_<classe>_<guid>`), `EngagementType`. Les acteurs placés dans un niveau ont des noms stables d'une instance à l'autre (`…_UAID_…`).
- **Fuite** : `OnSelectFlee` → `BP_jRPG_Enemy_World_Base_Seamless_C:OnBattleFinished` + `BP_jRPG_Controller_World_C:ResumeExplorationOnBattleEnd`, **sans** `StartBattleEndFlow`. L'hôte peut commencer un tour de plus que le client avant de sortir.
- **Passer le tour** (`OnSelectSkip`) ne fait rien quand le héros est seul dans l'équipe (TEST_A et TEST_B avec Verso seul).
- **Interactions** : interface `BPI_Interaction:OnInteractionRequested(InteractingWorldCharacter, Instant)`, implémentée par 13 classes (PNJ `BP_jRPG_NPC_Dialog_C`, `BP_jRPG_NPC_Base_C`, coffres `BP_Chest_Regular_C`, `BP_InteractionObjectBase_C`, `BP_jRPG_InteractionObject_Base_C`, zones `BP_GameActionTriggerZone_C`, cordes, prises d'escalade, points de grappin, documents…). L'appeler sur un PNJ lance son dialogue.
- **Dialogues** : `BP_DialogueSystemComponent_C` (`Controller.BP_DialogueSystemComponent`) : `ActivateDialogue` / `EndDialogue`, `IsInDialogue()`. Question : `StartChoice(...)` ; **`CurrentChoices` reste vide pendant la question** (ne pas s'en servir) ; réponse : `OnChoiceMade(ChoiceIndex)` (0 = première réponse). Au camp, Lune : « Souhaitez-vous passer du temps avec Lune ? Oui / Non ».
- **Zones de déclenchement** : `BP_GameActionTriggerZone_C` : `GameActionOnOverlap` / `GameActionOnInteract` (`FGameAction_Wrapper{GameActionReference, GameActionInline}`), conditions `BP_ConditionCheckerComponent`. Exécution directe : `Controller.BP_GameActionExecutorComponent:ExecuteGameAction(action)`. Types vus : `BP_GameAction_PlayInworldDialogue_C`, `…_SetQuestObjectiveStatus_C`, `…_TriggerEnemyBattle_C`, `…_LevelSequenceExecutor_C` (séquence dans le monde, pas une cinématique), `…_LevelSequenceOneShot_C`, `…_Sequential_C`, `…_RequestSaveGame_C`, `…_Hint_C`.
- **Cinématique réelle (TEST_A, Lumière acte 3)** : zone `BP_GameActionTriggerZone_C_UAID_C87F5409F1ECDB9A01_1914046506`, action `DA_GA_SQT_BossRenoir` : `TriggerCinematic` → `OnSequenceStarted` (`IsPlayingCinematic` vrai, plusieurs minutes). Elle dure **≈ 295 s** et `SkipCinematic()` (comme `SetSequencePlayRate`) reste sans effet sur toute sa durée (2 essais : fin naturelle à 294 et 295 s ; jamais vue dans cette save, donc pas passable ?). Fin : `ResumeGameAfterCinematic` → `OnSequenceFinished`, puis le combat de boss (Renoir) démarre par `BP_GameActionInstance_TriggerBattle`.
- **Scène de Sophie (Lumière, prologue, vu le 2026-10-23)** : au bas de la corde `BP_GPE_Rope_C_UAID_C87F5409F1EC030E02_1075633524`, la zone `BP_GameActionTriggerZone_C_UAID_581122C6E7A5B60902_1249392565` (au passage) exécute `DA_SEQ_MyFlower_P2` → `BP_CinematicSystem_C:TriggerCinematic` avec `Cinematic_LevelSequence_ID` = `None` et la séquence `/Game/Cinematics/Lumiere/Act01/MyFlower/MCS_MyFlower_P2` (puis ajout de Sophie à l'équipe, objectif `FoundSophie`). Chaîne d'une zone : `TryExecuteGameActionOnOverlap()` (zone) → `BP_GameActionExecutorComponent_C:ExecuteGameActionWrapperFromTriggerZone(wrapper, params)`. **Appeler `TryExecuteGameActionOnOverlap()` sur la zone d'un autre client, pion loin de la zone, lance la même scène** (état de la zone tenu par le jeu : `IsBehaviorDone`, `IsGameActionOnOverlapPlaying`). Sans passage, la séquence dure ~174 s dans les tests (les deux joueurs l'ont passée vers 30 s le 2026-10-22).
- **Passer une cinématique** : comme le joueur, `PauseCinematic()` (ses commandes s'affichent, `CinematicPaused` vrai) puis `SkipCinematic()` (`IsSkippingCinematic` vrai, `OnSequenceFinished` aussitôt). `SkipCinematic()` seul est ignoré (50 appels sans effet sur la scène de Sophie) : sans doute aussi la cause de l'échec sur Renoir ci-dessus.
- **Pause** : `Controller:OpenPauseMenu()` ouvre le menu (aussi en combat) ; `IsPauseMenuVisible` ; `PauseMenuWidget:OnResumeClicked()` le referme et le combat reprend. `GameplayStatics:SetGamePaused(ctx, bool)` met tout le jeu en pause (le tick du mod continue).

## 17. Robustesse, builds et interface (vérifié en jeu, build 23765773, 2026-10-20, phase 5)

- **Chargement = thread de jeu bloqué** : pendant un chargement de niveau ou de save (30 à 60 s), aucun tick ; un thread à part envoie des paquets « occupé » pour que les pairs ne déclarent pas la connexion perdue (`net::Link::busy_packet`).
- **Fin de cinématique** : le jeu enlève lui-même la pause de jeu (`SetGamePaused(false)` dans le déroulé de fin, avant `ResumeGameAfterCinematic`), d'où le combat de boss qui démarrait malgré la barrière. Réappliquer la pause (`IsGamePaused` faux alors qu'on la veut) suffit : le combat de Renoir ne démarre qu'à la levée de la barrière (`p4_cinematic_host`, 2026-10-20).
- **Écran de victoire** (« On continue ») : reste ouvert tant qu'on ne le ferme pas ; `BM.OnVictoryScreenClosedFlow()` le referme et rend l'exploration.
- **Menu pause sur perte de focus** : le jeu ouvre son menu pause quand sa fenêtre perd le focus (combat bloqué dans un test) ; `PauseMenuWidget:OnResumeClicked()` le referme.
- **Textes à l'écran** : `WBP_ToastCard_C` (`/Game/UI/Widgets/CommonElements/ToastNotification/`) créé par `WidgetBlueprintLibrary::Create(pc, classe, pc)` puis `AddToViewport(z)` ; le texte se pose par `Init(FText, Theme)` **après** l'ajout à l'écran (avant, il garde « Sample Text »). Bandeau en haut de l'écran, visible en exploration et en combat ; `Duration` règle le fondu ; `SetRenderTranslation` le décale. FText fabriqué par `KismetTextLibrary::Conv_StringToText`. `FL_ToastNotificationHelpers_C:ShowToastNotificationOnWidget` (sur le HUD d'exploration) n'a rien affiché de lisible.
- **Personnages** : `GI.CharactersCollection` (`TMap<FName, UBP_CharacterData_C*>`). `UBP_CharacterData_C:SaveCharacterState(out OutState, out StructOut)` donne le `FS_jRPG_CharacterSaveState` complet ; `LoadCharacterState(CharacterState)` le réapplique : l'empreinte relue après application est **identique** à celle de l'émetteur (test `p5_builds_client`). Sérialisation sans SDK : un `BP_SaveGameObject_V8_C` neuf dont seule la map `CharactersCollection` est remplie (copie profonde `FProperty::CopyCompleteValue` depuis `CreateSaveGameDataFromGameState`), écrit par `SaveGameToSlot` : ~28 Ko pour 5 personnages ; relu par `LoadGameFromSlot`.
- **Menus de build** : le panneau des Luminas (`WBP_GM_Luminas_Panel_C:ToggleLumina`) retire un Lumina par `UnequipLuminaPassiveList` mais en ajoute un en modifiant directement `EquippedPassiveEffects` (aucune fonction de données dédiée) : la synchro des builds se fait donc par état, pas par rejeu des clics. Fonctions utiles relevées : `AssignPointToAttribute`, `ResetAssignedAttributePoints`, `TryUnlockSkill`, `EquipSkill`/`UnequipSkill`, `EquipItemInSlot`, `ReplaceItemInstanceInSlot`, `DispatchModificationsIfPossible(reason)` (rafraîchit les dépendants).
- **Ressources communes** : or `GI.Gold` ; objets `GI.Inventory` (`TArray<FS_jRPG_Item_DynamicData{StacksAmount, ItemStaticData{Item_HardcodedName…}}>`) ; niveaux d'équipement `GI.InventorySystem.ItemInstances` (`TMap<FName, UBP_ItemInstance_Base_C*>`, `CurrentLevel` sur les instances d'équipement, `ChangeLevel(NewLevel, reason)`). Contrôleur : `AC_jRPG_InventoryManager` (`ReceiveGold`, `TrySpendGold(out Success)`, `AddItemToInventory`, `RemoveItemFromInventory`, `BuyItem`, `UseItemOnCharacter`). Amélioration d'arme : `BP_ItemUpgradeSystem_C:UpgradeWeaponFromInstance`.
- **Contextes d'entrée d'exploration** : `IMC_UI_Exploration` = seulement `IA_UI_Interact` (E, X manette) ; `IMC_UI_Dialogue` = `IA_UI_Dialogue_Confirm` (F, A), `_Leave` (Échap, B), `_AutoSkip` (Espace) ; `IMC_UI_Menu` = menu de jeu (Tab), pause (Échap), état de l'équipe (Q). Retirer les deux premiers chez un suiveur bloque ses interactions et ses réponses au clavier / à la manette sans toucher aux menus ; un clic de souris sur une réponse reste possible.
- **Écran du menu de jeu** (`OpenMainMenu`) : piloté à la souris et à la manette ; `Entrée` n'ouvre pas la page d'un personnage.

## 18. Menus de combat (lus dans le dump, build 23765773, 2026-10-22 ; miroir D17, à vérifier en jeu)

- Chez l'observateur (captures `p7_intent_host`), la roue des actions du héros d'un autre joueur est **déjà affichée** pendant son tour (le jeu du tour tourne ici) ; l'action rejouée par `SelectAction(1)` affiche l'infobulle « Sélectionnez une cible / Attaquer ».
- Roue : `BM.BattleScreenWidget` (`WBP_HUD_BattleScreen_C`, +0x2B8) → `WBP_HUD_WheelsContainer` (+0x5A8) ; aussi `BM.LGUI_Actor_BattleWheels.WheelsContainer`. Le conteneur [dump] : pages `WBP_BattleWheel_Action`, `WBP_HUD_SkillWheel_Skills`, `_Items`, `_GradientAttacks` ; page active `IActiveWheel` (`TScriptInterface<WBPI_WheelPage>`, +0x470 ; l'interface n'a que `Appear`/`Disappear`) ; `ChangeWheelPage(IWheelPage)`, `OnSkillsActionChosen`, `OnItemsActionChosen`, `OnUltimateActionChosen` (gradient), `OnAttackActionChosen`, `OnAimActionChosen`, `BackToActionPage`, `OnBackFromSkills`, `UpdateCharacterSkillWheels`, `UpdateItemsWheel`, `OnSkillChosenFromList(SkillID)`, `TriggerSkillFromWheel(SecondaryAction)`. Ce que font ces fonctions (appels au BM ou non) n'est pas lisible dans le dump [inferred : boutons de la roue].
- Liste (`WBP_HUD_SkillWheel_C`) : **pas de curseur** : deux colonnes de 3 boutons (touches directes), `IsLeftColumnActive`, `ResetColumnToLeft/Right`, `HighlightColumn(bool)`, `SetInputsEnabled(bool)`, colonnes `WBP_HUD_SkillList-Left/-Right` → `WBP_HUD_SkillButton_Big_1..3` (`Button` UButton, `SetPromptHighlighted(bool)`, `SetInputEnabled(bool)`).
- Ciblage (`AC_jRPG_TargetingManager_C`, contrôleur +0x908) : `TargetingEnabled?`, `SelectedTargetingType`, `TargetedCharacters`, `TargetGroups` (`BP_TargetingGroup_C` : `Characters`, `FocusTargetGroup`, `OnUIAction`, `OnUINavigationFocus`), `TryFocusTargetGroup(TargetGroup)`, `SetFocusedTargetingGroup(TargetingGroup)`, `ShiftTargetingGroup(int)`, `ConfirmTarget` (jamais appelé par le miroir), `CancelTargeting`. Les groupes passent par la navigation d'interface du jeu (`RegisterTargetingGroupsInUINavigation`).
- Navigation d'interface du jeu : `BP_UINavigationComponent_C` (contrôleur +0x8D0), `DisableNavigation` (+0x2F9), `DisableInput()`/`EnableInput()`, entrées par `BP_UINavigationInputActor_C` (axes et boutons de face, indépendant des contextes Enhanced Input retirés en phase 5) [dump].
- Clics sur la roue 3D : `ABP_LGUI_UIContainer_BattleWheelRoot_C.LGUIWorldSpaceRaycaster` (`ULGUIBaseRaycaster::DeactivateRaycaster` / `ActivateRaycaster`) [dump].
