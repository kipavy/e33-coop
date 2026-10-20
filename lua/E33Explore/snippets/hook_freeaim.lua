local C = "/Game/Gameplay/FreeAim/BP_FreeAimShooterBattleComponent.BP_FreeAimShooterBattleComponent_C:"
local B = "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:"
local res = {}
for _, p in ipairs({C .. "OnFreeAimProjectileHit", C .. "FireFreeAimProjectile", C .. "OnFreeAimShootAction", C .. "EndFreeAim",
                    C .. "EnableFreeAim", C .. "UpdateAimingLocation", B .. "StartFreeAimFromUI", B .. "CancelFreeAimFromUI", B .. "SelectAction"}) do
  local ok = pcall(function()
    RegisterHook(p, function(ctx, a, b)
      local s = p:match(":(.+)$")
      if s == "UpdateAimingLocation" then
        _G.E33_FA_UPD = (_G.E33_FA_UPD or 0) + 1
        if _G.E33_FA_UPD % 60 ~= 1 then return end
      end
      local function nm(x) local okk, v = pcall(function() return x:get():GetFName():ToString() end); return okk and v or "?" end
      local function num(x) local okk, v = pcall(function() return tostring(x:get()) end); return okk and v or "?" end
      E33Log("fa", s .. " a=" .. nm(a) .. "/" .. num(a) .. " b=" .. num(b))
    end)
  end)
  table.insert(res, p:match(":(.+)$") .. "=" .. tostring(ok))
end
return table.concat(res, " ")
