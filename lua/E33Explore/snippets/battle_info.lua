-- État du combat : héros (spot, classe, PV, PA, compétences équipées), ennemis, objets.
local pc = FindFirstOf("BP_jRPG_Controller_World_C")
local bm = pc.AC_jRPG_BattleManager
local out = {}
local cur = bm.CurrentCharacter
table.insert(out, "current=" .. (cur:IsValid() and cur:GetFName():ToString() or "nil") .. " sel=" .. tostring(bm.SelectedAction))
bm.PlayerCharacters:ForEach(function(i, e)
  local c = e:get()
  local st = c.AC_jRPG_CharacterStats
  local line = string.format("H spot=%d %s hp=%.0f ap=%.0f name=%s", c.BattleSpotIndex, c:GetClass():GetFName():ToString(), st.CurrentHP, st.CurrentAP, st.CharacterHardcodedName:ToString())
  local skills = {}
  st.SkillStates:ForEach(function(k, v)
    local sk = k:get()
    local ss = v:get()
    table.insert(skills, sk.NameID:ToString() .. "(" .. tostring(sk.APCost) .. ",t" .. tostring(sk.TargetingType) .. ")")
  end)
  table.insert(out, line .. " skills=" .. table.concat(skills, " "))
end)
bm.Enemies:ForEach(function(i, e)
  local c = e:get()
  local st = c.AC_jRPG_CharacterStats
  table.insert(out, string.format("E spot=%d %s hp=%.0f", c.BattleSpotIndex, c:GetClass():GetFName():ToString(), st.CurrentHP))
end)
local items = {}
bm.BattleItemsCount:ForEach(function(k, v) table.insert(items, k:get():ToString() .. "=" .. tostring(v:get())) end)
table.insert(out, "items " .. table.concat(items, " "))
return table.concat(out, "\n")
