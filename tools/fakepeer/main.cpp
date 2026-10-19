// fakepeer — faux pair hors jeu qui parle le protocole E33Coop.
//
//   fakepeer client --connect 127.0.0.1:7777 [--script s.json] [--log f.jsonl] [--record r.jsonl] [--timeout 900]
//     Faux client scripté face au vrai jeu hôte : se présente (Hello), joue les tours de ses héros
//     (ActionCommand) et répond aux coups (DefenseResult) selon le script ; renvoie les hash d'état reçus.
//
//   fakepeer host --port 7777 --replay r.jsonl [--log f.jsonl] [--timeout 900]
//     Faux hôte face au vrai jeu client : rejoue les messages hôte->client d'un enregistrement, en attendant
//     les messages du client aux mêmes endroits ; compare les hash d'état du client à ceux de l'hôte enregistré.
//
// Code de sortie : 0 = scénario déroulé sans erreur ni désync ; 1 = échec ; 2 = usage.

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "jsonlog.hpp"
#include "link.hpp"
#include "messages.hpp"
#include "recorder.hpp"
#include "minijson.hpp"
#include "transport.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <timeapi.h>

using namespace e33;

namespace {

double now_s()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

struct Args {
    std::string mode, connect, script, log, record, replay;
    int port = 7777;
    double timeout = 900;
};

using net::Recorder;
using net::from_hex;

json::Value load_json(const std::string& path, std::string& err)
{
    json::Value v;
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return v; }
    std::stringstream ss;
    ss << f.rdbuf();
    json::parse(ss.str(), v, err);
    return v;
}

// --- Faux client ---------------------------------------------------------------------------

// Résultat de QTE scripté : bit0 réussi, bit1 parfait.
uint8_t combo_bits(const std::string& c) { return c == "perfect" ? 3 : c == "success" ? 1 : 0; }

proto::Defense defense_of(const std::string& d, bool jumpable)
{
    if (jumpable) return d == "fail" ? proto::Defense::Fail : proto::Defense::Jump;
    if (d == "parry") return proto::Defense::Parry;
    if (d == "dodge") return proto::Defense::Dodge;
    if (d == "perfect_dodge") return proto::Defense::PerfectDodge;
    return proto::Defense::Fail;
}

int run_client(const Args& a, JsonLog& log, Recorder& rec)
{
    std::string err;
    json::Value script;
    if (!a.script.empty()) { script = load_json(a.script, err); if (!err.empty()) { log.write("error", JsonObj().add("what", err)); return 1; } }
    auto t = net::make_udp(0, err);
    if (!t) { log.write("error", JsonObj().add("what", err)); return 1; }
    net::Link link(*t, a.connect);
    int invalid = 0;
    link.on_invalid = [&](const std::string& why) { ++invalid; log.write("invalid", JsonObj().add("why", why)); };
    link.on_sent = [&](const proto::Message& m, bool) { rec.write("c2h", m); log.write("send", JsonObj().raw("msg", proto::to_json(m))); };
    link.on_delivered = [&](const proto::Message& m) { rec.write("h2c", m); log.write("recv", JsonObj().raw("msg", proto::to_json(m))); };

    proto::Hello hello;
    hello.game_build = static_cast<uint32_t>(script["game_build"].as_int(23765773));
    hello.mod_version = script["mod_version"].as_string("dev");
    for (size_t i = 0; i < script["gameplay_mods"].size(); ++i) hello.gameplay_mods.push_back(script["gameplay_mods"][i].as_string());
    hello.player_name = script["player_name"].as_string("FakeClient");
    hello.instance_id = 0xFA4E;
    link.send(hello);

    std::vector<std::string> defense;
    for (size_t i = 0; i < script["defense"].size(); ++i) defense.push_back(script["defense"][i].as_string("fail"));
    if (defense.empty()) defense = {"parry"};
    // Actions : même format que les scénarios du test runner ({hero, type, skill|item, target, combo}).
    const json::Value& actions = script["actions"];
    std::map<std::string, size_t> action_index;

    uint8_t my_id = 0xFF;
    std::vector<proto::SpotOwner> owners;
    int defense_index = 0, turns_played = 0;
    bool ended = false, rejected = false;
    double ended_at = -1;
    const double deadline = now_s() + a.timeout;
    auto spot_of_hero = [&](const std::string& name) -> int {
        for (auto& o : owners) if (o.character.find(name) != std::string::npos) return o.spot;
        return -1;
    };
    auto hero_of_spot = [&](uint8_t spot) -> std::string {
        for (auto& o : owners) if (o.spot == spot) return o.character;
        return {};
    };
    while ((ended_at < 0 || now_s() < ended_at + 3.0) && now_s() < deadline) {
        net::Datagram d;
        while (t->poll(d)) if (d.peer == a.connect) link.on_datagram(d.data.data(), d.data.size(), now_s());
        proto::Message m;
        while (link.pop(m)) {
            if (auto* w = std::get_if<proto::Welcome>(&m)) {
                my_id = w->player_id;
            } else if (auto* r = std::get_if<proto::Reject>(&m)) {
                log.write("rejected", JsonObj().add("detail", r->detail));
                rejected = ended = true;
                ended_at = now_s() - 3.0;
            } else if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
                owners = bs->owners;
            } else if (auto* ts = std::get_if<proto::TurnStart>(&m)) {
                bool mine = false;
                for (auto& o : owners) if (ts->kind == proto::ActorKind::Hero && o.spot == ts->index && o.player_id == my_id) mine = true;
                if (!mine) continue;
                const std::string cls = hero_of_spot(ts->index);
                // Prochaine action scriptée pour ce héros ("hero" absent = tout héros).
                std::vector<size_t> candidates;
                for (size_t i = 0; i < actions.size(); ++i) {
                    const std::string h = actions[i]["hero"].as_string("");
                    if (h.empty() || cls.find(h) != std::string::npos) candidates.push_back(i);
                }
                proto::ActionCommand ac;
                ac.battle_id = ts->battle_id;
                ac.turn = ts->turn;
                ac.actor_spot = ts->index;
                ac.action_type = 1;
                ac.targets = {{proto::ActorKind::Enemy, proto::kAutoSpot}};
                if (!candidates.empty()) {
                    const json::Value& act = actions[candidates[action_index[cls]++ % candidates.size()]];
                    const std::string type = act["type"].as_string("attack");
                    ac.action_type = static_cast<uint8_t>(type == "skill" ? 5 : type == "item" ? 3 : type == "skip" ? 6 : type == "flee" ? 4 : 1);
                    ac.secondary = act["skill"].is_string() ? act["skill"].as_string() : act["item"].as_string();
                    const std::string target = act["target"].as_string("auto");
                    if (target.rfind("hero:", 0) == 0) {
                        const int sp = spot_of_hero(target.substr(5));
                        if (sp >= 0) ac.targets = {{proto::ActorKind::Hero, static_cast<uint8_t>(sp)}};
                    }
                    const json::Value& combo = act["combo"];
                    for (size_t k = 0; combo.size() > 0 && k < 16; ++k) ac.combo.push_back(combo_bits(combo[k % combo.size()].as_string("success")));
                }
                if (ac.combo.empty()) ac.combo.assign(16, 1);   // par défaut : QTE réussis (comme DEBUG_ComboAutoSuccessCount)
                link.send(ac);
                ++turns_played;
            } else if (auto* dq = std::get_if<proto::DefenseRequest>(&m)) {
                proto::DefenseResult dr;
                dr.battle_id = dq->battle_id;
                dr.turn = dq->turn;
                dr.spot = dq->spot;
                dr.hit = dq->hit;
                dr.result = defense_of(defense[static_cast<size_t>(defense_index++) % defense.size()], dq->jumpable != 0);
                link.send(dr);
            } else if (auto* sh = std::get_if<proto::StateHash>(&m)) {
                link.send(*sh);   // le faux client n'a pas d'état propre : il renvoie le hash de l'hôte
            } else if (std::get_if<proto::BattleEnd>(&m)) {
                ended = true;
                ended_at = now_s();   // quelques secondes de plus pour les récompenses
            } else if (auto* p = std::get_if<proto::Ping>(&m)) {
                link.send(proto::Pong{p->t_us}, false);
            }
        }
        link.update(now_s());
        if (link.timed_out(now_s(), 20.0)) { log.write("error", JsonObj().add("what", "host timed out")); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // vider les acquittements
    for (int i = 0; i < 20; ++i) { link.update(now_s() + i); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const bool ok = ended && !rejected && invalid == 0;
    log.write("done", JsonObj().add("ok", ok).add("turns_played", turns_played).add("defenses", defense_index).add("invalid", invalid).add("rejected", rejected));
    return ok ? 0 : 1;
}

// --- Faux hôte (rejeu) -------------------------------------------------------------------------

struct RecLine { std::string dir; proto::Message msg; };

// Champs qui doivent être identiques entre un message client enregistré et celui du client rejoué
// (sinon l'hôte enregistré a appliqué autre chose : le rejeu ne compare plus la même partie).
std::string c2h_key(const proto::Message& m)
{
    if (auto* ac = std::get_if<proto::ActionCommand>(&m))
        return "action t" + std::to_string(ac->turn) + " s" + std::to_string(ac->actor_spot) + " a" + std::to_string(ac->action_type) + " " + ac->secondary;
    if (auto* dr = std::get_if<proto::DefenseResult>(&m))
        return "defense t" + std::to_string(dr->turn) + " s" + std::to_string(dr->spot) + " h" + std::to_string(dr->hit) + " r" + std::to_string(static_cast<int>(dr->result));
    if (auto* cr = std::get_if<proto::ComboResult>(&m))
        return "combo t" + std::to_string(cr->turn) + " s" + std::to_string(cr->spot) + " q" + std::to_string(cr->seq) + " " + std::to_string(cr->success) + std::to_string(cr->perfect);
    return {};
}

int run_host(const Args& a, JsonLog& log, Recorder& rec)
{
    std::string err;
    std::vector<RecLine> lines;
    {
        std::ifstream f(a.replay, std::ios::binary);
        if (!f) { log.write("error", JsonObj().add("what", "cannot open replay " + a.replay)); return 1; }
        std::string line;
        while (std::getline(f, line)) {
            json::Value v;
            if (line.empty() || !json::parse(line, v, err)) continue;
            auto bytes = from_hex(v["hex"].as_string());
            wire::Reader r(bytes.data(), bytes.size());
            auto m = proto::decode(r, err);
            if (!m) { log.write("error", JsonObj().add("what", "bad recording line: " + err)); return 1; }
            lines.push_back({v["dir"].as_string(), std::move(*m)});
        }
    }
    auto t = net::make_udp(static_cast<uint16_t>(a.port), err);
    if (!t) { log.write("error", JsonObj().add("what", err)); return 1; }
    log.write("listening", JsonObj().add("port", a.port).add("lines", static_cast<int64_t>(lines.size())));

    std::unique_ptr<net::Link> link;
    int invalid = 0, desyncs = 0, hashes = 0, mismatches = 0;
    std::map<uint64_t, uint64_t> host_hash;   // (battle<<32|turn) -> hash enregistré
    size_t cursor = 0;
    std::vector<proto::Message> client_msgs;
    const double deadline = now_s() + a.timeout;
    double finished_at = -1;

    auto key = [](uint32_t b, uint32_t tn) { return (static_cast<uint64_t>(b) << 32) | tn; };

    while ((finished_at < 0 || now_s() < finished_at + 3.0) && now_s() < deadline) {
        net::Datagram d;
        while (t->poll(d)) {
            if (!link) {
                link = std::make_unique<net::Link>(*t, d.peer);
                link->on_invalid = [&](const std::string& why) { ++invalid; log.write("invalid", JsonObj().add("why", why)); };
                link->on_sent = [&](const proto::Message& m, bool) { rec.write("h2c", m); log.write("send", JsonObj().raw("msg", proto::to_json(m))); };
                link->on_delivered = [&](const proto::Message& m) { rec.write("c2h", m); log.write("recv", JsonObj().raw("msg", proto::to_json(m))); };
                log.write("client", JsonObj().add("peer", d.peer));
            }
            if (d.peer == link->peer()) link->on_datagram(d.data.data(), d.data.size(), now_s());
        }
        if (link) {
            proto::Message m;
            while (link->pop(m)) {
                if (auto* sh = std::get_if<proto::StateHash>(&m)) {
                    ++hashes;
                    auto it = host_hash.find(key(sh->battle_id, sh->turn));
                    if (it != host_hash.end() && it->second != sh->hash) {
                        ++desyncs;
                        log.write("desync", JsonObj().add("battle", sh->battle_id).add("turn", sh->turn));
                    }
                }
                if (auto* p = std::get_if<proto::Ping>(&m)) { link->send(proto::Pong{p->t_us}, false); continue; }
                client_msgs.push_back(std::move(m));
            }
            // Avancer dans l'enregistrement : envoyer les h2c ; pour un c2h, attendre un message client du même type.
            while (cursor < lines.size()) {
                const auto& l = lines[cursor];
                if (l.dir == "h2c") {
                    if (auto* sh = std::get_if<proto::StateHash>(&l.msg)) host_hash[key(sh->battle_id, sh->turn)] = sh->hash;
                    link->send(l.msg);
                    ++cursor;
                } else {
                    const auto want = proto::type_of(l.msg);
                    auto it = std::find_if(client_msgs.begin(), client_msgs.end(), [&](const proto::Message& c) { return proto::type_of(c) == want; });
                    if (it == client_msgs.end()) break;   // barrière
                    const std::string k_rec = c2h_key(l.msg), k_live = c2h_key(*it);
                    if (k_rec != k_live) {
                        ++mismatches;
                        log.write("c2h_mismatch", JsonObj().add("recorded", k_rec).add("live", k_live));
                    }
                    client_msgs.erase(it);
                    ++cursor;
                }
            }
            if (cursor >= lines.size() && finished_at < 0) finished_at = now_s();
            link->update(now_s());
            // Le client ne répond pas pendant un chargement de niveau (thread de jeu bloqué) : marge large.
            if (link->timed_out(now_s(), 120.0)) { log.write("error", JsonObj().add("what", "client timed out")); break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int i = 0; i < 40 && link; ++i) { link->update(now_s() + i); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const bool ok = finished_at >= 0 && invalid == 0 && desyncs == 0 && mismatches == 0;
    log.write("done", JsonObj().add("ok", ok).add("replayed", static_cast<int64_t>(cursor)).add("of", static_cast<int64_t>(lines.size())).add("hashes", hashes)
                          .add("desyncs", desyncs).add("c2h_mismatches", mismatches).add("invalid", invalid));
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    timeBeginPeriod(1);   // sommeils de 1 ms réels (sinon ~15 ms)
    Args a;
    if (argc < 2) { std::fprintf(stderr, "usage: fakepeer client|host [options]\n"); return 2; }
    a.mode = argv[1];
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--connect") a.connect = v;
        else if (k == "--port") a.port = std::atoi(v.c_str());
        else if (k == "--script") a.script = v;
        else if (k == "--log") a.log = v;
        else if (k == "--record") a.record = v;
        else if (k == "--replay") a.replay = v;
        else if (k == "--timeout") a.timeout = std::atof(v.c_str());
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }
    JsonLog log;
    log.open(a.log.empty() ? std::string("fakepeer.jsonl") : a.log, "fakepeer", a.mode);
    Recorder rec;
    rec.open(a.record);
    int code = 2;
    if (a.mode == "client" && !a.connect.empty()) code = run_client(a, log, rec);
    else if (a.mode == "host" && !a.replay.empty()) code = run_host(a, log, rec);
    else std::fprintf(stderr, "usage: fakepeer client --connect ip:port | host --port p --replay file\n");
    std::printf("fakepeer %s exit %d\n", a.mode.c_str(), code);
    return code;
}
