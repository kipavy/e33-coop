-- Oriente chaque projectile de visée libre vers l'ennemi (test : un projectile replacé touche-t-il ?).
local C = "/Game/Gameplay/FreeAim/BP_FreeAimShooterBattleComponent.BP_FreeAimShooterBattleComponent_C:"
if _G.E33_REDIR then return "deja" end
_G.E33_REDIR = true
RegisterHook(C .. "PrepareNextFreeAimProjectile", function(ctx, proj)
  local p = proj:get()
  local bm = FindFirstOf("BP_jRPG_Controller_World_C").AC_jRPG_BattleManager
  local en
  bm.Enemies:ForEach(function(i, e) if not en then en = e:get() end end)
  local t = en:K2_GetActorLocation()
  t.Z = t.Z + (_G.E33_FA_Z or 100)
  local s = p:K2_GetActorLocation()
  local kml = StaticFindObject("/Script/Engine.Default__KismetMathLibrary")
  local rot = kml:FindLookAtRotation(s, t)
  p:K2_SetActorRotation(rot, false)
  local r2 = p:K2_GetActorRotation()
  E33Log("fa", string.format("redirect from (%.0f,%.0f,%.0f) rot=(%.2f,%.2f)", s.X, s.Y, s.Z, r2.Pitch, r2.Yaw))
end)
return "ok"
