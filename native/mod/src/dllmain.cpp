// E33Coop — mod C++ UE4SS : point d'entrée.
//  - journal JSON par instance (<run_dir>/e33coop-<instance>.jsonl)
//  - hooks de journalisation du combat (HookBus)
//  - test runner si -e33test=<scenario.json>
// Le travail sur les objets du jeu se fait dans le callback EngineTick (game thread).

#include <chrono>
#include <filesystem>
#include <string>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "../../common/jsonlog.hpp"
#include "../../hooks/game.hpp"
#include "../../hooks/hookbus.hpp"
#include "../../hooks/log.hpp"
#include "../../hooks/ue.hpp"
#include "../../remote/remote.hpp"
#include "../../sync/coop.hpp"
#include "../../testrunner/runner.hpp"
#include "../../sync/update.hpp"
#include "../../ui/options.hpp"
#include "../../ui/owners_ui.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace {

constexpr const char* kModVersion = E33COOP_VERSION;   // fichier VERSION
// STR(x) colle son préfixe avant l'expansion de x : un niveau de plus.
#define E33_STR2(x) STR(x)
#define E33_STR(x) E33_STR2(x)

void subscribe_logging_hooks()
{
    using e33::game::path::fn;
    namespace p = e33::game::path;
    auto& bus = e33::HookBus::get();
    struct Spec { const wchar_t* cls; const wchar_t* name; const char* ev; };
    static const Spec specs[] = {
        {p::BattleChar, L"OnTurnStart", "turn_start"},
        {p::BattleChar, L"OnTurnEnded", "turn_end"},
        {p::BattleMgr, L"StartCharacterTurn", "bm_start_character_turn"},
        {p::BattleMgr, L"RequestActionExecution", "bm_request_action"},
        {p::Stats, L"TryParryHitFromCharacter", "parry_check"},
        {p::Stats, L"ReceiveDamage", "receive_damage"},
        {p::BattleMgr, L"StartBattleEndFlow", "battle_end_flow"},
    };
    for (const auto& s : specs) {
        const char* ev = s.ev;
        bus.pre(fn(s.cls, s.name), [ev](e33::HookCtx& ctx, UFunction*) {
            e33::log().write(ev, e33::JsonObj().add("self", e33::ue::full_name(ctx.Context)));
        });
    }
}

} // namespace

class E33CoopMod : public RC::CppUserModBase {
public:
    E33CoopMod() : CppUserModBase()
    {
        ModName = STR("E33Coop");
        ModVersion = E33_STR(E33COOP_VERSION);
        ModDescription = STR("Coop en ligne pour Clair Obscur: Expedition 33");
        ModAuthors = STR("kipavy");
    }

    ~E33CoopMod() override
    {
        e33::HookBus::get().unregister_all();
        e33::log().write("mod_unload", e33::JsonObj());   // diagnostic : sortie propre (absent : processus tué ou planté)
        e33::log().close();
    }

    auto on_unreal_init() -> void override
    {
        namespace fs = std::filesystem;
        const auto& a = e33::launch_args();
        std::error_code ec;
        fs::create_directories(a.run_dir, ec);
        const fs::path file = fs::path(a.run_dir) / (L"e33coop-" + a.instance + L".jsonl");
        e33::log().open(e33::ue::to_utf8(file.wstring()), e33::ue::to_utf8(a.instance), e33::ue::to_utf8(a.role));
        e33::log().write("mod_init", e33::JsonObj()
                                         .add("version", kModVersion)
                                         .add("log", e33::ue::to_utf8(file.wstring()))
                                         .add("test", e33::ue::to_utf8(a.test)));
        Output::send<LogLevel::Default>(STR("[E33Coop] init, log: {}\n"), file.wstring());

        subscribe_logging_hooks();
        e33::coop::init();          // avant le test runner : ses hooks de début de tour fixent les propriétaires
        e33::testrunner::init();
        init_at_ = std::chrono::steady_clock::now();   // rien d'autre avant kStartupQuiet : voir on_tick

        Hook::RegisterEngineTickPreCallback(
            [this](auto&, UEngine*, float dt, bool) { on_tick(dt); },
            {false, false, STR("E33Coop"), STR("Tick")});
    }

private:
    // Gel du jeu au démarrage (écran d'avertissement) : ~1 lancement sur 8 avec le mod, 0 sur 9 sans (2026-10-19),
    // pendant que le mod cherche ses fonctions (StaticFindObject) et travaille dans les toutes premières secondes.
    // On attend que le moteur ait fini de démarrer (menu principal vers 20 s) avant toute activité.
    static constexpr std::chrono::seconds kStartupQuiet{10};
    std::chrono::steady_clock::time_point init_at_{};
    bool awake_ = false;
    double hook_retry_ = 0;
    bool hooks_done_ = false;

    void on_tick(float dt)
    {
        try {
            if (!awake_) {
                const auto quiet = std::chrono::steady_clock::now() - init_at_;
                if (quiet < kStartupQuiet) return;
                awake_ = true;
                e33::log().write("mod_awake", e33::JsonObj().add("after_ms",
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(quiet).count())));
                hooks_done_ = e33::HookBus::get().try_register();
                return;
            }
            if (!hooks_done_ || e33::HookBus::get().pending()) {   // abonnements ajoutés en jeu (rôle décidé, D18)
                hook_retry_ += dt;
                if (hook_retry_ > 1.0) { hook_retry_ = 0; hooks_done_ = e33::HookBus::get().try_register(); }
            }
            e33::coop::tick(dt);
            e33::update::tick(dt);
            e33::options::tick(dt);
            e33::owners_ui::tick(dt);
            e33::testrunner::tick(dt);
            e33::remote::tick(dt);
        } catch (const std::exception& ex) {
            e33::log().write("tick_exception", e33::JsonObj().add("what", ex.what()));
        } catch (...) {
            e33::log().write("tick_exception", e33::JsonObj());
        }
    }
};

#define E33COOP_API __declspec(dllexport)
extern "C" {
E33COOP_API RC::CppUserModBase* start_mod() { return new E33CoopMod(); }
E33COOP_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
