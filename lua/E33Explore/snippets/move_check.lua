local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local p = pawn:K2_GetActorLocation()
local s = _G.E33MoveStart or { X = 0, Y = 0, Z = 0 }
local v = pawn:GetVelocity()
return string.format("now %.0f %.0f %.0f moved %.0f vel %.0f %.0f", p.X, p.Y, p.Z, math.sqrt((p.X-s.X)^2+(p.Y-s.Y)^2), v.X, v.Y)
