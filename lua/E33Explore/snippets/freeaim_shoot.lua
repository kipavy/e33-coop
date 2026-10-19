-- Vise un ennemi (point _G.E33_FA_Z au-dessus de son origine) puis tire.
local bm = FindFirstOf("BP_jRPG_Controller_World_C").AC_jRPG_BattleManager
local cur = bm.CurrentCharacter
local c = cur.BP_FreeAimShooterBattleComponent
local en
bm.Enemies:ForEach(function(i, e) if not en then en = e:get() end end)
local l = en:K2_GetActorLocation()
local target = {X = l.X, Y = l.Y, Z = l.Z + (_G.E33_FA_Z or 100)}
if not _G.E33_FA_NOAIM then c:UpdateAimingLocation(target) end
local r = c.AimingRotation
c:OnFreeAimShootAction()
return string.format("shot at (%.0f,%.0f,%.0f) rot=(%.2f,%.2f) shots=%d fired=%d", target.X, target.Y, target.Z, r.Pitch, r.Yaw, c.ShotsLeft, c.ShotsFired)
