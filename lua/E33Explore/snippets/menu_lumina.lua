-- Phase 5 : action de menu « Lumina » pilotée par le widget (sans souris) : charge Verso dans le panneau des Luminas,
-- puis bascule son premier Lumina équipé deux fois (retiré puis remis). Les hooks de hook_builds.lua journalisent
-- les fonctions de données appelées.
local out = {}
local gi = FindFirstOf("BP_jRPG_GI_Custom_C")
local verso
gi.CharactersCollection:ForEach(function(k, v)
  if k:get():ToString() == "Verso" then verso = v:get() end
end)
out[#out+1] = "verso=" .. tostring(verso and verso:IsValid())
local panel
for _, p in ipairs(FindAllOf("WBP_GM_Luminas_Panel_C") or {}) do
  if p:IsValid() and not p:GetFullName():find("Default__") then panel = p end
end
out[#out+1] = "panel=" .. tostring(panel and panel:GetFullName())
local eq = verso.EquippedPassiveEffects
out[#out+1] = "equipped=" .. tostring(#eq)
local first = eq[1]:ToString()
out[#out+1] = "first=" .. first
local ok, err = pcall(function()
  panel:LoadCharacter(verso)
  E33Log("build_probe", "toggle 1 " .. first)
  panel:ToggleLumina(FName(first))
  out[#out+1] = "after toggle1 equipped=" .. tostring(#verso.EquippedPassiveEffects)
  E33Log("build_probe", "toggle 2 " .. first)
  panel:ToggleLumina(FName(first))
  out[#out+1] = "after toggle2 equipped=" .. tostring(#verso.EquippedPassiveEffects)
end)
out[#out+1] = "ok=" .. tostring(ok) .. " " .. tostring(err)
return table.concat(out, "\n")
