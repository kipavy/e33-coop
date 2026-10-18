-- E33Explore : mod d'exploration (phase 0), outil de dev uniquement.
-- * logs JSON par ligne dans RUN_DIR/explore.jsonl
-- * à l'apparition du menu principal : dumps UE4SS (objets, usmap, SDK)
-- * télécommande : RUN_DIR/cmd.lua est exécuté sur le game thread, résultat dans RUN_DIR/cmd.out

local RUN_DIR = os.getenv("E33_RUN_DIR") or "C:/Users/Killian/dev/e33-coop/runs/explore/"
local logf = io.open(RUN_DIR .. "explore.jsonl", "a")

local function esc(s)
    s = tostring(s)
    return (s:gsub('[%c"\\]', function(c)
        if c == '"' then return '\\"' elseif c == "\\" then return "\\\\"
        elseif c == "\n" then return "\\n" else return string.format("\\u%04x", c:byte()) end
    end))
end

local function log(ev, data)
    if not logf then return end
    logf:write(string.format('{"ts":%d,"clock":%.3f,"ev":"%s","data":"%s"}\n', os.time(), os.clock(), esc(ev), esc(data or "")))
    logf:flush()
end
_G.E33Log = log

log("start", "E33Explore loaded")

-- Dumps -----------------------------------------------------------------------
local dumped = {}
local function run_dumps(tag)
    if dumped[tag] then return end
    dumped[tag] = true
    for _, name in ipairs({ "DumpAllObjects", "DumpUSMAP", "GenerateSDK", "GenerateUHTCompatibleHeaders", "GenerateLuaTypes" }) do
        local fn = _G[name]
        if fn then
            log("dump_begin", tag .. ":" .. name)
            local ok, err = pcall(fn)
            log(ok and "dump_ok" or "dump_err", tag .. ":" .. name .. " " .. tostring(err))
        else
            log("dump_missing", name)
        end
    end
end
_G.E33RunDumps = run_dumps

-- Hooks de journalisation (réessayés jusqu'à ce que la classe soit chargée) -----
local BATTLE_CHAR = "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C:"
local BM = "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:"
local STATS = "/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_CharacterBattleStats.AC_jRPG_CharacterBattleStats_C:"
local CTRL = "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:"

local pending = {
    BATTLE_CHAR .. "OnTurnStart", BATTLE_CHAR .. "OnTurnEnded",
    BATTLE_CHAR .. "TryStartParry", BATTLE_CHAR .. "TryStartDodge", BATTLE_CHAR .. "TryStartJump",
    BATTLE_CHAR .. "OnParrySuccess", BATTLE_CHAR .. "OnDodgeSuccess",
    BM .. "StartCharacterTurn", BM .. "SelectAction", BM .. "SelectSecondaryAction", BM .. "RequestActionExecution",
    BM .. "CheckBattleEnd", BM .. "StartBattleEndFlow",
    STATS .. "ReceiveDamage", STATS .. "TryParryHitFromCharacter",
    CTRL .. "StartBattleNEW", CTRL .. "ResumeExplorationOnBattleEnd", CTRL .. "PrepareToChangeMap",
}
local hooked = {}

local function obj_name(o)
    local ok, n = pcall(function() return o:GetFullName() end)
    return ok and n or "?"
end

local function try_hooks()
    for _, path in ipairs(pending) do
        if not hooked[path] then
            local ok, err = pcall(function()
                RegisterHook(path, function(ctx, ...)
                    log("hook", path:match(":(.+)$") .. " self=" .. obj_name(ctx:get()))
                end)
            end)
            if ok then
                hooked[path] = true
                log("hook_registered", path)
            end
        end
    end
end

-- Menu principal ------------------------------------------------------------------
pcall(function()
    NotifyOnNewObject("/Game/UI/Widgets/MainMenu/WBP_MM_MainMenu.WBP_MM_MainMenu_C", function(w)
        log("main_menu_created", obj_name(w))
        ExecuteWithDelay(8000, function() ExecuteInGameThread(function() run_dumps("mainmenu") end) end)
    end)
end)

-- Télécommande --------------------------------------------------------------------
local CMD, OUT = RUN_DIR .. "cmd.lua", RUN_DIR .. "cmd.out"
local busy = false
LoopAsync(500, function()
    if busy then return false end
    local f = io.open(CMD, "r")
    if f then
        local src = f:read("a"); f:close(); os.remove(CMD)
        busy = true
        ExecuteInGameThread(function()
            local chunk, perr = load(src, "cmd", "t", _G)
            local res
            if not chunk then res = "PARSE ERROR: " .. tostring(perr)
            else
                local ok, r = pcall(chunk)
                res = (ok and "OK: " or "ERROR: ") .. tostring(r)
            end
            local o = io.open(OUT, "w"); if o then o:write(res); o:close() end
            log("cmd", res:sub(1, 500))
            busy = false
        end)
    end
    return false
end)

LoopAsync(3000, function()
    ExecuteInGameThread(try_hooks)
    return false
end)
