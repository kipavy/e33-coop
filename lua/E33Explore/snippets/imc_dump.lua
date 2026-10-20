-- Phase 5 : actions et touches des contextes d'entrée retirés chez un suiveur (vérifier qu'on ne coupe pas les menus).
local out = {}
for _, path in ipairs({
  "/Game/Gameplay/Inputs/InputMappingProfiles/IMC_UI_Exploration.IMC_UI_Exploration",
  "/Game/Gameplay/Inputs/InputMappingProfiles/IMC_UI_Dialogue.IMC_UI_Dialogue",
  "/Game/Gameplay/Inputs/InputMappingProfiles/IMC_UI_Menu.IMC_UI_Menu",
}) do
  local imc = StaticFindObject(path)
  out[#out+1] = "== " .. path:match("%.(.*)$") .. " valid=" .. tostring(imc and imc:IsValid())
  if imc and imc:IsValid() then
    local ok, err = pcall(function()
      local maps = imc.DefaultKeyMappings.Mappings
      for i = 1, #maps do
        local m = maps[i]
        local a = m.Action and m.Action:IsValid() and m.Action:GetFName():ToString() or "?"
        local k = ""
        pcall(function() k = m.Key.KeyName:ToString() end)
        out[#out+1] = "  " .. a .. " <- " .. k
      end
    end)
    if not ok then
      local ok2, err2 = pcall(function()
        local maps = imc.Mappings
        for i = 1, #maps do
          local m = maps[i]
          local a = m.Action and m.Action:IsValid() and m.Action:GetFName():ToString() or "?"
          local k = ""
          pcall(function() k = m.Key.KeyName:ToString() end)
          out[#out+1] = "  " .. a .. " <- " .. k
        end
      end)
      if not ok2 then out[#out+1] = "  err " .. tostring(err) .. " / " .. tostring(err2) end
    end
  end
end
return table.concat(out, "\n")
