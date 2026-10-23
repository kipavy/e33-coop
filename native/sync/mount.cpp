#include "mount.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FMapProperty.hpp>
#include <Unreal/Property/FSetProperty.hpp>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/minijson.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/reflect.hpp"
#include "../hooks/ue.hpp"

namespace e33::mount {

namespace {

using RC::Unreal::UObject;

// EMovementMode (Engine_enums du dump)
constexpr int kWalk = 1, kNavWalk = 2, kFall = 3, kFly = 5, kCustom = 6;   // 0 aucun, 4 nage
constexpr double kEnterDelay = 0.3;   // mode du pilote (vol, custom) : quelques images, pas un saut isolé
constexpr double kExitDelay = 1.0;    // état reproductible tenu 1 s avant de rendre le pion
constexpr int kLogPerSecond = 10;     // journal des changements de mode : au plus 10 lignes par seconde

bool ground(int m) { return m == kWalk || m == kNavWalk || m == kFall; }

struct S {
    double now = 0;
    // Pilote : dernier état journalisé
    int p_move = -2, p_custom = -1, p_flags = -1;
    std::string p_cls;
    std::string other_cls;                 // pion possédé hors BP_jRPG_Character_World_C
    std::set<std::string> listed;          // classes dont les fonctions ont été journalisées
    // Suiveur
    bool ghost = false;
    UObject* pawn = nullptr;
    std::string reason;
    double since = -1;
    double want_since = -1, clear_since = -1;
    double followed_at = -1;
    bool have_tgt = false;
    double tgt[3]{};
    double last_t = -1;
    double next_mode_fix = 0;
    int mode_fixes = 0, teleports = 0;
    double next_dismount = 0;
    int dismount_tries = 0;
    // Suiveur : dernier état journalisé (pilote vu d'ici, et pion d'ici)
    int f_pmove = -2, f_pflags = -1, f_move = -2;
    bool f_mount = false;
    std::string f_pcls, f_cls;
    // Limite du journal
    double log_window = 0;
    int log_count = 0, log_dropped = 0;
} s;

bool log_allowed()
{
    if (s.now - s.log_window >= 1.0) {
        if (s.log_dropped > 0) log().write("coop_move_log_dropped", JsonObj().add("lines", s.log_dropped));
        s.log_window = s.now; s.log_count = 0; s.log_dropped = 0;
    }
    if (s.log_count >= kLogPerSecond) { ++s.log_dropped; return false; }
    ++s.log_count;
    return true;
}

std::string join(const std::vector<std::string>& v)
{
    std::string out;
    for (const auto& x : v) { if (!out.empty()) out += ","; out += x; }
    return out;
}

// Diagnostic pour la suite (monter sur Esquie chez le suiveur) : fonctions du pion et du contrôleur qui parlent de
// monture, de vol, de nage. Une fois par classe.
void list_functions(UObject* o, const char* owner)
{
    if (!o) return;
    const std::string cls = ue::short_name(o->GetClassPrivate());
    if (!s.listed.insert(cls).second) return;
    static const std::vector<std::string> needles = {"mount", "esquie", "fly", "flying", "swim", "takeoff", "land", "dive",
                                                     "capabilit", "capacit", "boost", "possess", "water"};
    const auto names = game::function_names(o, needles);
    log().write("coop_mount_functions", JsonObj().add("owner", owner).add("class", cls).add("count", static_cast<int>(names.size())).add("names", join(names)));
}

UObject* possessed_pawn()
{
    UObject* ctrl = game::controller();
    if (!ctrl) return nullptr;
    ue::Call c(ctrl, STR("K2_GetPawn"));
    if (!c || !c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

// Fantôme : caché, sans collision, sans gravité (vol). Le mod le place lui-même à chaque image.
void apply_ghost(UObject* pawn)
{
    game::set_pawn_visible(pawn, false);
    game::set_movement_mode(pawn, kFly);
}

void restore(UObject* pawn, const double* pos, double yaw)
{
    if (pos) game::teleport_pawn(pawn, pos, yaw);
    game::set_pawn_visible(pawn, true);
    game::set_movement_mode(pawn, kFall);   // retombe et se pose (marche ensuite, comme après un saut)
}

void end_ghost(UObject* pawn, const char* why, const double* pos, double yaw)
{
    if (pawn) restore(pawn, pos, yaw);
    log().write("coop_follower_ghost", JsonObj().add("on", false).add("why", why).add("reason", s.reason).add("restored", pawn != nullptr)
                                           .add("seconds", s.now - s.since).add("teleports", s.teleports).add("mode_fixes", s.mode_fixes));
    s.ghost = false;
    s.pawn = nullptr;
    s.reason.clear();
    s.have_tgt = false;
    s.want_since = s.clear_since = -1;
}

// Pourquoi l'état du pilote n'est pas reproductible par le pion d'ici (vide : il l'est).
std::string ghost_reason(const proto::PilotState& p, const game::MoveInfo& me)
{
    // Carte du monde seulement : ailleurs, le suivi reste celui d'avant (escalade, cordes, ascenseurs…), même si un
    // drapeau de monture restait levé.
    const bool world_map = (p.flags & proto::kPilotWorldMap) || me.world_map || p.level.find("WorldMap") != std::string::npos;
    if (!world_map) return {};
    const bool p_mount = (p.flags & proto::kPilotMount) != 0;
    if (p_mount != me.mount) return p_mount ? "pilot_mounted" : "follower_mounted";
    if (!p.pawn.empty() && p.pawn != me.cls) return "pawn_class";
    if ((p.flags & proto::kPilotAttached) && !me.attached) return "pilot_attached";
    // Pas MOVE_None : posé pendant les cinématiques et menus (le pion caché disparaîtrait de la scène).
    if (p.move == kFly || p.move == kCustom) return "pilot_move";
    return {};
}

void log_follow_changes(const proto::PilotState& p, const game::MoveInfo& me, const double my_pos[3])
{
    const bool changed = p.move != s.f_pmove || p.flags != s.f_pflags || p.pawn != s.f_pcls || me.move != s.f_move || me.mount != s.f_mount ||
                         me.cls != s.f_cls;
    if (!changed) return;
    s.f_pmove = p.move; s.f_pflags = p.flags; s.f_pcls = p.pawn; s.f_move = me.move; s.f_mount = me.mount; s.f_cls = me.cls;
    if (!log_allowed()) return;
    log().write("coop_follow_mode", JsonObj().add("pilot_move", static_cast<int>(p.move)).add("pilot_custom", static_cast<int>(p.custom))
                                         .add("pilot_flags", static_cast<int>(p.flags)).add("pilot_pawn", p.pawn).add("pilot_z", p.z).add("pilot_vz", p.vz)
                                         .add("move", me.move).add("custom", me.custom).add("mount", me.mount).add("world_map", me.world_map)
                                         .add("attached", me.attached).add("pawn", me.cls).add("z", my_pos[2]).add("ghost", s.ghost));
}

// Suiveur sur Esquie, pilote à pied : on descend avec la fonction du contrôleur (OnDismountRequested, dump), comme la
// touche du jeu. Rien pour monter : aucune fonction connue (voir coop_mount_functions au prochain essai).
void try_dismount(const proto::PilotState& p, const game::MoveInfo& me, const Bump& bump)
{
    static const bool enabled = cmdline::arg(L"e33mountsync") != L"0";
    if (!enabled || !me.mount || (p.flags & proto::kPilotMount) || !ground(p.move)) return;
    if (s.dismount_tries >= 3 || s.now < s.next_dismount) return;
    s.next_dismount = s.now + 5.0;
    ++s.dismount_tries;
    UObject* ctrl = game::controller();
    ue::Call c(ctrl, STR("OnDismountRequested"));
    const bool ok = c && c.invoke();
    if (bump) bump("follower_dismounts", 1);
    log().write("coop_follower_dismount", JsonObj().add("ok", ok).add("try", s.dismount_tries)
                                              .add("mount_after", ue::get_bool(game::gi(), STR("IsMountInUse"))));
}

} // namespace

void fill_pilot(UObject* pawn, proto::PilotState& m, double now)
{
    s.now = now;
    game::MoveInfo mi;
    if (!game::move_info(pawn, mi)) return;
    m.move = mi.move < 0 || mi.move > 255 ? proto::kMoveUnknown : static_cast<uint8_t>(mi.move);
    m.custom = static_cast<uint8_t>(mi.custom);
    m.flags = static_cast<uint8_t>((mi.mount ? proto::kPilotMount : 0) | (mi.world_map ? proto::kPilotWorldMap : 0) |
                                   (mi.attached ? proto::kPilotAttached : 0));
    m.pawn = mi.cls;
    s.other_cls.clear();
    if (mi.move == s.p_move && mi.custom == s.p_custom && m.flags == s.p_flags && mi.cls == s.p_cls) return;
    const bool big = m.flags != s.p_flags || mi.cls != s.p_cls;   // monture ou pion changés (pas un simple saut)
    s.p_move = mi.move; s.p_custom = mi.custom; s.p_flags = m.flags; s.p_cls = mi.cls;
    if (big || log_allowed())
        log().write("coop_pilot_move", JsonObj().add("level", m.level).add("move", mi.move).add("custom", mi.custom).add("mount", mi.mount)
                                           .add("world_map", mi.world_map).add("attached", mi.attached).add("pawn", mi.cls).add("z", m.z).add("vz", m.vz));
    if (big && (mi.mount || mi.world_map)) { list_functions(pawn, "pawn"); list_functions(game::controller(), "controller"); }
}

void pilot_without_pawn(double now)
{
    s.now = now;
    UObject* p = possessed_pawn();
    const std::string cls = p ? ue::short_name(p->GetClassPrivate()) : std::string();
    if (cls == s.other_cls) return;
    s.other_cls = cls;
    if (cls.empty()) return;
    const bool mount = ue::get_bool(game::gi(), STR("IsMountInUse"));
    log().write("coop_pilot_pawn_other", JsonObj().add("pawn", cls).add("mount", mount));
    if (mount || cls.find("squie") != std::string::npos || cls.find("ount") != std::string::npos) {   // pas les pions de combat
        list_functions(p, "possessed");
        list_functions(game::controller(), "controller");
    }
}

bool follow(UObject* pawn, const proto::PilotState& p, double age, double now, const Bump& bump)
{
    s.now = now;
    s.followed_at = now;
    if (p.move == proto::kMoveUnknown) {   // pilote d'une version sans ces champs : suivi normal
        if (s.ghost) end_ghost(pawn, "pilot_unknown", nullptr, 0);
        return false;
    }
    game::MoveInfo me;
    game::move_info(pawn, me);
    game::PawnState ps;
    game::pawn_state(pawn, ps);
    log_follow_changes(p, me, ps.pos);
    try_dismount(p, me, bump);

    const std::string why = ghost_reason(p, me);
    if (!why.empty()) { s.clear_since = -1; if (s.want_since < 0) s.want_since = now; }
    else { s.want_since = -1; if (s.clear_since < 0) s.clear_since = now; }

    // Cible : position du pilote prédite (vitesse, 0,25 s au plus), lissée (~0,1 s) ; un saut net est pris tel quel.
    const double a = std::clamp(age, 0.0, 0.25);
    const double raw[3] = {p.x + p.vx * a, p.y + p.vy * a, p.z + p.vz * a};

    if (s.ghost && pawn != s.pawn) {
        // Nouveau pion (niveau rechargé) : l'ancien a disparu avec son niveau, le nouveau est mis en fantôme.
        log().write("coop_follower_ghost_pawn", JsonObj().add("reason", s.reason));
        s.pawn = pawn;
        apply_ghost(pawn);
    }
    if (!s.ghost) {
        const double delay = why == "pilot_move" ? kEnterDelay : 0.0;
        if (why.empty() || now - s.want_since < delay) return false;
        s.ghost = true;
        s.pawn = pawn;
        s.reason = why;
        s.since = now;
        s.mode_fixes = s.teleports = 0;
        s.have_tgt = false;
        apply_ghost(pawn);
        if (bump) bump("follower_ghosts", 1);
        log().write("coop_follower_ghost", JsonObj().add("on", true).add("reason", why).add("pilot_move", static_cast<int>(p.move))
                                               .add("pilot_flags", static_cast<int>(p.flags)).add("pilot_pawn", p.pawn).add("move", me.move)
                                               .add("mount", me.mount).add("pawn", me.cls).add("dz", p.z - ps.pos[2]));
    } else if (why.empty() && now - s.clear_since >= kExitDelay) {
        end_ghost(pawn, "reproducible", raw, p.yaw);
        return false;
    } else if (!why.empty() && why != s.reason) {
        log().write("coop_follower_ghost_reason", JsonObj().add("from", s.reason).add("to", why));
        s.reason = why;
    }

    const double dt = s.last_t < 0 ? 0.0 : std::clamp(now - s.last_t, 0.0, 0.25);
    s.last_t = now;
    const double jump = std::hypot(raw[0] - s.tgt[0], raw[1] - s.tgt[1], raw[2] - s.tgt[2]);
    if (!s.have_tgt || jump > 500) { for (int i = 0; i < 3; ++i) s.tgt[i] = raw[i]; s.have_tgt = true; }
    else { const double k = 1.0 - std::exp(-dt / 0.1); for (int i = 0; i < 3; ++i) s.tgt[i] += (raw[i] - s.tgt[i]) * k; }
    game::teleport_pawn(pawn, s.tgt, p.yaw);
    ++s.teleports;
    // Le jeu peut repasser le pion en marche, chute ou nage (volume d'eau traversé) : remis en vol, sans insister.
    if (me.move != kFly && now >= s.next_mode_fix) {
        s.next_mode_fix = now + 0.5;
        ++s.mode_fixes;
        if (bump) bump("follower_ghost_mode_fixes", 1);
        if (s.mode_fixes <= 20 || log_allowed())
            log().write("coop_follower_ghost_mode", JsonObj().add("move", me.move).add("custom", me.custom).add("fixes", s.mode_fixes));
        game::set_movement_mode(pawn, kFly);
    }
    return true;
}

void tick(double now)
{
    s.now = now;
    if (s.ghost && s.followed_at < now) release("not_following");
}

void release(const char* why)
{
    if (!s.ghost) return;
    UObject* cur = game::explore_pawn();
    if (!cur) return;   // combat, chargement : on réessaie à l'image suivante
    // Pion différent : l'ancien est parti avec son niveau, le nouveau n'a jamais été caché.
    end_ghost(cur == s.pawn ? cur : nullptr, why, nullptr, 0);
}

bool ghosted() { return s.ghost; }

void reset()
{
    const auto listed = s.listed;
    s = S{};
    s.listed = listed;
}

// --- Banc d'essai (étapes "mount" des scénarios) ----------------------------------------------------------------
// Diagnostic sur la carte du monde (classe et fonctions d'Esquie, état de la monture) et actions de monture par le
// code (jamais de touche simulée). Seul le test runner appelle ces fonctions.
namespace {

using RC::Unreal::UStruct;
using RC::Unreal::UFunction;
using RC::Unreal::FProperty;

struct Bench {
    std::map<std::string, int> counts;
    std::set<std::string> members_logged;     // classes dont les membres ont été journalisés
    std::set<std::wstring> traced;            // fonctions accrochées (trace)
    std::map<std::string, int> calls;         // appels vus par fonction (trace)
    double next_watch = 0;
} b;

std::string lower(std::string v)
{
    for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return v;
}

bool has_any(const std::string& name, const std::vector<std::string>& needles)
{
    std::string l = lower(name);
    for (size_t at; (at = l.find("amount")) != std::string::npos;) l.erase(at, 6);   // « Amount » n'est pas une monture
    for (const auto& k : needles) if (l.find(k) != std::string::npos) return true;
    return false;
}

const std::vector<std::string>& mount_needles()
{
    static const std::vector<std::string> n = {"mount", "esquie", "ride", "rider", "fly", "flying", "swim", "dive", "water", "takeoff",
                                               "land", "glide", "boat", "vehicle", "capabilit", "worldmap", "possess", "underwater"};
    return n;
}

bool is_bp_class(UStruct* c) { return c && c->GetClassPrivate() && c->GetClassPrivate()->GetName() == STR("BlueprintGeneratedClass"); }

std::string class_chain(UObject* o)
{
    std::string out;
    if (!o) return out;
    for (UStruct* c = o->GetClassPrivate(); c; c = c->GetSuperStruct()) { if (!out.empty()) out += ">"; out += ue::to_utf8(c->GetName()); }
    return out;
}

UObject* possessed()
{
    UObject* ctrl = game::controller();
    if (!ctrl) return nullptr;
    ue::Call c(ctrl, STR("K2_GetPawn"));
    if (!c || !c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

UObject* attach_parent(UObject* a)
{
    ue::Call c(a, STR("GetAttachParentActor"));
    return c && c.invoke() ? c.out_object(STR("ReturnValue")) : nullptr;
}

// Fonctions et propriétés déclarées par chaque classe Blueprint de o (une fois par classe).
void log_members(const std::string& label, const char* what, UObject* o)
{
    if (!o) return;
    for (UStruct* c = o->GetClassPrivate(); c && is_bp_class(c); c = c->GetSuperStruct()) {
        const std::string cname = ue::to_utf8(c->GetName());
        if (!b.members_logged.insert(cname).second) continue;
        std::vector<std::string> fns, props;
        for (UFunction* f : RC::Unreal::TFieldRange<UFunction>(c, RC::Unreal::EFieldIterationFlags::IncludeDeprecated)) {
            if (!f) continue;
            const std::string n = ue::to_utf8(f->GetName());
            if (n.rfind("ExecuteUbergraph", 0) == 0) continue;
            fns.push_back(n);
            if (fns.size() >= 700) break;
        }
        for (FProperty* p : RC::Unreal::TFieldRange<FProperty>(c, RC::Unreal::EFieldIterationFlags::IncludeDeprecated)) {
            if (!p) continue;
            props.push_back(reflect::clean_name(p->GetName()));
            if (props.size() >= 500) break;
        }
        log().write("coop_mount_diag_members", JsonObj().add("label", label).add("what", what).add("class", cname)
                                                   .add("functions", join(fns)).add("props", join(props)));
    }
}

// Valeurs des propriétés dont le nom parle de monture / vol / nage (toute la chaîne de classes).
std::string state_props(UObject* o, bool all = false)
{
    std::string out;
    if (!o) return out;
    int n = 0;
    for (FProperty* p : RC::Unreal::TFieldRange<FProperty>(o->GetClassPrivate(), RC::Unreal::EFieldIterationFlags::IncludeSuper)) {
        if (!p) continue;
        const std::string name = reflect::clean_name(p->GetName());
        if (!all && !has_any(name, mount_needles())) continue;
        // Pointeurs souples / faibles : pas un UObject* brut (lus comme tels, ils ont fait planter le jeu le 2026-10-23).
        if (RC::Unreal::CastField<RC::Unreal::FObjectPropertyBase>(p) && !RC::Unreal::CastField<RC::Unreal::FObjectProperty>(p)) continue;
        // Tout lire : valeurs simples seulement (pas de structs ni de conteneurs d'objets inconnus).
        if (all && (RC::Unreal::CastField<RC::Unreal::FStructProperty>(p) || RC::Unreal::CastField<RC::Unreal::FArrayProperty>(p) ||
                    RC::Unreal::CastField<RC::Unreal::FMapProperty>(p) || RC::Unreal::CastField<RC::Unreal::FSetProperty>(p)))
            continue;
        reflect::Leaves l;
        reflect::flatten(p, p->ContainerPtrToValuePtr<void>(o), name, l, 5);   // profondeur réduite : valeurs simples
        int k = 0;
        for (auto& [path, val] : l) {
            if (++k > 12) break;
            if (!out.empty()) out += ";";
            out += path + "=" + val;
        }
        if (++n >= 80) break;
    }
    return out;
}

int capabilities_mask(UObject* pawn)
{
    int mask = 0;
    for (int i = 0; i < 11; ++i) {
        ue::Call c(pawn, STR("IsCapabilityAllowed"));
        if (!c) return -1;
        c.set_int(STR("Capability"), i);
        if (c.invoke() && c.out_bool(STR("isAllowed"))) mask |= 1 << i;
    }
    return mask;
}

std::vector<std::string> bp_classes(const std::vector<std::string>& needles)
{
    // FindAllOf("BlueprintGeneratedClass") ne rend rien (vu le 2026-10-23) : parcours de tous les objets.
    std::vector<std::string> out;
    RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* c, RC::Unreal::int32, RC::Unreal::int32) {
        if (c && is_bp_class(static_cast<UStruct*>(c))) {
            const std::string n = ue::to_utf8(c->GetName());
            if (has_any(n, needles)) out.push_back(n);
        }
        return RC::LoopAction::Continue;
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<UObject*> instances(const std::string& cls)
{
    std::vector<UObject*> all, out;
    RC::Unreal::UObjectGlobals::FindAllOf(ue::from_utf8(cls).c_str(), all);
    for (UObject* o : all) {
        if (!o) continue;
        const std::string n = ue::to_utf8(o->GetName());
        if (n.rfind("Default__", 0) == 0) continue;   // objet par défaut de la classe
        out.push_back(o);
    }
    return out;
}

// Acteur Esquie : instance d'une classe Blueprint « *squie* » qui est un acteur placé (pas une animation, un widget…).
UObject* find_esquie()
{
    UObject* best = nullptr;
    double best_d = 1e18;
    double me[3]{};
    UObject* p = possessed();
    const bool have_me = p && game::location(p, me);
    for (const auto& cls : bp_classes({"esquie"})) {
        if (cls.rfind("WBP_", 0) == 0 || cls.rfind("ABP_", 0) == 0) continue;
        for (UObject* o : instances(cls)) {
            if (!ue::is_a(o, STR("Actor"))) continue;
            double pos[3]{};
            if (!game::location(o, pos)) continue;
            const double d = have_me ? std::hypot(pos[0] - me[0], pos[1] - me[1], pos[2] - me[2]) : 0;
            if (!best || d < best_d) { best = o; best_d = d; }
        }
    }
    return best;
}

UObject* target_of(const std::string& t)
{
    // "controller.BP_WorldMapControllerComponent" : objet d'une propriété de la cible
    if (const auto dot = t.find('.'); dot != std::string::npos)
        return ue::get_object(target_of(t.substr(0, dot)), ue::from_utf8(t.substr(dot + 1)).c_str());
    if (t == "pawn") return game::explore_pawn();
    if (t == "possessed") return possessed();
    if (t == "controller") return game::controller();
    if (t == "gi") return game::gi();
    if (t == "esquie") return find_esquie();
    return nullptr;
}

void diag(const std::string& label)
{
    UObject* ctrl = game::controller();
    UObject* pos_pawn = possessed();
    UObject* pawn = game::explore_pawn();
    UObject* last = ue::get_object(ctrl, STR("LastControlledCharacterReference"));
    game::MoveInfo mi;
    game::move_info(pawn ? pawn : last, mi);
    double at[3]{};
    game::location(pos_pawn, at);
    UObject* parent = attach_parent(pawn ? pawn : last);
    log().write("coop_mount_diag", JsonObj().add("label", label).add("level", game::current_level())
                                       .add("possessed", class_chain(pos_pawn)).add("explore_pawn", pawn != nullptr)
                                       .add("last_controlled", ue::short_name(last)).add("same", last == pos_pawn)
                                       .add("move", mi.move).add("custom", mi.custom).add("mount", ue::get_bool(game::gi(), STR("IsMountInUse")))
                                       .add("world_map_pawn", mi.world_map).add("attach_parent", parent ? class_chain(parent) : std::string())
                                       .add("x", at[0]).add("y", at[1]).add("z", at[2])
                                       .add("capabilities", capabilities_mask(pawn ? pawn : last)));
    log().write("coop_mount_diag_state", JsonObj().add("label", label).add("pawn", state_props(pawn ? pawn : last))
                                             .add("possessed", pos_pawn != (pawn ? pawn : last) ? state_props(pos_pawn) : std::string())
                                             .add("controller", state_props(ctrl)).add("gi", state_props(game::gi())));
    const auto classes = bp_classes({"esquie", "mount", "ride", "vehicle", "worldmap"});
    log().write("coop_mount_diag_classes", JsonObj().add("label", label).add("count", static_cast<int>(classes.size())).add("names", join(classes)));
    int logged = 0;
    for (const auto& cls : classes) {
        if (!has_any(cls, {"esquie", "mount", "ride", "vehicle"}) || cls.rfind("WBP_", 0) == 0) continue;
        const auto inst = instances(cls);
        if (inst.empty()) continue;
        UObject* o = inst.front();
        double p[3]{};
        const bool has_pos = game::location(o, p);
        UObject* par = has_pos ? attach_parent(o) : nullptr;
        log().write("coop_mount_diag_instance", JsonObj().add("label", label).add("class", cls).add("count", static_cast<int>(inst.size()))
                                                    .add("name", ue::full_name(o)).add("chain", class_chain(o)).add("has_pos", has_pos)
                                                    .add("x", p[0]).add("y", p[1]).add("z", p[2]).add("attach_parent", ue::short_name(par))
                                                    .add("state", state_props(o, true)));
        log_members(label, cls.c_str(), o);
        if (++logged >= 25) break;
    }
    log_members(label, "possessed", pos_pawn);
    log_members(label, "pawn", pawn);
    log_members(label, "controller", ctrl);
    // Composants et objets liés (contrôleur de la carte du monde, gestionnaire d'apparition, monture…)
    for (UObject* owner : {ctrl, pos_pawn, game::gi()}) {
        if (!owner) continue;
        for (FProperty* p : RC::Unreal::TFieldRange<FProperty>(owner->GetClassPrivate(), RC::Unreal::EFieldIterationFlags::IncludeSuper)) {
            if (!p || !RC::Unreal::CastField<RC::Unreal::FObjectProperty>(p)) continue;
            const std::string name = ue::to_utf8(p->GetName());
            if (!has_any(name, {"worldmap", "mount", "esquie", "spawner", "vehicle", "ride"})) continue;
            UObject* o = *static_cast<UObject**>(p->ContainerPtrToValuePtr<void>(owner));
            if (!o) continue;
            log().write("coop_mount_diag_sub", JsonObj().add("label", label).add("owner", ue::short_name(owner)).add("prop", name)
                                                   .add("object", ue::full_name(o)).add("chain", class_chain(o)).add("state", state_props(o, true)));
            log_members(label, name.c_str(), o);
        }
    }
    ++b.counts["diags"];
}

// Accroche les fonctions des classes Esquie (toutes) et celles du pion / contrôleur qui parlent de monture : chaque
// appel est journalisé (5 premiers par fonction), pour voir ce que le jeu appelle quand on monte.
void trace()
{
    // all : toutes les fonctions des « levels » premières classes Blueprint, puis seulement celles qui parlent de monture.
    auto hook_class = [](UStruct* c, bool all, int levels) {
        for (int lv = 0; c && is_bp_class(c); c = c->GetSuperStruct(), ++lv) {
            const bool every = all && lv < levels;
            for (UFunction* f : RC::Unreal::TFieldRange<UFunction>(c, RC::Unreal::EFieldIterationFlags::IncludeDeprecated)) {
                if (!f) continue;
                const std::string n = ue::to_utf8(f->GetName());
                if (n.rfind("ExecuteUbergraph", 0) == 0 || n.find("Tick") != std::string::npos || n.find("Update") != std::string::npos ||
                    n.find("Debug") != std::string::npos || n.find("DEBUG") != std::string::npos || n.find("InpAxisEvt") != std::string::npos ||
                    n.find("CameraOverride") != std::string::npos || n == "IsCapabilityAllowed")
                    continue;
                if (!every && !has_any(n, mount_needles())) continue;
                std::wstring path = f->GetFullName();   // « Function /Game/…:Nom »
                if (const auto sp = path.find(L' '); sp != std::wstring::npos) path = path.substr(sp + 1);
                if (!b.traced.insert(path).second) continue;
                const std::string key = ue::to_utf8(c->GetName()) + ":" + n;
                HookBus::get().post(path, [key](HookCtx& ctx, UFunction*) {
                    const int k = ++b.calls[key];
                    ++b.counts["traced_calls"];
                    if (k <= 5) log().write("coop_mount_call", JsonObj().add("fn", key).add("n", k).add("self", ue::short_name(ctx.Context))
                                                                  .add("mount", ue::get_bool(game::gi(), STR("IsMountInUse"))));
                });
            }
            if (b.traced.size() > 900) break;
        }
    };
    for (const auto& cls : bp_classes({"esquie", "mount"})) {
        if (cls.rfind("WBP_", 0) == 0) continue;
        const auto inst = instances(cls);
        if (!inst.empty()) hook_class(inst.front()->GetClassPrivate(), cls.find("squie") != std::string::npos, 3);
    }
    if (UObject* p = possessed()) hook_class(p->GetClassPrivate(), true, 1);
    if (UObject* ctrl = game::controller()) {
        hook_class(ctrl->GetClassPrivate(), true, 1);
        // Composants de la carte du monde (contrôleur de carte, apparition…) : toutes leurs fonctions.
        for (FProperty* p : RC::Unreal::TFieldRange<FProperty>(ctrl->GetClassPrivate(), RC::Unreal::EFieldIterationFlags::IncludeSuper)) {
            if (!p || !RC::Unreal::CastField<RC::Unreal::FObjectProperty>(p)) continue;
            if (!has_any(ue::to_utf8(p->GetName()), {"worldmap", "mount", "esquie", "spawner"})) continue;
            if (UObject* o = *static_cast<UObject**>(p->ContainerPtrToValuePtr<void>(ctrl))) hook_class(o->GetClassPrivate(), true, 2);
        }
    }
    HookBus::get().try_register();
    log().write("coop_mount_trace", JsonObj().add("functions", static_cast<int>(b.traced.size())));
}

// État courant (étape watch), toutes les 0,5 s.
void watch_line(const std::string& label)
{
    UObject* p = possessed();
    UObject* pawn = game::explore_pawn();
    UObject* ctrl = game::controller();
    UObject* last = ue::get_object(ctrl, STR("LastControlledCharacterReference"));
    game::MoveInfo mi;
    game::move_info(pawn ? pawn : last, mi);
    game::PawnState ps;
    game::pawn_state(p, ps);
    UObject* parent = attach_parent(pawn ? pawn : last);
    log().write("coop_mount_watch", JsonObj().add("label", label).add("possessed", ue::short_name(p ? p->GetClassPrivate() : nullptr))
                                        .add("explore_pawn", pawn != nullptr).add("mount", ue::get_bool(game::gi(), STR("IsMountInUse")))
                                        .add("move", mi.move).add("custom", mi.custom).add("parent", ue::short_name(parent))
                                        .add("x", ps.pos[0]).add("y", ps.pos[1]).add("z", ps.pos[2]).add("vz", ps.vel[2])
                                        .add("speed", std::hypot(ps.vel[0], ps.vel[1])));
}

bool set_param(ue::Call& c, const std::string& name, const json::Value& v)
{
    const std::wstring w = ue::from_utf8(name);
    if (v.is_string()) {
        const std::string s = v.as_string();
        if (UObject* o = target_of(s)) { c.set_object(w.c_str(), o); return true; }
        c.set_fname(w.c_str(), ue::from_utf8(s).c_str());
        return true;
    }
    if (v.type == json::Value::Type::Bool) { c.set_bool(w.c_str(), v.as_bool()); return true; }
    if (v.is_number()) {
        if (c.param_size(w.c_str()) == 8 && v.as_number() != static_cast<double>(v.as_int())) c.set_double(w.c_str(), v.as_number());
        else c.set_int(w.c_str(), v.as_int());
        return true;
    }
    return false;
}

} // namespace

bool test_step(const json::Value& st, double step_t, bool first, std::string& err)
{
    const std::string op = st["op"].as_string("diag");
    const std::string label = st["label"].as_string(op);
    if (op == "diag") { diag(label); return true; }
    if (op == "trace") { trace(); return true; }
    if (op == "interact" || op == "call") {
        UObject* t = target_of(st["target"].as_string("esquie"));
        if (!t) {
            ++b.counts["mount_calls_no_target"];
            log().write("coop_mount_bench_call", JsonObj().add("op", op).add("target", st["target"].as_string("esquie")).add("found", false));
            if (st["optional"].as_bool(false)) return true;
            err = "mount " + op + ": no target " + st["target"].as_string("esquie");
            return false;
        }
        bool ok = false;
        std::string fn = st["fn"].as_string("OnInteractionRequested");
        if (op == "interact") {
            ok = game::interact(t, st["instant"].as_bool(false));
        } else {
            ue::Call c(t, ue::from_utf8(fn).c_str());
            if (c) {
                for (const auto& [k, v] : st["params"].obj) set_param(c, k, v);
                ok = c.invoke();
            }
        }
        ++b.counts[ok ? "mount_calls_ok" : "mount_calls_failed"];
        log().write("coop_mount_bench_call", JsonObj().add("op", op).add("target", ue::full_name(t)).add("fn", fn).add("ok", ok)
                                                 .add("mount_after", ue::get_bool(game::gi(), STR("IsMountInUse"))));
        if (!ok && !st["optional"].as_bool(false)) { err = "mount " + op + " " + fn + " failed"; return false; }
        return true;
    }
    if (op == "move") {   // pion possédé (Esquie compris) : AddMovementInput, composante verticale comprise
        if (step_t >= st["s"].as_number(2)) return true;
        UObject* p = possessed();
        ue::Call c(p, STR("AddMovementInput"));
        if (c) {
            if (auto* d = static_cast<double*>(c.param_ptr(STR("WorldDirection")))) { d[0] = st["dir"][0].as_number(1); d[1] = st["dir"][1].as_number(0); d[2] = st["dir"][2].as_number(0); }
            if (auto* sc = static_cast<float*>(c.param_ptr(STR("ScaleValue")))) *sc = static_cast<float>(st["scale"].as_number(1));
            c.set_bool(STR("bForce"), true);
            c.invoke();
        }
        if (first || step_t >= b.next_watch) { b.next_watch = step_t + 0.5; watch_line(label); }
        return false;
    }
    if (op == "watch") {
        if (first) b.next_watch = 0;
        if (step_t >= b.next_watch) { b.next_watch = step_t + 0.5; watch_line(label); }
        return step_t >= st["s"].as_number(3);
    }
    if (op == "wait_mount") {   // attend IsMountInUse == is
        return ue::get_bool(game::gi(), STR("IsMountInUse")) == st["is"].as_bool(true);
    }
    err = "mount: unknown op " + op;
    return false;
}

int count(const std::string& name)
{
    auto it = b.counts.find(name);
    return it == b.counts.end() ? 0 : it->second;
}

} // namespace e33::mount
