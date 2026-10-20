-- État de la visée libre du personnage courant (+ démarrage si _G.E33_FA_START).
local bm = FindFirstOf("BP_jRPG_Controller_World_C").AC_jRPG_BattleManager
local cur = bm.CurrentCharacter
local c = cur.BP_FreeAimShooterBattleComponent
if _G.E33_FA_START then _G.E33_FA_START = nil; bm:StartFreeAimFromUI() end
local r = c.AimingRotation
local st = cur.AC_jRPG_CharacterStats
return string.format("%s aiming=%s shots=%d fired=%d time=%.2f type=%d rot=(%.2f,%.2f,%.2f) ap=%.0f", cur:GetFName():ToString(),
  tostring(c.IsAiming), c.ShotsLeft, c.ShotsFired, c.TimeLeft, c.CurrentFreeAimType, r.Pitch, r.Yaw, r.Roll, st.CurrentAP)
