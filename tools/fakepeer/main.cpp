// fakepeer — faux pair hors jeu qui parle le protocole E33Coop.
//
//   fakepeer client --connect 127.0.0.1:7777 [--script s.json] [--log f.jsonl] [--record r.jsonl] [--timeout 900]
//     Faux client scripté face au vrai jeu hôte : se présente (Hello), joue les tours de ses héros
//     (ActionCommand) et répond aux coups (DefenseResult) selon le script ; renvoie les hash d'état reçus.
//
//   fakepeer host --port 7777 --replay r.jsonl [--log f.jsonl] [--timeout 900]
//     Faux hôte face au vrai jeu client : rejoue les messages hôte->client d'un enregistrement, en attendant
//     les messages du client aux mêmes endroits ; compare les hash d'état du client à ceux de l'hôte enregistré.
//     --pilot-rate 20 : positions du pilote rejouées à 20/s (l'enregistrement n'en garde que 5/s), interpolées.
//
//   Options communes : --net "lag=60,jitter=25,loss=0.01,spike=0.02:200,reorder=0" : réseau simulé sur les
//     datagrammes envoyés (latence, gigue, pertes, pointes de latence), comme une liaison Steam réelle.
//
//   fakepeer slice --in rec.jsonl --out tranche.jsonl --xfer N [--to T] [--snapshot snapshot-N.sav]
//     Tranche d'un enregistrement réel : session ouverte (Hello, Welcome, Roster), instantané n° N (SaveOffer) et
//     tout ce qui suit jusqu'au temps T (temps de l'enregistrement). Noms des joueurs anonymisés ("Host", "Client").
//     --no-resync 1 : resynchros du client retirées (session continue). --pilot-clock 0 (hôte) : PilotState sans t_ms.
//
// Code de sortie : 0 = scénario déroulé sans erreur ni désync ; 1 = échec ; 2 = usage.

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <cmath>
#include <deque>
#include <optional>
#include <random>
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
    double pilot_rate = 0;     // hôte : positions du pilote rejouées à ce rythme (interpolées entre les lignes)
    double pilot_gap_from = 1e18, pilot_gap_to = 1e18;   // hôte : --pilot-gap a:b, positions retenues entre a et b
    std::string net;           // réseau simulé (voir NetSim)
    std::string in, out;       // slice : enregistrement source, tranche écrite
    int xfer = 0;              // slice : n° de l'instantané de départ
    double to = 1e18;          // slice : fin de la tranche (temps de l'enregistrement)
    bool no_resync = false;    // slice : retire les resynchros du client (ResyncRequest, SaveOffer suivant, JoinReady, PilotChange)
    bool pilot_clock = true;   // hôte --paced : horloge du pilote (PilotState.t_ms) = temps de l'enregistrement
    int port = 7777;
    double timeout = 900;
};

using net::Recorder;
using net::from_hex;
using net::to_hex;

// --- Réseau simulé ---------------------------------------------------------------------------
// Chaque datagramme envoyé part après lag + gigue (loi exponentielle de moyenne "jitter", bornée à 4 fois), parfois
// avec une pointe en plus ("spike=p:ms"), ou se perd ("loss"). Sans "reorder=1", l'ordre d'envoi est conservé (une
// pointe retarde aussi les suivants, comme un relais Steam qui hoquette).
struct NetSim {
    double lag_ms = 0, jitter_ms = 0, loss = 0, spike_p = 0, spike_ms = 0;
    bool reorder = false;
    bool active() const { return lag_ms > 0 || jitter_ms > 0 || loss > 0 || spike_p > 0; }
};

bool parse_netsim(const std::string& s, NetSim& n, std::string& err)
{
    std::stringstream ss(s);
    std::string kv;
    while (std::getline(ss, kv, ',')) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) { err = "net: " + kv; return false; }
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "lag") n.lag_ms = std::atof(v.c_str());
        else if (k == "jitter") n.jitter_ms = std::atof(v.c_str());
        else if (k == "loss") n.loss = std::atof(v.c_str());
        else if (k == "reorder") n.reorder = v == "1";
        else if (k == "spike") {
            const auto c = v.find(':');
            n.spike_p = std::atof(v.substr(0, c).c_str());
            n.spike_ms = c == std::string::npos ? 200 : std::atof(v.substr(c + 1).c_str());
        } else { err = "net: unknown key " + k; return false; }
    }
    return true;
}

double now_s();

class SimTransport : public net::Transport {
public:
    SimTransport(std::unique_ptr<net::Transport> inner, NetSim n) : inner_(std::move(inner)), n_(n), rng_(0xE33C0) {}
    bool send(const std::string& peer, const uint8_t* data, size_t len) override
    {
        flush();
        ++sent;
        std::uniform_real_distribution<double> u(0.0, 1.0);
        if (u(rng_) < n_.loss) { ++dropped; return true; }
        double delay = n_.lag_ms;
        if (n_.jitter_ms > 0) delay += std::min(4.0 * n_.jitter_ms, std::exponential_distribution<double>(1.0 / n_.jitter_ms)(rng_));
        if (n_.spike_p > 0 && u(rng_) < n_.spike_p) { delay += n_.spike_ms; ++spikes; }
        double at = now_s() + delay / 1000.0;
        if (!n_.reorder) at = std::max(at, last_at_);
        last_at_ = at;
        Pending p{at, peer, std::vector<uint8_t>(data, data + len)};
        auto it = std::upper_bound(q_.begin(), q_.end(), at, [](double t, const Pending& x) { return t < x.at; });
        q_.insert(it, std::move(p));
        flush();
        return true;
    }
    bool poll(net::Datagram& out) override { flush(); return inner_->poll(out); }
    std::string local_address() const override { return inner_->local_address(); }
    int sent = 0, dropped = 0, spikes = 0;

private:
    void flush()
    {
        const double t = now_s();
        while (!q_.empty() && q_.front().at <= t) { inner_->send(q_.front().peer, q_.front().data.data(), q_.front().data.size()); q_.pop_front(); }
    }
    struct Pending { double at; std::string peer; std::vector<uint8_t> data; };
    std::unique_ptr<net::Transport> inner_;
    NetSim n_;
    std::mt19937_64 rng_;
    std::deque<Pending> q_;
    double last_at_ = 0;
};

// Transport UDP, avec le réseau simulé de --net s'il y en a un.
std::unique_ptr<net::Transport> make_transport(const Args& a, uint16_t port, JsonLog& log, SimTransport*& sim, std::string& err)
{
    sim = nullptr;
    auto t = net::make_udp(port, err);
    if (!t || a.net.empty()) return t;
    NetSim n;
    if (!parse_netsim(a.net, n, err)) return nullptr;
    log.write("netsim", JsonObj().add("lag_ms", n.lag_ms).add("jitter_ms", n.jitter_ms).add("loss", n.loss).add("spike_p", n.spike_p)
                            .add("spike_ms", n.spike_ms).add("reorder", n.reorder));
    auto s = std::make_unique<SimTransport>(std::move(t), n);
    sim = s.get();
    return s;
}

void log_netsim(JsonLog& log, const SimTransport* sim)
{
    if (sim) log.write("netsim_stats", JsonObj().add("sent", sim->sent).add("dropped", sim->dropped).add("spikes", sim->spikes));
}

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
    SimTransport* sim = nullptr;
    auto t = make_transport(a, 0, log, sim, err);
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
    hello.mod_version = script["mod_version"].as_string("current");
    if (hello.mod_version == "current") hello.mod_version = E33COOP_VERSION;   // version de ce build (fichier VERSION)
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
    std::vector<std::pair<double, proto::Message>> later;   // envois différés (aperçu de l'action, puis l'action)
    // Attentes de l'hôte (essai à deux PC du 2026-10-22, client figé par la barrière, hôte figé en attente) :
    //   "no_ready": true                    ne jamais envoyer BattleReady (client qui n'entre jamais dans le combat) ;
    //   "narrative": {"on":"welcome"|"battle_notice"|"battle_start","after_s":2,"s":30,"kind":"dialogue"|"cinematic"}
    //                                       scène propre au faux client, after_s après l'événement "on" ;
    //   "ready_after_narrative": true       BattleReady retenu tant que cette scène dure (on ne lance pas un combat en pleine scène) ;
    //   "stuck_at_turn": N                  à partir du tour N : plus rien (ni tour, ni hash, ni défense), lien gardé vivant ;
    //   "defense_delay_ms": D, "defense_delay_every": K   une défense sur K envoyée D ms après la demande (propriétaire en retard).
    const bool no_ready = script["no_ready"].as_bool(false);
    const bool ready_after_narr = script["ready_after_narrative"].as_bool(false);
    const json::Value& own_narr = script["narrative"];
    const std::string own_narr_on = own_narr["on"].as_string("welcome");
    double own_narr_from = -1, own_narr_end = -1;   // instant de l'événement "on", fin de la scène
    int own_narr_state = 0;   // 0 pas commencée, 1 en cours, 2 finie
    const int stuck_at = static_cast<int>(script["stuck_at_turn"].as_int(0));
    const bool hash_echo = script["hash_echo"].as_bool(true);
    bool stuck = false;
    const double def_delay_s = script["defense_delay_ms"].as_number(0) / 1000.0;
    const int def_delay_every = std::max<int>(1, static_cast<int>(script["defense_delay_every"].as_int(1)));

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
                if (own_narr_on == "welcome" && own_narr_from < 0) own_narr_from = now_s();
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
            } else if (auto* notice = std::get_if<proto::BattleStart>(&m); notice && notice->battle_id == 0) {
                ++counts["battle_notices"];   // avis précoce de l'hôte : le vrai BattleStart suit
                if (own_narr_on == "battle_notice" && own_narr_from < 0) own_narr_from = now_s();
            } else if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
                owners = bs->owners;
                ++counts["battle_starts"];
                if (own_narr_on == "battle_start" && own_narr_from < 0) own_narr_from = now_s();
                // Démarrage coordonné : le faux client est « dans » le combat (ou un peu plus tard : "ready_delay_s").
                ready_at = no_ready ? -1 : now_s() + script["ready_delay_s"].as_number(0);
                if (no_ready) ++counts["ready_withheld"];
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
                if (stuck_at > 0 && !stuck && ts->turn >= static_cast<uint32_t>(stuck_at)) {
                    stuck = true;
                    ++counts["stuck"];
                    log.write("stuck", JsonObj().add("turn", ts->turn));
                }
                if (stuck) continue;
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
                // "intent_preview" : {"delay_s":3} : aperçu de l'action (choix sans cible, puis avec cible) envoyé avant
                // elle, l'action partant delay_s plus tard (affichage « X prépare … » chez l'hôte).
                const json::Value& ip = script["intent_preview"];
                const double act_at = ip.is_object() ? now_s() + ip["delay_s"].as_number(3) : 0;
                uint16_t intent_seq = 0;
                auto preview = [&](double at, uint8_t type, const std::string& secondary, bool aiming, std::vector<proto::Unit> targets) {
                    if (!ip.is_object()) return;
                    proto::IntentPreview p;
                    p.battle_id = ts->battle_id; p.turn = ts->turn; p.spot = ts->index; p.seq = ++intent_seq;
                    p.action_type = type; p.secondary = secondary; p.aiming = aiming; p.targets = std::move(targets);
                    later.emplace_back(at, p);
                    ++counts["intent_previews_sent"];
                };
                // "intent_preview": {"menus": true | {"targets": ["enemy:0", …], "item_targets": ["hero:Lune", …]}} : état des menus (MenuState)
                // comme un joueur qui fouille la roue avant d'agir (miroir des menus chez l'hôte).
                const json::Value& mv = ip.is_object() ? ip["menus"] : json::Value();
                const bool menus = mv.is_object() || mv.as_bool(false);
                uint16_t menu_seq = 0;
                auto menu = [&](double at, proto::MenuPage page, uint8_t col, int8_t hover, uint8_t type, const std::string& sec, bool tgt,
                                std::vector<proto::Unit> targets) {
                    if (!menus) return;
                    proto::MenuState s;
                    s.battle_id = ts->battle_id; s.turn = ts->turn; s.spot = ts->index; s.seq = ++menu_seq;
                    s.page = page; s.column = col; s.hover = hover; s.action_type = type; s.secondary = sec; s.targeting = tgt;
                    s.targeting_type = tgt ? (targets.empty() || targets[0].kind == proto::ActorKind::Enemy ? 1 : 4) : 0;
                    s.targets = std::move(targets);
                    later.emplace_back(at, s);
                    ++counts["menu_states_sent"];
                };
                auto unit_of = [&](const std::string& t) -> std::optional<proto::Unit> {
                    if (t.rfind("enemy:", 0) == 0) return proto::Unit{proto::ActorKind::Enemy, static_cast<uint8_t>(std::atoi(t.c_str() + 6))};
                    if (t.rfind("hero:", 0) == 0) { const int sp = spot_of_hero(t.substr(5)); if (sp >= 0) return proto::Unit{proto::ActorKind::Hero, static_cast<uint8_t>(sp)}; }
                    return std::nullopt;
                };
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
                        preview(now_s(), 0, "", true, {});
                        menu(now_s(), proto::MenuPage::FreeAim, 0, -1, 0, "", false, {});
                        const uint32_t end_ms = 1500 + static_cast<uint32_t>(shots) * 1200;
                        // "aim_points" : point visé (relatif au premier ennemi) toutes les 250 ms, qui balaie autour de lui.
                        if ((*actp)["aim_points"].as_bool(false)) {
                            for (uint32_t t = 250; t < end_ms; t += 250) {
                                proto::FreeAimEvent e;
                                e.battle_id = ts->battle_id; e.turn = ts->turn; e.spot = ts->index; e.kind = proto::FreeAimKind::Aim; e.t_ms = t;
                                e.y = 60.0 * ((t / 250) % 5) - 120.0; e.z = 100.0; e.target = {proto::ActorKind::Enemy, 0};
                                lp->send(e, false);
                                ++counts["aim_points_sent"];
                            }
                        }
                        for (int i = 1; i <= shots; ++i) {
                            const uint32_t t = 1500 + static_cast<uint32_t>(i - 1) * 1200;
                            ev(proto::FreeAimKind::Fire, static_cast<uint16_t>(i), t);
                            ev(proto::FreeAimKind::Shot, static_cast<uint16_t>(i), t);
                        }
                        ev(proto::FreeAimKind::Cancel, 0, end_ms);
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
                if (ip.is_object()) {
                    // Cible affichée : celle de l'action, sinon le premier ennemi (cible automatique résolue par l'hôte).
                    std::vector<proto::Unit> shown;
                    for (auto& u : ac.targets) shown.push_back(u.spot == proto::kAutoSpot ? proto::Unit{u.kind, 0} : u);
                    const double d = act_at - now_s();
                    preview(now_s() + d * 0.3, ac.action_type, ac.secondary, false, {});
                    preview(now_s() + d * 0.6, ac.action_type, ac.secondary, false, shown);
                    if (menus && ac.action_type != 6 && ac.action_type != 4) {
                        // Fouille de la roue : compétences (deux colonnes, survol), objets, gradient, retours ; puis le choix
                        // de l'action, son ciblage (annulé une fois, puis repris), le curseur sur chaque cible de "targets",
                        // et enfin la cible de l'action (l'action part à act_at par ActionCommand).
                        using P = proto::MenuPage;
                        const double t0 = now_s();
                        auto at = [&](double f) { return t0 + d * f; };
                        menu(at(0.02), P::Actions, 0, -1, 0, "", false, {});
                        menu(at(0.08), P::Skills, 0, -1, 5, "", false, {});
                        menu(at(0.14), P::Skills, 0, 1, 5, "", false, {});
                        menu(at(0.20), P::Skills, 1, -1, 5, "", false, {});
                        menu(at(0.26), P::Skills, 0, -1, 5, "", false, {});
                        menu(at(0.31), P::Actions, 0, -1, 0, "", false, {});
                        menu(at(0.36), P::Items, 0, -1, 3, "", false, {});
                        menu(at(0.41), P::Actions, 0, -1, 0, "", false, {});
                        menu(at(0.46), P::Gradient, 0, -1, 0, "", false, {});
                        menu(at(0.51), P::Actions, 0, -1, 0, "", false, {});
                        const P page = ac.action_type == 5 ? P::Skills : ac.action_type == 3 ? P::Items : P::Actions;
                        std::vector<proto::Unit> cursor;
                        if (mv.is_object()) {
                            const json::Value& list = ac.action_type == 3 && mv["item_targets"].size() > 0 ? mv["item_targets"] : mv["targets"];
                            for (size_t k = 0; k < list.size(); ++k) if (auto u = unit_of(list[k].as_string(""))) cursor.push_back(*u);
                        }
                        if (cursor.empty()) cursor = shown;
                        if (cursor.empty()) cursor = {{proto::ActorKind::Enemy, 0}};
                        if (page != P::Actions) menu(at(0.55), page, 0, -1, ac.action_type, "", false, {});
                        menu(at(0.60), page, 0, -1, ac.action_type, ac.secondary, true, {cursor.front()});
                        menu(at(0.65), page, 0, -1, ac.action_type, "", false, {});   // retour : ciblage annulé
                        menu(at(0.70), page, 0, -1, ac.action_type, ac.secondary, true, {cursor.front()});
                        for (size_t k = 0; k < cursor.size(); ++k)
                            menu(at(0.74 + 0.16 * static_cast<double>(k) / static_cast<double>(cursor.size())), page, 0, -1, ac.action_type, ac.secondary, true, {cursor[k]});
                        menu(at(0.93), page, 0, -1, ac.action_type, ac.secondary, true, shown.empty() ? std::vector<proto::Unit>{cursor.front()} : shown);
                    }
                    later.emplace_back(act_at, ac);
                } else {
                    lp->send(ac);
                }
                ++turns_played;
            } else if (auto* dq = std::get_if<proto::DefenseRequest>(&m)) {
                if (stuck) continue;
                proto::DefenseResult dr;
                dr.battle_id = dq->battle_id;
                dr.turn = dq->turn;
                dr.spot = dq->spot;
                dr.hit = dq->hit;
                const int idx = defense_index++;
                dr.result = defense_of(defense[static_cast<size_t>(idx) % defense.size()], dq->jumpable != 0);
                if (def_delay_s > 0 && idx % def_delay_every == 0) { later.emplace_back(now_s() + def_delay_s, dr); ++counts["defenses_delayed"]; }
                else lp->send(dr);
            } else if (auto* sh = std::get_if<proto::StateHash>(&m)) {
                // Le faux client n'a pas d'état propre : il renvoie le hash de l'hôte ("hash_echo": false : jamais, pour
                // tester que ses menus et aperçus suffisent à prouver qu'il est dans son tour).
                if (!stuck && hash_echo) lp->send(*sh);
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
            } else if (std::get_if<proto::IntentPreview>(&m)) {
                ++counts["intent_previews_received"];   // aperçu des actions des héros de l'hôte
            } else if (std::get_if<proto::MenuState>(&m)) {
                ++counts["menu_states_received"];
            } else if (std::get_if<proto::AimView>(&m)) {
                ++counts["aim_views_received"];
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
        // Envois différés arrivés à échéance, dans l'ordre de programmation.
        for (size_t i = 0; i < later.size();) {
            if (now_s() >= later[i].first) { lp->send(later[i].second); later.erase(later.begin() + static_cast<std::ptrdiff_t>(i)); }
            else ++i;
        }
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
        // Scène propre au faux client ("narrative"), comme un dialogue ou une cinématique que l'hôte n'a pas.
        if (own_narr.is_object() && own_narr_from >= 0 && my_id != 0xFF) {
            const auto kind = own_narr["kind"].as_string("dialogue") == "cinematic" ? proto::NarrativeKind::Cinematic : proto::NarrativeKind::Dialogue;
            if (own_narr_state == 0 && now_s() - own_narr_from >= own_narr["after_s"].as_number(0)) {
                lp->send(proto::NarrativeState{my_id, kind, true, ++narr_seq});
                own_narr_state = 1;
                own_narr_end = now_s() + own_narr["s"].as_number(30);
                ++counts["own_narratives"];
            } else if (own_narr_state == 1 && now_s() >= own_narr_end) {
                lp->send(proto::NarrativeState{my_id, kind, false, ++narr_seq});
                own_narr_state = 2;
            }
        }
        if (ready_at >= 0 && now_s() >= ready_at && !(ready_after_narr && own_narr_state == 1)) {
            lp->send(proto::BattleReady{ready_battle, my_id}); ready_at = -1; ++counts["ready_sent"];
        }
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
    log_netsim(log, sim);
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
    SimTransport* sim = nullptr;
    auto t = make_transport(a, static_cast<uint16_t>(a.port), log, sim, err);
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
    // --pilot-rate : interpolation entre la position enregistrée envoyée (a) et la suivante (b).
    std::optional<proto::PilotState> up_a, up_b;
    double up_a_t = 0, up_b_t = 0, up_next = 0;
    uint32_t up_last_seq = 0;
    int interpolated = 0, pilot_gapped = 0;
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
                    if (auto* ps = std::get_if<proto::PilotState>(&l.msg)) {
                        // --pilot-gap a:b : positions retenues entre a et b (temps de l'enregistrement) : le suiveur reste en
                        // arrière, comme chez le vrai client qui n'a pas déclenché le dialogue de l'hôte (duel du prologue).
                        if (l.t >= a.pilot_gap_from && l.t < a.pilot_gap_to) { up_a.reset(); up_b.reset(); ++pilot_gapped; ++cursor; continue; }
                        // Comme le vrai mod : positions non fiables (une perte n'est pas renvoyée). Horloge du pilote
                        // (t_ms) = temps de l'enregistrement, comme le vrai pilote qui la note à l'envoi.
                        proto::PilotState out = *ps;
                        if (a.paced && a.pilot_clock) out.t_ms = 1000 + static_cast<uint32_t>(l.t * 1000.0);
                        else if (!a.pilot_clock) out.t_ms = 0;
                        link->send(out, false);
                        up_a.reset();
                        up_b.reset();
                        if (a.pilot_rate > 0 && a.paced) {
                            // Position enregistrée suivante (même pilote, même niveau, < 0,6 s) : on interpolera jusqu'à elle.
                            for (size_t j = cursor + 1; j < lines.size() && j < cursor + 60; ++j) {
                                auto* nb = std::get_if<proto::PilotState>(&lines[j].msg);
                                if (!nb || lines[j].dir != "h2c") continue;
                                if (nb->pilot == ps->pilot && nb->level == ps->level && nb->seq > ps->seq + 1 && lines[j].t - l.t < 0.6) {
                                    up_a = *ps; up_a_t = l.t; up_b = *nb; up_b_t = lines[j].t; up_last_seq = ps->seq; up_next = l.t;
                                }
                                break;
                            }
                        }
                        ++cursor;
                        continue;
                    }
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
                           std::holds_alternative<proto::BuildOffer>(l.msg) || std::holds_alternative<proto::BuildChunk>(l.msg) || std::holds_alternative<proto::EconomyDelta>(l.msg) ||
                           std::holds_alternative<proto::IntentPreview>(l.msg) || std::holds_alternative<proto::AimView>(l.msg) ||
                           std::holds_alternative<proto::MenuState>(l.msg) || std::holds_alternative<proto::SceneStart>(l.msg) ||
                           std::holds_alternative<proto::SceneSkip>(l.msg)) {
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
            // Positions intermédiaires (--pilot-rate) : courbe d'Hermite entre deux positions enregistrées (vitesses aux
            // extrémités), au rythme du vrai pilote ; n° de séquence strictement croissants, sous celui de la suivante.
            if (up_a && up_b) {
                const double rec_now = pace_rec + (now_s() - pace_now);
                const double step = 1.0 / a.pilot_rate;
                if (rec_now >= up_b_t) { up_a.reset(); up_b.reset(); }
                else if (rec_now >= up_next + step) {
                    up_next = rec_now;
                    const double h = up_b_t - up_a_t, f = std::clamp((rec_now - up_a_t) / h, 0.0, 1.0);
                    const double h00 = 2 * f * f * f - 3 * f * f + 1, h10 = f * f * f - 2 * f * f + f, h01 = -2 * f * f * f + 3 * f * f, h11 = f * f * f - f * f;
                    proto::PilotState st = *up_a;
                    auto herm = [&](double pa, double va, double pb, double vb) { return h00 * pa + h10 * h * va + h01 * pb + h11 * h * vb; };
                    st.x = herm(up_a->x, up_a->vx, up_b->x, up_b->vx);
                    st.y = herm(up_a->y, up_a->vy, up_b->y, up_b->vy);
                    st.z = herm(up_a->z, up_a->vz, up_b->z, up_b->vz);
                    st.vx = up_a->vx + (up_b->vx - up_a->vx) * f;
                    st.vy = up_a->vy + (up_b->vy - up_a->vy) * f;
                    st.vz = up_a->vz + (up_b->vz - up_a->vz) * f;
                    double dyaw = std::fmod(up_b->yaw - up_a->yaw + 540.0, 360.0) - 180.0;
                    st.yaw = up_a->yaw + dyaw * f;
                    const uint32_t seq = up_a->seq + 1 + static_cast<uint32_t>(f * (up_b->seq - up_a->seq - 1));
                    if (seq > up_last_seq && seq < up_b->seq) {
                        st.seq = seq;
                        st.t_ms = a.pilot_clock ? 1000 + static_cast<uint32_t>(rec_now * 1000.0) : 0;
                        up_last_seq = seq;
                        link->send(st, false);
                        ++interpolated;
                    }
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
    log_netsim(log, sim);
    log.write("pilot_interpolated", JsonObj().add("count", interpolated).add("gapped", pilot_gapped));
    log.write("done", JsonObj().add("ok", ok).add("replayed", static_cast<int64_t>(cursor)).add("of", static_cast<int64_t>(lines.size())).add("hashes", hashes)
                          .add("desyncs", desyncs).add("c2h_mismatches", mismatches).add("invalid", invalid));
    return ok ? 0 : 1;
}

// --- Tranche d'un enregistrement réel (slice) -------------------------------------------------

int run_slice(const Args& a)
{
    std::ifstream f(a.in, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", a.in.c_str()); return 1; }
    std::string line, err;
    std::vector<RecLine> lines;
    while (std::getline(f, line)) {
        json::Value v;
        if (line.empty() || !json::parse(line, v, err)) continue;
        auto bytes = from_hex(v["hex"].as_string());
        wire::Reader r(bytes.data(), bytes.size());
        auto m = proto::decode(r, err);
        if (!m) { std::fprintf(stderr, "bad line: %s\n", err.c_str()); return 1; }
        lines.push_back({v["dir"].as_string(), std::move(*m), v["t"].as_number(0)});
    }
    // Instantané de départ : SaveOffer n° xfer.
    size_t start = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
        if (auto* so = std::get_if<proto::SaveOffer>(&lines[i].msg); so && lines[i].dir == "h2c" && so->xfer == static_cast<uint32_t>(a.xfer)) { start = i; break; }
    if (start == lines.size()) { std::fprintf(stderr, "SaveOffer xfer %d not found\n", a.xfer); return 1; }
    const auto& offer = std::get<proto::SaveOffer>(lines[start].msg);
    if (!a.snapshot.empty()) {
        std::ifstream sf(a.snapshot, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
        const bool ok = bytes.size() == offer.size && fnv64(bytes) == offer.hash;
        std::printf("snapshot %s : %s (size %zu, want %u)\n", a.snapshot.c_str(), ok ? "matches" : "DOES NOT MATCH", bytes.size(), offer.size);
        if (!ok) return 1;
    }
    // Ouverture de session (le client se présente, l'hôte l'accueille), sans les noms des joueurs.
    std::vector<RecLine> out;
    auto anon = [](proto::Message m) {
        if (auto* h = std::get_if<proto::Hello>(&m)) h->player_name = "Client";
        if (auto* r = std::get_if<proto::Roster>(&m)) for (auto& e : r->players) e.name = e.player == 0 ? "Host" : "Client" + std::to_string(e.player);
        return m;
    };
    const double t0 = lines[start].t - 1.0;
    bool hello = false, welcome = false, roster = false;
    for (size_t i = 0; i < start; ++i) {
        const auto& l = lines[i];
        if (!hello && l.dir == "c2h" && std::holds_alternative<proto::Hello>(l.msg)) { out.push_back({l.dir, anon(l.msg), 0.0}); hello = true; }
        else if (hello && !welcome && std::holds_alternative<proto::Welcome>(l.msg)) { out.push_back({l.dir, l.msg, 0.0}); welcome = true; }
        else if (welcome && !roster && std::holds_alternative<proto::Roster>(l.msg)) { out.push_back({l.dir, anon(l.msg), 0.0}); roster = true; }
    }
    std::map<std::string, int> counts;
    // --no-resync 1 : resynchros du client retirées (ResyncRequest, instantané suivant, JoinReady et PilotChange qui
    // le suivent) : le rejeu reste une seule session continue. Pendant le rechargement réel, l'hôte n'envoyait pas de
    // positions : le trou reste (le pilote « saute » à la reprise).
    uint32_t dropped_xfer = 0;
    for (size_t i = start; i < lines.size() && lines[i].t <= a.to; ++i) {
        if (a.no_resync && i > start) {
            const auto& m = lines[i].msg;
            bool drop = false;
            if (std::holds_alternative<proto::ResyncRequest>(m) && lines[i].dir == "c2h") drop = true;
            else if (auto* so = std::get_if<proto::SaveOffer>(&m)) { dropped_xfer = so->xfer; drop = true; }
            else if (auto* jr = std::get_if<proto::JoinReady>(&m); jr && dropped_xfer && jr->xfer == dropped_xfer) drop = true;
            else if (std::holds_alternative<proto::PilotChange>(m) && i > 0 && std::holds_alternative<proto::JoinReady>(lines[i - 1].msg) &&
                     dropped_xfer && std::get<proto::JoinReady>(lines[i - 1].msg).xfer == dropped_xfer) drop = true;
            if (drop) { ++counts["dropped " + std::string(proto::type_name(proto::type_of(m)))]; continue; }
        }
        out.push_back({lines[i].dir, anon(lines[i].msg), lines[i].t - t0});
        ++counts[std::string(lines[i].dir) + " " + proto::type_name(proto::type_of(lines[i].msg))];
    }
    std::ofstream o(a.out, std::ios::binary);
    for (const auto& l : out) {
        wire::Writer w;
        proto::encode(l.msg, w);
        o << JsonObj().add("t", l.t).add("dir", l.dir).add("type", proto::type_name(proto::type_of(l.msg))).add("hex", to_hex(w.data())).raw("msg", proto::to_json(l.msg)).str() << "\n";
    }
    std::printf("slice: %zu lines, xfer %d at t=%.1f, until t=%.1f (%.1f s)\n", out.size(), a.xfer, lines[start].t, std::min(a.to, lines.back().t),
                std::min(a.to, lines.back().t) - lines[start].t);
    for (auto& [k, n] : counts) std::printf("  %-28s %d\n", k.c_str(), n);
    return 0;
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
        else if (k == "--pilot-rate") a.pilot_rate = std::atof(v.c_str());
        else if (k == "--net") a.net = v;
        else if (k == "--pilot-gap") { const auto c = v.find(':'); a.pilot_gap_from = std::atof(v.c_str()); a.pilot_gap_to = c == std::string::npos ? 1e18 : std::atof(v.c_str() + c + 1); }
        else if (k == "--in") a.in = v;
        else if (k == "--out") a.out = v;
        else if (k == "--xfer") a.xfer = std::atoi(v.c_str());
        else if (k == "--to") a.to = std::atof(v.c_str());
        else if (k == "--no-resync") a.no_resync = v == "1";
        else if (k == "--pilot-clock") a.pilot_clock = v == "1";
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }
    if (a.mode == "slice") return a.in.empty() || a.out.empty() || !a.xfer ? 2 : run_slice(a);
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
