-- Personnages de la collection et équipe courante.
local gi = FindFirstOf("BP_jRPG_GI_Custom_C")
local out = {}
gi.CharactersCollection:ForEach(function(k, v) table.insert(out, k:get():ToString()) end)
local party = {}
gi.CurrentParty:ForEach(function(i, s) local slot = s:get(); table.insert(party, slot.CharacterHardcodedName_2_2A63D4C5433428900D69748563F50580:ToString()) end)
return "collection=" .. table.concat(out, ",") .. " party=" .. table.concat(party, ",")
