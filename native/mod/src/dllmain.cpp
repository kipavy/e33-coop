// E33Coop — mod C++ UE4SS (phase 0 : hook minimal qui journalise les événements de combat).
//
// Les fonctions du jeu visées sont des fonctions Blueprint (script) : on passe donc par
// UObjectGlobals::RegisterHook (hook global ProcessLocalScriptFunction/ProcessInternal),
// pas par UFunction::RegisterPreHook qui ne voit pas les appels BP→BP.
// Les classes BP ne sont pas forcément chargées à on_unreal_init : on réessaie dans on_update.

#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks.hpp>

#include "../../common/jsonlog.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

using namespace RC;
using namespace RC::Unreal;

namespace e33coop {

e33::JsonLog g_log;

std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::filesystem::path module_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &self);
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(buf).parent_path();
}

// Dossier des journaux : %E33COOP_RUN_DIR% sinon <mod>/logs
std::filesystem::path log_dir()
{
    wchar_t buf[1024]{};
    if (GetEnvironmentVariableW(L"E33COOP_RUN_DIR", buf, static_cast<DWORD>(std::size(buf))) > 0) return buf;
    return module_dir().parent_path() / L"logs";
}

struct HookSpec {
    const TCHAR* path;
    const char* event;
    UFunction* fn = nullptr;
    std::pair<int, int> ids{-1, -1};
    bool registered = false;
};

#define BATTLE_CHAR STR("/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_Battle_Base.BP_jRPG_Character_Battle_Base_C:")
#define BATTLE_MGR STR("/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_BattleManager.AC_jRPG_BattleManager_C:")
#define BATTLE_STATS STR("/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_CharacterBattleStats.AC_jRPG_CharacterBattleStats_C:")

std::array<HookSpec, 6> g_hooks{{
    {BATTLE_CHAR STR("OnTurnStart"), "turn_start"},
    {BATTLE_CHAR STR("OnTurnEnded"), "turn_end"},
    {BATTLE_MGR STR("StartCharacterTurn"), "bm_start_character_turn"},
    {BATTLE_MGR STR("RequestActionExecution"), "bm_request_action"},
    {BATTLE_STATS STR("TryParryHitFromCharacter"), "parry_check"},
    {BATTLE_STATS STR("ReceiveDamage"), "receive_damage"},
}};

void noop_hook(UnrealScriptFunctionCallableContext&, void*) {}

void on_hook_pre(UnrealScriptFunctionCallableContext& ctx, void* custom)
{
    try {
        const auto* spec = static_cast<const HookSpec*>(custom);
        UObject* self = ctx.Context;
        g_log.write(spec->event, e33::JsonObj().add("self", self ? to_utf8(self->GetFullName()) : std::string("null")));
    } catch (...) {
        // Un hook ne doit jamais faire tomber le jeu.
    }
}

bool try_register_hooks()
{
    bool all = true;
    for (auto& h : g_hooks) {
        if (h.registered) continue;
        h.fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, h.path);
        if (!h.fn) { all = false; continue; }
        try {
            h.ids = UObjectGlobals::RegisterHook(h.fn, &on_hook_pre, &noop_hook, &h);
            h.registered = true;
            g_log.write("hook_registered", e33::JsonObj().add("event", h.event).add("path", to_utf8(h.path)));
        } catch (const std::exception& e) {
            g_log.write("hook_error", e33::JsonObj().add("event", h.event).add("what", e.what()));
            h.fn = nullptr;
            all = false;
        }
    }
    return all;
}

void unregister_hooks()
{
    for (auto& h : g_hooks) {
        if (h.registered && h.fn) {
            try { UObjectGlobals::UnregisterHook(h.fn, h.ids); } catch (...) {}
        }
        h.registered = false;
    }
}

} // namespace e33coop

class E33CoopMod : public RC::CppUserModBase {
public:
    E33CoopMod() : CppUserModBase()
    {
        ModName = STR("E33Coop");
        ModVersion = STR("0.0.1");
        ModDescription = STR("Coop en ligne pour Clair Obscur: Expedition 33");
        ModAuthors = STR("kipavy");
    }

    ~E33CoopMod() override { e33coop::unregister_hooks(); e33coop::g_log.close(); }

    auto on_unreal_init() -> void override
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = e33coop::log_dir();
        fs::create_directories(dir, ec);
        const fs::path file = dir / L"e33coop-solo.jsonl";
        e33coop::g_log.open(e33coop::to_utf8(file.wstring()), "solo", "solo");
        e33coop::g_log.write("mod_init", e33::JsonObj().add("version", "0.0.1").add("log", e33coop::to_utf8(file.wstring())));
        Output::send<LogLevel::Default>(STR("[E33Coop] init, log: {}\n"), file.wstring());
        hooks_done_ = e33coop::try_register_hooks();
    }

    auto on_update() -> void override
    {
        if (hooks_done_ || ++tick_ < 60) return;
        tick_ = 0;
        try { hooks_done_ = e33coop::try_register_hooks(); } catch (...) {}
    }

private:
    bool hooks_done_ = false;
    int tick_ = 0;
};

#define E33COOP_API __declspec(dllexport)
extern "C" {
E33COOP_API RC::CppUserModBase* start_mod() { return new E33CoopMod(); }
E33COOP_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
