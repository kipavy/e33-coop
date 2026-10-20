-- Phase 5 : essai d'affichage d'un texte du mod avec les notifications « toast » du jeu.
local out = {}
local fl = StaticFindObject("/Game/UI/Widgets/CommonElements/ToastNotification/FL_ToastNotificationHelpers.Default__FL_ToastNotificationHelpers_C")
if not fl or not fl:IsValid() then
  -- chemin inconnu : on cherche le CDO par nom de classe
  local all = FindAllOf("FL_ToastNotificationHelpers_C")
  out[#out+1] = "CDO direct introuvable ; instances : " .. tostring(all and #all or 0)
end
local hud = FindFirstOf("WBP_Exploration_HUD_C")
out[#out+1] = "hud=" .. tostring(hud and hud:IsValid() and hud:GetFullName() or "nil")
local pc = FindFirstOf("BP_jRPG_Controller_World_C")
local ok, err = pcall(function()
  local cdo = fl
  if not cdo or not cdo:IsValid() then
    cdo = StaticFindObject("/Script/Engine.Default__Object")
  end
  cdo:ShowToastNotificationOnWidget(FText("Tour de Ami (test coop)"), 0, hud, pc)
end)
out[#out+1] = "call ok=" .. tostring(ok) .. " err=" .. tostring(err)
return table.concat(out, "\n")
