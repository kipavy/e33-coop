# Étude du mod Kouzukii/expedition33-coop

Commit étudié `6038f67` (2025-12-19), sous-module `ue-hook-blueprints` `98629d7`. Release v0.9.5, cible le jeu **1.5.1**. MIT « Copyright 2025 Kouzukii ».
Lecture seule : rien n'a été exécuté ni compilé. Logique des graphes obtenue en décodant les données « FiBData » (UTF-16 décalé de +1) des .uasset éditeur. Listes de noms compilés du jeu (v1.5.1) extraites du .ucas de la release : copie locale dans `.tools/ref/report-kouzukii-appendix/` (non versionnée, c'est du contenu du jeu).

[fait] = lu dans les données ; [déduit] = inférence.

## 1. Structure, build, chargement

- `Sandfall/Sandfall.uproject` : projet UE **5.4** nommé comme le jeu, plugin CommonUI. `Source/Sandfall/SandfallBlueprintHelpers.*` = dump d'en-têtes (style UE4SS CXX) de `USandfallBlueprintHelpers` natif, corps vides, pour que les BP compilent. Module natif réel du jeu : `/Script/SandFall`.
- `Content/Coop/` : BP du mod (`ModActor`, `CoopController`, `ModLibrary`, `CoopConfig` (SaveGame), `ModDebugWidget`, `PlayerIndicator`, `UserSwitchWidget`). Autres dossiers = stubs aux chemins exacts du jeu, portant des fonctions `hook_X` / `orig_X`.
- `Mappings.usmap` (2,2 Mo) pour le jeu 1.5.x.
- Build [fait] : `retoc to-legacy <Paks> .\Export` → package Shipping sans pak/IoStore → `Program.cs` (.NET 10) : corrige les noms de membres de structs à suffixe GUID (`CharacterHardcodedName_10_<GUID>`, `OverrideParty_3_<GUID>`, depuis `S_jRPG_PartySlot` et `FBattleStartParams`), patche en bytecode `ModActor.set_PlayerControllerClass` (écrire `GameModeBase.PlayerControllerClass`), lance ue-hook-blueprints sur 7 assets, `retoc to-zen` → `expedition33_coop_P` (~1,35 Mo).
- Add-on optionnel : patche `DT_CommonUISpecificAction` / `DT_CommonUIActionComposite` (actions maintenues des menus sur X au lieu de A).
- Chargement : simple pak de remplacement dans `Content\Paks\~mods\`. **Ni UE4SS, ni loader BP, ni DLL.** Entrée : `BP_jRPG_GM_World.ReceiveBeginPlay` hooké → spawn `ModActor_C`.
- **ue-hook-blueprints** (inspiré de spaghetti) : hors ligne, renomme la fonction d'origine `Name` en `orig_Name`, copie `hook_Name` comme `Name`, réécrit imports/exports/noms, vérifie la signature, écrit l'asset augmenté. Conséquences : seulement les fonctions implémentées en BP (pas le natif ni les events d'event-graph) ; l'asset livré est une **copie complète du BP du jeu**, à reconstruire à chaque patch (issue #5 : crash 1.5.0 « Serial size mismatch » sur `BP_CharacterBattle_Dummy`) ; exclusif avec tout autre mod touchant ces assets ; pas de nouvelles variables.

## 2. Classes, fonctions, chemins du jeu

### 2.1 Assets hookés [fait]
1. `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C` (parent `Character`) : `OnTurnStart`, `OnTurnEnded`, `TryStartParry`, `TryStartDodge`, `TryStartJump`, `TryStartGradientParry`, `TryTriggerJumpCounterAttack` (renvoient bool `Success`), `StartDefensiveMove(LockDuration, ImmuneDuration, WarmUpDuration : double, Reason)`. Variables : `BattleSpotIndex` (int), `IsPlayerControlledCharacter` (bool).
2. `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World_C` (PlayerController, base `ALS_Player_Controller`) : `PrepareToChangeMap(LevelData: FLevelDataEntry)`, `ResumeExplorationOnBattleEnd(EnemyGlobalID, BattleEndResult: EBattleEndResult)` (s'appelait `OnBattleEnd` avant 1.4.0). Utilise `IsInBattle()`, composant `AC_jRPG_BattleManager`.
3. `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GM_World_C` (GameModeBase) : `ReceiveBeginPlay`, `HandleStartingNewPlayer`, `K2_OnRestartPlayer`.
4. `/Game/UI/Widgets/MainMenu/WBP_MM_MainMenu` : `GetBuildText`.
5. `/Game/UI/Widgets/InGame_Menu/Team/WBP_TeamCard_CharacterStatus` : `LoadCharacter(CharacterData: BP_CharacterData_C)` (GridPanel parent, `PartyIndicatorOverlay`).
6. `/Game/UI/Widgets/InGame_Menu/Team/CharacterSelector/WBP_CharacterCardButton` : `LoadCharacterState(Character)` (`CharacterImage`).
7. `/Game/UI/Widgets/InGame_Menu/Settings/OptionsList/WBP_Settings_GeneralOptionsList` : `SetupAvailableOptions` (ajoute une section de réglages, sauvegarde slot « Coop »).

### 2.2 Autres membres utilisés [fait]
- `/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C` (sur Controller_World) : `CurrentCharacter` (tour courant ; cast `BP_jRPG_Enemy_Battle_Base_C` pendant les tours ennemis), `BattleParty` (array `S_jRPG_PartySlot`), `CurrentBattleStartParams` (`FBattleStartParams` : `OverrideParty`, `AllowReserveTeam`), `GetPlayerCharacterAtBattleSpot(SpotIndex)`, `IsAttackingHeroes(Enemy) -> IsAttacking, TargetedHeroes[]`, `ShowBattleTooltip(TooltipName, AutoFadeout)`.
- `/Game/jRPGTemplate/Blueprints/Basics/FL_jRPG_CustomFunctionLibrary_C.GetBattleManager`.
- `/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom_C` (GameInstance) : `CurrentParty`.
- `/Game/jRPGTemplate/Structures/S_jRPG_PartySlot` : `CharacterHardcodedName` (FName ; « Sophie » cas spécial du prologue), `Formation` (`E_jRPG_FormationType` Frontline/Backline).
- `/Game/Gameplay/Battle/EBattleEndResult` (Victory/Defeat/Neutral/Invalid, ordre non établi), `/Game/Gameplay/Battle/FBattleStartParams`, `/Game/Gameplay/LevelData/FLevelDataEntry`.
- `/Game/Gameplay/CharacterData/BP_CharacterData_C` (`HardcodedNameID`), `/Game/Gameplay/SkillTree/BP_DataAsset_Skill` (`NameID`).
- Input : `/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_BattleDefense`, `.../Battle/InputActions/IA_Battle_Defense_{Parry,Dodge,Jump,JumpCounter,GradientCounter}`, enums `/Game/Gameplay/Inputs/EInputAction`, `EFaceActions`.
- UI réglages : `WBP_Settings_OptionEntry`, `WBP_Settings_OptionEntriesHeader`, `WBP_Settings_GenericPage`, `WBP_NamedIntegerSelector`, `WBP_SliderSelector`, `BP_ButtonGenericStyle`.
- Moteur : `CreatePlayer`, `RemovePlayer`, `SetPlayerPlatformUserId`, `GetNumPlayerControllers`, `SetForceDisableSplitscreen`, `GetAllActiveUsers`, `GetPrimaryPlatformUser`, `EnhancedInputLocalPlayerSubsystem.AddMappingContext`.

### 2.3 Listes de noms compilés du jeu v1.5.1 (noms = fait ; rôles = déduit)

**BP_jRPG_Character_Battle_Base**
- Tours : `OnTurnStart`, `OnTurnEnded`, `OnSelfTurnStart/End`, `OnCharacterTurnEnd`, `OnPuppetTurnEnded`, `IsCurrentTurn`, `IsTurnActive`, `TurnState`, `EBattleTurnState`, `Push/Pop/UpdateBattleTurnState`, `OnBattleTurnStateChanged`, `BattleStateStack`, `RequestAdditionalTurnEnd`, `AddInitiative`, `CurrentInitiative`, `OnInitiativeReduced`, `PlayBattleTurnSequence`, `OnSkipTurnChosen`, `GetForcedSkillOnNextTurn`.
- Actions : `ExecuteAction`, `ExecuteAttack/Skill/Magic/Item/Defend/Flee/MoveAction`, `ExecuteSkill_Internal`, `ExecuteSkillScript`, `ExecuteSkillEvent`, `ExecuteGradientCounterAttack`, `ExecuteSoloCounterAttack`, `PerformActionByAI`, `PerformActionByAI_Internal` (décision IA probable), `SelectedActionType`, `SelectedTargetingType`, `CurrentBattleSkill`, `SkillID`, `GetSkillCost`, `BattleActionQueue`, `OnCharacterActionFinished`, `HasMoreActions`, `OnSkillPreExecution`, `TickBattleSequenceTimeline`, `OnBattleSequenceActionEffect`, `ActivateActionEffect`, `BP_Battle_SkillScript_C`, `BP_SkillState_C`, `BP_BattleSequenceTimeline_C`.
- Dégâts : `DealDamages`, `ReceiveDamage`, `OnDamageReceived(Public)`, `MakeDamageBuilder`, `BP_BattleDamageBuilder_C`, `BP_BattleDamages_C`, `DamageMultiplier`, `EDamageReason`, `GetIsCriticalHit`, `PerformStandardHit`, `ApplyStandardStatusEffectOnTarget`, `AddShieldPoints`, `SetDead`, `OnDeath`, `NotifyDeath`, `CheckBattleEnd`, `IsAFullTeamDead`, `KickCharacterFromBattle`, `Generic_*Counter_BaseDamage`, `DT_Balancing_MainCharacterSkills`, `DT_Balancing_BattleDifficultySettings`.
- Défense : `TryStartParry/Dodge/Jump/GradientParry`, `TryTriggerJumpCounterAttack`, `StartDefensiveMove`, `EndDefensiveMove`, `StartParry`, `StartDefend`, `StopDefend`, `UpdateDefend`, `CanDoDefensiveMove`, `CanDoJumpCounterAttack`, `IsParrying`, `IsGradientParrying`, `IsJumping`, `IsPerfectDodge`, `GetIsDefending` ; verrous `StartDefenseLock`, `ClearDefensiveMoveLock`, `AdjustDefensiveMoveLockTimer`, `IsDefenseLockActive`, `IsDodgeLockActive`, `IsParryLocked`, `DefensiveMoveImmunityStartTime/EndTime`, `DefensiveMoveLockEndTime` ; paramètres `ParryImmuneDuration`, `ParryLockDuration`, `DodgeImmuneDuration`, `DodgeLockDuration`, `DodgeSuccessLockDuration`, `GradientParryImmune/LockDuration`, `JumpImmune/Lock/WarmUpDuration`, `ImmuneDuration_{Parry,Dodge}_{Easy,Normal,Hard}`, `Get(Global)ParryWindowModifier`, `Get(Global)DodgeWindowModifier`, `JumpCounterAttackWindowDuration/Slomo/StartDelay` ; événements `OnParryStart`, `OnParrySuccess`, `OnDodgeStart`, `OnDodgeSuccess`, `OnPerfectDodge`, `OnJumpStart`, `OnJumpCounter`, `OnGradientParryStart`, `OnDefendSuccess`, `OnDefenseStateChange`, `OnDefenseLockStart/End`, `OnDefensiveMoveEnd`, `FeedbackSuccessfulParry/Dodge` ; attaque ennemie `IsCurrentAttackUnblockable/Undodgeable/Jumpable/Gradient`, `CanBeCountered`, `CounterAttackRequiresPerfect`, `ECounterType` ; PA `APGainParry`, `APParryCost`, `APDodgeCost`.
- QTE offensifs / combos : `GetQTEManager`, `BP_QTEManager_C`, `AC_jRPG_QTEManager`, `OnStartAllowQTE`, `OnStartedAllowQTE`, `UseDiegeticQTE`, `QTE_Keyframes`, `QTEKeyframeType`, `SkillComboQTE_SuccessFeedback/FailFeedback`, `IsSkillComboSuccessful`, `IsLastSkillComboSuccessful`, `ComboSuccessHistory`, `ComboPerfectHistory`, `OnSkillComboAction`, `OnSkillComboFailed`, `local_ComboWindowStart/EndTime`, `DEBUG_CurrentComboAutoSuccess`.
- Visée libre : `EnableFreeAim`, `StartFreeAim_Regular`, `FireFreeAimProjectile`, `OnFreeAimHit`, `TryCancelFreeAim`, `FreeAimEndTurnInternal`, `IA_Battle_FreeAim_Shoot/Cancel`.
- Ciblage : `AcquiredTargets`, `ConfirmTarget`, `TargetedCharacters`, `AC_jRPG_TargetingManager_C`.
- Aléatoire : seul `Array_Random` dans ce BP [déduit : le reste est dans les skill scripts, le damage builder ou le natif].
- Possession : `IsPlayerControlledCharacter`, `IsPlayerTeam`, `BattleSpotIndex`, `GetBattleSpotIndex`, `CanGetInput`, `GetJRPGControllerWorld`.
- Classes importées : `/Game/Gameplay/QTE/BP_QTEManager`, `/Game/Gameplay/Battle/FL_BattleHelpers`, `.../Battle/BP_BattleDamageBuilder`, `.../Battle/Skills/BP_Battle_SkillScript`, `.../SkillStateSystem/BP_SkillState`, `.../SkillCombos/BP_BattleSequenceTimeline`, `.../EventRoutingSystem/BP_EventRoutingSystemComponent`, `/Game/Gameplay/Buffs/BP_BattleBuffComponent`, `AC_jRPG_{ActionsManager,BattleManager,CharacterBattleStats,CharactersManager,CombatMovement,DynamicCameraManager,InventoryManager,TargetingManager}`, `BPI_BattleCommunication`, `BP_jRPG_CharacterBattleSpot`, `BP_jRPG_EnemyBattleSpot`, `/Game/Gameplay/Inputs/BP_InputManager`, `.../InputLock/BP_InputLockComponent`, `/Game/UI/Widgets/HUD_Battle/WBP_HUD_BattleScreen`.

**BP_jRPG_Controller_World**
- Combat : `OnBattleRequested`, `OnBattleBeginTransitionRequested`, `StartBattleNEW`, `OnBattleStart(s)`, `OnBattleEnd(s)`, `OnBattleFinished`, `OnBattleFled`, `ResumeExplorationOnBattleEnd`, `RestartCurrentEncounter`, `RollEncounter`, `DT_jRPG_Encounters`, `IsInBattle`, `IsTakingActionInBattle`, `E_BattleFlowState`, `S_BattleInitialState`, `CurrentBattleStartParams`, `PrepareSystemsForBattle`, `BP_GameAction_TriggerBattle_C`.
- Input défense : `TryBindDefendInput`, `UnbindDefendInput`, `IsDefenseInputBound`, `OnParryAction`, `OnDodgeAction` ; lie `IA_Battle_Defense_*` et `/Game/Gameplay/Inputs/InputMappingProfiles/Battle/InputActions/IA_Battle_QTE`.
- Plomberie : `SetInputModeBattle`, `Add/RemoveInputLock`, `GetInputManager`, `OnControllerConnectionChanged`, `Possess`, `AC_jRPG_QTEManager`, `UseDiegeticQTE`, `DEBUG_CVarChange_SingleCharacterParty`.
- Carte / save : `PrepareToChangeMap`, `IsMapChangeInProgress`, `LoadCharacterStateFromGameInstance`, `RequestAutoSave`, `BP_SaveManager_C`.

**BP_jRPG_GM_World** : `HandleStartingNewPlayer`, `K2_OnRestartPlayer`, `RestartPlayerAtTransform`, `PlayerCanRestart`, `OnPreStartPlay`, `RequestGameFlowTransition`, `GetCustomGI`.

## 3. Son modèle manette → personnage

- `ModActor.ReceiveBeginPlay` : `SetForceDisableSplitscreen(true)`, charge `CoopConfig` (slot « Coop »), échange temporairement `PlayerControllerClass` pour `CoopController_C` et `CreatePlayer` pour chaque joueur supplémentaire. P2/P3 = vrais joueurs locaux sans pion ni HUD (les hooks GameMode sautent l'original pour eux). `PrepareToChangeMap` les supprime avant changement de carte ; le niveau suivant les recrée.
- `CoopConfig` : `Character{1,2,3}Assignment`, `Player{1,2,3}DefenseWindow`. `ModLibrary.MapBattleSpotToUser(BattleSpotIndex)` : spot → index d'équipe (cas spéciaux : combats solo / marchand via `OverrideParty` + recherche par nom dans `GI.CurrentParty`, prologue « Sophie ») → joueur → utilisateur d'entrée.
- **Choix d'action et QTE offensifs** : un seul contrôleur PC0 ; `OnTurnStart` hooké fait `SetPlayerPlatformUserId(PC0, manette du propriétaire)` + tooltip « Player i's Turn ». Tout le reste (menus, ciblage, QTE, visée libre) tourne tel quel sur PC0 → d'où l'issue #4 (clignotement des invites, double appui en QTE pour P2).
- **Défense** : les `TryStart*` hookés n'exécutent l'original que si le personnage appartient à P1. Les `CoopController` ajoutent `IMC_BattleDefense` ; sur action, `IsControlledCharacterDefending` (`BattleManager.CurrentCharacter` → `IsAttackingHeroes` → `TargetedHeroes`) puis appelle `orig_TryStart*` sur le(s) personnage(s) visé(s) du joueur. `StartDefensiveMove` hooké multiplie les durées par la fenêtre de défense du joueur (compensation du lag Remote Play).
- Pas d'échange de possession, pas de modification de l'ordre des tours.

## 4. Adéquation avec notre architecture

- L'échange de périphérique sur PC0 n'a pas de sens en ligne (les manettes distantes n'existent pas chez l'hôte) : hooks de tour, bascule Start et add-on de boutons seraient tous à retirer.
- L'idée de défense est réutilisable : filtrer `TryStart*` par propriétaire. Chez le propriétaire : capturer le résultat (`OnParrySuccess`, `OnDodgeSuccess`, `OnPerfectDodge`, `OnJumpCounter`, `OnDefenseStateChange`, horodatages d'immunité, `ReceiveDamage`). Chez l'hôte : bloquer `TryStart*` locaux pour les héros distants et injecter le résultat distant — difficulté : l'hôte décide touché/paré dans sa propre timeline (`TickBattleSequenceTimeline`, `OnBattleSequenceActionEffect`, `ReceiveDamage`), il faut donc retarder ou forcer le résultat. Le mod ne rencontre jamais ce problème (une seule simulation).
- QTE offensifs non traités par le mod. Nos points : `OnStartAllowQTE`/`OnStartedAllowQTE`, `BP_QTEManager`/`AC_jRPG_QTEManager`, `IA_Battle_QTE`, fonctions de résultat de combo, `OnFreeAimHit`.
- Aucune notion de résultat, d'aléatoire ou de réplication.
- Forker imposerait projet éditeur UE 5.4, cook, patcheur .NET/UAssetAPI, retoc, usmap et remplacement complet de 7 BP à refaire à chaque patch, plus UE4SS de toute façon pour le réseau → deux systèmes de hooks. UE4SS C++ peut hooker les mêmes fonctions à l'exécution (`RegisterHook("/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C:TryStartParry", …)`) sans remplacer d'asset, et atteint aussi le natif.
- Réutilisable sans forker (MIT, avec crédit) : la carte des hooks, les requêtes sur le BattleManager, la logique de propriété avec ses cas spéciaux, les points d'injection UI, le `.usmap`, les leçons (les noms changent entre patchs → résoudre par nom et échouer en douceur ; les remplacements complets de BP cassent ; plusieurs manettes sur un contrôleur font clignoter CommonUI).

## 5. Licence, activité, problèmes connus

- MIT « Copyright 2025 Kouzukii » (repo et ue-hook-blueprints) ; ue-hook-blueprints crédite spaghetti ; UAssetAPI est MIT séparé. **Attention** : le repo et le .ucas contiennent des copies d'assets du jeu (propriété de Sandfall), non couvertes par la MIT.
- Créé 2025-05-26, dernière release v0.9.5 (2025-12-18, jeu 1.5.1), dernier commit 2025-12-19 ; ~10 mois d'inactivité ; auteur unique.
- Issues : #4 (ouverte) clignotement d'invites / double appui QTE ; #2 crash après combat en 1.4.0 (renommage `OnBattleEnd`) ; #5 crash 1.5.0 (copie de BP périmée) ; #1/#3 environnement de build.
- Limites : 3 joueurs ↔ 3 emplacements, réserve non gérée, local uniquement.
