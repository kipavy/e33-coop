-- Phase 5 : cartographie des menus de personnages, d'inventaire, d'amélioration et de marchand.
-- Journalise (explore.jsonl, "build_hook") chaque appel des fonctions candidates, avec l'objet appelé et les
-- paramètres simples (noms, entiers, objets).
local CD = "/Game/Gameplay/CharacterData/BP_CharacterData.BP_CharacterData_C:"
local INV = "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_InventoryManager.AC_jRPG_InventoryManager_C:"
local UPG = "/Game/Gameplay/Pictos/BP_ItemUpgradeSystem.BP_ItemUpgradeSystem_C:"
local MER = "/Game/Gameplay/Inventory/Merchant/BP_MerchantComponent.BP_MerchantComponent_C:"
local fns = {
  CD.."AssignPointToAttribute", CD.."ResetAssignedAttributePoints", CD.."LoadAssignedAttributePoints",
  CD.."TryUnlockSkill", CD.."EquipSkill", CD.."UnequipSkill", CD.."SetEquippedSkillOverrides",
  CD.."EquipItemInSlot", CD.."EquipItemInstanceInSlot", CD.."UnequipItemInstanceInSlot", CD.."ReplaceItemInstanceInSlot",
  CD.."UnequipItemFromInstance", CD.."SetManuallyEquippedPassiveEffects", CD.."UnequipLuminaPassiveList",
  CD.."LearnPassiveEffectShared", CD.."ProgressPassiveEffectShared", CD.."AddLuminaPointFromConsumable",
  CD.."ApplyCharacterCustomizationItem", CD.."SaveCharacterState", CD.."LoadCharacterState",
  CD.."DispatchModificationsIfPossible", CD.."FlushDeferredUpdate",
  INV.."EquipItem", INV.."UnequipItem", INV.."UseItemOnCharacter", INV.."BuyItem", INV.."TrySpendGold", INV.."ReceiveGold",
  INV.."RemoveItemFromInventory", INV.."AddItemToInventory", INV.."OnVendorItemSold",
  UPG.."UpgradeWeaponFromInstance", UPG.."UpgradeShardToNextLevel", UPG.."UpgradeWeaponFromHandle",
  MER.."OnItemBought", MER.."AddItemToInventory",
}
local function describe(v)
  local s = ""
  pcall(function()
    local x = v:get()
    if type(x) == "userdata" then
      local ok, n = pcall(function() return x:ToString() end)
      if ok and n then s = n return end
      local ok2, n2 = pcall(function() return x:GetFName():ToString() end)
      if ok2 and n2 then s = n2 return end
      s = tostring(x)
    else
      s = tostring(x)
    end
  end)
  return s
end
local res = {}
_G.E33BuildHooks = _G.E33BuildHooks or {}
for _, f in ipairs(fns) do
  if not _G.E33BuildHooks[f] then
    local ok, err = pcall(function()
      RegisterHook(f, function(ctx, ...)
        local who = ""
        pcall(function() who = ctx:get():GetFName():ToString() end)
        local args = {}
        for i, v in ipairs({...}) do args[#args+1] = describe(v) end
        E33Log("build_hook", f:match(":(.*)$") .. " on " .. who .. " (" .. table.concat(args, ", ") .. ")")
      end)
    end)
    if ok then _G.E33BuildHooks[f] = true end
    res[#res+1] = (ok and "ok   " or "FAIL ") .. f:match("([^%.]+:.*)$") .. (ok and "" or (" " .. tostring(err)))
  end
end
return table.concat(res, "\n")
