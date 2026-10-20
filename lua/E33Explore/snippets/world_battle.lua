-- Phase 4 : combat d'exploration naturel (StartBattle de l'ennemi du monde le plus proche).
local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local p = pawn:K2_GetActorLocation()
local best, bd
for _, e in ipairs(FindAllOf("BP_jRPG_Enemy_World_Base_Seamless_C") or {}) do
  if e:IsValid() then
    local l = e:K2_GetActorLocation()
    local d = math.sqrt((l.X-p.X)^2 + (l.Y-p.Y)^2)
    if not bd or d < bd then best, bd = e, d end
  end
end
_G.E33Enemy = best
best:StartBattle(0)
return string.format("StartBattle on %s d=%.0f enc=%s", best:GetFName():ToString(), bd, best.SelectedEncounter.RowName:ToString())
