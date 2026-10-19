-- Libellés affichés (DisplayNameMap) d'énumérations BP.
local paths = _G.E33_ENUMS or {
  "/Game/jRPGTemplate/Enumerations/E_jRPG_ActionType.E_jRPG_ActionType",
  "/Game/Gameplay/Battle/EBattleEndResult.EBattleEndResult",
}
local out = {}
for _, p in ipairs(paths) do
  local e = StaticFindObject(p)
  if not e or not e:IsValid() then table.insert(out, p .. ": introuvable")
  else
    local names = {}
    local ok, err = pcall(function()
      e.DisplayNameMap:ForEach(function(k, v) table.insert(names, k:get():ToString() .. "=" .. v:get():ToString()) end)
    end)
    local vals = {}
    pcall(function() e.Names:ForEach(function(i, pair) local pr = pair:get(); table.insert(vals, pr.Key:ToString() .. ":" .. tostring(pr.Value)) end) end)
    table.insert(out, p:match("[^.]+$") .. " " .. (ok and table.concat(names, ", ") or ("err " .. tostring(err))) .. " | " .. table.concat(vals, ","))
  end
end
return table.concat(out, "\n")
