-- Déclenche un combat via BP_GameAction_TriggerBattle (recette de E33 Versus, MIT).
local ENC = _G.E33_ENC or "L_LumiereCitizenFleuret_Male*1"
local function loadObj(path, cls)
  local o = StaticFindObject(path .. "." .. cls)
  if not o or not o:IsValid() then LoadAsset(path); o = StaticFindObject(path .. "." .. cls) end
  assert(o and o:IsValid(), "could not load " .. path)
  return o
end
local dt = loadObj("/Game/jRPGTemplate/Datatables/DT_jRPG_Encounters", "DT_jRPG_Encounters")
local cls = loadObj("/Game/Gameplay/GameActionsSystem/TriggerBattle/BP_GameAction_TriggerBattle", "BP_GameAction_TriggerBattle_C")
loadObj("/Game/Gameplay/GameActionsSystem/TriggerBattle/BP_GameActionInstance_TriggerBattle", "BP_GameActionInstance_TriggerBattle_C")
local ex = FindFirstOf("BP_GameActionExecutorComponent_C")
assert(ex and ex:IsValid(), "no executor component")
local maps = FindAllOf("BP_BattleMap_C") or {}
table.sort(maps, function(a, b) return a:GetFName():ToString() < b:GetFName():ToString() end)
local a = StaticConstructObject(cls, ex, FName("E33Coop_Trigger_" .. os.time()))
local s = a["Battle Start Params"]
local row = s.EncounterRow_25_76E166214BC95AA2A6A68DAD97A47C62
row.DataTable = dt
row.RowName = FName(ENC)
s.EnemyGlobalID_12_57EC6FC84879C6C042EC7BB68F507FE2 = FName("E33CoopTest")
s.EngagementType_15_D00812C641AFF3AF663DC6B910763D7D = 0
s.BattleMapBP_18_E90DB870407FE894CA7200B075D98B4A = maps[1]
s.TransitionType_31_74930912436832AA7F5A1C8F139A7E9A = 0
s.PlayCameraIntro_21_FE94DAD74E9218AB0C1D48A3DC163272 = false
s.BackToExplorationOnLost_38_6E7B04094D32F81767771FA52BAE96D1 = true
ex:ExecuteGameAction(a)
return "battle triggered: " .. ENC .. " on " .. tostring(maps[1] and maps[1]:GetFName():ToString())
