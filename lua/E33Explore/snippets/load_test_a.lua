local gi = FindFirstOf("BP_jRPG_GI_Custom_C")
local sm = gi.SaveManager
sm.EnableAutoSave = false
local gps = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local sg = gps:LoadGameFromSlot("e33coop-test/TEST_A", 0)
sm:LoadGameFromSaveFile(sg, "e33coop-test/TEST_A")
return "loading TEST_A"
