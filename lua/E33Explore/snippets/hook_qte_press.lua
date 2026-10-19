-- Appuie sur QTE à une fraction de chaque fenêtre (liste _G.E33_PRESS : fraction ou -1 = pas d'appui).
local TL = "/Game/Gameplay/Battle/SkillCombos/BP_BattleSequenceTimeline.BP_BattleSequenceTimeline_C:"
_G.E33_PRESS = _G.E33_PRESS or {0.3, 0.9, -1, 0.5, 0.9}
_G.E33_WIN = 0
if not _G.E33_PRESS_HOOKED then
  _G.E33_PRESS_HOOKED = true
  RegisterHook(TL .. "OnSkillComboWindowStartReached", function(ctx)
    local t = ctx:get()
    _G.E33_WIN = _G.E33_WIN + 1
    local k = _G.E33_WIN
    local dur = t.CurrentWindowEndRealTime - t.CurrentWindowStartRealTime
    local f = _G.E33_PRESS[((k - 1) % #_G.E33_PRESS) + 1]
    E33Log("press_plan", string.format("win=%d dur=%.3f frac=%.2f start=%.3f", k, dur, f, t.CurrentWindowStartRealTime))
    if f >= 0 then
      ExecuteWithDelay(math.floor(dur * f * 1000), function()
        ExecuteInGameThread(function()
          local pc = FindFirstOf("BP_jRPG_Controller_World_C")
          pc.AC_jRPG_BattleManager:OnComboPressed()
          E33Log("press_done", "win=" .. k)
        end)
      end)
    end
  end)
end
return "ok"
