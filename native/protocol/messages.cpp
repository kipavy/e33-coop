#include "messages.hpp"

#include <iterator>
#include <type_traits>

#include "../common/jsonlog.hpp"

namespace e33::proto {

namespace {

constexpr size_t kMaxStr = 512;
constexpr uint16_t kMaxList = 64;
constexpr uint16_t kMaxUnits = 16;
constexpr uint16_t kMaxBuffs = 64;

template <class> inline constexpr bool always_false = false;

void w_u8list(wire::Writer& w, const std::vector<uint8_t>& v)
{
    w.u16(static_cast<uint16_t>(v.size()));
    for (auto x : v) w.u8(x);
}
std::vector<uint8_t> r_u8list(wire::Reader& r)
{
    std::vector<uint8_t> v(r.count(kMaxList));
    for (auto& x : v) x = r.u8();
    return v;
}

bool valid_kind(uint8_t k) { return k <= 1; }

void w_unit(wire::Writer& w, const Unit& u) { w.u8(static_cast<uint8_t>(u.kind)); w.u8(u.spot); }
Unit r_unit(wire::Reader& r)
{
    Unit u;
    const uint8_t k = r.u8();
    if (!valid_kind(k)) r.fail();
    u.kind = static_cast<ActorKind>(k);
    u.spot = r.u8();
    return u;
}

void encode_payload(const Message& m, wire::Writer& w)
{
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Hello>) {
            w.u16(x.protocol); w.u32(x.game_build); w.str(x.mod_version);
            w.u16(static_cast<uint16_t>(x.gameplay_mods.size()));
            for (auto& s : x.gameplay_mods) w.str(s);
            w.str(x.player_name); w.u64(x.instance_id);
        } else if constexpr (std::is_same_v<T, Welcome>) {
            w.u8(x.player_id); w.u32(x.session_id);
        } else if constexpr (std::is_same_v<T, Reject>) {
            w.u8(static_cast<uint8_t>(x.reason)); w.str(x.detail);
        } else if constexpr (std::is_same_v<T, Bye>) {
            w.str(x.reason);
        } else if constexpr (std::is_same_v<T, Ping> || std::is_same_v<T, Pong>) {
            w.u64(x.t_us);
        } else if constexpr (std::is_same_v<T, BattleStart>) {
            w.u32(x.battle_id); w.str(x.encounter);
            w.u16(static_cast<uint16_t>(x.owners.size()));
            for (auto& o : x.owners) { w.u8(o.spot); w.str(o.character); w.u8(o.player_id); }
            w.u64(x.rng_seed);
            w.u16(static_cast<uint16_t>(x.party.size()));
            for (auto& p : x.party) w.str(p);
        } else if constexpr (std::is_same_v<T, TurnStart>) {
            w.u32(x.battle_id); w.u32(x.turn); w.u8(static_cast<uint8_t>(x.kind)); w.u8(x.index); w.str(x.actor);
        } else if constexpr (std::is_same_v<T, ActionCommand>) {
            w.u32(x.battle_id); w.u32(x.turn); w.u8(x.actor_spot); w.u8(x.action_type); w.str(x.secondary);
            w.u16(static_cast<uint16_t>(x.targets.size()));
            for (auto& t : x.targets) w_unit(w, t);
            w_u8list(w, x.combo);
        } else if constexpr (std::is_same_v<T, ComboResult>) {
            w.u32(x.battle_id); w.u32(x.turn); w.u8(x.spot); w.u16(x.seq); w.boolean(x.success); w.boolean(x.perfect);
        } else if constexpr (std::is_same_v<T, DefenseRequest>) {
            w.u32(x.battle_id); w.u32(x.turn); w.u8(x.spot); w.u16(x.hit); w.u8(x.jumpable); w.u16(x.timeout_ms);
        } else if constexpr (std::is_same_v<T, DefenseResult>) {
            w.u32(x.battle_id); w.u32(x.turn); w.u8(x.spot); w.u16(x.hit); w.u8(static_cast<uint8_t>(x.result));
        } else if constexpr (std::is_same_v<T, StateHash>) {
            w.u32(x.battle_id); w.u32(x.turn); w.u64(x.hash);
        } else if constexpr (std::is_same_v<T, BattleEnd>) {
            w.u32(x.battle_id); w.u8(x.result);
        } else if constexpr (std::is_same_v<T, Draws>) {
            w.u32(x.battle_id); w.u32(x.turn);
            w.u16(static_cast<uint16_t>(x.draws.size()));
            for (auto& d : x.draws) { w.u32(d.key); w.u16(d.rank); w.f64(d.value); }
        } else if constexpr (std::is_same_v<T, StateSync>) {
            w.u32(x.battle_id); w.u32(x.turn); w.u8(x.reason);
            w.u16(static_cast<uint16_t>(x.units.size()));
            for (auto& u : x.units) {
                w_unit(w, u.unit); w.str(u.cls); w.f64(u.hp); w.f64(u.ap); w.i32(u.shield); w.u64(static_cast<uint64_t>(u.break_bar)); w.boolean(u.dead);
                w.u16(static_cast<uint16_t>(u.buffs.size()));
                for (auto& b : u.buffs) { w.str(b.cls); w.i32(b.stacks); w.i32(b.turns); w.boolean(b.permanent); }
            }
            w.i32(x.gradient_charges); w.f64(x.gradient_progress);
        } else if constexpr (std::is_same_v<T, BattleRewards>) {
            w.u32(x.battle_id); w.i32(x.xp); w.i32(x.gold);
            w.u16(static_cast<uint16_t>(x.loot.size()));
            for (auto& l : x.loot) { w.str(l.item); w.i32(l.quantity); }
        } else {
            static_assert(always_false<T>, "message non géré");
        }
    }, m);
}

std::optional<Message> decode_payload(MsgType t, wire::Reader& r, std::string& err)
{
    auto finish = [&](Message m) -> std::optional<Message> {
        if (!r.ok()) { err = std::string("truncated or invalid ") + type_name(t); return std::nullopt; }
        if (!r.at_end()) { err = std::string("trailing bytes in ") + type_name(t); return std::nullopt; }
        return m;
    };
    switch (t) {
    case MsgType::Hello: {
        Hello x;
        x.protocol = r.u16(); x.game_build = r.u32(); x.mod_version = r.str(64);
        const uint16_t n = r.count(kMaxList);
        for (uint16_t i = 0; i < n && r.ok(); ++i) x.gameplay_mods.push_back(r.str(128));
        x.player_name = r.str(64); x.instance_id = r.u64();
        return finish(x);
    }
    case MsgType::Welcome: { Welcome x; x.player_id = r.u8(); x.session_id = r.u32(); if (x.player_id > 3) r.fail(); return finish(x); }
    case MsgType::Reject: {
        Reject x; const uint8_t k = r.u8(); if (k < 1 || k > 6) r.fail();
        x.reason = static_cast<RejectReason>(k); x.detail = r.str(kMaxStr); return finish(x);
    }
    case MsgType::Bye: { Bye x; x.reason = r.str(kMaxStr); return finish(x); }
    case MsgType::Ping: { Ping x; x.t_us = r.u64(); return finish(x); }
    case MsgType::Pong: { Pong x; x.t_us = r.u64(); return finish(x); }
    case MsgType::BattleStart: {
        BattleStart x; x.battle_id = r.u32(); x.encounter = r.str(128);
        const uint16_t n = r.count(8);
        for (uint16_t i = 0; i < n && r.ok(); ++i) { SpotOwner o; o.spot = r.u8(); o.character = r.str(64); o.player_id = r.u8(); if (o.player_id > 3) r.fail(); x.owners.push_back(o); }
        x.rng_seed = r.u64();
        const uint16_t np = r.count(8);
        for (uint16_t i = 0; i < np && r.ok(); ++i) x.party.push_back(r.str(64));
        return finish(x);
    }
    case MsgType::TurnStart: {
        TurnStart x; x.battle_id = r.u32(); x.turn = r.u32(); const uint8_t k = r.u8(); if (!valid_kind(k)) r.fail();
        x.kind = static_cast<ActorKind>(k); x.index = r.u8(); x.actor = r.str(128); return finish(x);
    }
    case MsgType::ActionCommand: {
        ActionCommand x; x.battle_id = r.u32(); x.turn = r.u32(); x.actor_spot = r.u8(); x.action_type = r.u8();
        if (x.action_type > 6) r.fail();
        x.secondary = r.str(128);
        const uint16_t n = r.count(kMaxUnits);
        for (uint16_t i = 0; i < n && r.ok(); ++i) x.targets.push_back(r_unit(r));
        x.combo = r_u8list(r);
        for (auto c : x.combo) if (c > 3) r.fail();
        return finish(x);
    }
    case MsgType::ComboResult: {
        ComboResult x; x.battle_id = r.u32(); x.turn = r.u32(); x.spot = r.u8(); x.seq = r.u16(); x.success = r.boolean(); x.perfect = r.boolean();
        if (x.perfect && !x.success) r.fail();
        return finish(x);
    }
    case MsgType::DefenseRequest: {
        DefenseRequest x; x.battle_id = r.u32(); x.turn = r.u32(); x.spot = r.u8(); x.hit = r.u16(); x.jumpable = r.u8(); x.timeout_ms = r.u16();
        if (x.jumpable > 1) r.fail();
        return finish(x);
    }
    case MsgType::DefenseResult: {
        DefenseResult x; x.battle_id = r.u32(); x.turn = r.u32(); x.spot = r.u8(); x.hit = r.u16(); const uint8_t d = r.u8();
        if (d > 5) r.fail();
        x.result = static_cast<Defense>(d); return finish(x);
    }
    case MsgType::StateHash: { StateHash x; x.battle_id = r.u32(); x.turn = r.u32(); x.hash = r.u64(); return finish(x); }
    case MsgType::BattleEnd: { BattleEnd x; x.battle_id = r.u32(); x.result = r.u8(); if (x.result > 3) r.fail(); return finish(x); }
    case MsgType::Draws: {
        Draws x; x.battle_id = r.u32(); x.turn = r.u32();
        const uint16_t n = r.count(kMaxList);
        for (uint16_t i = 0; i < n && r.ok(); ++i) { Draw d; d.key = r.u32(); d.rank = r.u16(); d.value = r.f64(); x.draws.push_back(d); }
        return finish(x);
    }
    case MsgType::StateSync: {
        StateSync x; x.battle_id = r.u32(); x.turn = r.u32(); x.reason = r.u8();
        if (x.reason > 1) r.fail();
        const uint16_t n = r.count(kMaxUnits);
        for (uint16_t i = 0; i < n && r.ok(); ++i) {
            UnitState u;
            u.unit = r_unit(r); u.cls = r.str(128); u.hp = r.f64(); u.ap = r.f64(); u.shield = r.i32();
            u.break_bar = static_cast<int64_t>(r.u64()); u.dead = r.boolean();
            const uint16_t nb = r.count(kMaxBuffs);
            for (uint16_t j = 0; j < nb && r.ok(); ++j) { BuffState b; b.cls = r.str(128); b.stacks = r.i32(); b.turns = r.i32(); b.permanent = r.boolean(); u.buffs.push_back(b); }
            x.units.push_back(std::move(u));
        }
        x.gradient_charges = r.i32(); x.gradient_progress = r.f64();
        return finish(x);
    }
    case MsgType::BattleRewards: {
        BattleRewards x; x.battle_id = r.u32(); x.xp = r.i32(); x.gold = r.i32();
        const uint16_t n = r.count(kMaxList);
        for (uint16_t i = 0; i < n && r.ok(); ++i) { LootEntry l; l.item = r.str(128); l.quantity = r.i32(); x.loot.push_back(l); }
        return finish(x);
    }
    }
    err = "unknown message type " + std::to_string(static_cast<unsigned>(t));
    return std::nullopt;
}

} // namespace

const char* type_name(MsgType t)
{
    switch (t) {
    case MsgType::Hello: return "Hello";
    case MsgType::Welcome: return "Welcome";
    case MsgType::Reject: return "Reject";
    case MsgType::Bye: return "Bye";
    case MsgType::Ping: return "Ping";
    case MsgType::Pong: return "Pong";
    case MsgType::BattleStart: return "BattleStart";
    case MsgType::TurnStart: return "TurnStart";
    case MsgType::ActionCommand: return "ActionCommand";
    case MsgType::DefenseRequest: return "DefenseRequest";
    case MsgType::DefenseResult: return "DefenseResult";
    case MsgType::StateHash: return "StateHash";
    case MsgType::BattleEnd: return "BattleEnd";
    case MsgType::Draws: return "Draws";
    case MsgType::ComboResult: return "ComboResult";
    case MsgType::StateSync: return "StateSync";
    case MsgType::BattleRewards: return "BattleRewards";
    }
    return "?";
}

MsgType type_of(const Message& m)
{
    static constexpr MsgType order[] = {MsgType::Hello, MsgType::Welcome, MsgType::Reject, MsgType::Bye, MsgType::Ping, MsgType::Pong,
                                        MsgType::BattleStart, MsgType::TurnStart, MsgType::ActionCommand,
                                        MsgType::DefenseRequest, MsgType::DefenseResult, MsgType::StateHash, MsgType::BattleEnd, MsgType::Draws,
                                        MsgType::ComboResult, MsgType::StateSync, MsgType::BattleRewards};
    static_assert(std::size(order) == std::variant_size_v<Message>);
    return order[m.index()];
}

void encode(const Message& m, wire::Writer& w)
{
    wire::Writer payload;
    encode_payload(m, payload);
    w.u16(static_cast<uint16_t>(type_of(m)));
    w.u16(static_cast<uint16_t>(payload.size()));
    w.bytes(payload.data().data(), payload.size());
}

std::optional<Message> decode(wire::Reader& r, std::string& err)
{
    const uint16_t type = r.u16();
    const uint16_t len = r.u16();
    if (!r.ok() || len > r.remaining()) { err = "bad message header"; r.fail(); return std::nullopt; }
    // Le contenu est lu dans un sous-lecteur borné à sa longueur annoncée.
    std::vector<uint8_t> body(len);
    for (auto& b : body) b = r.u8();
    wire::Reader sub(body.data(), body.size());
    return decode_payload(static_cast<MsgType>(type), sub, err);
}

static std::string unit_json(const Unit& u) { return JsonObj().add("k", static_cast<int>(u.kind)).add("s", static_cast<int>(u.spot)).str(); }

std::string to_json(const Message& m)
{
    JsonObj o;
    o.add("type", type_name(type_of(m)));
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        auto list = [](const std::vector<uint8_t>& v) { std::string s = "["; for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); } return s + "]"; };
        if constexpr (std::is_same_v<T, Hello>) {
            std::string mods = "[";
            for (size_t i = 0; i < x.gameplay_mods.size(); ++i) { if (i) mods += ","; mods += "\"" + json_escape(x.gameplay_mods[i]) + "\""; }
            o.add("protocol", static_cast<int>(x.protocol)).add("game_build", x.game_build).add("mod_version", x.mod_version).raw("mods", mods + "]").add("player", x.player_name);
        } else if constexpr (std::is_same_v<T, Welcome>) { o.add("player_id", static_cast<int>(x.player_id)).add("session", x.session_id); }
        else if constexpr (std::is_same_v<T, Reject>) { o.add("reason", static_cast<int>(x.reason)).add("detail", x.detail); }
        else if constexpr (std::is_same_v<T, Bye>) { o.add("reason", x.reason); }
        else if constexpr (std::is_same_v<T, Ping> || std::is_same_v<T, Pong>) { o.add("t_us", x.t_us); }
        else if constexpr (std::is_same_v<T, BattleStart>) {
            std::string ow = "[";
            for (size_t i = 0; i < x.owners.size(); ++i) { if (i) ow += ","; ow += JsonObj().add("spot", static_cast<int>(x.owners[i].spot)).add("char", x.owners[i].character).add("player", static_cast<int>(x.owners[i].player_id)).str(); }
            std::string party = "[";
            for (size_t i = 0; i < x.party.size(); ++i) { if (i) party += ","; party += "\"" + json_escape(x.party[i]) + "\""; }
            o.add("battle", x.battle_id).add("encounter", x.encounter).raw("owners", ow + "]").add("seed", static_cast<int64_t>(x.rng_seed)).raw("party", party + "]");
        } else if constexpr (std::is_same_v<T, TurnStart>) {
            o.add("battle", x.battle_id).add("turn", x.turn).add("kind", static_cast<int>(x.kind)).add("index", static_cast<int>(x.index)).add("actor", x.actor);
        } else if constexpr (std::is_same_v<T, ActionCommand>) {
            std::string ts = "[";
            for (size_t i = 0; i < x.targets.size(); ++i) { if (i) ts += ","; ts += unit_json(x.targets[i]); }
            o.add("battle", x.battle_id).add("turn", x.turn).add("spot", static_cast<int>(x.actor_spot)).add("action", static_cast<int>(x.action_type))
                .add("secondary", x.secondary).raw("targets", ts + "]").raw("combo", list(x.combo));
        } else if constexpr (std::is_same_v<T, ComboResult>) {
            o.add("battle", x.battle_id).add("turn", x.turn).add("spot", static_cast<int>(x.spot)).add("seq", static_cast<int>(x.seq)).add("success", x.success).add("perfect", x.perfect);
        } else if constexpr (std::is_same_v<T, DefenseRequest>) {
            o.add("battle", x.battle_id).add("turn", x.turn).add("spot", static_cast<int>(x.spot)).add("hit", static_cast<int>(x.hit)).add("jumpable", static_cast<int>(x.jumpable)).add("timeout_ms", static_cast<int>(x.timeout_ms));
        } else if constexpr (std::is_same_v<T, DefenseResult>) {
            o.add("battle", x.battle_id).add("turn", x.turn).add("spot", static_cast<int>(x.spot)).add("hit", static_cast<int>(x.hit)).add("result", static_cast<int>(x.result));
        } else if constexpr (std::is_same_v<T, StateHash>) {
            char h[24]; std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(x.hash));
            o.add("battle", x.battle_id).add("turn", x.turn).add("hash", std::string_view(h));
        } else if constexpr (std::is_same_v<T, BattleEnd>) {
            o.add("battle", x.battle_id).add("result", static_cast<int>(x.result));
        } else if constexpr (std::is_same_v<T, Draws>) {
            std::string ds = "[";
            for (size_t i = 0; i < x.draws.size(); ++i) { if (i) ds += ","; ds += JsonObj().add("key", static_cast<int64_t>(x.draws[i].key)).add("rank", static_cast<int>(x.draws[i].rank)).add("value", x.draws[i].value).str(); }
            o.add("battle", x.battle_id).add("turn", x.turn).raw("draws", ds + "]");
        } else if constexpr (std::is_same_v<T, StateSync>) {
            std::string us = "[";
            for (size_t i = 0; i < x.units.size(); ++i) {
                const auto& u = x.units[i];
                if (i) us += ",";
                std::string bs = "[";
                for (size_t j = 0; j < u.buffs.size(); ++j) { if (j) bs += ","; bs += JsonObj().add("c", u.buffs[j].cls).add("n", u.buffs[j].stacks).add("t", u.buffs[j].turns).add("p", u.buffs[j].permanent).str(); }
                us += JsonObj().raw("u", unit_json(u.unit)).add("c", u.cls).add("hp", u.hp).add("ap", u.ap).add("shield", u.shield)
                          .add("break", u.break_bar).add("dead", u.dead).raw("buffs", bs + "]").str();
            }
            o.add("battle", x.battle_id).add("turn", x.turn).add("reason", static_cast<int>(x.reason)).raw("units", us + "]")
                .add("gradient_charges", x.gradient_charges).add("gradient_progress", x.gradient_progress);
        } else if constexpr (std::is_same_v<T, BattleRewards>) {
            std::string ls = "[";
            for (size_t i = 0; i < x.loot.size(); ++i) { if (i) ls += ","; ls += JsonObj().add("item", x.loot[i].item).add("qty", x.loot[i].quantity).str(); }
            o.add("battle", x.battle_id).add("xp", x.xp).add("gold", x.gold).raw("loot", ls + "]");
        }
    }, m);
    return o.str();
}

} // namespace e33::proto
