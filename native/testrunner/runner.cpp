#include "runner.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../common/minijson.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../sync/coop.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

using RC::Unreal::UObject;

namespace e33::testrunner {

namespace {

enum class Phase { Off, WaitMenu, LoadSave, WaitWorld, Trigger, WaitBattle, Battle, Ending, Done };

const char* phase_name(Phase p)
{
    switch (p) {
    case Phase::Off: return "off";
    case Phase::WaitMenu: return "wait_menu";
    case Phase::LoadSave: return "load_save";
    case Phase::WaitWorld: return "wait_world";
    case Phase::Trigger: return "trigger";
    case Phase::WaitBattle: return "wait_battle";
    case Phase::Battle: return "battle";
    case Phase::Ending: return "ending";
    case Phase::Done: return "done";
    }
    return "?";
}

struct Action { int type = 1; int target = 0; };

struct Scenario {
    std::string name;
    std::wstring save;
    std::wstring encounter;
    double timeout_s = 600;
    double action_delay_s = 1.5;
    int combo_auto_success = 10;
    int max_turns = 60;
    int wait_players = 0;       // hôte : nombre de clients attendus avant de lancer le combat
    std::vector<Action> actions;
    std::vector<std::string> defense;     // "parry" | "fail", cyclique par coup reçu
    json::Value expect;
};

struct State {
    Scenario sc;
    Phase phase = Phase::Off;
    double t = 0;             // secondes depuis init
    double phase_t = 0;       // secondes dans la phase courante
    double next_poll = 0;
    int turn = 0;
    int player_turns = 0;
    int action_index = 0;
    int hit_index = 0;        // coups reçus par des persos joueurs
    int parry_cmd = 0, parry_ok = 0, parry_forced = 0, fail_cmd = 0, fail_ok = 0;
    int jump_cmd = 0, jump_ok = 0;
    std::map<UObject*, std::string> jump_decision;   // ReceiveDamageFromObject sautable en cours, par composant de stats
    int damage_events = 0;
    double pending_action_at = -1;
    UObject* pending_actor = nullptr;
    std::map<UObject*, std::string> decision;   // dernière décision par défenseur (stats component)
    int end_result = -1;
    double end_at = -1;
    std::string fail_reason;
    std::vector<std::string> errors;
};

State g;

void set_phase(Phase p)
{
    log().write("tr_phase", JsonObj().add("from", phase_name(g.phase)).add("to", phase_name(p)).add("t", g.t));
    g.phase = p;
    g.phase_t = 0;
}

std::string read_file(const std::wstring& path)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool load_scenario(const std::wstring& path, std::string& err)
{
    json::Value v;
    if (!json::parse(read_file(path), v, err)) return false;
    if (!v.is_object()) { err = "scenario is not an object"; return false; }
    auto& s = g.sc;
    s.name = v["name"].as_string("unnamed");
    s.save = ue::from_utf8(v["save"].as_string());
    s.encounter = ue::from_utf8(v["encounter"].as_string());
    s.timeout_s = v["timeout_s"].as_number(600);
    s.action_delay_s = v["action_delay_s"].as_number(1.5);
    s.combo_auto_success = static_cast<int>(v["combo_auto_success"].as_int(10));
    s.max_turns = static_cast<int>(v["max_turns"].as_int(60));
    s.wait_players = static_cast<int>(v["wait_players"].as_int(0));
    for (size_t i = 0; i < v["actions"].size(); ++i) {
        const auto& a = v["actions"][i];
        Action act;
        const std::string type = a["type"].as_string("attack");
        act.type = type == "attack" ? 1 : static_cast<int>(a["action_type"].as_int(1));
        act.target = static_cast<int>(a["target"].as_int(0));
        s.actions.push_back(act);
    }
    if (s.actions.empty()) s.actions.push_back(Action{});
    for (size_t i = 0; i < v["defense"].size(); ++i) s.defense.push_back(v["defense"][i].as_string("fail"));
    s.expect = v["expect"];
    if (s.save.empty() || s.encounter.empty()) { err = "scenario needs save and encounter"; return false; }
    return true;
}

// --- État ---------------------------------------------------------------------------

std::string char_json(UObject* ch)
{
    UObject* st = game::stats(ch);
    return JsonObj()
        .add("name", game::char_class(ch))
        .add("player", game::is_player(ch))
        .add("dead", game::is_dead(ch))
        .add("hp", ue::get_double(st, STR("CurrentHP"), -1))
        .add("ap", ue::get_double(st, STR("CurrentAP"), -1))
        .add("shield", ue::get_int(st, STR("CurrentShieldPoints"), -1))
        .add("initiative", ue::get_double(st, STR("CurrentInitiative"), -1))
        .str();
}

std::string state_json()
{
    std::string players = "[", enemies = "[";
    for (UObject* c : game::players()) { if (players.size() > 1) players += ","; players += char_json(c); }
    for (UObject* c : game::enemies()) { if (enemies.size() > 1) enemies += ","; enemies += char_json(c); }
    return JsonObj().raw("players", players + "]").raw("enemies", enemies + "]").str();
}

// --- Fin ------------------------------------------------------------------------------

struct Check { std::string name, expected, actual; bool ok; };

void finish(bool pass_if_checks_ok, const std::string& reason)
{
    std::vector<Check> checks;
    auto add = [&](const std::string& n, const std::string& e, const std::string& a, bool ok) { checks.push_back({n, e, a, ok}); };
    const auto& ex = g.sc.expect;
    if (!ex["battle_end"].is_null()) {
        const std::string want = ex["battle_end"].as_string("any");
        const bool ok = g.end_result >= 0 && (want == "any" || std::to_string(g.end_result) == want);
        add("battle_end", want, std::to_string(g.end_result), ok);
    }
    if (!ex["min_parries_ok"].is_null()) {
        const int want = static_cast<int>(ex["min_parries_ok"].as_int());
        add("min_parries_ok", std::to_string(want), std::to_string(g.parry_ok), g.parry_ok >= want);
    }
    if (!ex["min_defenses_ok"].is_null()) {
        const int want = static_cast<int>(ex["min_defenses_ok"].as_int());
        add("min_defenses_ok", std::to_string(want), std::to_string(g.parry_ok + g.jump_ok), g.parry_ok + g.jump_ok >= want);
    }
    if (!ex["defense_commands_obeyed"].is_null()) {
        const bool ok = g.parry_ok == g.parry_cmd && g.jump_ok == g.jump_cmd && g.fail_ok == g.fail_cmd;
        add("defense_commands_obeyed", "true", ok ? "true" : "false", ok);
    }
    if (!ex["min_fails_ok"].is_null()) {
        const int want = static_cast<int>(ex["min_fails_ok"].as_int());
        add("min_fails_ok", std::to_string(want), std::to_string(g.fail_ok), g.fail_ok >= want);
    }
    if (!ex["max_forced_parries"].is_null()) {
        const int want = static_cast<int>(ex["max_forced_parries"].as_int());
        add("max_forced_parries", std::to_string(want), std::to_string(g.parry_forced), g.parry_forced <= want);
    }
    if (!ex["min_player_turns"].is_null()) {
        const int want = static_cast<int>(ex["min_player_turns"].as_int());
        add("min_player_turns", std::to_string(want), std::to_string(g.player_turns), g.player_turns >= want);
    }
    if (coop::active()) {
        add("coop_session", "ok", coop::session_ok() ? "ok" : "ko", coop::session_ok());
        add("desyncs", "0", std::to_string(coop::desync_count()), coop::desync_count() == 0);
        add("invalid_messages", "0", std::to_string(coop::invalid_count()), coop::invalid_count() == 0);
        add("hashes_compared", ">0", std::to_string(coop::hashes_compared()), coop::hashes_compared() > 0);
        if (coop::role() == coop::Role::Client)
            add("hp_overrides", "0", std::to_string(coop::hp_override_count()), coop::hp_override_count() == 0);
    }
    bool all = pass_if_checks_ok && g.errors.empty();
    for (auto& c : checks) all = all && c.ok;

    std::string checks_json = "[";
    for (auto& c : checks) {
        if (checks_json.size() > 1) checks_json += ",";
        checks_json += JsonObj().add("name", c.name).add("expected", c.expected).add("actual", c.actual).add("ok", c.ok).str();
    }
    checks_json += "]";
    std::string errors_json = "[";
    for (auto& e : g.errors) { if (errors_json.size() > 1) errors_json += ","; errors_json += "\"" + json_escape(e) + "\""; }
    errors_json += "]";

    const std::string summary = JsonObj()
        .add("turns", g.turn).add("player_turns", g.player_turns)
        .add("hits_on_players", g.hit_index)
        .add("parry_cmd", g.parry_cmd).add("parry_ok", g.parry_ok).add("parry_forced", g.parry_forced)
        .add("fail_cmd", g.fail_cmd).add("fail_ok", g.fail_ok)
        .add("jump_cmd", g.jump_cmd).add("jump_ok", g.jump_ok)
        .add("damage_events", g.damage_events)
        .add("battle_end", g.end_result)
        .add("duration_s", g.t)
        .str();
    const std::string result = JsonObj()
        .add("scenario", g.sc.name)
        .add("status", all ? "pass" : "fail")
        .add("reason", reason)
        .raw("summary", summary)
        .raw("checks", checks_json)
        .raw("errors", errors_json)
        .str();

    log().write("tr_result", JsonObj().add("status", all ? "pass" : "fail").add("reason", reason).raw("summary", summary));
    const auto path = std::filesystem::path(launch_args().run_dir) / L"result.json";
    if (std::FILE* f = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(result.data(), 1, result.size(), f);
        std::fclose(f);
    }
    set_phase(Phase::Done);
    log().close();
    // Fermeture brutale mais sûre : aucune save n'est écrite (autosave coupée), le résultat est sur disque.
    TerminateProcess(GetCurrentProcess(), all ? 0 : 1);
}

void fail(const std::string& why)
{
    g.errors.push_back(why);
    finish(false, why);
}

// --- Hooks -----------------------------------------------------------------------------

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();

    bus.post(fn(game::path::BattleChar, L"OnTurnStart"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle && g.phase != Phase::WaitBattle) return;
        if (g.phase == Phase::WaitBattle) set_phase(Phase::Battle);
        UObject* ch = ctx.Context;
        UObject* bm = game::battle_manager();
        const bool counter = ue::get_bool(bm, STR("HasActiveCounterAttack"));
        ++g.turn;
        log().write("tr_turn", JsonObj().add("actor", game::char_class(ch)).add("player", game::is_player(ch)).add("counter", counter).raw("state", state_json()), g.turn);
        if (g.turn > g.sc.max_turns) { fail("max_turns exceeded"); return; }
        const int spot = static_cast<int>(ue::get_int(ch, STR("BattleSpotIndex"), -1));
        if (game::is_player(ch) && !counter && coop::controls_spot(spot)) {
            ++g.player_turns;
            g.pending_actor = ch;
            g.pending_action_at = g.t + g.sc.action_delay_s;
        }
    });

    // Vérification de parade : le contexte est le composant de stats du DÉFENSEUR.
    bus.pre(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle) return;
        UObject* stats = ctx.Context;
        UObject* defender = ue::component_owner(stats);
        if (!defender || !game::is_player(defender)) return;
        if (!coop::controls_spot(static_cast<int>(ue::get_int(defender, STR("BattleSpotIndex"), -1)))) return;   // héros d'un autre joueur
        const auto& d = g.sc.defense;
        const std::string decision = d.empty() ? "fail" : d[static_cast<size_t>(g.hit_index) % d.size()];
        ++g.hit_index;
        g.decision[stats] = decision;
        bool started = false;
        if (decision == "parry") { ++g.parry_cmd; started = game::start_parry(defender); }
        else ++g.fail_cmd;
        log().write("tr_incoming_hit", JsonObj().add("defender", game::char_class(defender)).add("decision", decision).add("parry_started", started).add("hit", g.hit_index), g.turn);
    });

    bus.post(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        if (g.phase != Phase::Battle) return;
        UObject* stats = ctx.Context;
        auto it = g.decision.find(stats);
        if (it == g.decision.end()) return;
        const std::string decision = it->second;
        g.decision.erase(it);
        auto verdict = ue::read_out_bool(ctx, f, STR("Successful"));
        bool forced = false;
        if (decision == "parry") {
            if (verdict.value_or(false)) ++g.parry_ok;
            else if (ue::write_out_bool(ctx, f, STR("Successful"), true)) { forced = true; ++g.parry_forced; ++g.parry_ok; }
        } else if (!verdict.value_or(false)) {
            ++g.fail_ok;
        }
        log().write("tr_parry_verdict", JsonObj().add("decision", decision).add("verdict", verdict.has_value() ? (*verdict ? "true" : "false") : "?").add("forced", forced), g.turn);
    });

    // Attaques sautables : pas de TryParryHitFromCharacter, le test du saut est dans ReceiveDamageFromObject.
    bus.pre(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        if (g.phase != Phase::Battle) return;
        UObject* defender = ue::component_owner(ctx.Context);
        if (!defender || !game::is_player(defender)) return;
        if (!coop::controls_spot(static_cast<int>(ue::get_int(defender, STR("BattleSpotIndex"), -1)))) return;
        if (!game::incoming_attack_jumpable(ue::read_param_object(ctx, f, STR("DamageBuilder")))) return;
        const auto& d = g.sc.defense;
        std::string decision = d.empty() ? "fail" : d[static_cast<size_t>(g.hit_index) % d.size()];
        ++g.hit_index;
        bool started = false;
        if (decision != "fail") { decision = "jump"; ++g.jump_cmd; started = game::start_jump(defender); }
        else ++g.fail_cmd;
        g.jump_decision[ctx.Context] = decision;
        log().write("tr_incoming_hit", JsonObj().add("defender", game::char_class(defender)).add("decision", decision).add("jumpable", true).add("jump_started", started).add("hit", g.hit_index), g.turn);
    });
    bus.post(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        auto it = g.jump_decision.find(ctx.Context);
        if (it == g.jump_decision.end()) return;
        const std::string decision = it->second;
        g.jump_decision.erase(it);
        const bool dodged = ue::read_out_bool(ctx, f, STR("DodgeSuccess")).value_or(false);
        if (decision == "jump" && dodged) ++g.jump_ok;
        if (decision == "fail" && !dodged) ++g.fail_ok;
        log().write("tr_jump_verdict", JsonObj().add("decision", decision).add("dodged", dodged), g.turn);
    });

    bus.post(fn(game::path::Stats, L"ReceiveDamage"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle) return;
        UObject* owner = ue::component_owner(ctx.Context);
        if (owner && game::is_player(owner)) ++g.damage_events;
        log().write("tr_damage", JsonObj().add("target", game::char_class(owner)).add("hp_after", ue::get_double(ctx.Context, STR("CurrentHP"), -1)), g.turn);
    });

    bus.pre(fn(game::path::BattleMgr, L"StartBattleEndFlow"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        if (g.phase != Phase::Battle && g.phase != Phase::WaitBattle) return;
        g.end_result = static_cast<int>(ue::read_param_int(ctx, f, STR("BattleEndResult")).value_or(-1));
        log().write("tr_battle_end", JsonObj().add("result", g.end_result).raw("state", state_json()), g.turn);
        g.end_at = g.t + 3.0;
        set_phase(Phase::Ending);
    });
}

} // namespace

bool active() { return g.phase != Phase::Off && g.phase != Phase::Done; }

void init()
{
    const auto& a = launch_args();
    if (a.test.empty()) return;
    std::string err;
    if (!load_scenario(a.test, err)) {
        log().write("tr_error", JsonObj().add("what", "scenario: " + err));
        g.errors.push_back("scenario: " + err);
        g.phase = Phase::WaitMenu;   // on écrira l'échec au premier tick
        return;
    }
    log().write("tr_init", JsonObj().add("scenario", g.sc.name).add("save", ue::to_utf8(g.sc.save)).add("encounter", ue::to_utf8(g.sc.encounter)));
    subscribe();
    set_phase(Phase::WaitMenu);
}

void tick(float dt)
{
    if (!active()) return;
    g.t += dt;
    g.phase_t += dt;
    if (!g.errors.empty()) { finish(false, g.errors.front()); return; }
    if (g.t > g.sc.timeout_s) { fail(std::string("timeout in phase ") + phase_name(g.phase)); return; }
    if (g.t < g.next_poll && g.phase != Phase::Battle) return;
    g.next_poll = g.t + 0.5;

    std::string err;
    switch (g.phase) {
    case Phase::WaitMenu:
        if (game::gi() && game::main_menu() && g.phase_t > 3.0) set_phase(Phase::LoadSave);
        break;
    case Phase::LoadSave:
        if (!game::load_save_slot(g.sc.save, err)) { fail("load_save: " + err); return; }
        set_phase(Phase::WaitWorld);
        break;
    case Phase::WaitWorld:
        // monde prêt : un contrôleur de monde existe, pas de changement de carte, et quelques secondes de stabilité
        if (game::controller() && !game::map_change_in_progress()) {
            if (g.phase_t > 8.0) set_phase(Phase::Trigger);
        } else {
            g.phase_t = 0;
        }
        break;
    case Phase::Trigger:
        if (coop::role() == coop::Role::Client) {   // le client attend le combat de l'hôte
            if (coop::connected_players() > 0) set_phase(Phase::WaitBattle);
            break;
        }
        if (coop::connected_players() < g.sc.wait_players) break;
        if (!game::trigger_battle(g.sc.encounter, err)) { fail("trigger_battle: " + err); return; }
        set_phase(Phase::WaitBattle);
        break;
    case Phase::WaitBattle:
        if (g.phase_t > (coop::role() == coop::Role::Client ? 300.0 : 60.0)) fail("battle did not start");
        break;
    case Phase::Battle:
        if (g.pending_action_at >= 0 && g.t >= g.pending_action_at) {
            g.pending_action_at = -1;
            if (game::current_character() != g.pending_actor) { log().write("tr_action_skipped", JsonObj().add("why", "actor changed"), g.turn); break; }
            const auto& act = g.sc.actions[static_cast<size_t>(g.action_index++) % g.sc.actions.size()];
            std::vector<UObject*> alive;
            for (UObject* e : game::enemies()) if (!game::is_dead(e)) alive.push_back(e);
            if (alive.empty()) break;
            UObject* target = alive[static_cast<size_t>(act.target) % alive.size()];
            if (!game::perform_action(act.type, {target}, g.sc.combo_auto_success, err)) { fail("perform_action: " + err); return; }
            log().write("tr_action", JsonObj().add("actor", game::char_class(g.pending_actor)).add("type", act.type).add("target", game::char_class(target)), g.turn);
        }
        break;
    case Phase::Ending:
        if (g.t >= g.end_at) finish(true, "battle ended");
        break;
    default:
        break;
    }
}

} // namespace e33::testrunner
