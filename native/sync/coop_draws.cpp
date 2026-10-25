// Tirages de gameplay communs (D10) et mise en place du combat (D30).
#include "coop_internal.hpp"

namespace e33::coop::detail {

// --- Tirages de gameplay (D10) ------------------------------------------------------------

bool on_game_thread() { return s.game_thread != 0 && GetCurrentThreadId() == s.game_thread; }

// Appelant Blueprint d'une fonction native hookée. Le cadre fourni par UE4SS est celui de la fonction
// elle-même (objet = CDO de KismetMathLibrary, vérifié en jeu le 2026-10-19) : on remonte PreviousFrame
// jusqu'au premier objet qui n'est pas ce CDO.
UObject* script_caller(HookCtx& ctx, UFunction** node_out)
{
    RC::Unreal::FFrame* f = &ctx.TheStack;
    for (int depth = 0; f && depth < 4; ++depth) {
        UObject* o = f->Object();
        if (o && o != ctx.Context) {
            if (node_out) *node_out = f->Node();
            return o;
        }
        RC::Unreal::FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    return nullptr;
}

// Fenêtre de mise en place ouverte (D30). Sécurité : jamais plus de 120 s (un combat annulé ne la laisse pas ouverte
// en exploration, où les tirages ne doivent jamais être touchés).
bool setup_window()
{
    return s.setup.open && s.now - s.setup.opened_at < 120.0;
}

// Diagnostic : chaîne des cadres de script d'un tirage (5 premiers appels par fonction, puis 600 par combat, et 400
// pendant la mise en place).
void trace_random(HookCtx& ctx, const std::string& fname)
{
    static std::map<std::string, int> seen;
    const bool early = seen[fname]++ < 5;
    const bool in_battle = s.battle.active && s.battle.traced_draws < 600;
    const bool in_setup = !in_battle && setup_window() && s.setup.traced < 400;
    if (!early && !in_battle && !in_setup) return;
    if (in_battle) ++s.battle.traced_draws;
    if (in_setup) ++s.setup.traced;
    std::string frames = "[";
    RC::Unreal::FFrame* f = &ctx.TheStack;
    for (int depth = 0; f && depth < 4; ++depth) {
        if (frames.size() > 1) frames += ",";
        frames += JsonObj().add("o", ue::full_name(f->Object())).add("n", ue::short_name(f->Node())).add("enemy", cached_enemy_spot(f->Object())).str();
        RC::Unreal::FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    log().write("rng_trace", JsonObj().add("fn", fname).add("battle", s.battle.active).add("setup", setup_window() && !(s.battle.active && s.battle.turn > 0)).raw("frames", frames + "]")
                                 .add("has_result", ctx.RESULT_DECL != nullptr), s.battle.turn);
}

uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Uniforme dans [0, 1), identique sur toutes les instances pour (graine, tour, flux, rang).
double derived_uniform_seed(uint64_t seed, uint32_t turn, uint32_t key, uint16_t rank)
{
    const uint64_t h = splitmix64(seed ^ splitmix64((static_cast<uint64_t>(turn) << 32) | key) ^ splitmix64(0x5EED0000ull + rank));
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

double derived_uniform(uint32_t turn, uint32_t key, uint16_t rank) { return derived_uniform_seed(s.battle.rng_seed, turn, key, rank); }

uint32_t fnv32(const std::string& str)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : str) { h ^= c; h *= 16777619u; }
    return h;
}

// Prochain tirage d'un flux : rang dans le tour, valeur uniforme.
double next_uniform(const std::string& stream, uint32_t& key, uint16_t& rank)
{
    key = fnv32(stream);
    rank = s.battle.draw_rank[key]++;
    s.battle.stream_names.emplace(key, stream);
    return derived_uniform(s.battle.turn, key, rank);
}

void record_draw(const std::string& stream, uint32_t key, uint16_t rank, double v, double a, double b)
{
    bump("gameplay_draws");
    s.battle.local_draws[s.battle.turn][(static_cast<uint64_t>(key) << 16) | rank] = v;
    log().write("coop_draw", JsonObj().add("stream", stream).add("rank", static_cast<int>(rank)).add("value", v).add("a", a).add("b", b), s.battle.turn);
    if (s.role == Role::Host) broadcast(proto::Draws{s.battle.id, s.battle.turn, {{key, rank, v}}});
}

// Nom du flux si le tirage vient du gameplay (premier appelant BP), vide sinon.
// Gameplay (traces du 2026-10-19, GAME-INTERNALS §15) : IA d'un ennemi, critiques, Luminas (BP_LU_*), activation de
// buffs, effets de buffs, scripts de compétence, récompenses (RollBattleRewards + système de butin), mécaniques propres
// des héros (composants *Mechanic* / StainHolder : roue de Monoco, taches de Lune…, D30).
// setup : mise en place du combat (avant le tour 1) : le cache des ennemis est celui du combat précédent chez l'hôte,
// il n'est pas consulté (même nom de flux partout).
std::string gameplay_stream(HookCtx& ctx, const std::string& fname, bool setup)
{
    UFunction* node = nullptr;
    UObject* caller = script_caller(ctx, &node);
    if (!caller) return {};
    const int es = setup ? -1 : cached_enemy_spot(caller);
    const std::string cls = game::char_class(caller);
    const std::string fn = ue::short_name(node);
    // Mise en place : les ennemis ne sont pas encore dans le cache (rempli au tour 1). Vu en vrai le 2026-10-24 : le
    // Portier tire un RandomBool à son OnBattleStart qui décide de ses attaques (zone chez l'hôte, un seul héros chez le
    // client). Flux par classe, sans emplacement (peut-être pas encore attribué) : l'ordre des appels départage.
    const bool setup_enemy = setup && ue::is_a(caller, STR("BP_jRPG_Enemy_Battle_Base_C"));
    if (setup_enemy) bump("setup_enemy_draws");
    const bool gameplay = es >= 0 || setup_enemy || cls == "BP_BattleDamages_C" || cls.rfind("BP_LU_", 0) == 0 || cls == "AC_jRPG_CharacterBattleStats_C" ||
                          cls.rfind("BP_BattleBuff", 0) == 0 || cls.rfind("BP_LootSystemComponent", 0) == 0 ||
                          cls.find("Mechanic") != std::string::npos || cls.find("StainHolder") != std::string::npos ||
                          (cls == "AC_jRPG_BattleManager_C" && fn == "RollBattleRewards") || ue::is_a(caller, STR("BP_Battle_SkillScript_C"));
    if (!gameplay) return {};
    std::string name = cls + ":" + fn + ":" + fname;
    if (es >= 0) name += "#" + std::to_string(es);   // un flux par ennemi (même classe possible)
    return name;
}

// Tirage de la mise en place (D30) : graine de la fenêtre, « tour 0 », rang propre à la fenêtre.
double next_setup_uniform(const std::string& stream, uint32_t& key, uint16_t& rank)
{
    key = fnv32(stream);
    rank = s.setup.rank[key]++;
    s.setup.names.emplace(key, stream);
    return derived_uniform_seed(s.setup.seed, 0, key, rank);
}

void record_setup_draw(const std::string& stream, uint32_t key, uint16_t rank, double v, double a, double b)
{
    bump("setup_draws_replaced");
    s.setup.draws[(static_cast<uint64_t>(key) << 16) | rank] = v;
    log().write("coop_draw", JsonObj().add("stream", stream).add("rank", static_cast<int>(rank)).add("value", v).add("a", a).add("b", b).add("setup", true), 0);
}

// Remplace la valeur de retour par la valeur dérivée ; l'hôte la publie pour vérification (mise en place : publiée
// avec le BattleStart, voir host_turn_start).
void replace_draw(HookCtx& ctx, UFunction* f, RandomKind kind, const std::string& stream, bool setup)
{
    uint32_t key = 0;
    uint16_t rank = 0;
    const double u = setup ? next_setup_uniform(stream, key, rank) : next_uniform(stream, key, rank);
    auto num = [&](const wchar_t* name, double& out) {
        int size = 0;
        void* p = ue::param_address(ctx, f, name, &size);
        if (!p) return false;
        if (kind == RandomKind::IntInRange || kind == RandomKind::Int) out = size == 4 ? *static_cast<int32_t*>(p) : static_cast<double>(*static_cast<int64_t*>(p));
        else out = size == 4 ? *static_cast<float*>(p) : *static_cast<double*>(p);
        return true;
    };
    double v = 0, a = 0, b = 0;
    bool ok = true;
    switch (kind) {
    case RandomKind::IntInRange: {
        ok = num(STR("Min"), a) && num(STR("Max"), b);
        const int64_t range = static_cast<int64_t>(b) - static_cast<int64_t>(a) + 1;   // FMath::RandRange : bornes incluses
        v = a + (range > 0 ? std::min<int64_t>(static_cast<int64_t>(u * range), range - 1) : 0);
        if (ok) *static_cast<int32_t*>(ctx.RESULT_DECL) = static_cast<int32_t>(v);
        break;
    }
    case RandomKind::Int:
        ok = num(STR("Max"), b);
        v = b > 0 ? std::min(std::floor(u * b), b - 1) : 0;
        if (ok) *static_cast<int32_t*>(ctx.RESULT_DECL) = static_cast<int32_t>(v);
        break;
    case RandomKind::Float:
        v = u;
        *static_cast<double*>(ctx.RESULT_DECL) = v;
        break;
    case RandomKind::FloatInRange:
        ok = num(STR("Min"), a) && num(STR("Max"), b);
        v = a + (b - a) * u;
        if (ok) *static_cast<double*>(ctx.RESULT_DECL) = v;
        break;
    case RandomKind::Bool:
        v = u < 0.5 ? 1 : 0;
        *static_cast<uint8_t*>(ctx.RESULT_DECL) = static_cast<uint8_t>(v);
        break;
    case RandomKind::BoolWeight:
        ok = num(STR("Weight"), a);
        v = (a > 0 && u <= a) ? 1 : 0;
        if (ok) *static_cast<uint8_t*>(ctx.RESULT_DECL) = static_cast<uint8_t>(v);
        break;
    }
    if (!ok) {
        bump("draw_errors");
        log().write("coop_draw_param_missing", JsonObj().add("stream", stream), s.battle.turn);
        return;
    }
    if (setup) record_setup_draw(stream, key, rank, v, a, b);
    else record_draw(stream, key, rank, v, a, b);
}

bool draw_allowed(const std::string& stream)
{
    if (s.draw_filter.empty()) return true;
    for (auto& f : s.draw_filter) {
        if (f == "none") return false;
        if (stream.find(f) != std::string::npos) return true;
    }
    return false;
}

double natural_value(HookCtx& ctx, RandomKind kind)
{
    switch (kind) {
    case RandomKind::IntInRange: case RandomKind::Int: return *static_cast<int32_t*>(ctx.RESULT_DECL);
    case RandomKind::Float: case RandomKind::FloatInRange: return *static_cast<double*>(ctx.RESULT_DECL);
    default: return *static_cast<uint8_t*>(ctx.RESULT_DECL) ? 1.0 : 0.0;
    }
}

// Client : compare les tirages de gameplay d'un tour terminé avec ceux de l'hôte (même flux, même rang, même valeur).
void compare_draws(uint32_t turn)
{
    auto& mine = s.battle.local_draws[turn];
    auto& host = s.battle.host_draws[turn];
    auto name = [](uint64_t k) {
        auto it = s.battle.stream_names.find(static_cast<uint32_t>(k >> 16));
        return it != s.battle.stream_names.end() ? it->second : std::to_string(k >> 16);
    };
    // Nombre de tirages par flux de chaque côté : un tirage au-delà du dernier rang de l'autre côté vient d'un
    // coup en plus (le nombre de coups d'une compétence varie d'un run à l'autre, même en solo : GAME-INTERNALS §15),
    // pas d'un tirage faux. Compté à part (draw_count_diffs) ; l'état complet de l'hôte corrige ses effets.
    std::map<uint32_t, uint16_t> mine_n, host_n;
    for (auto& [k, v] : mine) { auto& n = mine_n[static_cast<uint32_t>(k >> 16)]; n = std::max<uint16_t>(n, static_cast<uint16_t>((k & 0xFFFF) + 1)); }
    for (auto& [k, v] : host) { auto& n = host_n[static_cast<uint32_t>(k >> 16)]; n = std::max<uint16_t>(n, static_cast<uint16_t>((k & 0xFFFF) + 1)); }
    auto tail = [](const std::map<uint32_t, uint16_t>& other, uint64_t k) {
        auto it = other.find(static_cast<uint32_t>(k >> 16));
        return (k & 0xFFFF) >= (it == other.end() ? 0u : it->second);
    };
    for (auto& [k, v] : mine) {
        auto it = host.find(k);
        if (it == host.end() && tail(host_n, k)) {
            bump("draw_count_diffs");
            log().write("coop_draw_count_diff", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("extra", "local"), turn);
        } else if (it == host.end() || std::fabs(it->second - v) > 1e-9) {
            bump("draw_mismatches");
            log().write("coop_draw_mismatch", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("local", v)
                                                  .add("host_has", it != host.end()).add("host", it != host.end() ? it->second : 0.0), turn);
        }
    }
    for (auto& [k, v] : host) {
        if (mine.count(k)) continue;
        if (tail(mine_n, k)) {
            bump("draw_count_diffs");
            log().write("coop_draw_count_diff", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("extra", "host"), turn);
            continue;
        }
        bump("draw_mismatches");
        log().write("coop_draw_mismatch", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("local_has", false).add("host", v), turn);
    }
    s.battle.local_draws.erase(turn);
    s.battle.host_draws.erase(turn);
}

// --- Mise en place du combat (D30) ---------------------------------------------------------

void setup_reset(uint64_t seed, bool seeded, const std::string& source)
{
    s.setup = SetupDraws{};
    s.setup.open = true;
    s.setup.opened_at = s.now;
    s.setup.seed = seed;
    s.setup.seeded = seeded;
    s.setup.source = source;
    log().write("coop_setup_open", JsonObj().add("seeded", seeded).add("source", source).add("seed", std::to_string(seed)).add("enabled", s.setup_draws));
}

// Hôte : graine du prochain combat choisie dès son lancement (avis précoce d'un combat de la carte, ou StartBattleNEW,
// commun à tous les lancements). host_turn_start la reprend pour le BattleStart : mise en place et tours, même graine.
void host_setup_open(bool notice)
{
    const auto& u = s.setup;
    const bool recent = u.open && s.now - u.opened_at < 30.0;
    // Avis précoce : StartBattleNEW a pu passer juste avant (même combat). StartBattleNEW : fenêtre gardée seulement si
    // elle vient de l'avis précoce de ce combat.
    const bool keep = recent && (notice ? true : (u.from_notice && !u.bm_started));
    if (!keep) {
        const uint64_t seed = s.fixed_seed ? *s.fixed_seed
                                           : splitmix64((static_cast<uint64_t>(GetTickCount64()) << 20) ^ s.session_id ^ (static_cast<uint64_t>(s.next_battle_id) << 40));
        setup_reset(seed, true, notice ? "host notice" : "host StartBattleNEW");
        s.setup.from_notice = notice;
    }
    if (!notice) s.setup.bm_started = true;
}

// Client : graine du combat de l'hôte, si elle est déjà connue au lancement du combat ici (combat lancé par le mod après
// le BattleStart, ou engagé par le pion suiveur après l'avis précoce). Sinon tirages laissés au jeu (réparés au tour 1).
void client_setup_open()
{
    const proto::BattleStart* p = nullptr;
    std::string src = "none";
    if (s.pending_start) { p = &*s.pending_start; src = "pending BattleStart"; }
    else if (s.last_start && s.last_start->battle_id == s.battle.id && s.battle.turn == 0 && !s.battle.finalized) { p = &*s.last_start; src = "BattleStart"; }
    else if (s.early_start && s.now - s.early_start_at < 30.0 && s.early_start->rng_seed != 0) { p = &*s.early_start; src = "notice"; }
    setup_reset(p ? p->rng_seed : 0, p && p->rng_seed != 0, src);
    s.setup.bm_started = true;
}

// Fin de la mise en place (tour 1, ou fin de combat) : ses tirages deviennent ceux du « tour 0 » du combat.
void setup_close()
{
    if (!s.setup.open) return;
    s.setup.open = false;
    for (auto& [k, v] : s.setup.draws) s.battle.local_draws[0][k] = v;
    for (auto& [k, n] : s.setup.names) s.battle.stream_names.emplace(k, n);
    log().write("coop_setup_closed", JsonObj().add("draws", static_cast<int>(s.setup.draws.size())).add("seeded", s.setup.seeded)
                                         .add("source", s.setup.source), s.battle.turn);
}

// Client, tour 1 : tirages de la mise en place comparés à ceux de l'hôte (publiés avec son BattleStart). Un hôte plus
// ancien (ou un enregistrement d'avant D30) n'en publie pas : rien à comparer.
void compare_setup_draws()
{
    auto& b = s.battle;
    if (!b.host_draws.count(0) || !s.setup_draws || !s.setup.seeded) {
        log().write("coop_setup_draws_unverified", JsonObj().add("local", static_cast<int>(b.local_draws[0].size())).add("host_published", b.host_draws.count(0) > 0)
                                                       .add("seeded", s.setup.seeded).add("enabled", s.setup_draws), b.turn);
        b.local_draws.erase(0);
        b.host_draws.erase(0);
        return;
    }
    compare_draws(0);
}

// Client, tour 1 : mécaniques propres des héros (roue de Monoco, taches de Lune…) comparées à l'hôte avant toute
// réparation. Chaque champ différent compte (start_mech_diffs) ; position de départ de la roue journalisée des deux côtés.
void check_start_mech(const proto::StateSync& mine, const proto::StateSync& host)
{
    for (const auto& h : host.units) {
        const proto::UnitState* m = nullptr;
        for (const auto& x : mine.units) if (x.unit.kind == h.unit.kind && x.unit.spot == h.unit.spot) { m = &x; break; }
        if (!m) continue;
        for (const auto& hf : h.mech) {
            const proto::MechField* mf = nullptr;
            for (const auto& x : m->mech) if (x.name == hf.name) { mf = &x; break; }
            const bool differs = !mf || mf->value != hf.value;
            if (differs) bump("start_mech_diffs");
            if (differs || hf.name.find("Starting_Position") != std::string::npos || hf.name.find("CurrentMeter") != std::string::npos)
                log().write("coop_start_mech", JsonObj().add("unit", h.cls).add("field", hf.name).add("host", hf.value)
                                                   .add("local", mf ? mf->value : -1).add("same", !differs), 1);
        }
    }
}

// Mécanique propre (champs des composants *Mechanic* / StainHolder) différente pour au moins un héros.
bool mech_differs(const proto::StateSync& mine, const proto::StateSync& host)
{
    for (const auto& h : host.units) {
        for (const auto& m : mine.units) {
            if (m.unit.kind != h.unit.kind || m.unit.spot != h.unit.spot) continue;
            if (m.mech.size() != h.mech.size()) return true;
            for (size_t i = 0; i < h.mech.size(); ++i)
                if (m.mech[i].name != h.mech[i].name || m.mech[i].value != h.mech[i].value) return true;
        }
    }
    return false;
}

// Tirage hors des tours d'un combat. Dans la fenêtre de mise en place, avec la graine : valeur dérivée (D30). Sinon,
// tirage de gameplay laissé au jeu, compté (setup_draws_natural) : il peut différer d'une machine à l'autre.
// Hors combat et hors fenêtre (exploration) : jamais touché ni compté.
void offturn_draw(HookCtx& ctx, UFunction* f, RandomKind kind, const std::string& fname)
{
    const bool window = setup_window();
    if (!window && !s.battle.active) return;
    const std::string stream = gameplay_stream(ctx, fname, true);
    if (stream.empty()) return;
    if (window && s.setup.seeded && s.setup_draws && draw_allowed(stream)) { replace_draw(ctx, f, kind, stream, true); return; }
    bump("setup_draws_natural");
    if (++s.setup.natural_logs <= 40)
        log().write("coop_draw_offturn", JsonObj().add("stream", stream).add("value", natural_value(ctx, kind)).add("window", window)
                                             .add("seeded", s.setup.seeded).add("enabled", s.setup_draws), s.battle.turn);
}

} // namespace e33::coop::detail
