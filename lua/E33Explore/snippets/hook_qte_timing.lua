-- Diagnostic QTE (2026-10-20) : instant de chaque appui QTE et de chaque TryActivate, en temps réel du jeu,
-- par rapport aux bornes des fenêtres (CurrentWindowStart/EndRealTime). Même mesure avec et sans le mod coop.
local TL = "/Game/Gameplay/Battle/SkillCombos/BP_BattleSequenceTimeline.BP_BattleSequenceTimeline_C:"
local BM = "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:"
local GS = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local function rt()
  local r = -1
  pcall(function() r = GS:GetRealTimeSeconds(FindFirstOf("BP_jRPG_Controller_World_C")) end)
  return r
end
_G.QT_WINS = _G.QT_WINS or {}
local function windows(now)
  local s = {}
  for i = math.max(1, #_G.QT_WINS - 2), #_G.QT_WINS do
    local w = _G.QT_WINS[i]
    table.insert(s, string.format("w%d[%+.3f,%+.3f]", w.n, now - w.s, now - w.e))
  end
  return table.concat(s, " ")
end
if not _G.QT_HOOKED then
  _G.QT_HOOKED = true
  RegisterHook(TL .. "OnSkillComboWindowStartReached", function(ctx)
    local t = ctx:get()
    local w = { n = #_G.QT_WINS + 1, s = t.CurrentWindowStartRealTime, e = t.CurrentWindowEndRealTime }
    table.insert(_G.QT_WINS, w)
    E33Log("qte_timing", string.format("start w%d dur=%.3f event_late=%.3f", w.n, w.e - w.s, rt() - w.s))
  end)
  RegisterHook(TL .. "OnSkillComboWindowEndReached", function(ctx)
    local t = ctx:get()
    E33Log("qte_timing", string.format("end succ=%s perf=%s %s", tostring(t.IsSkillComboSuccessful), tostring(t.IsSkillComboPerfect), windows(rt())))
  end)
  RegisterHook(BM .. "OnComboPressed", function(ctx)
    E33Log("qte_timing", "pressed " .. windows(rt()))
  end)
  RegisterHook(TL .. "TryActivate", function(ctx)
    E33Log("qte_timing", "tryactivate " .. windows(rt()))
  end)
end
return "qte timing hooked"
