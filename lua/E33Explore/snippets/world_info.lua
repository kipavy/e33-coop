-- Phase 4 : pion, niveau, ennemis du monde proches (rencontre, id).
local r = {}
local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local p = pawn:K2_GetActorLocation()
local rot = pawn:K2_GetActorRotation()
r[#r+1] = string.format("pawn %s at %.0f %.0f %.0f yaw %.1f", pawn:GetFullName(), p.X, p.Y, p.Z, rot.Yaw)
local mv = pawn.CharacterMovement
r[#r+1] = "movement=" .. tostring(mv and mv:GetFullName()) .. " mode=" .. tostring(mv and mv.MovementMode)
local w = pawn:GetWorld()
r[#r+1] = "world=" .. w:GetFullName()
local list = FindAllOf("BP_jRPG_Enemy_World_Base_Seamless_C") or {}
local n = 0
for _, e in ipairs(list) do
  if e:IsValid() then
    local l = e:K2_GetActorLocation()
    local d = math.sqrt((l.X-p.X)^2 + (l.Y-p.Y)^2 + (l.Z-p.Z)^2)
    if d < 8000 then
      n = n + 1
      r[#r+1] = string.format("enemy %s d=%.0f enc=%s", e:GetFName():ToString(), d, e.SelectedEncounter.RowName:ToString())
    end
  end
end
r[#r+1] = "enemies total=" .. #list .. " near=" .. n
return table.concat(r, "\n")
