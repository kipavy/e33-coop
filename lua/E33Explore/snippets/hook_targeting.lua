local FL = "/Game/Gameplay/Battle/Targeting/FL_TargetingHelpers.FL_TargetingHelpers_C:"
local EN = "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_Battle_Base.BP_jRPG_Enemy_Battle_Base_C:"
local BS = "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_CharacterBattleStats.AC_jRPG_CharacterBattleStats_C:"
local list = { FL .. "GetRandomAliveHero", FL .. "GetSpecificHero", FL .. "GetMiddleHero", FL .. "GetRandomCharacterByBuffCritera",
  FL .. "GetAliveHero", EN .. "AcquireTargetRandomHero", EN .. "GetRandomEnemy", EN .. "AcquireSpecificTarget", EN .. "AcquireTargetMiddleHero",
  EN .. "AcquireTargetAllHeroes", BS .. "ReceiveDamageFromObject", BS .. "TryDodgeHitFromCharacter", BS .. "ActivateBuff" }
local res = {}
for _, p in ipairs(list) do
  local ok, err = pcall(function()
    RegisterHook(p, function(ctx, a, b, c, d, e)
      local s = p:match(":(.+)$")
      local function nm(x) local okk, v = pcall(function() local o = x:get(); return o:GetFName():ToString() end); return okk and v or "?" end
      E33Log("tgt", s .. " self=" .. nm(ctx) .. " a=" .. nm(a) .. " b=" .. nm(b) .. " c=" .. nm(c) .. " d=" .. nm(d))
    end)
  end)
  table.insert(res, (p:match(":(.+)$")) .. "=" .. tostring(ok))
end
return table.concat(res, " ")
