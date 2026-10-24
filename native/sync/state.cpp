#include "state.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"

using RC::Unreal::UObject;

namespace e33::sync {

namespace {

bool same_buff(const proto::BuffState& a, const proto::BuffState& b)
{
    return a.cls == b.cls && a.stacks == b.stacks && a.turns == b.turns && a.permanent == b.permanent;
}

bool buff_less(const proto::BuffState& a, const proto::BuffState& b)
{
    return std::tie(a.cls, a.stacks, a.turns, a.permanent) < std::tie(b.cls, b.stacks, b.turns, b.permanent);
}

std::string unit_key(const proto::Unit& u) { return std::string(u.kind == proto::ActorKind::Hero ? "H" : "E") + std::to_string(u.spot); }

const proto::UnitState* find(const proto::StateSync& st, const proto::Unit& u)
{
    for (auto& x : st.units) if (x.unit.kind == u.kind && x.unit.spot == u.spot) return &x;
    return nullptr;
}

bool same_mech(const std::vector<proto::MechField>& a, const std::vector<proto::MechField>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].name != b[i].name || a[i].value != b[i].value) return false;
    return true;
}

// Champs de a absents de b ou de valeur différente (log court).
std::string mech_str(const std::vector<proto::MechField>& a, const std::vector<proto::MechField>& b)
{
    std::string s;
    for (auto& f : a) {
        auto it = std::find_if(b.begin(), b.end(), [&](const proto::MechField& g) { return g.name == f.name; });
        if (it != b.end() && it->value == f.value) continue;
        s += (s.empty() ? "" : " ") + f.name + "=" + std::to_string(f.value);
    }
    return s;
}

std::string buffs_str(const std::vector<proto::BuffState>& v)
{
    std::string s;
    for (auto& b : v) s += (s.empty() ? "" : " ") + b.cls + "x" + std::to_string(b.stacks) + "t" + std::to_string(b.turns) + (b.permanent ? "p" : "");
    return s;
}

} // namespace

proto::Unit unit_of(UObject* ch)
{
    return {game::is_player(ch) ? proto::ActorKind::Hero : proto::ActorKind::Enemy, static_cast<uint8_t>(std::clamp(game::spot(ch), 0, 254))};
}

proto::StateSync snapshot(uint32_t battle_id, uint32_t turn)
{
    proto::StateSync st;
    st.battle_id = battle_id;
    st.turn = turn;
    auto add = [&](UObject* c) {
        UObject* s = game::stats(c);
        proto::UnitState u;
        u.unit = unit_of(c);
        u.cls = game::char_class(c);
        u.hp = ue::get_double(s, STR("CurrentHP"));
        u.ap = ue::get_double(s, STR("CurrentAP"));
        u.shield = static_cast<int32_t>(ue::get_int(s, STR("CurrentShieldPoints")));
        u.break_bar = ue::get_int(s, STR("StunBreakBarCurrent"));
        u.dead = game::is_dead_fast(c);
        for (auto& b : game::buffs(s)) {
            if (game::is_helper_buff(b)) continue;
            u.buffs.push_back({b.cls, b.stacks, b.turns, b.permanent});
            // Chemin de la classe : un client qui ne l'a jamais chargée (picto qu'il ne possède pas) la charge pour la réparation.
            if (b.instance && b.cls.rfind("BP_LU_", 0) == 0 || b.instance && b.cls.rfind("BP_BattleBuff", 0) == 0) {
                bool known = false;
                for (auto& [c, p] : st.class_paths) known = known || c == b.cls;
                if (!known && st.class_paths.size() < 200) {
                    const std::string full = ue::full_name(b.instance->GetClassPrivate());
                    const auto sp = full.find(' ');
                    if (sp != std::string::npos) st.class_paths.emplace_back(b.cls, full.substr(sp + 1));
                }
            }
        }
        std::sort(u.buffs.begin(), u.buffs.end(), buff_less);
        if (u.unit.kind == proto::ActorKind::Hero)
            for (auto& [name, value] : game::mechanic_fields(c)) u.mech.push_back({name, value});
        st.units.push_back(std::move(u));
    };
    auto heroes = game::players();
    auto foes = game::enemies();
    auto by_spot = [](UObject* a, UObject* b) { return game::spot(a) < game::spot(b); };
    std::sort(heroes.begin(), heroes.end(), by_spot);
    std::sort(foes.begin(), foes.end(), by_spot);
    for (UObject* c : heroes) add(c);
    for (UObject* c : foes) add(c);
    if (UObject* g = game::gradient_gauge()) {
        st.gradient_charges = static_cast<int32_t>(ue::get_int(g, STR("AvailableCharges")));
        st.gradient_progress = ue::get_double(g, STR("CurrentProgress"));
    }
    return st;
}

uint64_t hash(const proto::StateSync& st, proto::Unit actor)
{
    wire::Fnv64 h;
    for (auto& u : st.units) {
        // Ennemi mort : compté comme absent. Le jeu le retire de BM.Enemies, parfois pas au même instant partout (et
        // un ennemi tué ici par la réparation d'un écart « extra » peut y rester le temps de sa mort).
        if (u.unit.kind == proto::ActorKind::Enemy && u.dead) continue;
        h.i64(static_cast<int64_t>(u.unit.kind));
        h.i64(u.unit.spot);
        h.i64(std::llround(u.hp));
        h.i64(std::llround(u.ap));
        h.i64(u.shield);
        h.i64(u.break_bar);
        h.i64(u.dead ? 1 : 0);
        h.i64(static_cast<int64_t>(u.buffs.size()));
        for (auto& b : u.buffs) { h.str(b.cls); h.i64(b.stacks); h.i64(b.turns); h.i64(b.permanent ? 1 : 0); }
        h.i64(static_cast<int64_t>(u.mech.size()));
        for (auto& f : u.mech) { h.str(f.name); h.i64(f.value); }
    }
    h.i64(st.gradient_charges);
    h.i64(std::llround(st.gradient_progress * 1000));
    h.i64(static_cast<int64_t>(actor.kind));
    h.i64(actor.spot);
    return h.value();
}

std::string diff(const proto::StateSync& local, const proto::StateSync& host)
{
    std::string out = "[";
    auto item = [&](const std::string& unit, const std::string& field, const std::string& l, const std::string& r) {
        if (out.size() > 1) out += ",";
        out += JsonObj().add("u", unit).add("f", field).add("local", l).add("host", r).str();
    };
    auto num = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return std::string(b); };
    for (auto& hu : host.units) {
        const auto* lu = find(local, hu.unit);
        const std::string k = unit_key(hu.unit) + ":" + hu.cls;
        if (!lu) { if (!(hu.unit.kind == proto::ActorKind::Enemy && hu.dead)) item(k, "missing", "-", "present"); continue; }
        if (hu.unit.kind == proto::ActorKind::Enemy && hu.dead && lu->dead) continue;   // ennemi mort des deux côtés : hors hash
        if (std::llround(lu->hp) != std::llround(hu.hp)) item(k, "hp", num(lu->hp), num(hu.hp));
        if (std::llround(lu->ap) != std::llround(hu.ap)) item(k, "ap", num(lu->ap), num(hu.ap));
        if (lu->shield != hu.shield) item(k, "shield", std::to_string(lu->shield), std::to_string(hu.shield));
        if (lu->break_bar != hu.break_bar) item(k, "break", std::to_string(lu->break_bar), std::to_string(hu.break_bar));
        if (lu->dead != hu.dead) item(k, "dead", lu->dead ? "1" : "0", hu.dead ? "1" : "0");
        bool buffs_equal = lu->buffs.size() == hu.buffs.size();
        for (size_t i = 0; buffs_equal && i < hu.buffs.size(); ++i) buffs_equal = same_buff(lu->buffs[i], hu.buffs[i]);
        if (!buffs_equal) item(k, "buffs", buffs_str(lu->buffs), buffs_str(hu.buffs));
        if (!same_mech(lu->mech, hu.mech)) item(k, "mech", mech_str(lu->mech, hu.mech), mech_str(hu.mech, lu->mech));
    }
    for (auto& lu : local.units)
        if (!find(host, lu.unit) && !(lu.unit.kind == proto::ActorKind::Enemy && lu.dead)) item(unit_key(lu.unit) + ":" + lu.cls, "extra", "present", "-");
    if (local.gradient_charges != host.gradient_charges || std::llround(local.gradient_progress * 1000) != std::llround(host.gradient_progress * 1000))
        item("gradient", "gauge", std::to_string(local.gradient_charges) + "/" + num(local.gradient_progress),
             std::to_string(host.gradient_charges) + "/" + num(host.gradient_progress));
    return out + "]";
}

RepairStats repair(const proto::StateSync& local, const proto::StateSync& host)
{
    for (auto& [c, p] : host.class_paths) game::note_class_path(c, p);   // classes de buff à charger si absentes ici
    RepairStats r;
    for (auto& hu : host.units) {
        const auto* lu = find(local, hu.unit);
        UObject* ch = game::find_unit(hu.unit.kind == proto::ActorKind::Hero, hu.unit.spot);
        UObject* st = game::stats(ch);
        if (!lu && hu.unit.kind == proto::ActorKind::Enemy && hu.dead) continue;   // déjà retiré ici : rien à faire
        if (!lu || !st) { ++r.failed; continue; }
        if (hu.unit.kind == proto::ActorKind::Enemy && hu.dead && lu->dead) continue;
        if (lu->dead != hu.dead) {
            ++r.death;
            if (hu.dead) {
                ue::Call c(st, STR("SetDead"));
                if (!c || !c.invoke()) ++r.failed;
            } else {
                ue::Call c(st, STR("Resurrect"));
                if (!c) ++r.failed;
                else c.set_double(STR("HP Amount"), hu.hp).set_object(STR("SourceCharacter"), nullptr).invoke();
            }
        }
        if (std::llround(lu->hp) != std::llround(hu.hp)) { ++r.hp; ue::set_double(st, STR("CurrentHP"), hu.hp); }
        if (std::llround(lu->ap) != std::llround(hu.ap)) { ++r.ap; ue::set_double(st, STR("CurrentAP"), hu.ap); }
        if (lu->shield != hu.shield) {
            ++r.shield;
            ue::Call c(st, STR("SetCurrentShieldPoints"));
            if (c) c.set_int(STR("NewValue"), hu.shield).set_string(STR("DebugReason"), STR("E33Coop resync")).invoke();
            else ue::set_int(st, STR("CurrentShieldPoints"), hu.shield);
        }
        if (!hu.mech.empty() && !same_mech(lu->mech, hu.mech)) {
            std::vector<std::pair<std::string, int64_t>> want;
            for (auto& f : hu.mech) want.emplace_back(f.name, f.value);
            int failed = 0;
            r.mech += game::set_mechanic_fields(ch, want, failed);
            r.failed += failed;
        }
        if (lu->break_bar != hu.break_bar) {
            ++r.breakbar;
            ue::Call c(st, STR("SetCurrentBreakBarDamage"));
            if (c) c.set_int(STR("NewValue"), hu.break_bar).set_object(STR("SourceCharacter"), nullptr).set_string(STR("Reason"), STR("E33Coop resync")).invoke();
            else ue::set_int(st, STR("StunBreakBarCurrent"), hu.break_bar);
        }
        // Buffs : par classe, on apparie les instances (triées), on corrige piles et durées, on retire les
        // instances en trop et on crée celles qui manquent.
        bool buffs_equal = lu->buffs.size() == hu.buffs.size();
        for (size_t i = 0; buffs_equal && i < hu.buffs.size(); ++i) buffs_equal = same_buff(lu->buffs[i], hu.buffs[i]);
        if (buffs_equal) continue;
        std::map<std::string, std::vector<game::Buff>> mine;
        for (auto& b : game::buffs(st)) if (!game::is_helper_buff(b)) mine[b.cls].push_back(b);
        std::map<std::string, std::vector<proto::BuffState>> want;
        for (auto& b : hu.buffs) want[b.cls].push_back(b);
        for (auto& [cls, list] : mine) {
            auto& w = want[cls];
            std::sort(list.begin(), list.end(), [](const game::Buff& a, const game::Buff& b) { return std::tie(a.stacks, a.turns, a.permanent) < std::tie(b.stacks, b.turns, b.permanent); });
            for (size_t i = 0; i < list.size(); ++i) {
                if (i < w.size()) {
                    if (list[i].stacks != w[i].stacks) { ue::set_int(list[i].instance, STR("StackCount"), w[i].stacks); ++r.buffs; }
                    if (list[i].turns != w[i].turns) { ue::set_int(list[i].instance, STR("TurnDuration"), w[i].turns); ++r.buffs; }
                    if (list[i].permanent != w[i].permanent) { ue::set_bool(list[i].instance, STR("IsPermanent"), w[i].permanent); ++r.buffs; }
                } else {
                    ++r.buffs;
                    if (!game::remove_buff(st, list[i].instance)) ++r.failed;
                }
            }
        }
        for (auto& [cls, w] : want) {
            const size_t have = mine.count(cls) ? mine[cls].size() : 0;
            for (size_t i = have; i < w.size(); ++i) {
                ++r.buffs;
                std::string err;
                if (!game::add_buff(st, cls, w[i].stacks, w[i].turns, w[i].permanent, err)) {
                    ++r.failed;
                    log().write("coop_repair_error", JsonObj().add("what", err).add("unit", unit_key(hu.unit)));
                }
            }
        }
    }
    // Ennemi vivant ici, absent chez l'hôte : l'hôte l'a tué (le jeu retire les morts de BM.Enemies). Mis à mort ici aussi,
    // sinon le combat continue ici après la victoire de l'hôte (Lancelier, essai à deux PC du 2026-10-22). Réparation
    // faite au début d'un tour, comme celle des morts de héros (SetDead) ; un héros ne quitte jamais la liste.
    for (auto& lu : local.units) {
        if (lu.unit.kind != proto::ActorKind::Enemy || lu.dead || find(host, lu.unit)) continue;
        ++r.death;
        if (!game::kill_unit(game::find_unit(false, lu.unit.spot))) ++r.failed;
    }
    if (local.gradient_charges != host.gradient_charges || std::llround(local.gradient_progress * 1000) != std::llround(host.gradient_progress * 1000)) {
        ++r.gradient;
        ue::Call c(game::gradient_gauge(), STR("SetCurrentGradientGauge"));
        if (!c) ++r.failed;
        else c.set_int(STR("NewCharges"), host.gradient_charges).set_double(STR("NewProgress"), host.gradient_progress).set_string(STR("Reason"), STR("E33Coop resync")).invoke();
    }
    return r;
}

std::string repair_json(const RepairStats& r)
{
    return JsonObj().add("hp", r.hp).add("ap", r.ap).add("shield", r.shield).add("break", r.breakbar).add("death", r.death)
        .add("buffs", r.buffs).add("mech", r.mech).add("gradient", r.gradient).add("failed", r.failed).str();
}

} // namespace e33::sync
