#include "runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../common/minijson.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../sync/coop.hpp"
#include "../sync/explore.hpp"
#include "../ui/ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

using RC::Unreal::UObject;

namespace e33::testrunner {

namespace {

enum class Phase { Off, WaitMenu, LoadSave, WaitWorld, Trigger, WaitBattle, Battle, Ending, Explore, Done };

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
    case Phase::Explore: return "explore";
    case Phase::Done: return "done";
    }
    return "?";
}

using Clock = std::chrono::steady_clock;

constexpr int kActFreeAim = 100;   // action du runner : visée libre (pas un E_jRPG_ActionType)

// Action scriptée d'un héros. "target" : auto | enemy:N | all_enemies | self | hero:<Nom> | dead_hero.
// "combo" : résultat voulu pour chaque fenêtre de QTE (perfect | success | fail), joué par de vrais appuis.
struct Action {
    std::string hero;                    // nom interne du héros (vide = tout héros)
    int type = game::ActAttack;
    std::wstring secondary;              // compétence ou objet
    std::string target = "auto";
    std::vector<std::string> combo;
    std::shared_ptr<Action> fallback;    // "else" : si l'action est impossible (PA, objet, aucune cible)
    int shots = 0;                       // "freeaim" : nombre de tirs (visée sur "target", puis action "then")
    std::shared_ptr<Action> then;
};

struct Scenario {
    std::string name;
    std::wstring save;
    std::wstring encounter;
    std::vector<std::wstring> party;
    double timeout_s = 600;
    double action_delay_s = 1.5;
    int combo_auto_success = 10;
    int max_turns = 60;
    int wait_players = 0;       // hôte : nombre de clients attendus avant de lancer le combat
    bool manual = false;        // "manual": true : le joueur choisit lui-même les actions de ses héros (validation en jeu)
    std::vector<Action> actions;
    std::vector<std::string> defense;     // parry | dodge | perfect_dodge | jump | fail | manual, cyclique par vague de coups
    json::Value expect;
    json::Value setup;                    // {"<Héros>": {"hp": n, "shield": n, "dead": true}} : appliqué au tour 1, à l'identique sur chaque instance
    bool join = false;                    // client : rejoindre la partie de l'hôte (instantané) au lieu de charger "save"
    int pause_at_turn = 0;                // ouvre le menu pause au début de ce tour (3 s), puis le referme (phase 4)
    bool end_on_battle = false;           // exploration : le test se termine (vert) quand un combat commence
    bool end_on_session_end = false;      // client : le test se termine quand la session est finie et le menu principal atteint (phase 5)
    std::vector<json::Value> explore;     // étapes d'exploration (phase 4) : voir run_explore_step
};

struct State {
    Scenario sc;
    Phase phase = Phase::Off;
    double t = 0;             // secondes depuis init
    double phase_t = 0;       // secondes dans la phase courante
    double next_poll = 0;
    uint64_t frame = 0;
    int turn = 0;
    int player_turns = 0;
    std::map<std::string, int> action_index;   // par héros
    int hit_index = 0;        // coups reçus par des héros contrôlés ici
    int wave_index = 0;       // vagues de coups (une décision par vague, D11 : une parade couvre tous les héros du joueur)
    std::pair<UObject*, uint64_t> last_wave{nullptr, 0};
    std::string wave_decision;
    int parry_cmd = 0, parry_ok = 0, parry_forced = 0, fail_cmd = 0, fail_ok = 0;
    int jump_cmd = 0, jump_ok = 0, dodge_cmd = 0, dodge_ok = 0;
    std::map<UObject*, std::string> jump_decision;
    std::map<UObject*, std::string> decision;   // dernière décision par défenseur (stats component)
    int damage_events = 0;
    double pending_action_at = -1;
    UObject* pending_actor = nullptr;
    int planned_turn = -1;                // dernier tour (g.turn) dont l'action d'un héros d'ici a été programmée
    double session_end_t = -1;
    bool triggered = false;               // étape "trigger" : combat déjà déclenché
    // QTE offensifs joués par de vrais appuis
    std::vector<std::string> combo_plan;
    int combo_window = 0;
    std::vector<Clock::time_point> presses;
    // Visée libre en cours (action "freeaim") : étapes horodatées
    struct FreeAim { bool active = false; UObject* actor = nullptr; Action action; int fired = 0; double next = 0; int step = 0; } fa;
    std::map<std::string, int> metrics;       // compteurs du runner (actions par type, morts, réanimations…)
    int end_result = -1;
    double end_at = -1;
    bool rewards_rolled = false;
    std::string fail_reason;
    // Exploration
    size_t step = 0;
    double step_t = 0;
    bool step_started = false;
    bool battle_from_explore = false;
    double follow_since = -1;
    double pause_until = -1;
    double seq_since = -1;
    double next_skip = 0;
    int battles = 0;
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

int action_type_of(const std::string& t)
{
    if (t == "skill") return game::ActSkill;
    if (t == "item") return game::ActItem;
    if (t == "skip") return game::ActSkip;
    if (t == "flee") return game::ActFlee;
    if (t == "freeaim") return kActFreeAim;
    return game::ActAttack;
}

Action parse_action(const json::Value& a)
{
    Action act;
    act.hero = a["hero"].as_string("");
    act.type = a["type"].is_string() ? action_type_of(a["type"].as_string("attack")) : static_cast<int>(a["action_type"].as_int(1));
    act.secondary = ue::from_utf8(a["skill"].is_string() ? a["skill"].as_string() : a["item"].as_string());
    act.target = a["target"].is_string() ? a["target"].as_string("auto") : "auto";
    for (size_t i = 0; i < a["combo"].size(); ++i) act.combo.push_back(a["combo"][i].as_string("success"));
    if (a["else"].is_object()) act.fallback = std::make_shared<Action>(parse_action(a["else"]));
    act.shots = static_cast<int>(a["shots"].as_int(0));
    if (a["then"].is_object()) act.then = std::make_shared<Action>(parse_action(a["then"]));
    return act;
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
    for (size_t i = 0; i < v["party"].size(); ++i) s.party.push_back(ue::from_utf8(v["party"][i].as_string()));
    s.timeout_s = v["timeout_s"].as_number(600);
    s.action_delay_s = v["action_delay_s"].as_number(1.5);
    s.combo_auto_success = static_cast<int>(v["combo_auto_success"].as_int(10));
    s.max_turns = static_cast<int>(v["max_turns"].as_int(60));
    s.wait_players = static_cast<int>(v["wait_players"].as_int(0));
    s.manual = v["manual"].as_bool(false);
    for (size_t i = 0; i < v["actions"].size(); ++i) s.actions.push_back(parse_action(v["actions"][i]));
    if (s.actions.empty()) s.actions.push_back(Action{});
    for (size_t i = 0; i < v["defense"].size(); ++i) s.defense.push_back(v["defense"][i].as_string("fail"));
    s.expect = v["expect"];
    s.setup = v["setup"];
    s.join = v["join"].as_bool(false);
    s.pause_at_turn = static_cast<int>(v["pause_at_turn"].as_int(0));
    s.end_on_battle = v["end_on_battle"].as_bool(false);
    s.end_on_session_end = v["end_on_session_end"].as_bool(false);
    for (size_t i = 0; i < v["explore"].size(); ++i) s.explore.push_back(v["explore"][i]);
    if (s.save.empty() && !s.join) { err = "scenario needs save (or join)"; return false; }
    if (s.encounter.empty() && s.explore.empty()) { err = "scenario needs encounter (or explore steps)"; return false; }
    return true;
}

// --- État ---------------------------------------------------------------------------

std::string char_json(UObject* ch)
{
    UObject* st = game::stats(ch);
    return JsonObj()
        .add("name", game::char_class(ch))
        .add("spot", game::spot(ch))
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

int metric(const std::string& name)
{
    if (name.rfind("ui:", 0) == 0) return ui::count(name.substr(3));   // textes affichés (phase 5)
    if (auto it = g.metrics.find(name); it != g.metrics.end()) return it->second;
    return coop::counter(name);
}

// Attente : "any", "N" (égal), ">N", ">=N", "<N", "<=N".
bool satisfies(int v, const std::string& want)
{
    if (want == "any") return true;
    auto num = [](const std::string& x) { return std::atoi(x.c_str()); };
    if (want.rfind(">=", 0) == 0) return v >= num(want.substr(2));
    if (want.rfind("<=", 0) == 0) return v <= num(want.substr(2));
    if (want.rfind(">", 0) == 0) return v > num(want.substr(1));
    if (want.rfind("<", 0) == 0) return v < num(want.substr(1));
    return v == num(want);
}

std::string expect_string(const json::Value& v)
{
    if (v.is_string()) return v.as_string();
    if (v.is_number()) return std::to_string(v.as_int());
    return "";
}

void finish(bool pass_if_checks_ok, const std::string& reason)
{
    std::vector<Check> checks;
    const auto& ex = g.sc.expect;
    // "expect": {"<nom>": "any"} : la vérification est relevée mais ne bloque pas (ex. défense manuelle).
    auto add = [&](const std::string& n, const std::string& e, const std::string& a, bool ok) {
        const bool waived = ex[n].is_string() && ex[n].as_string("") == "any" && n != "battle_end";
        checks.push_back({n, waived ? "any" : e, a, ok || waived});
    };
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
        add("min_defenses_ok", std::to_string(want), std::to_string(g.parry_ok + g.jump_ok + g.dodge_ok), g.parry_ok + g.jump_ok + g.dodge_ok >= want);
    }
    if (!ex["defense_commands_obeyed"].is_null()) {
        const bool ok = g.parry_ok == g.parry_cmd && g.jump_ok == g.jump_cmd && g.fail_ok == g.fail_cmd && g.dodge_ok == g.dodge_cmd;
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
    // Compteurs : vérifications par défaut du coop, puis toute attente "nom": "<expr>" du scénario
    // sur un compteur du runner (actions_skill, hero_deaths…) ou du coop (raw_hash_diffs, targets_synced…).
    std::map<std::string, std::string> wants;
    if (coop::active()) {
        // Fin de session attendue (hôte parti, refus) : la session n'est plus « ok » et le combat n'a pas pu finir.
        const bool session_end_test = g.sc.end_on_session_end;
        if (!session_end_test) add("coop_session", "ok", coop::session_ok() ? "ok" : "ko", coop::session_ok());
        for (const char* c : {"desyncs", "draw_mismatches", "draw_count_diffs", "draw_errors", "invalid_messages", "defense_timeouts", "combo_timeouts", "turn_wait_timeouts",
                              "actor_mismatches", "end_mismatches", "rewards_mismatches", "repair_failures", "defense_verdict_mismatches", "combo_verdict_mismatches", "remote_action_errors", "remote_action_invalid"})
            wants[c] = "0";
        if ((g.sc.explore.empty() || g.battles > 0) && !session_end_test) {   // scénario d'exploration sans combat : pas de contrôles de combat
            wants["hashes_compared"] = ">0";
            wants["gameplay_draws"] = ">0";
            wants["battle_finalized"] = "1";
        }
        if (coop::role() == coop::Role::Client) wants["raw_hash_diffs"] = "0";
    }
    static const char* special[] = {"battle_end", "min_parries_ok", "min_defenses_ok", "defense_commands_obeyed", "min_fails_ok", "max_forced_parries", "min_player_turns"};
    for (const auto& [k, v] : ex.obj) {
        if (std::find_if(std::begin(special), std::end(special), [&](const char* x) { return k == x; }) != std::end(special)) continue;
        const std::string w = expect_string(v);
        if (!w.empty()) wants[k] = w;
    }
    g.metrics["battle_finalized"] = coop::active() ? (coop::battle_finalized() ? 1 : 0) : 1;
    for (const auto& [k, w] : wants) {
        const int v = metric(k);
        checks.push_back({k, w, std::to_string(v), satisfies(v, w)});
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
    std::string counters_json = "{";
    for (const auto& [k, v] : coop::counters()) { if (counters_json.size() > 1) counters_json += ","; counters_json += "\"" + json_escape(k) + "\":" + std::to_string(v); }
    for (const auto& [k, v] : g.metrics) { if (counters_json.size() > 1) counters_json += ","; counters_json += "\"" + json_escape(k) + "\":" + std::to_string(v); }
    counters_json += "}";

    const std::string summary = JsonObj()
        .add("turns", g.turn).add("player_turns", g.player_turns)
        .add("hits_on_players", g.hit_index)
        .add("parry_cmd", g.parry_cmd).add("parry_ok", g.parry_ok).add("parry_forced", g.parry_forced)
        .add("fail_cmd", g.fail_cmd).add("fail_ok", g.fail_ok)
        .add("jump_cmd", g.jump_cmd).add("jump_ok", g.jump_ok)
        .add("dodge_cmd", g.dodge_cmd).add("dodge_ok", g.dodge_ok)
        .add("damage_events", g.damage_events)
        .add("battle_end", g.end_result)
        .add("duration_s", g.t)
        .str();
    const std::string result = JsonObj()
        .add("scenario", g.sc.name)
        .add("status", all ? "pass" : "fail")
        .add("reason", reason)
        .raw("summary", summary)
        .raw("counters", counters_json)
        .raw("checks", checks_json)
        .raw("errors", errors_json)
        .str();

    log().write("tr_result", JsonObj().add("status", all ? "pass" : "fail").add("reason", reason).raw("summary", summary).raw("counters", counters_json));
    const auto path = std::filesystem::path(launch_args().run_dir) / L"result.json";
    if (std::FILE* f = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(result.data(), 1, result.size(), f);
        std::fclose(f);
    }
    coop::shutdown(all ? "test passed" : "test failed");
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

// --- Actions ---------------------------------------------------------------------------

std::vector<UObject*> resolve_targets(UObject* actor, const Action& a)
{
    if (a.target == "auto") return game::auto_targets(actor, a.type, a.secondary);
    std::vector<UObject*> alive_e, heroes;
    for (UObject* e : game::enemies()) if (!game::is_dead_fast(e)) alive_e.push_back(e);
    std::sort(alive_e.begin(), alive_e.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
    heroes = game::players();
    std::sort(heroes.begin(), heroes.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
    if (a.target == "all_enemies") return alive_e;
    if (a.target == "self") return {actor};
    if (a.target.rfind("enemy:", 0) == 0) {
        if (alive_e.empty()) return {};
        return {alive_e[static_cast<size_t>(std::atoi(a.target.c_str() + 6)) % alive_e.size()]};
    }
    if (a.target == "dead_hero") {
        for (UObject* h : heroes) if (game::is_dead_fast(h)) return {h};
        return {};
    }
    if (a.target.rfind("hero:", 0) == 0) {
        for (UObject* h : heroes) if (game::hardcoded_name(h) == a.target.substr(5)) return {h};
        return {};
    }
    return game::auto_targets(actor, a.type, a.secondary);
}

// Une action est jouable si ses cibles existent et (compétence) si les PA suffisent, (objet) s'il en reste.
bool playable(UObject* actor, const Action& a, std::vector<UObject*>& targets, std::string& why)
{
    if (a.type == game::ActSkill) {
        int targeting = -1, cost = 0;
        if (!game::skill_info(actor, a.secondary, targeting, cost)) { why = "skill not equipped"; return false; }
        if (ue::get_double(game::stats(actor), STR("CurrentAP")) < cost) { why = "not enough AP"; return false; }
    }
    if (a.type == game::ActItem && game::battle_item_count(a.secondary) <= 0) { why = "no item left"; return false; }
    if (a.type == game::ActSkip || a.type == game::ActFlee) { targets.clear(); return true; }
    if (a.type == kActFreeAim && ue::get_double(game::stats(actor), STR("CurrentAP")) < a.shots) { why = "not enough AP to shoot"; return false; }
    targets = resolve_targets(actor, a.type == kActFreeAim ? Action{} : a);
    if (targets.empty()) { why = "no target"; return false; }
    return true;
}

const Action* next_action(UObject* actor)
{
    const std::string hero = game::hardcoded_name(actor);
    std::vector<const Action*> mine;
    for (auto& a : g.sc.actions) if (a.hero.empty() || a.hero == hero) mine.push_back(&a);
    if (mine.empty()) return nullptr;
    return mine[static_cast<size_t>(g.action_index[hero]++) % mine.size()];
}

// Point visé par le « joueur » simulé : la cible + 1 m (milieu du corps).
bool aim_point(UObject* target, double out[3])
{
    if (!game::location(target, out)) return false;
    out[2] += 100.0;
    return true;
}

void play_action(UObject* actor, const Action* a);

void play_turn(UObject* actor) { play_action(actor, next_action(actor)); }

// Visée libre (action "freeaim") : début, tirs toutes les 1,2 s sur la cible, annulation, puis action "then".
// Même chronologie que le faux client (fakepeer) : le rejeu d'un enregistrement voit les mêmes tirs aux mêmes instants.
void tick_free_aim()
{
    auto& fa = g.fa;
    if (!fa.active || g.t < fa.next) return;
    UObject* bm = game::battle_manager();
    if (fa.step == 0) {
        ue::Call c(bm, STR("StartFreeAimFromUI"));
        if (c) c.invoke();
        fa.step = 1;
        fa.next = g.t + 1.5;
        ++g.metrics["actions_freeaim"];
        log().write("tr_free_aim", JsonObj().add("step", "start").add("shots", fa.action.shots), g.turn);
    } else if (fa.step == 1 && fa.fired < fa.action.shots) {
        auto targets = resolve_targets(fa.actor, fa.action);
        double p[3];
        if (!targets.empty() && aim_point(targets.front(), p)) coop::set_local_aim(p);
        ue::Call c(ue::get_object(fa.actor, STR("BP_FreeAimShooterBattleComponent")), STR("OnFreeAimShootAction"));
        if (c) c.invoke();
        ++fa.fired;
        fa.next = g.t + 1.2;
        log().write("tr_free_aim", JsonObj().add("step", "fire").add("n", fa.fired), g.turn);
    } else if (fa.step == 1) {
        ue::Call c(bm, STR("CancelFreeAimFromUI"));
        if (c) c.invoke();
        coop::set_local_aim(nullptr);
        fa.step = 2;
        fa.next = g.t + 1.0;
        log().write("tr_free_aim", JsonObj().add("step", "cancel"), g.turn);
    } else {
        fa.active = false;
        static const Action basic{};
        play_action(fa.actor, fa.action.then ? fa.action.then.get() : &basic);
    }
}

void play_action(UObject* actor, const Action* a)
{
    static const Action basic{};
    if (!a) a = &basic;
    std::vector<UObject*> targets;
    std::string why;
    while (a && !playable(actor, *a, targets, why)) {
        log().write("tr_action_fallback", JsonObj().add("hero", game::hardcoded_name(actor)).add("why", why).add("secondary", ue::to_utf8(a->secondary)), g.turn);
        a = a->fallback ? a->fallback.get() : (a == &basic ? nullptr : &basic);
    }
    if (!a) { fail("no playable action for " + game::hardcoded_name(actor)); return; }
    if (a->type == kActFreeAim) {
        g.fa = {};
        g.fa.active = true;
        g.fa.actor = actor;
        g.fa.action = *a;
        g.fa.next = g.t;
        return;
    }
    g.combo_plan = a->combo;
    g.combo_window = 0;
    g.presses.clear();
    std::string err;
    const int auto_success = a->combo.empty() ? g.sc.combo_auto_success : 0;
    if (!game::perform_action(a->type, a->secondary, targets, auto_success, err)) { fail("perform_action: " + err); return; }
    const char* kind = a->type == game::ActSkill ? "actions_skill" : a->type == game::ActItem ? "actions_item"
                     : a->type == game::ActSkip ? "actions_skip" : a->type == game::ActFlee ? "actions_flee" : "actions_attack";
    ++g.metrics[kind];
    std::string ts = "[";
    for (UObject* t : targets) { if (ts.size() > 1) ts += ","; ts += "\"" + game::char_class(t) + "#" + std::to_string(game::spot(t)) + "\""; }
    log().write("tr_action", JsonObj().add("actor", game::char_class(actor)).add("hero", game::hardcoded_name(actor)).add("type", a->type)
                                  .add("secondary", ue::to_utf8(a->secondary)).raw("targets", ts + "]").add("combo_plan", static_cast<int64_t>(a->combo.size())), g.turn);
}

// Décision de défense pour un coup sur un héros d'ici : une par vague (même attaquant, même image).
std::string defense_decision(UObject* attacker)
{
    ++g.hit_index;
    if (g.last_wave.first == attacker && g.last_wave.second == g.frame && !g.wave_decision.empty()) return g.wave_decision;
    const auto& d = g.sc.defense;
    g.wave_decision = d.empty() ? "fail" : d[static_cast<size_t>(g.wave_index) % d.size()];
    ++g.wave_index;
    g.last_wave = {attacker, g.frame};
    return g.wave_decision;
}

bool start_dodge(UObject* defender, bool perfect)
{
    game::clear_defense_lock(defender);
    const bool ok = game::start_dodge(defender);
    if (ok && !perfect) {
        const double start = ue::get_double(defender, STR("DefensiveMoveImmunityStartTime"));
        ue::set_double(defender, STR("DefensiveMoveImmunityStartTime"), start - game::parry_immune_duration(defender) - 0.05);
    }
    return ok;
}

// Réglage initial du scénario (tour 1) : PV, bouclier ou mort d'un héros. Chaque instance du test l'applique de la même
// façon au même moment (après le contrôle du tour 1) : la simulation reste identique partout.
void apply_setup()
{
    if (!g.sc.setup.is_object()) return;
    for (UObject* h : game::players()) {
        const json::Value& cfg = g.sc.setup[game::hardcoded_name(h)];
        if (!cfg.is_object()) continue;
        UObject* st = game::stats(h);
        if (cfg["hp"].is_number()) ue::set_double(st, STR("CurrentHP"), cfg["hp"].as_number());
        if (cfg["shield"].is_number()) {
            ue::Call c(st, STR("SetCurrentShieldPoints"));
            if (c) c.set_int(STR("NewValue"), cfg["shield"].as_int()).set_string(STR("DebugReason"), STR("E33Coop test setup")).invoke();
        }
        if (cfg["dead"].as_bool(false)) {
            ue::set_double(st, STR("CurrentHP"), 0);
            ue::Call c(st, STR("SetDead"));
            if (c) c.invoke();
        }
        log().write("tr_setup", JsonObj().add("hero", game::hardcoded_name(h)).raw("cfg", "{}").add("hp", ue::get_double(st, STR("CurrentHP")))
                                     .add("shield", ue::get_int(st, STR("CurrentShieldPoints"))).add("dead", game::is_dead(h)), g.turn);
    }
}

// --- Hooks -----------------------------------------------------------------------------

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();

    bus.post(fn(game::path::BattleChar, L"OnTurnStart"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        // Un client coop peut voir le combat (lancé par le module coop) démarrer avant que le runner ait fini
        // d'attendre le monde : tout début de tour pendant ces attentes fait passer en combat.
        const bool waiting = g.phase == Phase::WaitWorld || g.phase == Phase::Trigger || g.phase == Phase::WaitBattle || g.phase == Phase::Explore;
        if (g.phase != Phase::Battle && !waiting) return;
        if (g.phase == Phase::Explore) {   // combat d'exploration (lancé ici ou par un autre joueur) : on le joue puis on reprend
            if (g.sc.end_on_battle) { finish(true, "battle started"); return; }
            g.battle_from_explore = true;
            g.turn = 0;
            g.action_index.clear();
            ++g.battles;
            ++g.metrics["explore_battles"];
        }
        if (waiting) set_phase(Phase::Battle);
        UObject* ch = ctx.Context;
        UObject* bm = game::battle_manager();
        const bool counter = ue::get_bool(bm, STR("HasActiveCounterAttack"));
        ++g.turn;
        if (g.turn == 1) { log().write("tr_inventory", JsonObj().raw("inventory", game::battle_inventory_json())); apply_setup(); }
        log().write("tr_turn", JsonObj().add("actor", game::char_class(ch)).add("player", game::is_player(ch)).add("counter", counter).raw("state", state_json()), g.turn);
        if (g.turn > g.sc.max_turns) { fail("max_turns exceeded"); return; }
        if (g.sc.pause_at_turn > 0 && g.turn == g.sc.pause_at_turn && g.pause_until < 0) {
            ue::Call c(game::controller(), STR("OpenPauseMenu"));
            if (c) c.invoke();
            g.pause_until = g.t + 3.0;
            ++g.metrics["pauses_opened"];
            log().write("tr_pause_open", JsonObj(), g.turn);
        }
        if (game::is_player(ch) && !counter && coop::controls_spot(game::spot(ch))) {
            ++g.player_turns;
            g.pending_actor = ch;
            if (!g.sc.manual) g.pending_action_at = g.t + g.sc.action_delay_s;
            g.planned_turn = g.turn;
        }
    });

    // Vérification de parade : le contexte est le composant de stats du DÉFENSEUR.
    bus.pre(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        if (g.phase != Phase::Battle) return;
        UObject* stats = ctx.Context;
        UObject* defender = ue::component_owner(stats);
        if (!defender || !game::is_player(defender)) return;
        if (!coop::controls_spot(game::spot(defender))) return;   // héros d'un autre joueur
        const std::string decision = defense_decision(ue::read_param_object(ctx, f, STR("AttackingCharacter")));
        g.decision[stats] = decision;
        bool started = false;
        if (decision == "parry") { ++g.parry_cmd; started = game::start_parry(defender); }
        else if (decision == "dodge" || decision == "perfect_dodge") { ++g.dodge_cmd; started = start_dodge(defender, decision == "perfect_dodge"); }
        else if (decision != "manual") ++g.fail_cmd;   // "manual" : le joueur pare lui-même
        log().write("tr_incoming_hit", JsonObj().add("defender", game::char_class(defender)).add("decision", decision).add("started", started).add("hit", g.hit_index), g.turn);
    });

    bus.post(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        if (g.phase != Phase::Battle) return;
        UObject* stats = ctx.Context;
        auto it = g.decision.find(stats);
        if (it == g.decision.end()) return;
        const std::string decision = it->second;
        if (decision != "dodge" && decision != "perfect_dodge") g.decision.erase(it);   // esquive : verdict dans TryDodgeHitFromCharacter
        auto verdict = ue::read_out_bool(ctx, f, STR("Successful"));
        if (decision == "manual") {
            if (verdict.value_or(false)) ++g.parry_ok; else ++g.fail_ok;
        } else if (decision == "parry") {
            if (verdict.value_or(false)) ++g.parry_ok;
        } else if (decision == "fail" && !verdict.value_or(false)) {
            ++g.fail_ok;
        }
        log().write("tr_parry_verdict", JsonObj().add("decision", decision).add("verdict", verdict.has_value() ? (*verdict ? "true" : "false") : "?"), g.turn);
    });
    bus.post(fn(game::path::Stats, L"TryDodgeHitFromCharacter"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        auto it = g.decision.find(ctx.Context);
        if (it == g.decision.end()) return;
        const std::string decision = it->second;
        g.decision.erase(it);
        const bool ok = ue::read_out_bool(ctx, f, STR("Successful")).value_or(false);
        const bool perfect = ue::read_out_bool(ctx, f, STR("IsPerfectDodge")).value_or(false);
        if (ok && perfect == (decision == "perfect_dodge")) ++g.dodge_ok;
        log().write("tr_dodge_verdict", JsonObj().add("decision", decision).add("ok", ok).add("perfect", perfect), g.turn);
    });

    // Attaques sautables : pas de TryParryHitFromCharacter, le test du saut est dans ReceiveDamageFromObject.
    bus.pre(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, RC::Unreal::UFunction* f) {
        if (g.phase != Phase::Battle) return;
        UObject* defender = ue::component_owner(ctx.Context);
        if (!defender || !game::is_player(defender)) return;
        if (!coop::controls_spot(game::spot(defender))) return;
        UObject* builder = ue::read_param_object(ctx, f, STR("DamageBuilder"));
        if (!game::incoming_attack_jumpable(builder)) return;
        std::string decision = defense_decision(game::damage_source_character(builder));
        bool started = false;
        if (decision == "manual") {}   // le joueur saute lui-même
        else if (decision != "fail") { decision = "jump"; ++g.jump_cmd; started = game::start_jump(defender); }
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
        if ((decision == "jump" || decision == "manual") && dodged) ++g.jump_ok;
        if (decision == "fail" && !dodged) ++g.fail_ok;
        log().write("tr_jump_verdict", JsonObj().add("decision", decision).add("dodged", dodged), g.turn);
    });

    bus.post(fn(game::path::Stats, L"ReceiveDamage"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle) return;
        UObject* owner = ue::component_owner(ctx.Context);
        if (owner && game::is_player(owner)) ++g.damage_events;
        log().write("tr_damage", JsonObj().add("target", game::char_class(owner)).add("hp_after", ue::get_double(ctx.Context, STR("CurrentHP"), -1)), g.turn);
    });

    // QTE offensifs d'un héros d'ici avec un plan : vrai appui (OnComboPressed) à un instant choisi de la fenêtre.
    // Fenêtre parfaite = dernier quart (GAME-INTERNALS §8.1) : « perfect » à 85 %, « success » à 30 %, « fail » sans appui.
    // L appui part à l image suivante (jusqu à +30 ‰ à 60 i/s) ; au-delà de ~900 ‰, Combo1 porte un coup de plus
    // que l enregistrement (fait à ~880 ‰) : p3_actions_client échouait 3 fois sur 3 le 2026-10-20 avec des appuis à 910 ‰.
    bus.post(fn(game::path::Timeline, L"OnSkillComboWindowStartReached"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle || g.combo_plan.empty()) return;
        UObject* hero = ue::get_object(ctx.Context, STR("BattleCharacter"));
        if (!hero || !game::is_player(hero) || !coop::controls_spot(game::spot(hero))) return;
        const std::string want = g.combo_plan[static_cast<size_t>(g.combo_window++) % g.combo_plan.size()];
        const double dur = ue::get_double(ctx.Context, STR("CurrentWindowEndRealTime")) - ue::get_double(ctx.Context, STR("CurrentWindowStartRealTime"));
        const double frac = want == "perfect" ? 0.85 : want == "success" ? 0.30 : -1;
        if (frac >= 0 && dur > 0) g.presses.push_back(Clock::now() + std::chrono::microseconds(static_cast<int64_t>(dur * frac * 1e6)));
        log().write("tr_combo_window", JsonObj().add("window", g.combo_window).add("want", want).add("duration", dur), g.turn);
    });
    bus.post(fn(game::path::Timeline, L"OnSkillComboWindowEndReached"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle) return;
        UObject* hero = ue::get_object(ctx.Context, STR("BattleCharacter"));
        if (!hero || !game::is_player(hero)) return;
        const bool ok = ue::get_bool(ctx.Context, STR("IsSkillComboSuccessful"));
        const bool perfect = ue::get_bool(ctx.Context, STR("IsSkillComboPerfect"));
        ++g.metrics[perfect ? "combo_perfect" : ok ? "combo_success" : "combo_fail"];
        log().write("tr_combo_result", JsonObj().add("hero", game::hardcoded_name(hero)).add("success", ok).add("perfect", perfect), g.turn);
    });

    bus.post(fn(game::path::BattleChar, L"OnDeath"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle && g.phase != Phase::Ending) return;
        if (game::is_player(ctx.Context)) ++g.metrics["hero_deaths"]; else ++g.metrics["enemy_deaths"];
        log().write("tr_death", JsonObj().add("who", game::char_class(ctx.Context)), g.turn);
    });
    bus.post(fn(game::path::BattleChar, L"OnResurrected"), [](HookCtx& ctx, RC::Unreal::UFunction*) {
        if (g.phase != Phase::Battle) return;
        ++g.metrics["hero_revives"];
        log().write("tr_revive", JsonObj().add("who", game::char_class(ctx.Context)), g.turn);
    });

    bus.post(fn(game::path::BattleMgr, L"RollBattleRewards"), [](HookCtx&, RC::Unreal::UFunction*) {
        g.rewards_rolled = true;
        ++g.metrics["rewards_rolled"];
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

// --- Exploration (phase 4) ------------------------------------------------------------------
// Étapes "explore" d'un scénario, exécutées dans l'ordre :
//   {"do":"wait","s":3} | {"do":"wait","until":"<compteur>","is":">=1","timeout":60}
//   {"do":"wait_world"}                    monde prêt et stable 3 s (après chargement / transition)
//   {"do":"wait_join","players":1}         hôte : clients entrés dans la partie
//   {"do":"move","dir":[1,0],"s":3}        pilote : marche dans une direction (AddMovementInput)
//   {"do":"interact","actor":"<préfixe>"}  interaction avec un PNJ / objet (nom d'acteur commençant par)
//   {"do":"choose","index":1}              choix de dialogue (attend que la question soit posée)
//   {"do":"wait_dialogue_end"}
//   {"do":"change_map","level":"Level_Camp_Main","spawn":"<tag>"}
//   {"do":"world_battle"}                  pilote : combat avec l'ennemi du monde le plus proche (actions du scénario)
//   {"do":"battle"}                        attend un combat lancé par un autre joueur, puis le joue
//   {"do":"wait_follow","max":250}        suiveur : à moins de max (u) du pilote pendant 2 s
//   {"do":"zone_action","zone":"<nom>"}  action « au passage » d'une zone de déclenchement (cinématique, combat…)
//   {"do":"skip_cinematic","after_s":5}   passe la cinématique en cours (après after_s s dans l'étape)
//   {"do":"wait_key"}                     validation en jeu : attend la touche F9
//   {"do":"trigger","players":1}          combat du scénario (encounter, party) dès que "players" clients sont connectés
//   {"do":"request_pilot"} | {"do":"end"}
// "check" (facultatif, sur toute étape) : {"<compteur>":"<attente>"} vérifié à la fin de l'étape.
UObject* find_interactable(const std::string& prefix)
{
    for (const wchar_t* cls : {STR("BP_jRPG_NPC_Dialog_C"), STR("BP_jRPG_NPC_Base_C"), STR("BP_InteractionObjectBase_C"),
                               STR("BP_jRPG_InteractionObject_Base_C"), STR("BP_Chest_Regular_C")}) {
        std::vector<UObject*> all;
        RC::Unreal::UObjectGlobals::FindAllOf(cls, all);
        for (UObject* o : all)
            if (ue::short_name(o).rfind(prefix, 0) == 0) return o;
    }
    return nullptr;
}

void next_step(const char* why)
{
    g.triggered = false;
    const auto& st = g.sc.explore[g.step];
    log().write("tr_step_done", JsonObj().add("step", static_cast<int64_t>(g.step)).add("do", st["do"].as_string("")).add("why", why).add("t", g.step_t));
    if (st["check"].is_object()) {
        for (const auto& [k, v] : st["check"].obj) {
            const std::string w = expect_string(v);
            const int val = metric(k);
            if (!satisfies(val, w)) {
                g.errors.push_back("step " + std::to_string(g.step) + " check " + k + " " + w + " (got " + std::to_string(val) + ")");
                return;
            }
        }
    }
    ++g.step;
    g.step_t = 0;
    g.step_started = false;
}

bool world_stable(double need)
{
    const bool ok = game::controller() && !game::map_change_in_progress() && game::explore_pawn() && !game::in_battle();
    if (!ok) g.step_t = 0;
    return ok && g.step_t >= need;
}

// Fin d'un combat d'exploration : retour aux étapes (l'étape "battle" / "world_battle" en cours est terminée).
void resume_explore(const char* why)
{
    g.battle_from_explore = false;
    log().write("tr_explore_resume", JsonObj().add("why", why).add("battles", g.battles));
    set_phase(Phase::Explore);
    if (g.step < g.sc.explore.size()) {
        const std::string what = g.sc.explore[g.step]["do"].as_string("");
        if (what == "battle" || what == "world_battle" || what == "trigger") next_step(why);
    }
}

void run_explore_step(float dt)
{
    if (g.step >= g.sc.explore.size()) { finish(true, "explore done"); return; }
    g.step_t += dt;
    const auto& st = g.sc.explore[g.step];
    const std::string what = st["do"].as_string("");
    const double timeout = st["timeout"].as_number(what == "battle" ? 600 : 180);
    const bool first = !g.step_started;
    if (first) {
        g.step_started = true;
        log().write("tr_step", JsonObj().add("step", static_cast<int64_t>(g.step)).add("do", what));
    }
    if (g.step_t > timeout) { fail("explore step " + std::to_string(g.step) + " (" + what + ") timed out"); return; }
    std::string err;
    if (what == "wait") {
        if (st["until"].is_string()) {
            if (satisfies(metric(st["until"].as_string()), expect_string(st["is"]))) next_step("condition");
        } else if (g.step_t >= st["s"].as_number(1)) next_step("time");
    } else if (what == "wait_world") {
        if (world_stable(st["s"].as_number(3))) next_step("world ready");
    } else if (what == "wait_join") {
        if (coop::counter("joined_players") >= st["players"].as_int(1)) next_step("joined");
    } else if (what == "move") {
        if (g.step_t >= st["s"].as_number(2)) { next_step("time"); return; }
        if (UObject* pawn = game::explore_pawn())
            game::move_pawn(pawn, st["dir"][0].as_number(1), st["dir"][1].as_number(0), st["scale"].as_number(1));
    } else if (what == "interact") {
        UObject* o = find_interactable(st["actor"].as_string());
        if (!o) { fail("interact: no actor starting with " + st["actor"].as_string()); return; }
        if (!game::interact(o, false)) { fail("interact failed"); return; }
        ++g.metrics["interactions"];
        next_step("done");
    } else if (what == "choose") {
        if (explore::choice_pending() && g.step_t > 1.0) {
            if (!game::choose_dialogue(static_cast<int>(st["index"].as_int(0)))) { fail("choose failed"); return; }
            ++g.metrics["choices"];
            next_step("done");
        }
    } else if (what == "wait_dialogue_end") {
        if (!game::in_dialogue() && !game::playing_cinematic() && g.step_t > 1.0) next_step("done");
    } else if (what == "change_map") {
        if (!game::change_map(ue::from_utf8(st["level"].as_string()), ue::from_utf8(st["spawn"].as_string("")))) { fail("change_map failed"); return; }
        ++g.metrics["map_changes"];
        next_step("requested");
    } else if (what == "world_battle" || what == "battle" || what == "trigger") {
        // "trigger" : combat du scénario (encounter, party), une fois "players" joueurs dans la partie (phase 5).
        if (what == "trigger" && !g.triggered) {
            if (coop::connected_players() < st["players"].as_int(0)) return;
            if (!game::trigger_battle(g.sc.encounter, g.sc.party, err)) { fail("trigger: " + err); return; }
            g.triggered = true;
        }
        if (what == "world_battle" && first) {
            UObject* e = game::find_world_enemy(st["enemy"].as_string(""));
            if (!game::start_world_battle(e, ue::from_utf8(st["encounter"].as_string("")), static_cast<int>(st["engage"].as_int(0)), err)) { fail("world_battle: " + err); return; }
        }
        // Le passage en combat se fait au premier début de tour (hook OnTurnStart).
    } else if (what == "wait_follow") {   // suiveur : écart au pilote sous "max" (u) pendant 2 s
        const double e = explore::follow_error();
        g.metrics["follow_error"] = static_cast<int>(e);
        if (first) g.follow_since = -1;
        if (e >= 0 && e <= st["max"].as_number(250)) {
            if (g.follow_since < 0) g.follow_since = g.step_t;
            if (g.step_t - g.follow_since >= 2.0) next_step("close");
        } else {
            g.follow_since = -1;
        }
    } else if (what == "zone_action") {
        if (!game::run_zone_action(st["zone"].as_string(), err)) { fail("zone_action: " + err); return; }
        ++g.metrics["zone_actions"];
        next_step("done");
    } else if (what == "skip_cinematic") {
        // Seulement une fois la séquence commencée (OnSequenceStarted, ~4 s après le déclenchement) : passée plus tôt,
        // la cinématique ne se terminait jamais (2026-10-20).
        if (first) { g.seq_since = -1; g.next_skip = 0; }
        if (explore::sequence_playing() && g.seq_since < 0) g.seq_since = g.step_t;
        // Le jeu ignore un skip juste après le début de la séquence (vu le 2026-10-20) : on réessaie toutes les 2 s.
        if (g.seq_since >= 0 && !explore::sequence_playing() && !game::playing_cinematic()) {
            ++g.metrics["cinematics_skipped"];
            next_step("skipped");
        } else if (g.seq_since >= 0 && g.step_t - g.seq_since >= st["after_s"].as_number(0) && g.step_t >= g.next_skip) {
            g.next_skip = g.step_t + 2.0;
            if (!game::skip_cinematic()) { fail("skip_cinematic failed"); return; }
            ++g.metrics["skip_attempts"];
            log().write("tr_skip_cinematic", JsonObj().add("t", g.step_t));
        }
    } else if (what == "wait_key") {   // validation en jeu : le joueur termine le test (F9, fenêtre du jeu au premier plan)
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000)) next_step("key");
    } else if (what == "request_pilot") {
        explore::request_pilot();
        next_step("requested");
    } else if (what == "end") {
        finish(true, "explore end");
    } else {
        fail("unknown explore step: " + what);
    }
}

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
    ++g.frame;
    if (!g.errors.empty()) { finish(false, g.errors.front()); return; }
    if (g.t > g.sc.timeout_s) { fail(std::string("timeout in phase ") + phase_name(g.phase)); return; }
    // Fin de session (hôte parti, refus) : le jeu revient au menu principal ; le scénario conclut alors.
    if (g.sc.end_on_session_end && coop::session_ended()) {
        if (g.session_end_t < 0) g.session_end_t = g.t;
        if (game::main_menu() && g.t - g.session_end_t > 2.0) { ++g.metrics["back_to_title"]; finish(true, "session ended, back to title"); return; }
        if (g.t - g.session_end_t > 60.0) { finish(true, "session ended (title not reached)"); return; }
        return;
    }
    // Appuis de QTE programmés (temps réel)
    if (!g.presses.empty() && Clock::now() >= g.presses.front()) {
        g.presses.erase(g.presses.begin());
        ue::Call c(game::battle_manager(), STR("OnComboPressed"));
        if (c) c.invoke();
        ++g.metrics["combo_presses"];
        log().write("tr_combo_press", JsonObj(), g.turn);
    }
    if (g.t < g.next_poll && g.phase != Phase::Battle && g.phase != Phase::Explore) return;
    g.next_poll = g.t + 0.5;

    std::string err;
    switch (g.phase) {
    case Phase::WaitMenu:
        if (game::gi() && game::main_menu() && g.phase_t > 3.0) set_phase(Phase::LoadSave);
        break;
    case Phase::LoadSave:
        if (g.sc.join) {   // client qui rejoint : le module coop charge l'instantané de l'hôte
            if (explore::joined()) set_phase(Phase::WaitWorld);
            else if (g.phase_t > 400.0) fail("join: snapshot not loaded");
            break;
        }
        if (!game::load_save_slot(g.sc.save, err)) { fail("load_save: " + err); return; }
        set_phase(Phase::WaitWorld);
        break;
    case Phase::WaitWorld:
        // monde prêt : un contrôleur de monde existe, pas de changement de carte, et quelques secondes de stabilité
        if (game::controller() && !game::map_change_in_progress()) {
            if (g.phase_t > 8.0) set_phase(g.sc.explore.empty() ? Phase::Trigger : Phase::Explore);
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
        if (!game::trigger_battle(g.sc.encounter, g.sc.party, err)) { fail("trigger_battle: " + err); return; }
        set_phase(Phase::WaitBattle);
        break;
    case Phase::WaitBattle:
        if (g.phase_t > (coop::role() == coop::Role::Client ? 300.0 : 60.0)) fail("battle did not start");
        break;
    case Phase::Battle:
        tick_free_aim();
        // Menu pause que le scénario n'a pas demandé (le jeu se met en pause quand sa fenêtre perd le focus, vu le
        // 2026-10-20 sur p3_party_client) : refermé, sinon le combat reste bloqué jusqu'au délai du test.
        if (!g.sc.manual && g.pause_until <= 0 && g.frame % 15 == 0 && game::pause_menu_visible()) {
            ue::Call c(ue::get_object(game::controller(), STR("PauseMenuWidget")), STR("OnResumeClicked"));
            if (c) c.invoke();
            ++g.metrics["unexpected_pauses"];
            log().write("tr_unexpected_pause_closed", JsonObj(), g.turn);
        }
        if (g.pause_until > 0 && g.t >= g.pause_until) {   // menu pause du scénario : refermé par « Reprendre »
            g.pause_until = 0;
            ue::Call c(ue::get_object(game::controller(), STR("PauseMenuWidget")), STR("OnResumeClicked"));
            if (c) c.invoke();
            log().write("tr_pause_close", JsonObj().add("menu_visible", game::pause_menu_visible()), g.turn);
        }
        // Fuite : le jeu revient à l'exploration sans StartBattleEndFlow.
        if (g.battle_from_explore && g.frame % 30 == 0 && g.phase_t > 5.0 && !game::in_battle() && game::explore_pawn()) { resume_explore("left battle"); break; }
        // Menu pause ouvert (le nôtre ou celui d'un autre joueur : jeu en pause) : aucune action avant la reprise.
        // Héros repris à un joueur parti pendant son tour (phase 5) : le « joueur » de l'hôte le joue.
        if (g.pending_action_at < 0 && g.planned_turn != g.turn && !g.sc.manual) {
            UObject* cur = game::current_character();
            if (cur && game::is_player(cur) && coop::active() && coop::controls_spot(game::spot(cur)) && !coop::action_forced(game::spot(cur)) &&
                !ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) {
                g.planned_turn = g.turn;
                g.pending_actor = cur;
                g.pending_action_at = g.t + g.sc.action_delay_s;
                ++g.metrics["takeover_turns"];
                log().write("tr_takeover_turn", JsonObj().add("actor", game::char_class(cur)), g.turn);
            }
        }
        if (g.pending_action_at >= 0 && g.t >= g.pending_action_at && (g.pause_until > 0 || explore::remote_paused() || game::pause_menu_visible() || coop::holding())) {
            g.pending_action_at = g.t + 0.5;
            break;
        }
        if (g.pending_action_at >= 0 && g.t >= g.pending_action_at) {
            g.pending_action_at = -1;
            if (game::current_character() != g.pending_actor) { log().write("tr_action_skipped", JsonObj().add("why", "actor changed"), g.turn); break; }
            if (coop::action_forced(game::spot(g.pending_actor))) { log().write("tr_action_skipped", JsonObj().add("why", "forced by the host (turn timer)"), g.turn); break; }
            play_turn(g.pending_actor);
        }
        break;
    case Phase::Ending: {
        // Attendre la fin de combat complète (récompenses tirées et comparées), bornée.
        const bool done = coop::active() ? coop::battle_finalized() : (g.rewards_rolled || g.end_result != game::EndVictory);
        if ((done && g.t >= g.end_at) || g.t >= g.end_at + 15.0) {
            if (g.battle_from_explore) {
                // Écran de victoire (« On continue ») : refermé comme le ferait le joueur, sinon le jeu reste en combat.
                if (g.end_result == game::EndVictory) {
                    ue::Call c(game::battle_manager(), STR("OnVictoryScreenClosedFlow"));
                    if (c) c.invoke();
                    ++g.metrics["victory_screens_closed"];
                }
                resume_explore(done ? "battle ended" : "battle ended (end flow incomplete)");
                break;
            }
            finish(true, done ? "battle ended" : "battle ended (end flow incomplete)");
        }
        break;
    }
    case Phase::Explore:
        run_explore_step(dt);
        break;
    default:
        break;
    }
}

} // namespace e33::testrunner
