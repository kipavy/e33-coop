// Tests unitaires du protocole et du lien fiable (sans le jeu). Exécutés en CI via ctest.
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "../../native/net/link.hpp"
#include "../../native/protocol/messages.hpp"

using namespace e33;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::vector<proto::Message> samples()
{
    proto::Hello h; h.game_build = 23765773; h.mod_version = "0.2.0"; h.gameplay_mods = {"A", "B"}; h.player_name = "Killian"; h.instance_id = 42;
    proto::BattleStart bs; bs.battle_id = 7; bs.encounter = "L_LumiereCitizenFleuret_Male*1"; bs.owners = {{0, "Verso", 0}, {1, "Maelle", 1}}; bs.rng_seed = 0x0123456789ABCDEFull;
    bs.party = {"Verso", "Maelle", "Lune"};
    proto::ActionCommand ac; ac.battle_id = 7; ac.turn = 3; ac.actor_spot = 1; ac.action_type = 5; ac.secondary = "Combo1";
    ac.targets = {{proto::ActorKind::Enemy, 0}, {proto::ActorKind::Hero, 2}}; ac.combo = {3, 1, 0};
    proto::DefenseRequest dq; dq.battle_id = 7; dq.turn = 4; dq.spot = 1; dq.hit = 2; dq.jumpable = 1;
    proto::DefenseResult dr; dr.battle_id = 7; dr.turn = 4; dr.spot = 1; dr.hit = 2; dr.result = proto::Defense::Parry;
    proto::StateSync ss; ss.battle_id = 7; ss.turn = 4; ss.gradient_charges = 2; ss.gradient_progress = 0.5;
    proto::UnitState us; us.unit = {proto::ActorKind::Hero, 2}; us.cls = "BP_Verso_Battle2_C"; us.hp = 13965; us.ap = 9; us.shield = 1; us.break_bar = 1234567890123ll;
    us.buffs = {{"BP_BattleBuff_Burn_C", 1, 3, false}, {"BP_BattleBuff_Shell_C", 2, 0, true}};
    us.mech = {{"BP_PerfectionMechanicComponent_C.Perfection", 42}, {"BP_StainHolderComponent_C.Stains[0]", 3}};
    ss.units = {us, proto::UnitState{{proto::ActorKind::Enemy, 0}, "BP_Enemy", 5.5, 0, 0, 0, true, {}}};
    proto::BattleRewards rw; rw.battle_id = 7; rw.xp = 521235; rw.gold = 10448; rw.loot = {{"Lumicolor", 5}, {"ChromaCatalyst", 2}};
    h.join = true;
    proto::ActionCommand forced = ac; forced.forced = true;
    bs.turn_timer_s = 60;
    proto::BattleStart wb = bs; wb.world_enemy = "BP_EnemyWorld_SpringMeadowElementalGrunt_C_UAID_18C04D921BE3793601_2147027147";
    wb.enemy_global_id = "ObjectID_Enemy_Level_SpringMeadows_Main_V2_X"; wb.engage = 2;
    proto::SaveChunk ch; ch.xfer = 3; ch.offset = 16000; ch.data = {0x47, 0x56, 0x41, 0x53, 0, 255};
    return {h, proto::Welcome{1, 99}, proto::Reject{proto::RejectReason::GameBuildMismatch, "host 1 vs client 2"}, proto::Bye{"bye"},
            proto::Ping{5}, proto::Pong{6}, bs, proto::TurnStart{7, 3, proto::ActorKind::Hero, 1, "BP_Maelle"}, ac, dq, dr,
            proto::StateHash{7, 3, 0xDEADBEEFCAFEull}, proto::BattleEnd{7, 1}, proto::Draws{7, 3, {{0xABCD1234u, 0, 2.0}, {7u, 1, 0.25}}},
            proto::ComboResult{7, 3, 1, 2, true, true, 880}, proto::ComboResult{7, 3, 1, 3, false, false, proto::kNoPress}, ss, rw,
            proto::FreeAimEvent{7, 3, 2, proto::FreeAimKind::Shot, 4, 3900, -32163.5, 16193.25, 1188.0, {proto::ActorKind::Enemy, proto::kAutoSpot}},
            wb, proto::SaveOffer{3, 438435, 0x1122334455667788ull, "Level_SpringMeadows_Main_V2"}, ch, proto::JoinWait{"battle"},
            proto::JoinReady{3, "Level_SpringMeadows_Main_V2"}, proto::PilotState{0, 12, "Level_Camp_Main", -2191.5, -1232, 192, 120, 300, -10, 0},
            proto::PilotRequest{1}, proto::PilotChange{1}, proto::WorldInteract{4, "/Game/Levels/X.X:PersistentLevel.NPC_1", false},
            proto::DialogueChoice{5, 1}, proto::MapChange{6, "Level_Camp_Main", "Level.SpawnPoint.Generic.Dynamic"},
            proto::NarrativeState{1, proto::NarrativeKind::Cinematic, true, 7}, proto::BarrierRelease{7}, proto::PauseState{2, true, false},
            proto::Owners{7, {{0, "Verso", 0}, {1, "Maelle", 0}}}, proto::BattleReady{7, 2}, proto::Roster{{{0, "Killian"}, {1, "Ami"}}},
            proto::WorldHash{12, "Level_Camp_Main", {{"gold", 10448}, {"items", 0x1234567890ull}}}, proto::ResyncRequest{"gold differs"}, forced,
            proto::BuildOffer{0x01000002, 1, true, 51234, 0xABCDull, {{"Verso", 42}, {"Lune", 7}}, "menu"}, proto::BuildChunk{0x01000002, 16000, {1, 2, 3}}};
}

static void test_roundtrip()
{
    for (const auto& m : samples()) {
        wire::Writer w;
        proto::encode(m, w);
        wire::Reader r(w.data().data(), w.size());
        std::string err;
        auto back = proto::decode(r, err);
        CHECK(back.has_value());
        if (!back) { std::printf("  decode error for %s: %s\n", proto::type_name(proto::type_of(m)), err.c_str()); continue; }
        CHECK(proto::to_json(*back) == proto::to_json(m));
        CHECK(r.at_end());
    }
}

static void test_invalid()
{
    // Énumération hors bornes
    proto::DefenseResult dr; dr.result = proto::Defense::Parry;
    wire::Writer w; proto::encode(dr, w);
    w.data().back() = 99;
    { wire::Reader r(w.data().data(), w.size()); std::string err; CHECK(!proto::decode(r, err)); }
    // Tronqué
    for (size_t cut = 0; cut + 1 < w.size(); ++cut) { wire::Reader r(w.data().data(), cut); std::string err; CHECK(!proto::decode(r, err)); }
    // Type inconnu
    { wire::Writer u; u.u16(999); u.u16(0); wire::Reader r(u.data().data(), u.size()); std::string err; CHECK(!proto::decode(r, err)); }
    // Fuzz : octets aléatoires, ne doit jamais planter
    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) {
        std::vector<uint8_t> b(rng() % 64);
        for (auto& x : b) x = static_cast<uint8_t>(rng());
        if (b.size() >= 2 && (rng() & 1)) { b[0] = static_cast<uint8_t>(1 + rng() % 55); b[1] = 0; }
        wire::Reader r(b.data(), b.size()); std::string err;
        (void)proto::decode(r, err);
    }
}

// Transport mémoire avec pertes, doublons et réordonnancement.
class LossyPipe : public net::Transport {
public:
    LossyPipe(std::deque<net::Datagram>& to, std::string self, uint32_t seed) : to_(to), self_(std::move(self)), rng_(seed) {}
    bool send(const std::string&, const uint8_t* d, size_t n) override
    {
        if (rng_() % 100 < 30) return true;                    // 30 % de pertes
        net::Datagram g{self_, std::vector<uint8_t>(d, d + n)};
        if (rng_() % 100 < 20 && !to_.empty()) to_.push_front(g); else to_.push_back(g);   // réordonnancement
        if (rng_() % 100 < 10) to_.push_back(g);               // doublon
        return true;
    }
    bool poll(net::Datagram& out) override { if (in_.empty()) return false; out = in_.front(); in_.pop_front(); return true; }
    std::string local_address() const override { return self_; }
    std::deque<net::Datagram> in_;

private:
    std::deque<net::Datagram>& to_;
    std::string self_;
    std::mt19937 rng_;
};

static void test_link()
{
    std::deque<net::Datagram> a_to_b, b_to_a;
    LossyPipe ta(a_to_b, "A", 1), tb(b_to_a, "B", 2);
    net::Link la(ta, "B"), lb(tb, "A");
    int invalid = 0;
    la.on_invalid = lb.on_invalid = [&](const std::string&) { ++invalid; };
    const int N = 300;
    for (int i = 0; i < N; ++i) la.send(proto::StateHash{1, static_cast<uint32_t>(i), static_cast<uint64_t>(i) * 7}, true);
    std::vector<uint32_t> got;
    double now = 0;
    for (int step = 0; step < 4000 && got.size() < N; ++step) {
        now += 0.02;
        la.update(now); lb.update(now);
        while (!a_to_b.empty()) { auto d = a_to_b.front(); a_to_b.pop_front(); lb.on_datagram(d.data.data(), d.data.size(), now); }
        while (!b_to_a.empty()) { auto d = b_to_a.front(); b_to_a.pop_front(); la.on_datagram(d.data.data(), d.data.size(), now); }
        proto::Message m;
        while (lb.pop(m)) if (auto* h = std::get_if<proto::StateHash>(&m)) got.push_back(h->turn);
    }
    CHECK(got.size() == static_cast<size_t>(N));
    bool ordered = true;
    for (size_t i = 0; i < got.size(); ++i) ordered = ordered && got[i] == i;
    CHECK(ordered);
    CHECK(invalid == 0);
    // Garbage sur le lien : signalé, ignoré
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    lb.on_datagram(junk, sizeof junk, now);
    CHECK(invalid == 1);
}

// Gros messages (état complet de plusieurs Ko) : fragmentés, réassemblés dans l'ordre malgré pertes et doublons.
static void test_link_fragments()
{
    std::deque<net::Datagram> a_to_b, b_to_a;
    LossyPipe ta(a_to_b, "A", 3), tb(b_to_a, "B", 4);
    net::Link la(ta, "B"), lb(tb, "A");
    int invalid = 0;
    la.on_invalid = lb.on_invalid = [&](const std::string&) { ++invalid; };
    std::vector<std::string> sent;
    for (uint32_t i = 0; i < 40; ++i) {
        if (i % 3 == 0) {
            proto::StateSync ss; ss.battle_id = 1; ss.turn = i;
            for (uint8_t u = 0; u < 6; ++u) {
                proto::UnitState us; us.unit = {proto::ActorKind::Enemy, u}; us.cls = "BP_EnemyBattle_Lancelier_Unique_V2_C"; us.hp = 1000.5 * u;
                for (int b = 0; b < 12; ++b) us.buffs.push_back({"BP_BattleBuff_Burn_C_" + std::to_string(b), b, 3, false});
                ss.units.push_back(us);
            }
            sent.push_back(proto::to_json(ss));
            la.send(ss, true);
        } else {
            proto::StateHash h{1, i, i * 11ull};
            sent.push_back(proto::to_json(h));
            la.send(h, true);
        }
    }
    std::vector<std::string> got;
    double now = 0;
    for (int step = 0; step < 8000 && got.size() < sent.size(); ++step) {
        now += 0.02;
        la.update(now); lb.update(now);
        while (!a_to_b.empty()) { auto d = a_to_b.front(); a_to_b.pop_front(); CHECK(d.data.size() <= 1300); lb.on_datagram(d.data.data(), d.data.size(), now); }
        while (!b_to_a.empty()) { auto d = b_to_a.front(); b_to_a.pop_front(); la.on_datagram(d.data.data(), d.data.size(), now); }
        proto::Message m;
        while (lb.pop(m)) got.push_back(proto::to_json(m));
    }
    CHECK(got == sent);
    CHECK(invalid == 0);
}


// Extensions optionnelles en fin de message : les enregistrements des versions précédentes restent lisibles.
static void test_compat()
{
    proto::ActionCommand ac; ac.battle_id = 1; ac.turn = 2; ac.actor_spot = 0; ac.action_type = 1; ac.combo = {1};
    wire::Writer w; proto::encode(ac, w);
    auto b = w.data();
    b.pop_back();                                       // sans le champ "forced" (v5)
    b[2] = static_cast<uint8_t>(b[2] - 1);              // longueur de la charge utile
    wire::Reader r(b.data(), b.size()); std::string err;
    auto m = proto::decode(r, err);
    CHECK(m.has_value());
    if (m) CHECK(!std::get<proto::ActionCommand>(*m).forced);
}

// Transport mémoire parfait (pour les tests de cycle de vie du lien).
class Pipe : public net::Transport {
public:
    Pipe(std::deque<net::Datagram>& to, std::string self) : to_(to), self_(std::move(self)) {}
    bool send(const std::string&, const uint8_t* d, size_t n) override { to_.push_back({self_, std::vector<uint8_t>(d, d + n)}); return true; }
    bool poll(net::Datagram&) override { return false; }
    std::string local_address() const override { return self_; }
private:
    std::deque<net::Datagram>& to_;
    std::string self_;
};

static void pump(std::deque<net::Datagram>& q, net::Link& to, double now)
{
    while (!q.empty()) { auto d = q.front(); q.pop_front(); to.on_datagram(d.data.data(), d.data.size(), now); }
}

// Le pair redémarre (nouveau processus, même adresse) : détecté par l'époque, l'échange repart de zéro et le
// premier message du nouveau pair est bien délivré (sans reset, il serait pris pour un doublon).
static void test_link_restart()
{
    std::deque<net::Datagram> a_to_b, b_to_a;
    Pipe ta(a_to_b, "A"), tb(b_to_a, "B");
    net::Link host(tb, "A");
    int resets = 0;
    host.on_remote_reset = [&] { ++resets; };
    double now = 0;
    {
        net::Link client(ta, "B");
        for (uint32_t i = 0; i < 5; ++i) client.send(proto::Ping{i});
        client.update(now); pump(a_to_b, host, now);
        proto::Message m; int n = 0; while (host.pop(m)) ++n;
        CHECK(n == 5);
    }
    net::Link client2(ta, "B");
    client2.send(proto::Bye{"again"});
    now += 1; client2.update(now); pump(a_to_b, host, now);
    proto::Message m;
    CHECK(host.pop(m) && std::holds_alternative<proto::Bye>(m));
    CHECK(resets == 1);
    CHECK(host.remote_epoch() == client2.epoch());
}

// Thread de jeu bloqué (chargement) : les battements « occupé » gardent le pair en vie, mais pas indéfiniment.
static void test_link_busy()
{
    std::deque<net::Datagram> a_to_b, b_to_a;
    Pipe ta(a_to_b, "A"), tb(b_to_a, "B");
    net::Link a(ta, "B"), b(tb, "A");
    a.send(proto::Ping{1}); a.update(0); pump(a_to_b, b, 0);
    CHECK(!b.timed_out(10));
    CHECK(b.timed_out(16));
    for (int s = 1; s <= 60; ++s) { auto p = net::Link::busy_packet(a.epoch()); b.on_datagram(p.data(), p.size(), s); }
    CHECK(!b.timed_out(60));
    CHECK(b.busy(60));
    CHECK(b.timed_out(80));                              // plus rien depuis 20 s
    for (int s = 61; s <= 400; ++s) { auto p = net::Link::busy_packet(a.epoch()); b.on_datagram(p.data(), p.size(), s); }
    CHECK(b.timed_out(400));                             // « occupé » depuis plus de 300 s : perdu
    proto::Message m; int n = 0; while (b.pop(m)) ++n;
    CHECK(n == 1);
}

// Pair d'une autre version de protocole : signalé une fois, aucun message interprété.
static void test_link_version()
{
    std::deque<net::Datagram> a_to_b;
    Pipe ta(a_to_b, "A");
    net::Link b(ta, "A");
    int mism = 0; uint16_t seen = 0;
    b.on_protocol_mismatch = [&](uint16_t v) { ++mism; seen = v; };
    auto p = net::Link::version_packet(1234);
    p[4] = 5; p[5] = 0;                                  // protocole 5
    b.on_datagram(p.data(), p.size(), 0);
    b.on_datagram(p.data(), p.size(), 0);
    CHECK(mism == 1);
    CHECK(seen == 5);
    CHECK(net::Link::packet_protocol(p.data(), p.size()) == 5);
    CHECK(net::Link::packet_epoch(p.data(), p.size()) == 0);
    proto::Message m;
    CHECK(!b.pop(m));
}

int main()
{
    test_roundtrip();
    test_invalid();
    test_link();
    test_link_fragments();
    test_compat();
    test_link_restart();
    test_link_busy();
    test_link_version();
    std::printf(g_fail ? "%d FAILURE(S)\n" : "all protocol tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
