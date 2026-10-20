-- Phase 4 : journalise transitions, cinématiques, dialogues, combats d'exploration, pause (explore.jsonl).
local fns = {
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:ChangeMap",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:PrepareToChangeMap",
  "/Game/jRPGTemplate/Blueprints/Basics/FL_jRPG_CustomFunctionLibrary.FL_jRPG_CustomFunctionLibrary_C:ChangeMapByName",
  "/Game/jRPGTemplate/Blueprints/Basics/FL_jRPG_CustomFunctionLibrary.FL_jRPG_CustomFunctionLibrary_C:ChangeMapByAssetName",
  "/Game/jRPGTemplate/Blueprints/Basics/FL_jRPG_CustomFunctionLibrary.FL_jRPG_CustomFunctionLibrary_C:Change Map Internal",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:PrepareToChangeMap",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:OnAreaChanged",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:OpenPauseMenu",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:SetGameplayPaused",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:ResumeExplorationOnBattleEnd",
  "/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:TriggerCinematic",
  "/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceStarted",
  "/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceFinished",
  "/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:ResumeGameAfterCinematic",
  "/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:ActivateDialogue",
  "/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:EndDialogue",
  "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:StartBattleNEW",
  "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:InitBattle",
  "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:StartBattle",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:OnEnemyBattledEvent",
  "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:OnBattleFinished",
  "/Game/Gameplay/GameActionsSystem/TriggerBattle/BP_GameActionInstance_TriggerBattle.BP_GameActionInstance_TriggerBattle_C:ExecuteGameAction",
}
local res = {}
_G.E33WorldHooks = _G.E33WorldHooks or {}
for _, f in ipairs(fns) do
  if not _G.E33WorldHooks[f] then
    local ok, err = pcall(function()
      RegisterHook(f, function(ctx, ...)
        local who = ""
        pcall(function() who = ctx:get():GetFName():ToString() end)
        E33Log("world_hook", f:match(":(.*)$") .. " on " .. who)
      end)
    end)
    if ok then _G.E33WorldHooks[f] = true end
    res[#res+1] = (ok and "ok   " or "FAIL ") .. f:match("([^%.]+:.*)$") .. (ok and "" or (" " .. tostring(err)))
  end
end
return table.concat(res, "\n")
