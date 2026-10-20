local pc = FindFirstOf("BP_jRPG_Controller_World_C")
local bm = pc.AC_jRPG_BattleManager
local ITEM = _G.E33_ITEM or "Consumable_Health_Level2"
local SPOT = _G.E33_SPOT or 0
local target
bm.PlayerCharacters:ForEach(function(i, e) local c = e:get(); if c.BattleSpotIndex == SPOT then target = c end end)
bm:SelectAction(3)
bm:SelectSecondaryAction(FName(ITEM))
bm:RequestActionExecution({target})
return "item " .. ITEM .. " -> " .. target:GetFName():ToString() .. " by " .. bm.CurrentCharacter:GetFName():ToString()
