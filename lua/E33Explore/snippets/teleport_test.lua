-- Phase 4 : téléportation du pion (+300 en Y).
local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local p = pawn:K2_GetActorLocation()
local hit = {}
local ok = pawn:K2_SetActorLocation({ X = p.X, Y = p.Y + 300, Z = p.Z }, false, hit, true)
local q = pawn:K2_GetActorLocation()
return string.format("ok=%s from %.0f %.0f %.0f to %.0f %.0f %.0f", tostring(ok), p.X, p.Y, p.Z, q.X, q.Y, q.Z)
