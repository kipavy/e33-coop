-- Fait attaquer le personnage courant (s'il est contrôlé par le joueur) sur le premier ennemi vivant.
local pc = FindFirstOf("BP_jRPG_Controller_World_C")
local bm = pc.AC_jRPG_BattleManager
local cur = bm.CurrentCharacter
if not cur or not cur:IsValid() then return "no current character" end
if not cur.IsPlayerControlledCharacter then return "not a player turn: " .. cur:GetFName():ToString() end
local target
bm.Enemies:ForEach(function(i, e) if not target then target = e:get() end end)
if not target then return "no enemy" end
bm.DEBUG_ComboAutoSuccessCount = 10
bm:SelectAction(1)
bm:RequestActionExecution({target})
return "attack " .. cur:GetFName():ToString() .. " -> " .. target:GetFName():ToString()
