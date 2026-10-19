-- Journalise les fenêtres de QTE offensifs (timeline de séquence de combat).
local TL = "/Game/Gameplay/Battle/SkillCombos/BP_BattleSequenceTimeline.BP_BattleSequenceTimeline_C:"
local CH = "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C:"
local names = { TL .. "OnSkillComboWindowPreReached", TL .. "OnSkillComboWindowStartReached", TL .. "OnSkillComboWindowEndReached",
  TL .. "TryActivate", TL .. "OnSkillEffectReached", TL .. "StartAllowSkillCombo", CH .. "OnSkillComboFailed" }
local ok = {}
for _, p in ipairs(names) do
  local r, err = pcall(function()
    RegisterHook(p, function(ctx)
      local t = ctx:get()
      local info = ""
      pcall(function() info = string.format(" succ=%s perf=%s inwin=%s track=%d auto=%.3f", tostring(t.IsSkillComboSuccessful), tostring(t.IsSkillComboPerfect), tostring(t.IsInSkillComboWindow), t.TrackIndex, t.TriggerAutoQTEAtTime) end)
      E33Log("qte", p:match(":(.+)$") .. info)
    end)
  end)
  table.insert(ok, p:match(":(.+)$") .. "=" .. tostring(r) .. (r and "" or tostring(err)))
end
return table.concat(ok, " ")
