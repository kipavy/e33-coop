local pc = FindFirstOf("BP_jRPG_Controller_World_C")
local bm = pc.AC_jRPG_BattleManager
local SK = _G.E33_SKILL or "QuickStrike"
local target
bm.Enemies:ForEach(function(i, e) if not target then target = e:get() end end)
bm.DEBUG_ComboAutoSuccessCount = _G.E33_COMBO or 10
bm:SelectAction(5)
bm:SelectSecondaryAction(FName(SK))
bm:RequestActionExecution({target})
return "skill " .. SK .. " by " .. bm.CurrentCharacter:GetFName():ToString()
