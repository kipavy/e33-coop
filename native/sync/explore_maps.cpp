// Changements de niveau (D26) et leurs mesures.
#include "explore_internal.hpp"

namespace e33::explore::detail {

// Horloge murale (s) : un chargement bloque le thread de jeu, les mesures de transition se font en temps réel.
double wnow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double mt_window_start() { return std::max(mt.arrived_at, mt.pilot_at); }

bool mt_in_window() { return mt.open && mt.arrived_at >= 0 && wnow() - mt.arrived_at < 30.0; }

void mt_close(const char* why)
{
    if (!mt.open) return;
    mt.open = false;
    const bool done = mt.arrived_at >= 0;
    auto ms = [](double a, double b) { return a >= 0 && b >= 0 ? static_cast<int64_t>((a - b) * 1000.0) : -1; };
    const int64_t delay = ms(mt.load_at, mt.rx_at), load = ms(mt.arrived_at, mt.load_at), late = ms(mt.arrived_at, mt.pilot_at);
    const int64_t catchup = mt.caught_at >= 0 ? ms(mt.caught_at, mt.follow_at) : (mt.follow_at >= 0 ? ms(wnow(), mt.follow_at) : -1);
    log().write("coop_map_transition", JsonObj().add("level", mt.level).add("spawn", mt.spawn).add("how", mt.how).add("why", why).add("arrived", done)
        .add("loads", mt.loads).add("expect_loads", mt.expect_loads).add("delay_ms", delay).add("load_ms", load)
        .add("pilot_ready_ms", ms(mt.pilot_at, mt.rx_at)).add("after_pilot_ms", late)
        .add("err0", static_cast<int64_t>(mt.err0)).add("catchup_ms", catchup).add("teleports", mt.teleports)
        .add("starts", mt.starts).add("stops", mt.stops).add("paused_ms", static_cast<int64_t>(mt.paused_s * 1000.0))
        .add("ticked_ms", static_cast<int64_t>(mt.ticked_s * 1000.0)).add("mod_slow", mt.mod_slow).add("mod_max_ms", std::round(mt.mod_max_ms * 100.0) / 100.0).add("slow_frames", mt.slow));
    bump("map_transitions");
    // Remplacée : nouveau MapChange avant l'arrivée (le pilote est reparti) ; le suiveur va directement au niveau suivant.
    const bool superseded = !done && std::string_view(why) == "next";
    if (superseded) bump("map_transitions_superseded");
    else if (!done) bump("map_transitions_unfinished");
    if (mt.loads > mt.expect_loads) bump("map_loads_extra", mt.loads - mt.expect_loads);
    if (mt.loads < mt.expect_loads && !superseded) bump("map_loads_missing", mt.expect_loads - mt.loads);
    bump_max("map_follow_delay_ms_max", mmax.delay, static_cast<int>(std::max<int64_t>(0, delay)));
    bump_max("map_follower_load_ms_max", mmax.load, static_cast<int>(std::max<int64_t>(0, load)));
    bump_max("map_after_pilot_ms_max", mmax.late, static_cast<int>(std::max<int64_t>(0, late)));
    bump_max("map_arrival_err_max", mmax.err, static_cast<int>(std::max(0.0, mt.err0)));
    bump_max("map_catchup_ms_max", mmax.catchup, static_cast<int>(std::max<int64_t>(0, catchup)));
    bump_max("map_arrival_paused_ms_max", mmax.paused, static_cast<int>(mt.paused_s * 1000.0));
    bump_max("map_arrival_mod_ms_max", mmax.mod, static_cast<int>(mt.mod_max_ms));
    if (mt.teleports) bump("map_arrival_teleports", mt.teleports);
    if (mt.stops) bump("map_arrival_stops", mt.stops);
    if (mt.mod_slow) bump("map_arrival_mod_slow", mt.mod_slow);
    // Suivi jamais repris alors que le pilote était là et que le jeu a tourné 3 s (un gel du jeu ne compte pas).
    if (done && mt.follow_at < 0 && mt.pilot_at >= 0 && mt.ticked_s >= 3.0) bump("map_arrival_no_follow");
}

// Chargements d'ici : fronts de IsMapChangeInProgress. Un chargement peut finir et le suivant commencer dans la même
// image (MapChange appliqué dès la fin du précédent, avant map_tick) : le mod compte donc aussi ses propres ChangeMap.
void load_begin(const std::string& dest, const std::string& tag)
{
    load_in = true;
    load_rise_at = wnow();
    load_dest = dest;
    bump("map_loads_local");
    log().write("coop_map_load_start", JsonObj().add("level", load_dest).add("spawn", tag).add("pilot", is_pilot()).add("from", game::current_level()));
    if (mt.open) { ++mt.loads; if (mt.load_at < 0) mt.load_at = wnow(); }   // après l'arrivée : chargement de trop
}

void load_end()
{
    load_in = false;
    load_fall_at = wnow();
    log().write("coop_map_load_end", JsonObj().add("level", load_dest).add("load_s", wnow() - load_rise_at).add("pilot", is_pilot()));
}

void load_edge_tick()
{
    const bool in = game::map_change_in_progress();
    if (in && !load_in) {
        std::string dest, tag;
        if (!game::map_destination(dest, tag)) dest.clear();
        load_begin(dest, tag);
    } else if (!in && load_in) load_end();
}

// Chaque image (pilote et suiveur) : début et fin des chargements d'ici ; suiveur : arrivée et fenêtre d'observation.
void map_tick()
{
    const double dt = prev_now < 0 ? 0.0 : s.now - prev_now;
    prev_now = s.now;
    load_edge_tick();
    const bool in = load_in;
    // Suiveur : boîtes de passage du niveau coupées. Son pion, conduit derrière le pilote, ne change de niveau que sur
    // MapChange (sinon : passage par une autre sortie que le pilote, ou second déclenchement de la même pendant le
    // chargement). Rendues quand il redevient pilote. Une fois par niveau (FindAllOf parcourt tous les objets).
    {
        const bool want_off = zone_fix() && sharing() && !is_pilot();
        if (!in && world_ready()) {
            const std::string lvl = game::current_level();
            // Reprise toutes les 10 s (seule tâche lourde de son image) : parties de niveau chargées ensuite.
            const bool again = want_off && s.now - tp_at > 10.0 && !local_moving() && prof::claim_heavy("teleport_points");
            if ((lvl != tp_level && want_off) || want_off != tp_off || again) {
                tp_at = s.now;
                const int n = game::set_teleport_points_active(!want_off);
                if (!again || n != tp_count) log().write("coop_teleport_points", JsonObj().add("level", lvl).add("active", !want_off).add("count", n));
                tp_level = lvl;
                tp_count = n;
                tp_off = want_off;
            }
        }
    }
    if (!mt.open) return;
    if (mt.arrived_at < 0) {
        if (!in && (mt.loads > 0 || mt.expect_loads == 0) && world_ready() && game::current_level() == mt.level) {
            mt.arrived_at = wnow();
            log().write("coop_map_arrived", JsonObj().add("level", mt.level).add("after_rx_s", wnow() - mt.rx_at)
                                                .add("pilot_here", mt.pilot_at >= 0).add("loads", mt.loads));
        } else if (wnow() - mt.rx_at > 180.0) mt_close("never_arrived");
        return;
    }
    // Fenêtre d'observation : pause, images lentes (mod > 20 ms, image > 80 ms hors chargement).
    if (!s.pause_reasons.empty() || s.pause_applied) mt.paused_s += dt;
    const double mod_ms = prof::last().total_ms;
    mt.mod_max_ms = std::max(mt.mod_max_ms, mod_ms);
    if (mod_ms > 20.0) ++mt.mod_slow;
    if (dt > 0.08 && dt < 3.0 && !in) ++mt.slow;
    if (dt < 0.5) mt.ticked_s += dt;
    if (mt.pilot_at >= 0 && wnow() - mt_window_start() > kMapWindow && mt.ticked_s >= 3.0) mt_close("done");
    else if (wnow() - mt.arrived_at > 60.0) mt_close("pilot_absent");
}

// Suiveur : chaque image de suivi après l'arrivée (écart au pilote, téléportation, arrêt / reprise).
void mt_follow(double err, bool teleported, int started, int stopped)
{
    if (!mt_in_window() || mt.pilot_at < 0) return;
    if (mt.follow_at < 0) {
        mt.follow_at = wnow();
        mt.err0 = err;
        log().write("coop_map_follow_resumed", JsonObj().add("level", mt.level).add("err", static_cast<int64_t>(err))
                                                   .add("after_arrival_s", wnow() - mt.arrived_at).add("after_pilot_s", wnow() - mt.pilot_at));
    }
    if (mt.caught_at < 0 && err < 250) mt.caught_at = wnow();
    mt.teleports += teleported ? 1 : 0;
    mt.starts += started;
    mt.stops += stopped;
}

// Niveau du menu principal : jamais suivi ni publié (retour au menu = fin de session, gérée à part). Vu le 2026-10-22 :
// le client publiait MapChange « Level_MainMenu » en quittant ; venant de l'hôte, le client aurait chargé le menu
// comme une carte.
bool is_menu_level(const std::string& level) { return level.empty() || level == "None" || level.find("MainMenu") != std::string::npos; }

void apply_map_change(const proto::MapChange& mc)
{
    s.pending_map.reset();
    if (game::current_level() == mc.level) {
        mt.how = mt.how == "deferred" ? "deferred_same_level" : "same_level";
        mt.expect_loads = 0;
        log().write("coop_map_change_same_level", JsonObj().add("level", mc.level));
        return;
    }
    if (mt.how.empty()) mt.how = "followed";
    s.last_pilot.reset();
    s.level_mismatch_since = -1;
    s.replaying = true;
    const bool ok = game::change_map(ue::from_utf8(mc.level), ue::from_utf8(mc.spawn_tag));
    s.replaying = false;
    if (ok) { if (load_in) load_end(); load_begin(mc.level, mc.spawn_tag); }   // compté ici (voir load_begin)
    bump(ok ? "map_changes_followed" : "map_change_errors");
    log().write("coop_map_change_followed", JsonObj().add("level", mc.level).add("spawn", mc.spawn_tag).add("ok", ok).add("how", mt.how)
                                                .add("after_rx_s", wnow() - mt.rx_at));
}

// Suiveur : MapChange appliqué tout de suite si possible ; sinon gardé (le dernier seulement) et appliqué dès que le
// monde est prêt ici (pending_map_tick) : chargement d'un autre niveau en cours (pilote reparti avant notre arrivée),
// combat pas encore fini ici, instantané en cours de chargement. Avant : second ChangeMap en plein chargement, ou
// MapChange perdu pendant la jonction (retour par level_resync 8 s plus tard, sans point d'apparition).
void on_map_change(const proto::MapChange& mc)
{
    touch();
    load_edge_tick();   // état du chargement à jour (le message est lu avant map_tick)
    if (is_menu_level(mc.level)) {
        log().write("coop_map_change_ignored", JsonObj().add("level", mc.level).add("why", "menu"));
        return;
    }
    // Choix de dialogue du pilote encore en attente ici (dialogue jamais ouvert chez nous) : il ne viendra plus après
    // un changement de niveau. Gardé, il tenait l'exploration « jamais au calme » 120 s (pas d'empreinte, pas de copie
    // coop ; essai du 2026-10-25, feu de camp).
    if (!s.choices.empty()) {
        bump("choices_dropped_map_change", static_cast<int>(s.choices.size()));
        log().write("coop_choices_dropped", JsonObj().add("count", static_cast<int64_t>(s.choices.size())).add("why", "map_change"));
        s.choices.clear();
    }
    s.map_spawns[mc.level] = mc.spawn_tag;
    s.expect_level = mc.level;
    s.expect_level_at = s.now;
    mt_close("next");
    mt = MapTrans{};
    mt.open = true;
    mt.level = mc.level;
    mt.spawn = mc.spawn_tag;
    mt.rx_at = wnow();
    std::string dest, tag;
    const bool loading = game::map_change_in_progress();
    if (loading && game::map_destination(dest, tag) && dest == mc.level) {
        s.pending_map.reset();
        mt.how = "already";
        mt.loads = 1;
        mt.load_at = load_rise_at;
        log().write("coop_map_change_already", JsonObj().add("level", mc.level));
        return;
    }
    // Dernière position du pilote = ancienne zone : oubliée, sinon le suiveur arrivé avant lui y retournait au bout de 8 s.
    s.last_pilot.reset();
    s.level_mismatch_since = -1;
    if (!zone_fix() && !sharing()) return;   // ancien comportement (mesure « avant ») : MapChange ignoré pendant la jonction
    if (zone_fix() && (loading || game::in_battle() || !sharing() || !world_ready())) {
        s.pending_map = mc;
        mt.how = "deferred";
        bump("map_changes_deferred");
        log().write("coop_map_change_deferred", JsonObj().add("level", mc.level).add("loading", loading).add("loading_to", dest)
                                                    .add("battle", game::in_battle()).add("sharing", sharing()));
        return;
    }
    apply_map_change(mc);
}

void pending_map_tick()
{
    if (!s.pending_map || is_pilot()) { s.pending_map.reset(); return; }
    if (!sharing() || !world_ready()) return;
    const proto::MapChange mc = *s.pending_map;
    apply_map_change(mc);
}

} // namespace e33::explore::detail
