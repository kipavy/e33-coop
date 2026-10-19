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
    proto::ActionCommand ac; ac.battle_id = 7; ac.turn = 3; ac.actor_spot = 1; ac.targets = {0, 2}; ac.combo_hits = 3; ac.combo_perfect = 1;
    proto::ActionResult ar; ar.battle_id = 7; ar.turn = 3; ar.targets = {0}; ar.hits = {{proto::ActorKind::Enemy, 0, 123.5, true, 10.0}};
    proto::DefenseRequest dq; dq.battle_id = 7; dq.turn = 4; dq.hit = 2; dq.defender_spots = {1};
    proto::DefenseResult dr; dr.battle_id = 7; dr.turn = 4; dr.hit = 2; dr.result = proto::Defense::Parry;
    return {h, proto::Welcome{1, 99}, proto::Reject{proto::RejectReason::GameBuildMismatch, "host 1 vs client 2"}, proto::Bye{"bye"},
            proto::Ping{5}, proto::Pong{6}, bs, proto::TurnStart{7, 3, proto::ActorKind::Hero, 1, "BP_Maelle"}, ac, ar, dq, dr,
            proto::StateHash{7, 3, 0xDEADBEEFCAFEull}, proto::BattleEnd{7, 1}, proto::Draws{7, 3, {{0xABCD1234u, 0, 2.0}, {7u, 1, 0.25}}}};
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
        if (b.size() >= 2 && (rng() & 1)) { b[0] = static_cast<uint8_t>(1 + rng() % 30); b[1] = 0; }
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

int main()
{
    test_roundtrip();
    test_invalid();
    test_link();
    std::printf(g_fail ? "%d FAILURE(S)\n" : "all protocol tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
