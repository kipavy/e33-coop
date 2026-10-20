local bm = FindFirstOf("AC_jRPG_BattleManager_C")
local r = {}
r[#r+1] = "EncounterName=" .. bm.EncounterName:ToString()
local bp = bm.CurrentBattleStartParams
for _, k in ipairs({"EncounterRow", "EnemyGlobalID", "EngagementType", "BattleMapBP", "OverrideParty"}) do
  local ok, v = pcall(function()
    for name, val in pairs({}) do end
    return bp[k]
  end)
  r[#r+1] = k .. "=" .. tostring(ok and v)
end
return table.concat(r, "\n")
