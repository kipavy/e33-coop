-- Phase 4 : le pion peut-il être piloté par le code (AddMovementInput) pendant 2 s, entrées joueur verrouillées ?
local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local p0 = pawn:K2_GetActorLocation()
_G.E33MoveStart = { X = p0.X, Y = p0.Y, Z = p0.Z }
local n = 0
LoopAsync(16, function()
  n = n + 1
  ExecuteInGameThread(function()
    if pawn:IsValid() then pawn:AddMovementInput({ X = 1.0, Y = 0.0, Z = 0.0 }, 1.0, true) end
  end)
  return n >= 120
end)
return string.format("start %.0f %.0f %.0f", p0.X, p0.Y, p0.Z)
