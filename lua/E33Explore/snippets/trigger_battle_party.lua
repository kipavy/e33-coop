-- Combat de test avec une équipe imposée (OverrideParty), sur l'arène la plus proche.
local ENC = _G.E33_ENC or "SM_Lancelier_Alpha"
local PARTY = _G.E33_PARTY or {"Verso", "Maelle", "Lune"}
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
local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local pl = pawn:K2_GetActorLocation()
local best, bd
for _, m in ipairs(FindAllOf("BP_BattleMap_C") or {}) do
  local l = m:K2_GetActorLocation()
  local d = (l.X-pl.X)^2 + (l.Y-pl.Y)^2 + (l.Z-pl.Z)^2
  if not bd or d < bd then best, bd = m, d end
end
local a = StaticConstructObject(cls, ex, FName("E33Coop_Trigger_" .. os.time()))
local s = a["Battle Start Params"]
local row = s.EncounterRow_25_76E166214BC95AA2A6A68DAD97A47C62
row.DataTable = dt
row.RowName = FName(ENC)
s.EnemyGlobalID_12_57EC6FC84879C6C042EC7BB68F507FE2 = FName("E33CoopTest")
s.EngagementType_15_D00812C641AFF3AF663DC6B910763D7D = 0
s.BattleMapBP_18_E90DB870407FE894CA7200B075D98B4A = best
s.TransitionType_31_74930912436832AA7F5A1C8F139A7E9A = 0
s.PlayCameraIntro_21_FE94DAD74E9218AB0C1D48A3DC163272 = false
s.BackToExplorationOnLost_38_6E7B04094D32F81767771FA52BAE96D1 = true
local party = {}
for i, n in ipairs(PARTY) do
  party[i] = { CharacterHardcodedName_2_2A63D4C5433428900D69748563F50580 = FName(n), Formation_5_710E67F044BACE578CEE51AF24DC58B0 = 0 }
end
s.OverrideParty_63_56C3E35647CCE3DAC58BE292F8A3FFC0 = party
local n = 0
s.OverrideParty_63_56C3E35647CCE3DAC58BE292F8A3FFC0:ForEach(function(i, e) n = n + 1 end)
ex:ExecuteGameAction(a)
return "battle triggered: " .. ENC .. " party slots=" .. n .. " map=" .. best:GetFName():ToString()
