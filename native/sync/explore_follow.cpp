// Pilote et suiveurs : PilotState, suivi fluide (D23), entrées de déplacement du suiveur.
#include "explore_internal.hpp"

namespace e33::explore::detail {

// --- Pilote et suiveurs -------------------------------------------------------------------------
void pilot_tick()
{
    if (s.now < s.next_pilot_send) return;
    // Échéance cumulée : avec « maintenant + 50 ms », un envoi partait toutes les 4 images à 60 i/s (~15 par seconde,
    // mesuré le 2026-10-22 : écarts de 60-70 ms à la réception).
    s.next_pilot_send = std::max(s.next_pilot_send + kPilotPeriod, s.now - kPilotPeriod);
    if (s.host && joined_players() == 0) return;
    if (!s.host && s.cj != S::ClientJoin::Joined && s.want_join) return;
    UObject* pawn = game::explore_pawn();
    if (!pawn && !game::map_change_in_progress()) mount::pilot_without_pawn(s.now);   // diagnostic : monture ?
    if (!pawn || game::map_change_in_progress()) return;
    game::PawnState ps;
    if (!game::pawn_state(pawn, ps)) return;
    s.local_speed = std::hypot(ps.vel[0], ps.vel[1]);
    s.local_speed_at = s.now;
    proto::PilotState m;
    m.pilot = s.me;
    m.seq = ++s.pilot_seq;
    m.level = game::current_level();
    m.x = ps.pos[0]; m.y = ps.pos[1]; m.z = ps.pos[2]; m.yaw = ps.yaw;
    m.vx = ps.vel[0]; m.vy = ps.vel[1]; m.vz = ps.vel[2];
    mount::fill_pilot(pawn, m, s.now);   // mode de déplacement, monture (carte du monde)
    m.t_ms = 1 + static_cast<uint32_t>(std::fmod(s.now * 1000.0, 4.0e9));   // horloge du pilote (tampon du suiveur)
    if (auto it = g_char_of.find(pawn); it != g_char_of.end()) m.chr = it->second;   // personnage affiché
    s.net.publish(m, false);
}

// Maximum d'une mesure, tenu dans un compteur (les scénarios vérifient les compteurs) : on ajoute la différence.
void gauge_max(const char* name, double v)
{
    static std::map<std::string, int> mx;
    const int iv = static_cast<int>(std::lround(v));
    int& cur = mx[name];
    if (iv > cur) { bump(name, iv - cur); cur = iv; }
}

void follow_buf_reset(const char* why)
{
    if (!fc.buf.empty()) log().write("coop_follow_buffer_reset", JsonObj().add("why", why).add("samples", static_cast<int64_t>(fc.buf.size())));
    fc.buf.clear();
    fc.off_high = 0;
    fc.discontinuity_at = s.now;
}

// Suiveur : position du pilote reçue -> tampon.
void follow_push(const proto::PilotState& p)
{
    const bool clock = p.t_ms != 0;
    if (p.pilot != fc.pilot || p.level != fc.level || clock != fc.clock) {
        follow_buf_reset(p.pilot != fc.pilot ? "pilot" : p.level != fc.level ? "level" : "clock");
        fc.pilot = p.pilot; fc.level = p.level; fc.clock = clock;
    }
    PilotSample smp;
    smp.t = clock ? p.t_ms / 1000.0 : s.now;
    smp.pos[0] = p.x; smp.pos[1] = p.y; smp.pos[2] = p.z;
    smp.vel[0] = p.vx; smp.vel[1] = p.vy; smp.vel[2] = p.vz;
    smp.yaw = p.yaw;
    if (!fc.buf.empty()) {
        const PilotSample& last = fc.buf.back();
        if (!clock && smp.t <= last.t + 0.005) smp.t = last.t + 0.01;   // deux positions lues dans la même image
        if (clock && smp.t <= last.t) {
            // En retard (réordonnée) : rangée à sa place si elle sert encore, sinon ignorée.
            if (smp.t <= fc.buf.front().t) return;
            auto it = std::upper_bound(fc.buf.begin(), fc.buf.end(), smp.t, [](double t, const PilotSample& x) { return t < x.t; });
            if (it != fc.buf.begin() && std::prev(it)->t == smp.t) return;
            fc.buf.insert(it, smp);
            return;
        }
        // Trou de données (pilote en chargement, client qui recharge) ou saut du pilote (téléportation, grappin) :
        // pas d'interpolation à travers ; le suiveur peut alors être téléporté (classé « gap »).
        const double gap = smp.t - last.t;
        const double px = last.pos[0] + last.vel[0] * std::min(gap, 0.5), py = last.pos[1] + last.vel[1] * std::min(gap, 0.5);
        const double jump = std::hypot(smp.pos[0] - px, smp.pos[1] - py);
        if (gap > 1.0 || jump > 400 || std::fabs(smp.pos[2] - last.pos[2]) > 400) {
            bump("follow_discontinuities");
            log().write("coop_follow_discontinuity", JsonObj().add("gap_ms", static_cast<int64_t>(gap * 1000.0)).add("jump", std::round(jump)));
            follow_buf_reset("discontinuity");
        }
    }
    // Transit minimal (horloge du pilote -> la nôtre). Une latence durablement plus haute (> 0,5 s, 3 fois de suite) ou
    // une autre horloge du pilote réinitialise la référence.
    const double o = s.now - smp.t;
    if (fc.buf.empty()) { fc.off = o; fc.off_high = 0; }
    else if (o < fc.off) { fc.off = o; fc.off_high = 0; }
    else if (o > fc.off + 0.5) {
        if (++fc.off_high >= 3) {
            log().write("coop_follow_clock_reset", JsonObj().add("shift_ms", static_cast<int64_t>((o - fc.off) * 1000.0)));
            fc.off = o;
            fc.off_high = 0;
        }
    } else fc.off_high = 0;
    fc.buf.push_back(smp);
    fc.last_arrival = s.now;
    while (fc.buf.size() > 2 && fc.buf.back().t - fc.buf.front().t > 2.0) fc.buf.pop_front();
}

bool follow_render(double r, Rendered& out)
{
    if (fc.buf.empty()) return false;
    const PilotSample& front = fc.buf.front();
    const PilotSample& back = fc.buf.back();
    if (r <= front.t) {
        for (int i = 0; i < 3; ++i) { out.pos[i] = front.pos[i]; out.vel[i] = front.vel[i]; }
        out.yaw = front.yaw;
        return true;
    }
    if (r >= back.t) {
        const double ex = r - back.t;
        out.starved = ex > 0.0;
        out.stale = ex > kExtrapolateMax;
        const double k = std::min(ex, kExtrapolateMax);
        for (int i = 0; i < 3; ++i) { out.pos[i] = back.pos[i] + back.vel[i] * k; out.vel[i] = out.stale ? 0.0 : back.vel[i]; }
        out.yaw = back.yaw;
        return true;
    }
    auto it = std::upper_bound(fc.buf.begin(), fc.buf.end(), r, [](double t, const PilotSample& x) { return t < x.t; });
    const PilotSample& b = *it;
    const PilotSample& a = *std::prev(it);
    const double h = std::max(1e-3, b.t - a.t), f = std::clamp((r - a.t) / h, 0.0, 1.0);
    const double h00 = 2 * f * f * f - 3 * f * f + 1, h10 = f * f * f - 2 * f * f + f, h01 = -2 * f * f * f + 3 * f * f, h11 = f * f * f - f * f;
    for (int i = 0; i < 3; ++i) {
        out.pos[i] = h00 * a.pos[i] + h10 * h * a.vel[i] + h01 * b.pos[i] + h11 * h * b.vel[i];
        out.vel[i] = a.vel[i] + (b.vel[i] - a.vel[i]) * f;
    }
    out.yaw = a.yaw + (std::fmod(b.yaw - a.yaw + 540.0, 360.0) - 180.0) * f;
    return true;
}

// Vitesse de sprint de l'ALS relevée par le mod (rattrapage du suiveur) -> rendue.
void max_speed_restore(UObject* pawn)
{
    if (!fc.max_written || !pawn) return;
    fc.max_written = false;
    if (fc.speeds_ok) game::set_pawn_sprint_speed(pawn, fc.speeds.sprint);
}

void sprint_start(UObject* pawn)
{
    if (!s.sprint_touched) s.gait_before = game::pawn_gait(pawn, true);
    s.sprint_touched = true;
    s.follow_sprint = true;
    s.sprint_since = s.now;
    // SetDesiredGait directement : SprintAction (la touche du joueur) restait sans effet sur le pion suiveur (repli
    // systématique après 0,6 s dans p8_sprint_client, 2026-10-23).
    s.sprint_method = game::set_pawn_desired_gait(pawn, 2) ? 2 : 0;
    fc.next_gait_assert = s.now + 0.5;
    ++fstats.sprints;
    bump("follow_sprints");
    if (!s.sprint_logged) {   // une fois par session (le résumé compte les suivants)
        s.sprint_logged = true;
        log().write("coop_follow_sprint", JsonObj().add("method", s.sprint_method).add("gait_before", s.gait_before)
                                             .add("gait", game::pawn_gait(pawn)));
    }
}

void sprint_off(UObject* pawn)
{
    if (game::pawn_gait(pawn, true) == 2) game::set_pawn_desired_gait(pawn, s.gait_before >= 0 && s.gait_before != 2 ? s.gait_before : 1);
    max_speed_restore(pawn);
}

void sprint_stop(UObject* pawn)
{
    fc.sprint = false;
    fc.sprint_low_since = -1;
    if (!s.follow_sprint) { max_speed_restore(pawn); return; }
    s.follow_sprint = false;
    if (pawn) sprint_off(pawn);
}

// Plus suiveur (pilote, combat, chargement, fin) : sprint et vitesse rendus au pion s'il est encore là.
void sprint_release()
{
    fc.moving = false;
    fc.follow_since = -1;
    if (!s.follow_sprint && !fc.max_written) return;
    sprint_stop(game::in_battle() ? nullptr : game::explore_pawn());
}

// Défaut de fluidité journalisé (au plus un par seconde ; les compteurs comptent tout).
void follow_event(const char* what, double v, double rv, double own, double gap)
{
    if (s.now - fc.last_event_log < 1.0) return;
    fc.last_event_log = s.now;
    log().write("coop_follow_glitch", JsonObj().add("what", what).add("value", std::round(v)).add("pilot_speed", std::round(rv))
                                         .add("own_speed", std::round(own)).add("gap", std::round(gap)).add("sprint", fc.sprint)
                                         .add("starved", s.now - fc.last_arrival));
}

// Mesures de fluidité chez le suiveur (compteurs vérifiés par les scénarios ; D23) :
//  - follow_gap_max_{walk,run,sprint} : écart max (u) au pilote (sa dernière position prolongée jusqu'à maintenant),
//    allure du pilote stable (vitesse rendue et actuelle dans la même classe : marche 40-260, course 260-500, sprint
//    > 500) ; follow_gap_rest_max : écart au pilote arrêté depuis 2 s ;
//  - follow_stalls : pion presque arrêté (< 30 % de la vitesse du pilote) >= 150 ms alors que le pilote bouge (> 150
//    u/s depuis 0,5 s) ; follow_frozen_ms_max : plus longue immobilité du pion (< 20 u/s) pilote en mouvement (> 100) ;
//  - follow_dips : vitesse du pion < 60 % de celle du pilote >= 150 ms pendant une course / un sprint continu (> 300
//    u/s depuis 1 s) ; follow_speed_drops : chute de vitesse du pion > 250 u/s en 0,1 s sans ralentissement du pilote ;
//  - follow_parasitic_stops (follow_tick) : arrêt du pion alors que le pilote bouge et que ses positions arrivent.
// Hors mesure : 2 s après un trou de données ou une téléportation, positions vieilles de plus de 0,5 s, 2 premières s.
void follow_metrics(const Rendered& R, double rv, double own, double gap_now, double rest_gap, double pilot_now_speed)
{
    const bool excluded = s.now - fc.discontinuity_at < 2.0 || s.now - fc.teleport_at < 2.0 || s.now - fc.last_arrival > 0.5 ||
                          fc.follow_since < 0 || s.now - fc.follow_since < 2.0 || R.stale;
    fc.hist.push_back({s.now, own, rv});
    while (!fc.hist.empty() && s.now - fc.hist.front()[0] > 0.3) fc.hist.pop_front();
    if (rv > 150) { if (fc.rv_move_since < 0) fc.rv_move_since = s.now; } else fc.rv_move_since = -1;
    if (rv > 300) { if (fc.rv_fast_since < 0) fc.rv_fast_since = s.now; } else fc.rv_fast_since = -1;
    if (excluded) { fc.stall_since = fc.frozen_since = fc.dip_since = fc.rest_since = -1; return; }
    // Écarts par allure du pilote
    auto cls = [](double v) { return v < 5 ? 0 : v <= 40 ? -1 : v <= 260 ? 1 : v <= 500 ? 2 : 3; };
    const int c1 = cls(rv), c2 = cls(pilot_now_speed);
    if (c1 != fc.cls) { fc.cls = c1; fc.cls_since = s.now; }   // allure stable : même classe depuis 0,5 s
    if (c1 == c2 && c1 > 0 && s.now - fc.cls_since >= 0.5) gauge_max(c1 == 1 ? "follow_gap_max_walk" : c1 == 2 ? "follow_gap_max_run" : "follow_gap_max_sprint", gap_now);
    if (c1 == 0 && c2 == 0) { if (fc.rest_since < 0) fc.rest_since = s.now; else if (s.now - fc.rest_since >= 2.0) gauge_max("follow_gap_rest_max", rest_gap); }
    else fc.rest_since = -1;
    // Pion arrêté alors que le pilote bouge
    if (fc.rv_move_since >= 0 && s.now - fc.rv_move_since >= 0.5 && own < 0.3 * rv) {
        if (fc.stall_since < 0) { fc.stall_since = s.now; fc.stall_counted = false; }
        const double ms = (s.now - fc.stall_since) * 1000.0;
        gauge_max("follow_stall_ms_max", ms);
        if (ms >= 150 && !fc.stall_counted) { fc.stall_counted = true; bump("follow_stalls"); follow_event("stall", ms, rv, own, gap_now); }
    } else fc.stall_since = -1;
    if (rv > 100 && own < 20) {
        if (fc.frozen_since < 0) fc.frozen_since = s.now;
        gauge_max("follow_frozen_ms_max", (s.now - fc.frozen_since) * 1000.0);
    } else fc.frozen_since = -1;
    // Creux de vitesse pendant une course / un sprint continu
    if (fc.rv_fast_since >= 0 && s.now - fc.rv_fast_since >= 1.0 && own < 0.6 * rv) {
        if (fc.dip_since < 0) { fc.dip_since = s.now; fc.dip_counted = false; }
        if (s.now - fc.dip_since >= 0.15 && !fc.dip_counted) { fc.dip_counted = true; bump("follow_dips"); follow_event("dip", own / std::max(1.0, rv) * 100.0, rv, own, gap_now); }
    } else fc.dip_since = -1;
    // Chute brusque de la vitesse du pion (à-coup) : > 250 u/s en ~0,1 s, sous 85 % de la vitesse du pilote, qui n'a
    // pas ralenti de plus de 10 % sur les 0,3 dernières s (sinon le pion ne fait que le suivre).
    double rv_max = rv;
    for (const auto& h : fc.hist) rv_max = std::max(rv_max, h[2]);
    for (const auto& h : fc.hist) {
        if (s.now - h[0] > 0.13) continue;
        if (s.now - h[0] < 0.09) break;
        if (rv > 150 && h[1] - own > 250 && rv >= 0.9 * rv_max && own < 0.85 * rv && s.now - fc.last_drop_at > 0.3) {
            fc.last_drop_at = s.now;
            bump("follow_speed_drops");
            follow_event("speed_drop", h[1] - own, rv, own, gap_now);
        }
        break;
    }
}

void teleport_follower(UObject* pawn, const Rendered& R, const char* why, double err, double dz, double rv, double own_speed)
{
    game::location(pawn, fc.tp_from);
    game::teleport_pawn(pawn, R.pos, R.yaw);
    bump("follower_teleports");
    if (std::string(why) != "gap") bump("follower_teleports_nogap");
    ++fstats.teleports;
    fc.teleport_at = s.now;
    fc.far_since = fc.dz_since = fc.stuck_since = -1;
    log().write("coop_follower_teleport", JsonObj().add("why", why).add("distance", err).add("dz", dz).add("pilot_speed", rv).add("own_speed", own_speed)
                                             .add("gait", game::pawn_gait(pawn)).add("sprint", s.follow_sprint)
                                             .add("age_ms", static_cast<int64_t>((s.now - s.last_pilot_at) * 1000.0)));
}

void follow_tick()
{
    if (!s.last_pilot || s.last_pilot->pilot != s.pilot) { sprint_release(); return; }
    if (!s.host && s.want_join && s.cj != S::ClientJoin::Joined) return;
    if (game::map_change_in_progress() || game::in_battle()) {
        // MaxWalkSpeed éventuellement réglée : rendue au retour du pion (max_written gardé).
        s.follow_sprint = false; fc.sprint = false; fc.moving = false; fc.follow_since = -1;
        return;
    }
    UObject* pawn = game::explore_pawn();
    if (!pawn) return;
    const auto& p = *s.last_pilot;
    const std::string level = game::current_level();
    if (level != p.level) {
        // Niveau différent sans MapChange reçu (perdu, ou arrivé avant nous) : on rejoint le niveau du pilote.
        // Seulement sur une position fraîche : un pilote qui charge n'envoie plus rien (sa dernière zone est périmée).
        if (s.now - s.last_pilot_at > 1.5) { s.level_mismatch_since = -1; return; }
        if (s.level_mismatch_since < 0) s.level_mismatch_since = s.now;
        if (s.now - s.level_mismatch_since > 8.0) {
            s.level_mismatch_since = -1;
            bump("level_resyncs");
            log().write("coop_level_resync", JsonObj().add("mine", level).add("pilot", p.level));
            {
                // Point d'apparition du dernier MapChange vers ce niveau, s'il est connu (sinon point principal du niveau).
                auto it = s.map_spawns.find(p.level);
                game::change_map(ue::from_utf8(p.level), it == s.map_spawns.end() ? std::wstring() : ue::from_utf8(it->second));
            }
        }
        return;
    }
    s.level_mismatch_since = -1;
    // Personnage affiché (changement de personnage en exploration, essai à deux PC du 2026-10-24) : le même que le
    // pilote. Un essai par seconde au plus ; le pion d'un personnage inconnu ici (vide) est laissé tel quel.
    if (!p.chr.empty() && s.now >= s.next_char_fix) {
        auto it = g_char_of.find(pawn);
        if (it == g_char_of.end() || it->second != p.chr) {
            s.next_char_fix = s.now + 1.0;
            ue::Call c(pawn, STR("LoadAndActivateCharacter"));
            const bool ok = c && c.set_fname(STR("CharacterName"), ue::from_utf8(p.chr).c_str()).set_bool(STR("HasSwitchCharacter"), true).invoke();
            bump(ok ? "follower_character_switches" : "follower_character_switch_errors");
            log().write("coop_follower_character", JsonObj().add("from", it == g_char_of.end() ? std::string() : it->second).add("to", p.chr).add("ok", ok));
            if (ok) g_char_of[pawn] = p.chr;   // le hook le note aussi ; sans lui, pas de nouvel essai chaque seconde
        }
    }
    // Carte du monde : pilote sur Esquie (vol, nage) non reproductible ici -> pion tenu en fantôme sur le pilote (D19).
    if (mount::follow(pawn, p, s.now - s.last_pilot_at, s.now, s.net.bump)) {
        if (fc.sprint || fc.max_written || s.follow_sprint) sprint_stop(pawn);
        fc.moving = false;
        fc.follow_since = -1;
        return;
    }
    if (fc.buf.empty()) return;
    game::PawnState me;
    if (!game::pawn_state(pawn, me)) return;
    const double own_speed = std::hypot(me.vel[0], me.vel[1]);
    s.local_speed = own_speed;
    s.local_speed_at = s.now;
    if (fc.follow_since < 0) fc.follow_since = s.now;
    const double ctl_dt = std::clamp(s.now - fc.ctl_last, 0.0, 0.1);
    fc.ctl_last = s.now;
    // Vitesses ALS du pion (relues toutes les 2 s)
    if (s.now - fc.speeds_at > 2.0 && !fc.max_written) {   // pas pendant que le mod a relevé le sprint
        fc.speeds_at = s.now;
        fc.speeds_ok = game::pawn_gait_speeds(pawn, fc.speeds);
        if (!fc.speeds_logged) {
            fc.speeds_logged = true;
            const float* mws = game::pawn_max_walk_speed(pawn);
            log().write("coop_follow_als", JsonObj().add("ok", fc.speeds_ok).add("walk", fc.speeds.walk).add("run", fc.speeds.run)
                                              .add("sprint", fc.speeds.sprint).add("max_walk", mws ? static_cast<double>(*mws) : -1.0)
                                              .add("gait", game::pawn_gait(pawn)).add("desired", game::pawn_gait(pawn, true))
                                              .add("clock", fc.clock));
        }
    }
    const double run_speed = fc.speeds_ok ? fc.speeds.run : 350.0, sprint_speed = fc.speeds_ok ? fc.speeds.sprint : 720.0;
    // Pilote rendu (tampon d'interpolation)
    Rendered R;
    if (!follow_render(s.now - fc.off - (fc.clock ? kRenderDelay : kRenderDelayNoClock), R)) return;
    const PilotSample& last = fc.buf.back();
    const double ex_now = std::clamp(s.now - fc.off - last.t, 0.0, 0.25);
    const double pilot_now_speed = std::hypot(last.vel[0], last.vel[1]);
    const double ex = R.pos[0] - me.pos[0], ey = R.pos[1] - me.pos[1], dz = R.pos[2] - me.pos[2];
    const double err = std::hypot(ex, ey);
    const double gap_now = std::hypot(last.pos[0] + last.vel[0] * ex_now - me.pos[0], last.pos[1] + last.vel[1] * ex_now - me.pos[1]);
    const double rest_gap = std::hypot(last.pos[0] - me.pos[0], last.pos[1] - me.pos[1]);
    s.follow_error = gap_now;
    const double rv = std::hypot(R.vel[0], R.vel[1]);
    // Statistiques (résumé toutes les 15 s)
    auto& f = fstats;
    if (f.since < 0) f.since = s.now;
    const double fdt = f.last_t < 0 ? 0.0 : std::clamp(s.now - f.last_t, 0.0, 0.25);
    f.last_t = s.now;
    ++f.ticks;
    f.err_sum += gap_now;
    f.err_max = std::max(f.err_max, gap_now);
    f.age_max = std::max(f.age_max, s.now - s.last_pilot_at);
    f.pilot_speed_max = std::max(f.pilot_speed_max, pilot_now_speed);
    if (fc.moving) { f.own_speed_sum += own_speed; ++f.own_speed_n; }
    f.own_speed_max = std::max(f.own_speed_max, own_speed);
    if (R.starved) f.starved_s += fdt;
    // Téléportation
    const char* tp = nullptr;
    const bool after_gap = s.now - fc.discontinuity_at < 3.0;
    if (err > kTeleportDistance && after_gap) tp = "gap";
    else if (err > 2 * kTeleportDistance) tp = "far";
    else if (err > kTeleportDistance) {
        if (fc.far_since < 0) { fc.far_since = s.now; fc.far_err0 = err; }
        else if (s.now - fc.far_since > 1.0) { if (err > fc.far_err0 - 50) tp = "far"; else { fc.far_since = s.now; fc.far_err0 = err; } }
    } else fc.far_since = -1;
    if (!tp) {
        if (std::fabs(dz) > 400) { if (fc.dz_since < 0) fc.dz_since = s.now; else if (s.now - fc.dz_since > 0.5) tp = after_gap ? "gap" : "dz"; }
        else fc.dz_since = -1;
    }
    // Téléportation annulée par le jeu (pion revenu là où il était : 4017 téléportations en 25 min chez l'hôte le
    // 2026-10-25, une par image, cible 700 u plus bas) : attente croissante avant la suivante (1, 2, 4, 8 s, 10 s au plus).
    if (tp && s.now - fc.teleport_at < 2.0 && std::hypot(me.pos[0] - fc.tp_from[0], me.pos[1] - fc.tp_from[1]) < 300 && std::fabs(me.pos[2] - fc.tp_from[2]) < 300) {
        ++fc.tp_fails;
        fc.tp_block_until = s.now + std::min(10.0, static_cast<double>(1 << std::min(fc.tp_fails - 1, 4)));
        bump("follower_teleports_undone");
        log().write("coop_follower_teleport_undone", JsonObj().add("why", tp).add("fails", fc.tp_fails).add("wait_s", fc.tp_block_until - s.now).add("dz", std::round(dz)));
    } else if (tp && s.now - fc.teleport_at > 10.0) fc.tp_fails = 0;
    if (tp && s.now < fc.tp_block_until) tp = nullptr;
    const bool fresh = s.now - s.last_pilot_at < 1.0;   // mesures d'arrivée (changement de niveau) : position récente du pilote seulement
    if (tp) { if (fresh) mt_follow(gap_now, true, 0, 0); teleport_follower(pawn, R, tp, err, dz, rv, own_speed); return; }
    // Vitesse voulue : celle du pilote rendu + rattrapage de l'écart, plafonnée.
    double vx = R.vel[0] + kFollowGain * ex, vy = R.vel[1] + kFollowGain * ey;
    double vc = std::hypot(vx, vy);
    const double vmax = sprint_speed * kCatchUpMax;
    if (vc > vmax) { vx *= vmax / vc; vy *= vmax / vc; vc = vmax; }
    // Départ / arrêt (hystérésis) : arrêt seulement pilote arrêté et pion arrivé.
    // Positions en retard (pointe réseau, < 1 s) : ni arrêt ni sortie du sprint (mesuré : arrêt puis freinage en plein sprint).
    const bool late = R.starved && s.now - fc.last_arrival < 1.0;
    int started = 0, stopped = 0;
    if (!fc.moving && (rv > 30 || err > 70)) { fc.moving = true; bump("follow_starts"); ++f.starts; started = 1; }
    else if (fc.moving && rv < 10 && err < 25 && !late) {
        fc.moving = false;
        ++f.stops;
        stopped = 1;
        if (s.now - fc.last_arrival < 0.3 && pilot_now_speed > 100 && s.now - fc.follow_since >= 2.0) {
            bump("follow_parasitic_stops");
            follow_event("parasitic_stop", pilot_now_speed, rv, own_speed, gap_now);
        }
    }
    if (fresh) mt_follow(gap_now, false, started, stopped);
    follow_metrics(R, rv, own_speed, gap_now, rest_gap, pilot_now_speed);
    // Trace détaillée (-e33followtrace=1, réglage du suivi) : une ligne toutes les 0,1 s.
    static const bool trace = !cmdline::arg(L"e33followtrace").empty();
    static double next_trace = 0;
    if (trace && s.now >= next_trace) {
        next_trace = s.now + 0.1;
        const float* m = game::pawn_max_walk_speed(pawn);
        log().write("coop_follow_trace", JsonObj().add("err", std::round(err)).add("gap", std::round(gap_now)).add("rv", std::round(rv))
                                            .add("own", std::round(own_speed)).add("vc", std::round(vc)).add("mv", fc.moving).add("sp", fc.sprint)
                                            .add("mws", m ? std::round(static_cast<double>(*m)) : -1.0).add("gait", game::pawn_gait(pawn))
                                            .add("st", R.starved).add("buf", static_cast<int64_t>(fc.buf.size())).add("dz", std::round(dz))
                                            .add("ss", std::round(fc.sprint_set))
                                            .add("ang", own_speed > 50 && vc > 1 ? std::round(std::acos(std::clamp((me.vel[0] * vx + me.vel[1] * vy) / (own_speed * vc), -1.0, 1.0)) * 57.2958) : 0.0)
                                            .add("x", std::round(me.pos[0])).add("y", std::round(me.pos[1])));
    }
    // Pion bloqué (obstacle) : vitesse voulue nette mais immobile 2 s -> téléportation.
    if (fc.moving && vc > 200 && own_speed < 40 && err > 150) {
        if (fc.stuck_since < 0) fc.stuck_since = s.now;
        else if (s.now - fc.stuck_since > 2.0) { teleport_follower(pawn, R, "stuck", err, dz, rv, own_speed); return; }
    } else fc.stuck_since = -1;
    // Allure : sprint quand le pilote rendu sprinte (> 1,1 x course), ou pour rattraper un retard net (> 120 u) ; course
    // dès qu'il repasse sous la course et que le retard est faible. Sans délai : le pion ralentit avec le pilote (avant :
    // sprint gardé 0,3 s, le pion dépassait le pilote puis freinait d'un coup).
    const bool want_sprint = fc.moving && (rv > run_speed * 1.1 || (err > 120 && vc > run_speed * 1.08));
    // Sortie du sprint seulement une fois la vitesse de sprint redescendue près de la course (rampe ci-dessous) : sinon
    // l'ALS freine d'un coup jusque sous la course (mesuré : 737 -> 206 u/s en 0,1 s).
    const bool keep_sprint = fc.moving && (late || rv > run_speed * 1.03 || vc > run_speed * 1.03 || fc.sprint_set > run_speed + 30);
    if (want_sprint && !fc.sprint) { fc.sprint = true; fc.sprint_set = std::max(own_speed, run_speed + 15); sprint_start(pawn); }
    else if (fc.sprint && !keep_sprint) sprint_stop(pawn);
    if (fc.sprint) {
        f.sprint_s += fdt;
        if (own_speed > run_speed + 50) f.sprint_gait_ok_s += fdt;
        // Diagnostic : sprint voulu mais pas accordé par l'ALS (hôte suiveur à 300 u/s, allure 1, pendant que le client
        // sprintait, 2026-10-25 ; jamais vu chez le client suiveur). L'ALS refuse le sprint si l'entrée < 0,9 ou, en mode
        // « LookingDirection », si le mouvement s'écarte de plus de 50° de la visée : on relève ces valeurs.
        static double refused_since = -1, next_refused_log = 0;
        if (own_speed < run_speed + 20 && fc.moving) {
            if (refused_since < 0) refused_since = s.now;
            if (s.now - refused_since > 1.5 && s.now >= next_refused_log) {
                next_refused_log = s.now + 10.0;
                bump("follow_sprint_refused");
                const auto* rm = ue::value_ptr<uint8_t>(pawn, STR("RotationMode"));
                const auto* st = ue::value_ptr<uint8_t>(pawn, STR("Stance"));
                const auto* in = ue::value_ptr<float>(pawn, STR("MovementInputAmount"));
                const auto* aim = ue::value_ptr<double>(pawn, STR("AimingRotation"));
                const double vel_yaw = std::atan2(me.vel[1], me.vel[0]) * 57.2958;
                log().write("coop_follow_sprint_refused", JsonObj().add("own", std::round(own_speed)).add("rv", std::round(rv))
                                                            .add("gait", game::pawn_gait(pawn)).add("desired", game::pawn_gait(pawn, true))
                                                            .add("rot_mode", rm ? static_cast<int>(*rm) : -1).add("stance", st ? static_cast<int>(*st) : -1)
                                                            .add("input", in ? static_cast<double>(*in) : -1.0)
                                                            .add("aim_yaw", aim ? std::round(aim[1]) : 0.0).add("vel_yaw", std::round(vel_yaw))
                                                            .add("speeds", fc.speeds_ok).add("sprint_set", std::round(fc.sprint_set)));
            }
        } else refused_since = -1;
        if (s.now >= fc.next_gait_assert) {   // le jeu peut remettre l'allure (fin de dialogue, zone…)
            fc.next_gait_assert = s.now + 0.5;
            if (game::pawn_gait(pawn, true) != 2) game::set_pawn_desired_gait(pawn, 2);
        }
    } else if (s.sprint_touched && s.now >= fc.next_gait_assert) {
        fc.next_gait_assert = s.now + 0.5;
        if (game::pawn_gait(pawn, true) == 2) sprint_off(pawn);   // sprint resté après un combat ou un chargement
    }
    const float* mws = game::pawn_max_walk_speed(pawn);
    if (!fc.sprint && fc.max_written) max_speed_restore(pawn);
    if (!fc.moving || vc < 1) return;
    double scale;
    if (fc.sprint) {
        // Entrée pleine (l'ALS n'accorde le sprint qu'au-delà de 0,9). La vitesse suit la vitesse voulue par la vitesse de
        // sprint des réglages ALS (de la course + 15 à 1,2 x le sprint, rendue ensuite) : le pion ralentit avec le pilote
        // au lieu de rester à 720 puis de freiner d'un coup au passage en course (mesuré : 662 -> 185 u/s en 0,1 s).
        // Vitesse lissée : montée en ~0,1 s, descente limitée à 1500 u/s² (le pilote passe de 720 à 350 en ~0,25 s), pas de
        // va-et-vient d'une image à l'autre (une vitesse max qui passe sous la vitesse du pion le fait freiner).
        scale = 1.0;
        const double target = std::clamp(vc, run_speed + 15.0, vmax);
        const double cdt = ctl_dt;
        if (target > fc.sprint_set) fc.sprint_set += (target - fc.sprint_set) * std::min(1.0, cdt / 0.1);
        else fc.sprint_set = std::max(target, fc.sprint_set - 1500.0 * cdt);
        if (fc.speeds_ok) {
            fc.max_written = true;
            game::set_pawn_sprint_speed(pawn, static_cast<float>(fc.sprint_set));
        }
    } else {
        const double cap = mws && *mws > 10.0f ? static_cast<double>(*mws) : run_speed;
        scale = std::clamp(vc / cap, 0.0, 1.0);
    }
    game::move_pawn(pawn, vx / vc, vy / vc, scale);
}

void set_pilot(uint8_t p, const char* why)
{
    if (p == s.pilot) return;
    log().write("coop_pilot_change", JsonObj().add("from", static_cast<int>(s.pilot)).add("to", static_cast<int>(p)).add("why", why));
    s.pilot = p;
    s.last_pilot.reset();
    bump("pilot_changes");
}

void key_tick()
{
    // F8 : prendre la main (fenêtre du jeu au premier plan seulement).
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !s.f8_down) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) request_pilot();
    }
    s.f8_down = down;
}

UObject* input_sub()
{
    static UObject* sub = nullptr;   // sous-système du joueur local : persistant d'un niveau à l'autre
    if (!sub) sub = game::input_subsystem();
    return sub;
}

UObject* locomotion_context(size_t i)
{
    static UObject* cache[std::size(kLocomotionContexts)] = {};
    if (!cache[i]) cache[i] = ue::find_object(kLocomotionContexts[i]);
    return cache[i];
}

std::optional<int> context_priority(UObject* sub, UObject* imc)
{
    ue::Call has(sub, STR("HasMappingContext"));
    if (!has) return std::nullopt;
    has.set_object(STR("MappingContext"), imc).invoke();
    if (!has.out_bool(STR("ReturnValue"))) return std::nullopt;
    return static_cast<int>(has.out_int(STR("OutFoundPriority")));
}

// -e33locomotioninbattle=1 : ancien comportement (contextes rendus même en combat), pour le scénario « avant ».
bool restore_in_battle()
{
    static const bool v = cmdline::arg(L"e33locomotioninbattle") == L"1";
    return v;
}

bool context_wanted_locked(size_t i)
{
    const bool follower = sharing() && !is_pilot() && !game::in_battle();
    if (i < 4) return follower && game::explore_pawn();   // déplacement
    if (i == 4) return follower && game::explore_pawn() && !game::in_dialogue();   // interactions
    return follower && s.choice_pending;                  // choix de dialogue
}

void movement_lock_tick()
{
    bool any = false;
    for (size_t i = 0; i < std::size(kLocomotionContexts); ++i) any = any || context_wanted_locked(i);
    if (!any && s.removed_locomotion.empty()) return;
    UObject* sub = input_sub();
    if (!sub) return;
    for (size_t i = 0; i < std::size(kLocomotionContexts); ++i) {
        UObject* imc = locomotion_context(i);
        if (!imc) continue;
        const auto prio = context_priority(sub, imc);
        const bool lock = context_wanted_locked(i);
        if (lock && prio) {
            ue::Call rm(sub, STR("RemoveMappingContext"));
            if (!rm) return;
            rm.set_object(STR("MappingContext"), imc).invoke();
            if (!s.removed_locomotion.count(imc)) s.removed_locomotion[imc] = *prio;
            bump(i < 4 ? "movement_contexts_removed" : "follower_actions_blocked");
            log().write("coop_movement_context", JsonObj().add("removed", ue::to_utf8(imc->GetName())).add("priority", *prio));
        } else if (!lock) {
            auto it = s.removed_locomotion.find(imc);
            if (it == s.removed_locomotion.end()) continue;
            // Jamais rendus en combat : le jeu y retire lui-même les contextes d'exploration et les remet à la fin.
            // Rendu en combat, IMC_Locomotion_Default_ALS (priorité 1) passait devant IMC_BattleDefense (0) et prenait
            // RB (parade) et RT (contres, tir de la visée libre) à la manette du client (essai à deux PC du 2026-10-24).
            if (game::in_battle() && !restore_in_battle()) continue;
            if (!prio) {
                ue::Call add(sub, STR("AddMappingContext"));
                if (!add) return;
                add.set_object(STR("MappingContext"), imc).set_int(STR("Priority"), it->second).invoke();
                bump("movement_contexts_restored");
                log().write("coop_movement_context", JsonObj().add("restored", ue::to_utf8(imc->GetName())).add("priority", it->second));
            }
            s.removed_locomotion.erase(it);
        }
    }
}

// Diagnostic (essai à deux PC du 2026-10-24, 16:08) : meneur client immobile malgré F8 et ses contextes de déplacement
// rendus ; relancer le jeu a réglé le problème. Une touche de déplacement tenue (clavier) ou une entrée de mouvement
// reçue par le pion, sans vitesse pendant 3 s : on relève tout ce qui peut bloquer (une fois par minute au plus).
void pilot_stuck_tick()
{
    static double next = 0, since = -1, next_log = 0;
    if (s.now < next) return;
    next = s.now + 0.5;
    UObject* pawn = game::explore_pawn();
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (!sharing() || !is_pilot() || !pawn || game::in_battle() || !world_ready() || pid != GetCurrentProcessId()) { since = -1; return; }
    bool key = false;
    for (int vk : std::initializer_list<int>{'W', 'A', 'S', 'D', 'Z', 'Q', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT}) key = key || (GetAsyncKeyState(vk) & 0x8000) != 0;
    double in[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    if (ue::Call c(pawn, STR("GetLastMovementInputVector")); c && c.invoke() && c.param_size(STR("ReturnValue")) == 24)
        std::memcpy(in, c.param_ptr(STR("ReturnValue")), sizeof in);
    if (ue::Call c(pawn, STR("GetVelocity")); c && c.invoke() && c.param_size(STR("ReturnValue")) == 24)
        std::memcpy(vel, c.param_ptr(STR("ReturnValue")), sizeof vel);
    const bool wants = key || std::abs(in[0]) + std::abs(in[1]) > 0.1;
    const bool moving = std::abs(vel[0]) + std::abs(vel[1]) + std::abs(vel[2]) > 10;
    if (!wants || moving) { since = -1; return; }
    if (since < 0) { since = s.now; return; }
    if (s.now - since < 3.0 || s.now < next_log) return;
    next_log = s.now + 60.0;
    bump("pilot_stuck");
    UObject* pc = game::controller();
    game::MoveInfo ps;
    game::move_info(pawn, ps);
    bool ignored = false;
    if (ue::Call c(pc, STR("IsMoveInputIgnored")); c && c.invoke()) ignored = c.out_bool(STR("ReturnValue"));
    JsonObj o;
    o.add("key", key).add("input_x", in[0]).add("input_y", in[1]).add("move", ps.move).add("custom", ps.custom).add("attached", ps.attached)
     .add("move_ignored", ignored)
     .add("dialogue", game::in_dialogue()).add("cinematic", game::playing_cinematic()).add("paused", game::pause_menu_visible())
     .add("removed_contexts", static_cast<int64_t>(s.removed_locomotion.size()))
     .add("w_contexts", game::active_contexts_with_key("W")).add("z_contexts", game::active_contexts_with_key("Z"));
    log().write("coop_pilot_stuck", o);
}

} // namespace e33::explore::detail
