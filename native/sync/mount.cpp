#include "mount.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
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

} // namespace e33::mount
