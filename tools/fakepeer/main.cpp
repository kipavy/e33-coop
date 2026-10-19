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
    const int action_type = static_cast<int>(script["action_type"].as_int(1));
    const int combo_hits = static_cast<int>(script["combo_hits"].as_int(10));

    uint8_t my_id = 0xFF;
    std::vector<uint8_t> my_spots;
    int defense_index = 0, turns_played = 0;
    bool ended = false, rejected = false;
    const double deadline = now_s() + a.timeout;
    while (!ended && now_s() < deadline) {
        net::Datagram d;
        while (t->poll(d)) if (d.peer == a.connect) link.on_datagram(d.data.data(), d.data.size(), now_s());
        proto::Message m;
        while (link.pop(m)) {
            if (auto* w = std::get_if<proto::Welcome>(&m)) my_id = w->player_id;
            else if (auto* r = std::get_if<proto::Reject>(&m)) { log.write("rejected", JsonObj().add("detail", r->detail)); rejected = true; ended = true; }
            else if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
                my_spots.clear();
                for (auto& o : bs->owners) if (o.player_id == my_id) my_spots.push_back(o.spot);
            } else if (auto* ts = std::get_if<proto::TurnStart>(&m)) {
                const bool mine = ts->kind == proto::ActorKind::Hero && std::find(my_spots.begin(), my_spots.end(), ts->index) != my_spots.end();
                if (mine) {
                    proto::ActionCommand ac;
                    ac.battle_id = ts->battle_id; ac.turn = ts->turn; ac.actor_spot = ts->index;
                    ac.action_type = static_cast<uint8_t>(action_type); ac.targets = {0};
                    ac.combo_hits = static_cast<uint8_t>(combo_hits); ac.combo_perfect = 0;
                    link.send(ac);
                    ++turns_played;
                }
            } else if (auto* dq = std::get_if<proto::DefenseRequest>(&m)) {
                proto::DefenseResult dr;
                dr.battle_id = dq->battle_id; dr.turn = dq->turn; dr.hit = dq->hit;
                const std::string& dec = defense[static_cast<size_t>(defense_index++) % defense.size()];
                dr.result = dec == "parry" ? proto::Defense::Parry : dec == "dodge" ? proto::Defense::Dodge : proto::Defense::Fail;
                link.send(dr);
            } else if (auto* sh = std::get_if<proto::StateHash>(&m)) {
                link.send(*sh);   // le faux client n'a pas d'état propre : il renvoie le hash de l'hôte
            } else if (std::get_if<proto::BattleEnd>(&m)) {
                ended = true;
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
    int invalid = 0, desyncs = 0, hashes = 0;
    std::map<uint64_t, uint64_t> host_hash;   // (battle<<32|turn) -> hash enregistré
    size_t cursor = 0;
    std::vector<proto::Message> client_msgs;
    const double deadline = now_s() + a.timeout;
    bool finished = false;

    auto key = [](uint32_t b, uint32_t tn) { return (static_cast<uint64_t>(b) << 32) | tn; };

    while (!finished && now_s() < deadline) {
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
                    if (std::get_if<proto::BattleEnd>(&l.msg)) finished = true;
                    ++cursor;
                } else {
                    const auto want = proto::type_of(l.msg);
                    auto it = std::find_if(client_msgs.begin(), client_msgs.end(), [&](const proto::Message& c) { return proto::type_of(c) == want; });
                    if (it == client_msgs.end()) break;   // barrière
                    client_msgs.erase(it);
                    ++cursor;
                }
            }
            link->update(now_s());
            // Le client ne répond pas pendant un chargement de niveau (thread de jeu bloqué) : marge large.
            if (link->timed_out(now_s(), 120.0)) { log.write("error", JsonObj().add("what", "client timed out")); break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int i = 0; i < 40 && link; ++i) { link->update(now_s() + i); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const bool ok = finished && invalid == 0 && desyncs == 0;
    log.write("done", JsonObj().add("ok", ok).add("replayed", static_cast<int64_t>(cursor)).add("of", static_cast<int64_t>(lines.size())).add("hashes", hashes).add("desyncs", desyncs).add("invalid", invalid));
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
