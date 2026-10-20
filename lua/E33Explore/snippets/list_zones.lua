-- Phase 4 : zones de déclenchement du niveau (action au passage / à l'interaction) et leur distance au pion.
local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local p = pawn:K2_GetActorLocation()
local r = {}
local function act(w)
  local out = {}
  for _, k in ipairs({"GameActionReference", "GameActionInline"}) do
    local ok, a = pcall(function() return w[k] end)
    if ok and a and a:IsValid() then out[#out+1] = a:GetClass():GetFName():ToString() .. ":" .. a:GetFName():ToString() end
  end
  return table.concat(out, ",")
end
for _, z in ipairs(FindAllOf("BP_GameActionTriggerZone_C") or {}) do
  if z:IsValid() then
    local l = z:K2_GetActorLocation()
    local d = math.sqrt((l.X-p.X)^2 + (l.Y-p.Y)^2 + (l.Z-p.Z)^2)
    r[#r+1] = string.format("%6.0f %s at %.0f %.0f %.0f overlap=[%s] interact=[%s]", d, z:GetFName():ToString(), l.X, l.Y, l.Z, act(z.GameActionOnOverlap), act(z.GameActionOnInteract))
  end
end
table.sort(r)
return #r .. " zones\n" .. table.concat(r, "\n")
