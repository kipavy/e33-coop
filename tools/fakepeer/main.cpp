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
#include <iterator>
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

uint64_t fnv64(const std::vector<uint8_t>& b)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t x : b) { h ^= x; h *= 1099511628211ull; }
    return h;
}

// Attente d'un compteur : "any", "N", ">N", ">=N", "<N", "<=N" (même syntaxe que le test runner).
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

double now_s()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

struct Args {
    std::string mode, connect, script, log, record, replay;
    std::string snapshot;      // hôte : instantané à envoyer quand l'enregistrement contient un SaveOffer (D12)
    std::string save_out;      // client : où écrire l'instantané reçu
    bool paced = false;        // hôte : respecter le rythme enregistré (exploration)
    std::string end_mode = "linger";   // hôte, enregistrement fini : "linger" (rester jusqu'au départ du client), "bye", "vanish"
    double end_after = 5;      // délai avant "bye" / "vanish" (s)
    std::string reject;        // hôte : refuse le client (Reject, version du mod) avec ce détail
    int version_reply = 0;     // hôte : répond à tout avec des paquets d'une autre version de protocole
    int stop_after_turn = 0;   // hôte : arrête le rejeu avant le tour suivant (départ de l'hôte en plein combat)
    bool resync_snapshot = false;   // hôte : renvoie l'instantané (--snapshot) à chaque ResyncRequest du client
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
    int invalid = 0;
    std::map<std::string, int> counts;
    std::unique_ptr<net::Link> lp;
    // Nouveau lien (nouvelle époque) : connexion initiale, puis reconnexion simulée (phase 5).
    auto make_link = [&] {
        lp = std::make_unique<net::Link>(*t, a.connect);
        lp->on_invalid = [&](const std::string& why) { ++invalid; log.write("invalid", JsonObj().add("why", why)); };
        lp->on_sent = [&](const proto::Message& m, bool) { rec.write("c2h", m); log.write("send", JsonObj().raw("msg", proto::to_json(m))); };
        lp->on_delivered = [&](const proto::Message& m) { rec.write("h2c", m); log.write("recv", JsonObj().raw("msg", proto::to_json(m))); };
        lp->on_protocol_mismatch = [&](uint16_t v) { ++counts["protocol_mismatches"]; log.write("protocol_mismatch", JsonObj().add("host_protocol", v)); };
    };
    make_link();

    proto::Hello hello;
    hello.game_build = static_cast<uint32_t>(script["game_build"].as_int(23765773));
    hello.mod_version = script["mod_version"].as_string("dev");
    for (size_t i = 0; i < script["gameplay_mods"].size(); ++i) hello.gameplay_mods.push_back(script["gameplay_mods"][i].as_string());
    hello.player_name = script["player_name"].as_string("FakeClient");
    hello.instance_id = 0xFA4E;
    // Session (phase 4) : "join" demande l'instantané de l'hôte ; "session" : le run ne s'arrête pas au premier
    // BattleEnd mais quand l'hôte se tait (jeu fermé), puis vérifie "expect" sur les compteurs ci-dessous.
    hello.join = script["join"].as_bool(false);
    const bool session = script["session"].as_bool(false);
    // "packet_protocol" : se présenter avec une autre version de protocole (paquets d'en-tête seul) : l'hôte doit
    // répondre avec la sienne (refus clair) sans créer de joueur.
    const int fake_proto = static_cast<int>(script["packet_protocol"].as_int(0));
    if (!fake_proto) lp->send(hello);
    // "disconnect" : {"at_turn":N, "mode":"bye"|"vanish", "silence_s":S, "reconnect":true, "join":true}
    // Au début du tour N (avant de jouer), le faux client quitte (Bye) ou se tait (plus aucun paquet) ; il revient
    // ensuite comme un nouveau processus (nouvelle époque, Hello avec "rejoindre").
    const json::Value& dc = script["disconnect"];
    double silent_until = -1, reconnect_at = -1;
    double ready_at = -1;
    struct BuildIn { proto::BuildOffer offer; std::vector<uint8_t> data; uint32_t got = 0; };
    std::optional<BuildIn> build_in;
    std::vector<uint8_t> last_build;
    size_t build_sends = 0;
    size_t econ_sends = 0;
    uint32_t ready_battle = 0;
    int stalled = 0;
    bool disconnected = false;
    struct Inbound { uint32_t xfer = 0, size = 0, got = 0; uint64_t hash = 0; std::string level; std::vector<uint8_t> data; };
    std::optional<Inbound> inbound;
    bool session_errors = false;
    const json::Value& tp = script["take_pilot"];
    std::optional<proto::PilotState> last_host_state;
    proto::PilotState pilot_base;
    double joined_at = -1, pilot_t0 = -1, next_pilot_send = 0;
    bool pilot_requested = false;
    double pause_off_at = -1;
    double narr_end_at = -1;
    uint32_t narr_seq = 0;
    proto::NarrativeKind narr_kind = proto::NarrativeKind::Cinematic;
    uint32_t pilot_seq = 0;

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
        // Déconnecté (silence simulé) : comme un processus mort, rien n'est lu ni envoyé.
        if (disconnected) {
            while (t->poll(d)) {}
            if (reconnect_at > 0 && now_s() >= reconnect_at) {
                reconnect_at = -1;
                disconnected = false;
                make_link();
                proto::Hello h2 = hello;
                h2.join = dc["join"].as_bool(true);
                h2.instance_id = 0xFA4F;
                lp->send(h2);
                ++counts["reconnects"];
                log.write("reconnect", JsonObj().add("join", h2.join));
            } else if (reconnect_at < 0 && silent_until > 0 && now_s() >= silent_until) {
                ended = true;                 // pas de retour prévu : fin du faux client
                ended_at = now_s() - 3.0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (fake_proto) {
            // Paquet d'en-tête seul d'une autre version, toutes les 0,5 s ; l'hôte répond avec sa version.
            static double next_probe = 0;
            if (now_s() >= next_probe) {
                next_probe = now_s() + 0.5;
                auto pkt = net::Link::version_packet(0xBEEF);
                pkt[4] = static_cast<uint8_t>(fake_proto); pkt[5] = static_cast<uint8_t>(fake_proto >> 8);
                t->send(a.connect, pkt.data(), pkt.size());
                ++counts["version_probes"];
            }
            while (t->poll(d)) {
                if (d.peer != a.connect) continue;
                const uint16_t pv = net::Link::packet_protocol(d.data.data(), d.data.size());
                ++counts[pv == proto::kProtocolVersion ? "version_replies" : "other_replies"];
                proto::Message ignored;
                lp->on_datagram(d.data.data(), d.data.size(), now_s());
                while (lp->pop(ignored)) ++counts["messages_while_mismatched"];
            }
            if (ended_at < 0 && (counts["version_replies"] >= 1 || counts["version_probes"] >= 240)) { ended = true; ended_at = now_s() - 2.0; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        while (t->poll(d)) if (d.peer == a.connect) lp->on_datagram(d.data.data(), d.data.size(), now_s());
        proto::Message m;
        while (lp->pop(m)) {
            if (auto* w = std::get_if<proto::Welcome>(&m)) {
                my_id = w->player_id;
                ++counts["welcomes"];
            } else if (auto* r = std::get_if<proto::Reject>(&m)) {
                log.write("rejected", JsonObj().add("detail", r->detail).add("reason", static_cast<int>(r->reason)));
                ++counts["rejects"];
                // "expect_reject" : refus attendu (test de version), avec ce texte dans le détail.
                const std::string want = script["expect_reject"].as_string("");
                rejected = want.empty() || r->detail.find(want) == std::string::npos;
                ended = true;
                ended_at = now_s() - 3.0;
            } else if (auto* fa = std::get_if<proto::ActionCommand>(&m); fa && fa->forced) {
                ++counts["forced_actions"];
            } else if (auto* bo = std::get_if<proto::BuildOffer>(&m)) {
                build_in = BuildIn{*bo, std::vector<uint8_t>(bo->size), 0};
                ++counts["build_offers"];
                if (bo->reason == "not yours") ++counts["build_rejections"];
            } else if (auto* bc = std::get_if<proto::BuildChunk>(&m)) {
                if (!build_in || bc->xfer != build_in->offer.xfer || bc->offset != build_in->got || bc->offset + bc->data.size() > build_in->offer.size) { session_errors = true; continue; }
                std::copy(bc->data.begin(), bc->data.end(), build_in->data.begin() + bc->offset);
                build_in->got += static_cast<uint32_t>(bc->data.size());
                if (build_in->got == build_in->offer.size) {
                    if (fnv64(build_in->data) != build_in->offer.hash) session_errors = true;
                    else { ++counts["builds_received_ok"]; last_build = build_in->data; }
                    build_in.reset();
                }
            } else if (auto* ed = std::get_if<proto::EconomyDelta>(&m)) {
                ++counts[ed->rejected ? "economy_rejections" : "economy_received"];
            } else if (std::get_if<proto::WorldHash>(&m)) {
                ++counts["world_hashes"];
            } else if (std::get_if<proto::JoinWait>(&m)) {
                ++counts["join_waits"];
            } else if (std::get_if<proto::Owners>(&m)) {
                ++counts["owner_updates"];
            } else if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
                owners = bs->owners;
                ++counts["battle_starts"];
                // Démarrage coordonné : le faux client est « dans » le combat (ou un peu plus tard : "ready_delay_s").
                ready_at = now_s() + script["ready_delay_s"].as_number(0);
                ready_battle = bs->battle_id;
                if (!bs->world_enemy.empty()) ++counts["world_battles"];
            } else if (auto* ts = std::get_if<proto::TurnStart>(&m)) {
                bool my_hero_turn = false;
                for (auto& o : owners) if (ts->kind == proto::ActorKind::Hero && o.spot == ts->index && o.player_id == my_id) my_hero_turn = true;
                if (dc.is_object() && !disconnected && counts["disconnects"] == 0 && ts->turn >= static_cast<uint32_t>(dc["at_turn"].as_int(3)) &&
                    (!dc["on_my_turn"].as_bool(false) || my_hero_turn)) {
                    const std::string mode = dc["mode"].as_string("vanish");
                    if (mode == "bye") {
                        lp->send(proto::Bye{"fake client leaves"});
                        for (int i = 0; i < 10; ++i) { lp->update(now_s() + 0.3 * i); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
                    }
                    disconnected = true;
                    ++counts["disconnects"];
                    silent_until = now_s() + dc["silence_s"].as_number(25);
                    if (dc["reconnect"].as_bool(false)) reconnect_at = silent_until;
                    log.write("disconnect", JsonObj().add("mode", mode).add("turn", ts->turn).add("silence_s", dc["silence_s"].as_number(25)));
                    break;
                }
                if (script["pause_in_battle"].is_object() && pause_off_at < 0 && ts->turn == static_cast<uint32_t>(script["pause_in_battle"]["turn"].as_int(3))) {
                    lp->send(proto::PauseState{my_id, true, true});
                    pause_off_at = now_s() + script["pause_in_battle"]["s"].as_number(3);
                    ++counts["pauses_sent"];
                }
                bool mine = false;
                for (auto& o : owners) if (ts->kind == proto::ActorKind::Hero && o.spot == ts->index && o.player_id == my_id) mine = true;
                if (!mine) continue;
                // "stall" : N premiers tours de ses héros sans jouer (minuteur de tour de l'hôte).
                if (stalled < script["stall"].as_int(0)) { ++stalled; ++counts["turns_stalled"]; continue; }
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
                    const json::Value* actp = &actions[candidates[action_index[cls]++ % candidates.size()]];
                    // Visée libre : même chronologie que le test runner (tirs à 1,5 s puis toutes les 1,2 s sur le premier
                    // ennemi, à 1 m du sol ; annulation 1,2 s après le dernier), puis l'action "then".
                    if ((*actp)["type"].as_string("") == "freeaim") {
                        const int shots = static_cast<int>((*actp)["shots"].as_int(1));
                        auto ev = [&](proto::FreeAimKind k, uint16_t seq, uint32_t t) {
                            proto::FreeAimEvent e;
                            e.battle_id = ts->battle_id; e.turn = ts->turn; e.spot = ts->index; e.kind = k; e.seq = seq; e.t_ms = t;
                            if (k == proto::FreeAimKind::Shot) { e.z = 100.0; e.target = {proto::ActorKind::Enemy, 0}; }
                            lp->send(e);
                        };
                        ev(proto::FreeAimKind::Start, 0, 0);
                        for (int i = 1; i <= shots; ++i) {
                            const uint32_t t = 1500 + static_cast<uint32_t>(i - 1) * 1200;
                            ev(proto::FreeAimKind::Fire, static_cast<uint16_t>(i), t);
                            ev(proto::FreeAimKind::Shot, static_cast<uint16_t>(i), t);
                        }
                        ev(proto::FreeAimKind::Cancel, 0, 1500 + static_cast<uint32_t>(shots) * 1200);
                        static const json::Value attack;   // "then" absent : attaque
                        actp = (*actp)["then"].is_object() ? &(*actp)["then"] : &attack;
                    }
                    const json::Value& act = *actp;
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
                lp->send(ac);
                ++turns_played;
            } else if (auto* dq = std::get_if<proto::DefenseRequest>(&m)) {
                proto::DefenseResult dr;
                dr.battle_id = dq->battle_id;
                dr.turn = dq->turn;
                dr.spot = dq->spot;
                dr.hit = dq->hit;
                dr.result = defense_of(defense[static_cast<size_t>(defense_index++) % defense.size()], dq->jumpable != 0);
                lp->send(dr);
            } else if (auto* sh = std::get_if<proto::StateHash>(&m)) {
                lp->send(*sh);   // le faux client n'a pas d'état propre : il renvoie le hash de l'hôte
            } else if (std::get_if<proto::BattleEnd>(&m)) {
                ++counts["battle_ends"];
                if (!session) {
                    ended = true;
                    ended_at = now_s();   // quelques secondes de plus pour les récompenses
                }
            } else if (auto* o = std::get_if<proto::SaveOffer>(&m)) {
                inbound = Inbound{o->xfer, o->size, 0, o->hash, o->level, std::vector<uint8_t>(o->size)};
                ++counts["save_offers"];
            } else if (auto* c = std::get_if<proto::SaveChunk>(&m)) {
                if (!inbound || c->xfer != inbound->xfer || c->offset != inbound->got || c->offset + c->data.size() > inbound->size) {
                    log.write("error", JsonObj().add("what", "bad save chunk").add("offset", c->offset));
                    session_errors = true;
                    continue;
                }
                std::copy(c->data.begin(), c->data.end(), inbound->data.begin() + c->offset);
                inbound->got += static_cast<uint32_t>(c->data.size());
                if (inbound->got == inbound->size) {
                    const uint64_t h = fnv64(inbound->data);
                    const bool gvas = inbound->size >= 4 && std::equal(inbound->data.begin(), inbound->data.begin() + 4, "GVAS");
                    log.write("snapshot", JsonObj().add("size", inbound->size).add("hash_ok", h == inbound->hash).add("gvas", gvas).add("level", inbound->level));
                    if (h != inbound->hash || !gvas) session_errors = true;
                    if (!a.save_out.empty()) {
                        std::ofstream f(a.save_out, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(inbound->data.data()), static_cast<std::streamsize>(inbound->data.size()));
                    }
                    ++counts["snapshots"];
                    lp->send(proto::JoinReady{inbound->xfer, inbound->level});
                    joined_at = now_s();
                    inbound.reset();
                }
            } else if (auto* ps = std::get_if<proto::PilotState>(&m)) {
                ++counts["pilot_states"];
                last_host_state = *ps;
            } else if (std::get_if<proto::MapChange>(&m)) {
                ++counts["map_changes"];
            } else if (std::get_if<proto::WorldInteract>(&m)) {
                ++counts["interactions"];
            } else if (std::get_if<proto::DialogueChoice>(&m)) {
                ++counts["choices"];
            } else if (auto* ns = std::get_if<proto::NarrativeState>(&m)) {
                ++counts[ns->active ? "narratives_started" : "narratives_finished"];
                // "narrative_echo" : le faux client « regarde » la même cinématique et la finit hold_s s après l'hôte (barrière).
                if (script["narrative_echo"].is_object() && ns->player != my_id) {
                    if (ns->active) { lp->send(proto::NarrativeState{my_id, ns->kind, true, ++narr_seq}); narr_kind = ns->kind; ++counts["narratives_echoed"]; }
                    else narr_end_at = now_s() + script["narrative_echo"]["hold_s"].as_number(10);
                }
            } else if (std::get_if<proto::PauseState>(&m)) {
                ++counts["pauses"];
            } else if (auto* pc = std::get_if<proto::PilotChange>(&m)) {
                ++counts["pilot_changes"];
                if (pc->pilot == my_id && my_id != 0xFF && last_host_state && pilot_t0 < 0) { pilot_t0 = now_s(); pilot_base = *last_host_state; }
                if (pc->pilot != my_id) pilot_t0 = -1;
            } else if (auto* bye = std::get_if<proto::Bye>(&m)) {
                log.write("bye", JsonObj().add("reason", bye->reason));
                if (session) { ended = true; ended_at = now_s() - 2.0; }
            } else if (auto* p = std::get_if<proto::Ping>(&m)) {
                lp->send(proto::Pong{p->t_us}, false);
            }
        }
        if (disconnected) continue;   // vient de se déconnecter : plus rien n'est envoyé
        // "build_send" : [{"after_ok":1,"names":["Lune"]}, …] : renvoie le dernier état reçu comme proposition de ce joueur.
        if (script["build_send"].is_array() && build_sends < script["build_send"].size() && !last_build.empty() &&
            counts["builds_received_ok"] >= script["build_send"][build_sends]["after_ok"].as_int(1)) {
            const json::Value& bs = script["build_send"][build_sends++];
            proto::BuildOffer o;
            o.xfer = 0xF0000000u + static_cast<uint32_t>(build_sends);
            o.origin = my_id;
            o.size = static_cast<uint32_t>(last_build.size());
            o.hash = fnv64(last_build);
            o.reason = "fake client";
            for (size_t i = 0; i < bs["names"].size(); ++i) o.names.push_back({bs["names"][i].as_string(), 0});
            lp->send(o);
            for (uint32_t off = 0; off < last_build.size(); off += proto::kMaxChunk) {
                proto::BuildChunk c;
                c.xfer = o.xfer;
                c.offset = off;
                c.data.assign(last_build.begin() + off, last_build.begin() + off + std::min<size_t>(proto::kMaxChunk, last_build.size() - off));
                lp->send(c);
            }
            ++counts["builds_sent"];
        }
        // "economy_send" : [{"after_s":10,"parts":{"gold":-100}}, …] : dépenses de ce joueur (après l'entrée dans la partie).
        if (script["economy_send"].is_array() && joined_at >= 0 && econ_sends < script["economy_send"].size() &&
            now_s() - joined_at > script["economy_send"][econ_sends]["after_s"].as_number(10)) {
            const json::Value& es = script["economy_send"][econ_sends++];
            proto::EconomyDelta d;
            d.seq = static_cast<uint32_t>(econ_sends);
            d.origin = my_id;
            for (const auto& [k, v] : es["parts"].obj) d.parts.push_back({k, v.as_int()});
            lp->send(d);
            ++counts["economy_sent"];
        }
        if (ready_at >= 0 && now_s() >= ready_at) { lp->send(proto::BattleReady{ready_battle, my_id}); ready_at = -1; ++counts["ready_sent"]; }
        // "resync_after_s" : une fois entré, demande un nouvel instantané (désync d'exploration simulée).
        if (joined_at >= 0 && counts["resyncs_sent"] == 0 &&
            ((script["resync_after_s"].is_number() && now_s() - joined_at > script["resync_after_s"].as_number(10)) ||
             (script["resync_after_hashes"].is_number() && counts["world_hashes"] >= script["resync_after_hashes"].as_int(2)))) {
            lp->send(proto::ResyncRequest{"fake client test"});
            ++counts["resyncs_sent"];
        }
        if (narr_end_at > 0 && now_s() >= narr_end_at) { lp->send(proto::NarrativeState{my_id, narr_kind, false, ++narr_seq}); narr_end_at = 0; ++counts["narratives_echoed"]; }
        if (pause_off_at > 0 && now_s() >= pause_off_at) { lp->send(proto::PauseState{my_id, false, true}); pause_off_at = 0; ++counts["pauses_sent"]; }
        // "take_pilot" : après l'entrée, le faux client prend la main et marche en ligne droite depuis la dernière position
        // du pilote ({"after_s":8,"dir":[1,0],"speed":250,"s":5}), puis reste immobile.
        if (tp.is_object() && joined_at >= 0) {
            if (!pilot_requested && now_s() - joined_at > tp["after_s"].as_number(8) && last_host_state) {
                lp->send(proto::PilotRequest{my_id});
                pilot_requested = true;
                ++counts["pilot_requests"];
            }
            if (pilot_t0 >= 0 && now_s() >= next_pilot_send) {
                next_pilot_send = now_s() + 0.05;
                const double el = std::min(now_s() - pilot_t0, tp["s"].as_number(5));
                const double sp = tp["speed"].as_number(250), dx = tp["dir"][0].as_number(1), dy = tp["dir"][1].as_number(0);
                const bool moving = now_s() - pilot_t0 < tp["s"].as_number(5);
                proto::PilotState st = pilot_base;
                st.pilot = my_id;
                st.seq = ++pilot_seq;
                st.x = pilot_base.x + dx * sp * el;
                st.y = pilot_base.y + dy * sp * el;
                st.vx = moving ? dx * sp : 0; st.vy = moving ? dy * sp : 0; st.vz = 0;
                lp->send(st, false);
                ++counts["pilot_states_sent"];
            }
        }
        lp->update(now_s());
        // L'hôte ne répond pas pendant ses chargements de niveau (30 à 60 s) : marge large en session.
        if (lp->timed_out(now_s(), session ? 150.0 : 20.0)) { log.write("error", JsonObj().add("what", "host timed out")); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // vider les acquittements
    for (int i = 0; i < 20; ++i) { lp->update(now_s() + i); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    bool expect_ok = true;
    std::string counts_json = "{";
    for (auto& [k, v] : counts) { if (counts_json.size() > 1) counts_json += ","; counts_json += "\"" + json_escape(k) + "\":" + std::to_string(v); }
    counts_json += "}";
    if (script["expect"].is_object())
        for (const auto& [k, v] : script["expect"].obj) {
            const std::string want = v.is_string() ? v.as_string() : std::to_string(v.as_int());
            if (!satisfies(counts[k], want)) { expect_ok = false; log.write("expect_failed", JsonObj().add("name", k).add("want", want).add("got", counts[k])); }
        }
    // Messages invalides attendus en test de version (l'hôte répond dans sa version).
    if (fake_proto) invalid = 0;
    const bool ok = ended && !rejected && invalid == 0 && !session_errors && expect_ok;
    log.write("done", JsonObj().add("ok", ok).raw("counts", counts_json).add("turns_played", turns_played).add("defenses", defense_index).add("invalid", invalid).add("rejected", rejected));
    return ok ? 0 : 1;
}

// --- Faux hôte (rejeu) -------------------------------------------------------------------------

struct RecLine { std::string dir; proto::Message msg; double t = 0; };

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
            lines.push_back({v["dir"].as_string(), std::move(*m), v["t"].as_number(0)});
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
    double pace_rec = 0, pace_now = -1;   // --paced : dernier point de synchro (temps enregistré, temps réel)

    auto key =[](uint32_t b, uint32_t tn) { return (static_cast<uint64_t>(b) << 32) | tn; };

    bool client_left = false;
    int resyncs = 0;
    // Fin de l'enregistrement : "linger" reste en ligne jusqu'au départ du client (Bye de fin de test), "bye" / "vanish"
    // simulent le départ de l'hôte (message de fin, ou silence complet) end_after secondes plus tard (phase 5).
    auto running = [&] {
        if (now_s() >= deadline) return false;
        if (finished_at < 0) return true;
        if (a.end_mode == "linger") return !client_left;
        return now_s() < finished_at + a.end_after;
    };
    while (running()) {
        net::Datagram d;
        while (a.version_reply && t->poll(d)) {
            // Hôte d'une autre version : chaque paquet reçoit un en-tête de cette version (le client doit refuser).
            auto pkt = net::Link::version_packet(0xBEEF);
            pkt[4] = static_cast<uint8_t>(a.version_reply); pkt[5] = static_cast<uint8_t>(a.version_reply >> 8);
            t->send(d.peer, pkt.data(), pkt.size());
            if (finished_at < 0) { finished_at = now_s(); log.write("version_reply", JsonObj().add("protocol", a.version_reply)); }
        }
        while (t->poll(d)) {
            if (!link) {
                link = std::make_unique<net::Link>(*t, d.peer);
                link->on_invalid = [&](const std::string& why) { ++invalid; log.write("invalid", JsonObj().add("why", why)); };
                link->on_sent = [&](const proto::Message& m, bool) { rec.write("h2c", m); log.write("send", JsonObj().raw("msg", proto::to_json(m))); };
                link->on_delivered = [&](const proto::Message& m) { rec.write("c2h", m); log.write("recv", JsonObj().raw("msg", proto::to_json(m))); };
                log.write("client", JsonObj().add("peer", d.peer));
                pace_now = now_s();
                pace_rec = cursor < lines.size() ? lines[cursor].t : 0;
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
                if (std::get_if<proto::Bye>(&m)) { client_left = true; log.write("client_bye", JsonObj()); continue; }
                if (a.resync_snapshot && std::get_if<proto::ResyncRequest>(&m)) {
                    std::ifstream sf(a.snapshot, std::ios::binary);
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
                    const uint32_t xfer = 1000 + static_cast<uint32_t>(++resyncs);
                    link->send(proto::SaveOffer{xfer, static_cast<uint32_t>(bytes.size()), fnv64(bytes), "resync"});
                    for (uint32_t off = 0; off < bytes.size(); off += proto::kMaxChunk) {
                        proto::SaveChunk c;
                        c.xfer = xfer;
                        c.offset = off;
                        c.data.assign(bytes.begin() + off, bytes.begin() + off + std::min<size_t>(proto::kMaxChunk, bytes.size() - off));
                        link->send(c);
                    }
                    log.write("resync_snapshot_sent", JsonObj().add("size", static_cast<int64_t>(bytes.size())));
                    continue;
                }
                if (!a.reject.empty() && std::get_if<proto::Hello>(&m)) {
                    link->send(proto::Reject{proto::RejectReason::ModVersionMismatch, a.reject});
                    log.write("rejecting", JsonObj().add("detail", a.reject));
                    cursor = lines.size();
                    continue;
                }
                client_msgs.push_back(std::move(m));
            }
            // Avancer dans l'enregistrement : envoyer les h2c ; pour un c2h, attendre un message client du même type.
            while (cursor < lines.size()) {
                const auto& l = lines[cursor];
                if (a.stop_after_turn > 0) {
                    const auto* ts = std::get_if<proto::TurnStart>(&l.msg);
                    if (ts && ts->turn > static_cast<uint32_t>(a.stop_after_turn)) { log.write("replay_stopped", JsonObj().add("turn", ts->turn)); cursor = lines.size(); break; }
                }
                if (l.dir == "h2c") {
                    // Rythme enregistré (exploration) : chaque message part au même délai après le dernier point de synchro.
                    if (a.paced && now_s() - pace_now < l.t - pace_rec) break;
                    if (auto* sh = std::get_if<proto::StateHash>(&l.msg)) host_hash[key(sh->battle_id, sh->turn)] = sh->hash;
                    link->send(l.msg);
                    // Instantané : les octets ne sont pas dans l'enregistrement, ils viennent du fichier local (même hash) :
                    // --snapshot, ou <nom>_<xfer>.sav quand la session en contient plusieurs (resynchro).
                    if (auto* bo = std::get_if<proto::BuildOffer>(&l.msg)) {
                        std::string stem = a.snapshot;
                        if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".sav") stem = stem.substr(0, stem.size() - 4);
                        std::ifstream bf(stem + "_build-" + std::to_string(bo->xfer) + ".sav", std::ios::binary);
                        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
                        if (bytes.size() != bo->size || fnv64(bytes) != bo->hash) {
                            log.write("error", JsonObj().add("what", "build file does not match the recorded BuildOffer").add("xfer", bo->xfer));
                            return 1;
                        }
                        for (uint32_t off = 0; off < bytes.size(); off += proto::kMaxChunk) {
                            proto::BuildChunk c;
                            c.xfer = bo->xfer;
                            c.offset = off;
                            c.data.assign(bytes.begin() + off, bytes.begin() + off + std::min<size_t>(proto::kMaxChunk, bytes.size() - off));
                            link->send(c);
                        }
                    }
                    if (auto* so = std::get_if<proto::SaveOffer>(&l.msg)) {
                        auto read_file = [](const std::string& p) { std::ifstream sf(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>()); };
                        std::vector<uint8_t> bytes = read_file(a.snapshot);
                        if (bytes.size() != so->size || fnv64(bytes) != so->hash) {
                            std::string alt = a.snapshot;
                            if (alt.size() > 4 && alt.substr(alt.size() - 4) == ".sav") alt = alt.substr(0, alt.size() - 4);
                            bytes = read_file(alt + "_" + std::to_string(so->xfer) + ".sav");
                        }
                        if (bytes.size() != so->size || fnv64(bytes) != so->hash) {
                            log.write("error", JsonObj().add("what", "snapshot file does not match the recorded SaveOffer").add("file", a.snapshot)
                                                   .add("size", static_cast<int64_t>(bytes.size())).add("want_size", so->size));
                            return 1;
                        }
                        for (uint32_t off = 0; off < bytes.size(); off += proto::kMaxChunk) {
                            proto::SaveChunk c;
                            c.xfer = so->xfer;
                            c.offset = off;
                            c.data.assign(bytes.begin() + off, bytes.begin() + off + std::min<size_t>(proto::kMaxChunk, bytes.size() - off));
                            link->send(c);
                        }
                        log.write("snapshot_sent", JsonObj().add("size", static_cast<int64_t>(bytes.size())));
                    }
                    ++cursor;
                } else if (std::holds_alternative<proto::NarrativeState>(l.msg) || std::holds_alternative<proto::PauseState>(l.msg) ||
                           std::holds_alternative<proto::BattleReady>(l.msg) || std::holds_alternative<proto::Pong>(l.msg) ||
                           std::holds_alternative<proto::BuildOffer>(l.msg) || std::holds_alternative<proto::BuildChunk>(l.msg) || std::holds_alternative<proto::EconomyDelta>(l.msg)) {
                    // Messages informatifs du client enregistré (écho de cinématique, pause…) : le client rejoué peut ne pas
                    // les envoyer (il n'a pas vu la cinématique de l'hôte) ; ils ne rythment pas le rejeu.
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
                    pace_rec = l.t;
                    pace_now = now_s();
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
    if (link && finished_at >= 0 && a.end_mode == "bye") {
        link->send(proto::Bye{"host quit"});
        log.write("host_bye", JsonObj());
    }
    if (a.end_mode != "vanish")
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
        else if (k == "--snapshot") a.snapshot = v;
        else if (k == "--save-out") a.save_out = v;
        else if (k == "--paced") a.paced = v == "1";
        else if (k == "--end") a.end_mode = v;
        else if (k == "--end-after") a.end_after = std::atof(v.c_str());
        else if (k == "--reject") a.reject = v;
        else if (k == "--version-reply") a.version_reply = std::atoi(v.c_str());
        else if (k == "--stop-after-turn") a.stop_after_turn = std::atoi(v.c_str());
        else if (k == "--resync-snapshot") a.resync_snapshot = v == "1";
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
