-- Charge la save de test TEST_B (autosave coupée).
local gi = FindFirstOf("BP_jRPG_GI_Custom_C")
local sm = gi.SaveManager
sm.EnableAutoSave = false
local gps = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local sg = gps:LoadGameFromSlot("e33coop-test/TEST_B", 0)
assert(sg and sg:IsValid(), "save introuvable")
sm:LoadGameFromSaveFile(sg, "e33coop-test/TEST_B")
return "loading " .. sg:GetFName():ToString()
