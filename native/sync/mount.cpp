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
    int p_move = -2, p_custom = -1, p_flags = -1, p_wm = -1;
    std::string p_cls;
    std::string other_cls;                 // pion possédé hors BP_jRPG_Character_World_C
    std::set<std::string> listed;          // classes dont les fonctions ont été journalisées
    // Suiveur
    double climb_exit_since = -1;          // pion d'ici encore en escalade alors que le pilote n'y est plus (depuis)
    double next_climb_fix = 0;
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
    // Suiveur sur Esquie (D20) : montée, mode d'Esquie, conduite
    bool last_p_mount = false;
    double next_mount = 0;
    int mount_tries = 0;
    bool riding = false;
    double ride_since = -1;
    double next_wm_try = 0;
    int wm_target = -1, wm_tries = 0;
    int wm_changes = 0, ride_snaps = 0;
    double ride_err_max = 0, ride_err_sum = 0, ride_err_max_all = 0;
    int ride_ticks = 0;
    double ride_stats_at = 0;
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

bool mount_sync_enabled()
{
    static const bool enabled = cmdline::arg(L"e33mountsync") != L"0";
    return enabled;
}

// Esquie sur la carte du monde (2026-10-23, D20) : pion BP_WorldMapCharacter_C possédé par le contrôleur à la place du
// pion à pied (BP_jRPG_Character_WorldMap_pawn_C). Son propre mode : CurrentMovementMode (0 marche, 2 vol ; changé par
// TryChangeMovementMode, comme au décollage par la touche de saut).
constexpr int kWmNone = 0xFF, kWmWalk = 0, kWmFly = 2;
bool is_esquie(UObject* pawn) { return pawn && ue::prop(pawn, STR("CurrentMovementMode")) && ue::is_a(pawn, STR("BP_WorldMapCharacter_C")); }
int wm_mode(UObject* pawn) { return is_esquie(pawn) ? static_cast<int>(ue::get_int(pawn, STR("CurrentMovementMode"), kWmNone)) : kWmNone; }
UObject* wm_component() { return ue::get_object(game::controller(), STR("BP_WorldMapControllerComponent")); }
// Montée ou descente en cours (animation), ou changement de mode d'Esquie en cours : on ne conduit pas.
bool mount_busy(UObject* pawn)
{
    return ue::get_bool(wm_component(), STR("IsMountPerformingAnAnimation")) || (is_esquie(pawn) && ue::get_bool(pawn, STR("IsInTransition")));
}

// Pilote sur Esquie, suiveur à pied : on monte comme la touche du jeu (BP_WorldMapControllerComponent.OnInputMountPressed,
// appelé par l'événement de la touche de monture du contrôleur ; repli : l'événement lui-même). 3 essais.
void try_mount(const game::MoveInfo& me, const Bump& bump)
{
    if (s.mount_tries >= 3 || s.now < s.next_mount) return;
    if (!ground(me.move)) {   // à la nage (ou en l'air) : pas de monture possible ici, repli sur le fantôme
        s.mount_tries = 3;
        log().write("coop_follower_mount", JsonObj().add("ok", false).add("why", "not_on_ground").add("move", me.move));
        return;
    }
    s.next_mount = s.now + 1.5;
    ++s.mount_tries;
    bool called = false;
    std::string how = "component";
    {
        ue::Call c(wm_component(), STR("OnInputMountPressed"));
        called = c && c.invoke();
    }
    if (!called || !ue::get_bool(game::gi(), STR("IsMountInUse"))) {
        ue::Call c(game::controller(), STR("InpActEvt_WorldMap_MountAction_K2Node_EnhancedInputActionEvent_2"));
        if (c && c.invoke()) { called = true; how = "input_event"; }
    }
    const bool ok = ue::get_bool(game::gi(), STR("IsMountInUse"));
    if (bump) bump(ok ? "follower_mounts" : "follower_mount_failures", 1);
    log().write("coop_follower_mount", JsonObj().add("ok", ok).add("called", called).add("how", how).add("try", s.mount_tries)
                                           .add("can_mount", ue::get_bool(ue::get_object(game::controller(), STR("BP_WorldMapUIManager")), STR("CanMount"))));
}

// Changement de mode d'Esquie ici (décollage, atterrissage…) : TryChangeMovementMode comme le jeu ; après 2 refus pour le
// même mode, ChangeMovementMode (imposé). Au plus toutes les 0,5 s.
void match_wm_mode(UObject* esq, int want, const Bump& bump)
{
    const int cur = wm_mode(esq);
    if (want == kWmNone || cur == kWmNone || cur == want) { s.wm_tries = 0; return; }
    if (s.now < s.next_wm_try) return;
    s.next_wm_try = s.now + 0.5;
    if (want != s.wm_target) { s.wm_target = want; s.wm_tries = 0; }
    ++s.wm_tries;
    bool changed = false;
    const bool forced = s.wm_tries > 2;
    if (!forced) {
        ue::Call c(esq, STR("TryChangeMovementMode"));
        if (c) {
            c.set_int(STR("MovementMode"), want).set_int(STR("ZoneType"), ue::get_int(esq, STR("CurrentZoneType"), 0));
            if (c.invoke()) changed = c.out_bool(STR("HasChanged"));
        }
    } else {
        ue::Call c(esq, STR("ChangeMovementMode"));
        if (c) { c.set_int(STR("MovementMode"), want); c.invoke(); }
        changed = wm_mode(esq) == want;
    }
    ++s.wm_changes;
    if (bump) bump(changed ? "follower_wm_changes" : "follower_wm_refused", 1);
    if (s.wm_tries <= 6 || log_allowed())
        log().write("coop_follower_wm_mode", JsonObj().add("from", cur).add("to", want).add("changed", changed).add("forced", forced)
                                                 .add("try", s.wm_tries).add("now", wm_mode(esq)));
}

// Suiveur sur Esquie, pilote à pied : on atterrit (mode marche) puis on descend avec la fonction du contrôleur
// (OnDismountRequested, comme la touche du jeu ; refusée tant qu'Esquie vole). 8 essais.
void try_dismount(UObject* pawn, const proto::PilotState& p, const game::MoveInfo& me, const Bump& bump)
{
    if (!mount_sync_enabled() || !me.mount || (p.flags & proto::kPilotMount) || !ground(p.move)) return;
    if (s.dismount_tries >= 8 || s.now < s.next_dismount || mount_busy(pawn)) return;
    if (is_esquie(pawn) && wm_mode(pawn) != kWmWalk) { match_wm_mode(pawn, kWmWalk, bump); return; }
    s.next_dismount = s.now + 1.5;
    ++s.dismount_tries;
    UObject* ctrl = game::controller();
    ue::Call c(ctrl, STR("OnDismountRequested"));
    const bool ok = c && c.invoke();
    if (bump) bump("follower_dismounts", 1);
    log().write("coop_follower_dismount", JsonObj().add("ok", ok).add("try", s.dismount_tries)
                                              .add("mount_after", ue::get_bool(game::gi(), STR("IsMountInUse"))));
}

// Suiveur et pilote sur Esquie : même mode qu'Esquie du pilote, puis conduite. En vol, Esquie est placé sur la position
// prédite et lissée du pilote à chaque image (vol à ~4000 u/s : une poursuite par entrées dépasserait ou traînerait) ;
// au sol / sur l'eau, il est conduit par AddMovementInput (animations du jeu), recalé seulement s'il décroche.
void ride(UObject* esq, const proto::PilotState& p, double age, const Bump& bump)
{
    if (!s.riding) {
        s.riding = true;
        s.ride_since = s.now;
        s.have_tgt = false;
        s.ride_stats_at = s.now;
        if (bump) bump("follower_rides", 1);
        log().write("coop_follower_ride", JsonObj().add("on", true).add("pilot_wm", static_cast<int>(p.wm_mode)).add("wm", wm_mode(esq)));
    }
    if (mount_busy(esq)) { s.have_tgt = false; return; }
    match_wm_mode(esq, p.wm_mode, bump);
    game::PawnState me;
    if (!game::pawn_state(esq, me)) return;
    const double a = std::clamp(age, 0.0, 0.25);
    const double raw[3] = {p.x + p.vx * a, p.y + p.vy * a, p.z + p.vz * a};
    const double dt = s.last_t < 0 ? 0.0 : std::clamp(s.now - s.last_t, 0.0, 0.25);
    s.last_t = s.now;
    // Cible : avancée à la vitesse du pilote (pas de retard en vol à ~4000 u/s), puis rapprochée de la position reçue
    // (~0,1 s) ; un saut net est pris tel quel.
    const double jump = std::hypot(raw[0] - s.tgt[0], raw[1] - s.tgt[1], raw[2] - s.tgt[2]);
    if (!s.have_tgt || jump > 1500) { for (int i = 0; i < 3; ++i) s.tgt[i] = raw[i]; s.have_tgt = true; }
    else {
        const double v[3] = {p.vx, p.vy, p.vz};
        const double k = 1.0 - std::exp(-dt / 0.1);
        for (int i = 0; i < 3; ++i) { s.tgt[i] += v[i] * dt; s.tgt[i] += (raw[i] - s.tgt[i]) * k; }
    }
    const double dx = s.tgt[0] - me.pos[0], dy = s.tgt[1] - me.pos[1], dz = s.tgt[2] - me.pos[2];
    const double d2 = std::hypot(dx, dy), d = std::hypot(d2, dz);
    const int mine = wm_mode(esq);
    // Décollage pas encore fait ici : on attend le mode vol (placé en l'air en mode marche, Esquie retomberait).
    if (p.wm_mode == kWmFly && mine != kWmFly) return;
    // Vol et nage d'Esquie (~3000-4000 u/s) : placé sur la cible ; marche (350 u/s) : conduit par les entrées.
    const bool flying = mine != kWmWalk && mine != kWmNone;
    // Écart à la position (prédite) du pilote : en vol, Esquie est placé sur la cible ; au sol, il est où il est.
    const double* at = flying ? s.tgt : me.pos;
    const double err = std::hypot(raw[0] - at[0], raw[1] - at[1], raw[2] - at[2]);
    // Statistiques (journal toutes les 15 s)
    ++s.ride_ticks;
    s.ride_err_sum += err;
    s.ride_err_max = std::max(s.ride_err_max, err);
    if (s.now - s.ride_since > 3.0) s.ride_err_max_all = std::max(s.ride_err_max_all, err);   // hors mise en place (montée, décollage)
    if (s.now - s.ride_stats_at >= 15.0) {
        log().write("coop_follower_ride_stats", JsonObj().add("ticks", s.ride_ticks).add("err_avg", s.ride_ticks ? s.ride_err_sum / s.ride_ticks : 0.0)
                                                    .add("err_max", s.ride_err_max).add("snaps", s.ride_snaps).add("wm_changes", s.wm_changes)
                                                    .add("wm", mine).add("pilot_wm", static_cast<int>(p.wm_mode)));
        s.ride_stats_at = s.now; s.ride_ticks = 0; s.ride_err_sum = 0; s.ride_err_max = 0;
    }
    if (flying || d > 1500) {
        if (!flying) {   // au sol mais décroché : recalé (rare)
            ++s.ride_snaps;
            if (bump) bump("follower_ride_snaps", 1);
            log().write("coop_follower_ride_snap", JsonObj().add("dist", d).add("wm", mine).add("pilot_move", static_cast<int>(p.move)));
        }
        game::teleport_pawn(esq, s.tgt, p.yaw);
        return;
    }
    // Au sol / sur l'eau : poursuite comme le suivi à pied (vitesse du pilote plus rattrapage proportionnel à l'écart).
    if (d2 < 40) return;
    const double sp = std::hypot(p.vx, p.vy);
    const double scale = std::clamp(sp / 350.0 + d2 / 300.0, 0.25, 1.0);
    game::move_pawn(esq, dx / d2, dy / d2, scale);
}

void end_ride(const char* why)
{
    if (!s.riding) return;
    s.riding = false;
    log().write("coop_follower_ride", JsonObj().add("on", false).add("why", why).add("seconds", s.now - s.ride_since).add("snaps", s.ride_snaps)
                                          .add("wm_changes", s.wm_changes));
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
    const int wm = wm_mode(pawn);   // Esquie : son mode sur la carte du monde (D20)
    m.wm_mode = static_cast<uint8_t>(wm < 0 || wm > 255 ? kWmNone : wm);
    s.other_cls.clear();
    if (mi.move == s.p_move && mi.custom == s.p_custom && m.flags == s.p_flags && mi.cls == s.p_cls && m.wm_mode == s.p_wm) return;
    const bool big = m.flags != s.p_flags || mi.cls != s.p_cls || m.wm_mode != s.p_wm;   // monture, pion ou mode d'Esquie changés
    s.p_move = mi.move; s.p_custom = mi.custom; s.p_flags = m.flags; s.p_cls = mi.cls; s.p_wm = m.wm_mode;
    if (big || log_allowed())
        log().write("coop_pilot_move", JsonObj().add("level", m.level).add("move", mi.move).add("custom", mi.custom).add("mount", mi.mount)
                                           .add("world_map", mi.world_map).add("attached", mi.attached).add("pawn", mi.cls).add("z", m.z).add("vz", m.vz)
                                           .add("wm_mode", static_cast<int>(m.wm_mode)));
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
    // Compteurs des tests : changements de mode de déplacement du pion d'ici et du pilote (boucle eau / air = beaucoup
    // de changements ici pour peu chez le pilote), passages en nage ici sans nage chez le pilote.
    if (bump && s.f_move != -2 && me.move != s.f_move) bump("follower_move_changes", 1);
    if (bump && s.f_pmove != -2 && p.move != s.f_pmove) bump("pilot_move_changes", 1);
    if (bump && me.move == 4 && s.f_move != 4 && p.move != 4) bump("follower_swims_unexpected", 1);
    log_follow_changes(p, me, ps.pos);

    // Monture reproduite ici (D20) : on monte sur Esquie quand le pilote y est, on le conduit comme lui (même mode : vol,
    // marche…), on en descend quand il est à pied. Le fantôme ne sert plus qu'en repli (montée refusée 3 fois).
    const bool world_map = (p.flags & proto::kPilotWorldMap) || me.world_map || p.level.find("WorldMap") != std::string::npos;
    const bool p_mount = (p.flags & proto::kPilotMount) != 0;
    if (p_mount != s.last_p_mount) { s.last_p_mount = p_mount; s.mount_tries = 0; s.dismount_tries = 0; s.next_mount = now + 0.3; }

    // Escalade (prises murales) : le suiveur rejoue la prise et grimpe, mais n'en sort pas sans entrée (essai à deux PC
    // du 2026-10-24 : Gustave resté en pose d'escalade dans les airs chez le client). Pilote sorti depuis 0,6 s -> fin
    // de l'escalade par le jeu (StopClimbing), marche, et placé sur le pilote.
    if (!world_map && me.move == kCustom && p.move != kCustom && p.move != 0) {
        if (s.climb_exit_since < 0) s.climb_exit_since = now;
        if (now - s.climb_exit_since >= 0.6 && now >= s.next_climb_fix) {
            s.next_climb_fix = now + 1.0;
            UObject* cc = ue::get_object(pawn, STR("BP_ClimbingComponent"));
            bool stopped = false;
            if (ue::Call c(cc, STR("StopClimbing")); c) stopped = c.invoke();
            if (ue::Call c(cc, STR("PrepareCharacterForWalking")); c) c.invoke();
            game::set_movement_mode(pawn, kWalk);
            const double pos[3] = {p.x, p.y, p.z};
            game::teleport_pawn(pawn, pos, p.yaw);
            if (bump) bump("follower_climb_exits", 1);
            log().write("coop_follower_climb_exit", JsonObj().add("custom", me.custom).add("pilot_move", static_cast<int>(p.move))
                                                       .add("component", cc != nullptr).add("stopped", stopped));
        }
    } else {
        s.climb_exit_since = -1;
    }
    if (world_map && mount_sync_enabled()) {
        if (me.mount && is_esquie(pawn)) {
            if (s.ghost) end_ghost(nullptr, "riding", nullptr, 0);
            if (p_mount) { ride(pawn, p, age, bump); return true; }
            if (s.dismount_tries < 8) {   // pilote à pied : on le rejoint au sol puis on descend
                proto::PilotState q = p;
                q.wm_mode = kWmWalk;
                ride(pawn, q, age, bump);
                try_dismount(pawn, p, me, bump);
                return true;
            }
        } else {
            end_ride(me.mount ? "not_esquie" : "dismounted");
        }
        if (p_mount && !me.mount && !s.ghost && s.mount_tries < 3) {   // montée en cours : pion laissé sur place
            try_mount(me, bump);
            return true;
        }
    }

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
    if (s.riding && s.followed_at < now) end_ride("not_following");
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
                                        .add("speed", std::hypot(ps.vel[0], ps.vel[1]))
                                        .add("wm_mode", ue::get_int(p, STR("CurrentMovementMode"), -1))
                                        .add("wm_prev", ue::get_int(p, STR("PreviousMovementMode"), -1))
                                        .add("zone", ue::get_int(p, STR("CurrentZoneType"), -1)).add("detected", ue::get_int(p, STR("DetectedZoneType"), -1))
                                        .add("ground_zone", ue::get_int(p, STR("GroundZoneType"), -1))
                                        .add("transition", ue::get_bool(p, STR("IsInTransition"))));
}

// Signature d'une fonction : noms, tailles et types des paramètres (pour l'appeler ensuite).
void signature(UObject* t, const std::string& fn)
{
    UFunction* f = t ? t->GetFunctionByNameInChain(ue::from_utf8(fn).c_str()) : nullptr;
    std::vector<std::string> ps;
    if (f) {
        for (FProperty* p : RC::Unreal::TFieldRange<FProperty>(f, RC::Unreal::EFieldIterationFlags::IncludeDeprecated)) {
            if (!p) continue;
            ps.push_back(ue::to_utf8(p->GetName()) + ":" + std::to_string(p->GetSize()) +
                         (RC::Unreal::CastField<RC::Unreal::FBoolProperty>(p) ? ":bool" : "") +
                         (RC::Unreal::CastField<RC::Unreal::FObjectPropertyBase>(p) ? ":object" : "") +
                         (RC::Unreal::CastField<RC::Unreal::FStructProperty>(p) ? ":struct" : ""));
        }
    }
    log().write("coop_mount_sig", JsonObj().add("target", ue::short_name(t)).add("fn", fn).add("found", f != nullptr).add("params", join(ps)));
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
    if (op == "sig") {
        UObject* t = target_of(st["target"].as_string("possessed"));
        for (size_t i = 0; i < st["fns"].size(); ++i) signature(t, st["fns"][i].as_string());
        return true;
    }
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
    // État du suiveur sur Esquie (scénarios client)
    if (name == "ride_err_max") return static_cast<int>(s.ride_err_max_all);
    if (name == "ride_snaps") return s.ride_snaps;
    if (name == "riding") return s.riding ? 1 : 0;
    if (name == "ghost") return s.ghost ? 1 : 0;
    auto it = b.counts.find(name);
    return it == b.counts.end() ? 0 : it->second;
}

} // namespace e33::mount
