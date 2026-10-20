-- Phase 5 : crée un WBP_ToastCard dans le viewport (texte du mod), sans passer par la fonction d'aide du jeu.
local out = {}
local pc = FindFirstOf("BP_jRPG_Controller_World_C")
local cls = StaticFindObject("/Game/UI/Widgets/CommonElements/ToastNotification/WBP_ToastCard.WBP_ToastCard_C")
local wbl = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
out[#out+1] = "cls=" .. tostring(cls and cls:IsValid()) .. " wbl=" .. tostring(wbl and wbl:IsValid())
local ok, err = pcall(function()
  local w = wbl:Create(pc, cls, pc)
  out[#out+1] = "widget=" .. tostring(w and w:IsValid() and w:GetFullName())
  w:Init(FText("Tour de Ami (essai coop)"), 1)
  w.Duration = 8.0
  w:AddToViewport(500)
  _G.E33Toast = w
end)
out[#out+1] = "ok=" .. tostring(ok) .. " err=" .. tostring(err)
return table.concat(out, "\n")
