-- Phase 4 : instantané de l'état courant -> objet de save V8 -> SaveGameToSlot("e33coop/SNAP").
local gi = FindFirstOf("BP_jRPG_GI_Custom_C")
local sm = gi.SaveManager
local out = {}
local r = {}
sm:CreateSaveGameDataFromGameState(out)
local data = out.SaveGameData
r[#r+1] = "data=" .. tostring(data and data:IsValid() and data:GetFullName())
if not (data and data:IsValid()) then return table.concat(r, "\n") end
r[#r+1] = "map=" .. tostring(data.MapToLoad:ToString()) .. " spawn=" .. tostring(data.SpawnPointTagToLoadAt.TagName:ToString())
local lt = data.LastTransform
r[#r+1] = string.format("last=%.0f %.0f %.0f", lt.Translation.X, lt.Translation.Y, lt.Translation.Z)
r[#r+1] = "gold=" .. tostring(data.Gold)
local cls = StaticFindObject("/Game/Gameplay/Save/SaveObjects/BP_SaveGameObject_V8.BP_SaveGameObject_V8_C")
r[#r+1] = "cls=" .. tostring(cls and cls:IsValid())
local obj = StaticConstructObject(cls, gi)
obj:ConvertFromSaveGameData(data)
r[#r+1] = "obj=" .. obj:GetFullName() .. " ver=" .. tostring(obj.SaveVersion)
local gps = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local ok = gps:SaveGameToSlot(obj, "e33coop/SNAP", 0)
r[#r+1] = "saved=" .. tostring(ok)
_G.E33Snap = obj
local pawn = FindFirstOf("BP_jRPG_Character_World_C")
local p = pawn:K2_GetActorLocation()
r[#r+1] = string.format("pawn=%.0f %.0f %.0f", p.X, p.Y, p.Z)
return table.concat(r, "\n")
