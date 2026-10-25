// Exploration en coop : état, API du module, messages, tick (D12, D14).
#include "explore_internal.hpp"

namespace e33::explore::detail {

S s;
std::map<UObject*, std::string> g_char_of;
FollowStats fstats;
FollowCtl fc;
MapTrans mt;
bool load_in = false;                 // chargement de niveau en cours ici (IsMapChangeInProgress)
double load_fall_at = -1;              // fin du dernier chargement (IsMapChangeInProgress retombé)
double load_rise_at = -1;
std::string load_dest;
double prev_now = -1;
std::string tp_level;                 // niveau où l'état des boîtes de passage a été appliqué
bool tp_off = false;                  // boîtes de passage coupées (suiveur)
double tp_at = -1;
int tp_count = -1;
MapMax mmax;
SceneSync sc;
DWORD g_explore_thread = 0;   // fil du jeu (relevé au tick d'exploration)

} // namespace e33::explore::detail

namespace e33::explore::detail {

uint64_t fnv64(const std::vector<uint8_t>& b)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t x : b) { h ^= x; h *= 1099511628211ull; }
    return h;
}

std::string hex64(uint64_t h)
{
    char s[24];
    std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(h));
    return s;
}

void follow_stats_flush(bool force)
{
    auto& f = fstats;
    if (f.since < 0) return;
    if (!force && s.now - f.since < 15.0) return;
    if (f.rx > 0 || f.ticks > 0) {
        auto r1 = [](double v) { return std::round(v * 10.0) / 10.0; };
        log().write("coop_follow_stats", JsonObj().add("s", r1(s.now - f.since))
            .add("rx", f.rx).add("rx_gap_avg_ms", f.rx > 1 ? r1(f.rx_gap_sum / (f.rx - 1) * 1000.0) : 0.0)
            .add("rx_gap_max_ms", static_cast<int64_t>(f.rx_gap_max * 1000.0)).add("rx_same_frame", f.rx_same_frame)
            .add("rx_gap_100", f.rx_gap_100).add("rx_gap_250", f.rx_gap_250).add("rx_gap_500", f.rx_gap_500)
            .add("seq_lost", f.seq_lost).add("rx_old", f.rx_old).add("age_max_ms", static_cast<int64_t>(f.age_max * 1000.0))
            .add("ticks", f.ticks).add("starts", f.starts).add("stops", f.stops).add("teleports", f.teleports)
            .add("err_avg", f.ticks ? r1(f.err_sum / f.ticks) : 0.0).add("err_max", r1(f.err_max))
            .add("pilot_speed_max", r1(f.pilot_speed_max))
            .add("own_speed_avg", f.own_speed_n ? r1(f.own_speed_sum / f.own_speed_n) : 0.0).add("own_speed_max", r1(f.own_speed_max))
            .add("sprints", f.sprints).add("sprint_s", r1(f.sprint_s)).add("sprint_gait_ok_s", r1(f.sprint_gait_ok_s))
            .add("sprint_method", s.sprint_method).add("starved_s", r1(f.starved_s)));
    }
    const double last_rx = f.last_rx;
    const uint32_t last_seq = f.last_seq;
    f = FollowStats{};
    f.since = s.now;
    f.last_rx = last_rx;
    f.last_seq = last_seq;
}

void follow_stats_rx(const proto::PilotState& p)
{
    auto& f = fstats;
    if (f.since < 0) f.since = s.now;
    ++f.rx;
    if (f.last_rx >= 0) {
        const double gap = s.now - f.last_rx;   // horloge de l'image : deux messages lus dans la même image -> écart nul
        f.rx_gap_sum += gap;
        f.rx_gap_max = std::max(f.rx_gap_max, gap);
        if (gap < 0.001) ++f.rx_same_frame;
        if (gap > 0.1) ++f.rx_gap_100;
        if (gap > 0.25) ++f.rx_gap_250;
        if (gap > 0.5) ++f.rx_gap_500;
    }
    if (f.last_seq && p.seq > f.last_seq + 1 && p.seq - f.last_seq < 1000) f.seq_lost += static_cast<int>(p.seq - f.last_seq - 1);
    if (f.last_seq && p.seq <= f.last_seq) ++f.rx_old;
    f.last_rx = s.now;
    if (p.seq > f.last_seq || p.seq + 1000 < f.last_seq) f.last_seq = p.seq;
}

void bump(const char* c, int n) { if (s.net.bump) s.net.bump(c, n); }

// Corrections des changements de niveau (2026-10-23) : -e33zonefix=0 les coupe (mesure « avant » des scénarios p9).
bool zone_fix() { static const bool v = cmdline::arg(L"e33zonefix") != L"0"; return v; }

bool world_ready()
{
    return game::controller() && !game::map_change_in_progress() && game::explore_pawn() && !game::in_battle();
}

// Client : moment où demander une resynchro (ni dialogue, ni cinématique, ni combat).
bool resync_calm()
{
    // Ni en pause : un chargement demandé menu pause ouvert reste bloqué (essai à deux PC du 2026-10-24, 00:12).
    return !game::in_dialogue() && !game::playing_cinematic() && !game::in_battle() && !game::pause_menu_visible();
}

bool safe_point()
{
    return world_ready() && !game::in_dialogue() && !game::playing_cinematic() && !s.intro;
}

} // namespace e33::explore::detail

namespace e33::explore {

// --- API --------------------------------------------------------------------------------------------
void init(bool host, bool want_join, const Net& net)
{
    {
        if (const std::wstring v = cmdline::arg(L"e33perturbworld"); !v.empty()) s.perturb_gold = _wtoi(v.c_str());
        if (const std::wstring v = cmdline::arg(L"e33joinstuck"); !v.empty()) s.join_stuck_test = _wtoi(v.c_str());
    }
    s.host = host;
    s.active = true;
    s.want_join = !host && want_join;
    s.net = net;
    s.pilot = 0;
    s.me = host ? 0 : 0xFE;   // client : identifiant reçu dans Welcome (pas pilote avant)
    for (const char* c : {"snapshots_sent", "snapshots_received", "joins_completed", "joined_players", "pilot_changes", "follower_teleports",
                          "interactions_published", "interactions_replayed", "interactions_missing", "choices_published", "choices_replayed",
                          "map_changes_published", "map_changes_followed", "narratives_started", "narratives_finished", "barrier_waits",
                          "barrier_timeouts", "barrier_releases", "snapshot_errors", "snapshot_hash_errors", "snapshot_chunk_errors", "join_errors",
                          "interaction_errors", "choice_errors", "choice_timeouts", "map_change_errors", "local_actions_on_follower",
                          "follower_teleports_nogap", "follow_stalls", "follow_dips", "follow_speed_drops", "follow_parasitic_stops",
                          "resyncs_while_moving", "resyncs_deferred_moving", "explore_hitches_moving", "explore_hitches_mod",
                          "join_load_retries", "join_load_stuck", "snapshot_timeouts",
                          "flags_published", "flags_mirrored", "flag_mirror_errors", "world_repairs_requested", "world_repairs_in_place",
                          "world_repairs_failed"})
        bump(c, 0);
}

void reset()
{
    // Gardés : suivis par les hooks hors session (cinématique, question en cours).
    mount::reset();
    S fresh;
    fresh.sequence_playing = s.sequence_playing;
    fresh.choice_pending = s.choice_pending;
    s = std::move(fresh);
    fstats = FollowStats{};
    fc = FollowCtl{};
    sc = SceneSync{};
    mt = MapTrans{};
    if (tp_off && game::controller()) game::set_teleport_points_active(true);
    tp_off = false;
    tp_level.clear();
    mmax = MapMax{};
    load_in = false;
    prev_now = -1;
}

void set_player_id(uint8_t id) { s.me = id; }

void on_peer_accepted(uint8_t player, bool wants_join)
{
    if (!s.host) return;
    if (!wants_join) { s.peers[player] = PeerJoin{JoinState::InGame, s.now, false}; return; }
    s.peers[player] = PeerJoin{JoinState::Pending, s.now, false};
    log().write("coop_join_requested", JsonObj().add("player", static_cast<int>(player)));
}

bool peer_in_game(uint8_t player)
{
    auto it = s.peers.find(player);
    return it != s.peers.end() && (it->second.state == JoinState::Joined || it->second.state == JoinState::InGame);
}

void on_session_end()
{
    s.ended = true;
    s.pause_reasons.clear();
    apply_pause();
    s.barrier_paused = false;
    s.remote_paused_game = false;
    s.interactions.clear();
    s.choices.clear();
    s.last_pilot.reset();
    sprint_release();
    follow_stats_flush(true);
    s.pilot = s.me;            // plus de pilote distant : le déplacement local est rendu (movement_lock_tick)
    mount::release("session_end");
    movement_lock_tick();
    if (tp_off && game::controller()) game::set_teleport_points_active(true);   // boîtes de passage rendues
    tp_off = false;
    tp_level.clear();
    ui::clear_status("pause");
    ui::clear_status("wait");
    ui::clear_status("waited");
}

void hold_pause(const char* reason, bool on)
{
    if (on) s.pause_reasons.insert(reason); else s.pause_reasons.erase(reason);
    apply_pause();
}

bool pause_held(const char* reason) { return s.pause_reasons.count(reason) > 0; }

void on_peer_left(uint8_t player)
{
    s.peers.erase(player);
    s.remote_narr.erase(player);
    s.remote_narr_since.erase(player);
    s.remote_pause.erase(player);
    presence::forget(player);
    if (s.host && s.pilot == player) { set_pilot(0, "pilot left"); s.net.publish(proto::PilotChange{0}, true); }
}

bool handle(uint8_t from, const proto::Message& m)
{
    if (!s.active) return false;
    if (auto* o = std::get_if<proto::SaveOffer>(&m)) {
        if (s.host) return true;
        Inbound in;
        in.xfer = o->xfer; in.size = o->size; in.hash = o->hash; in.level = o->level;
        in.data.assign(o->size, 0);
        s.inbound = std::move(in);
        s.cj = S::ClientJoin::Receiving;
        s.last_chunk_at = s.now;
        ui::set_status("session", "loading_host_game");
        log().write("coop_snapshot_offer", JsonObj().add("xfer", o->xfer).add("size", o->size).add("hash", hex64(o->hash)).add("level", o->level));
        return true;
    }
    if (auto* c = std::get_if<proto::SaveChunk>(&m)) { if (!s.host) on_chunk(*c); return true; }
    if (auto* w = std::get_if<proto::JoinWait>(&m)) {
        log().write("coop_join_waiting", JsonObj().add("reason", w->reason));
        bump("join_waits");
        // Écran d'attente : l'hôte est en combat (ou en cinématique, en chargement) ; entrée dès son prochain point sûr.
        const std::string host = s.net.name_of ? s.net.name_of(0) : std::string();
        if (w->reason == "battle") ui::set_status("session", "waiting_battle_end");
        else if (w->reason == "narrative") ui::set_status("session", "join_wait_narrative", {host});
        else if (w->reason == "loading") ui::set_status("session", "join_wait_loading", {host});
        else ui::set_status("session", "joining");
        if (w->reason == "new_game" && !s.host) follow_new_game();
        return true;
    }
    if (auto* wh = std::get_if<proto::WorldHash>(&m)) {
        if (!s.host) { s.pending_world = *wh; s.pending_world_at = s.now; }
        return true;
    }
    if (auto* rr = std::get_if<proto::ResyncRequest>(&m)) {
        if (!s.host) return true;
        // Client entré : il reste dans la partie jusqu'à l'envoi de l'instantané (avant : retiré aussitôt, puis un combat
        // lancé par le dialogue en cours était joué seul de chaque côté, essai à deux PC du 2026-10-23). Sinon : il ne
        // participe plus jusqu'à son JoinReady.
        if (auto it = s.peers.find(from); it != s.peers.end() && it->second.state == JoinState::Joined) it->second.resync = true;
        else s.peers[from] = PeerJoin{JoinState::Pending, s.now, false};
        bump("resyncs_requested");
        log().write("coop_resync_requested", JsonObj().add("player", static_cast<int>(from)).add("reason", rr->reason));
        // Diagnostic : feuilles de l'hôte des parties en écart (le client journalise les siennes avant sa demande).
        if (const std::string tag = "world state differs: "; rr->reason.rfind(tag, 0) == 0) {
            std::map<std::string, reflect::Leaves> leaves;
            world_parts(&leaves);
            for (const std::string& part : split_csv(rr->reason.substr(tag.size())))
                if (auto it = leaves.find(part); it != leaves.end()) log_world_leaves(part, it->second, s.world_seq);
        }
        return true;
    }
    // Réparation sur place (D38) : feuilles demandées par un client (hôte), reçues de l'hôte (client).
    if (auto* lr = std::get_if<proto::WorldLeavesRequest>(&m)) { if (s.host) send_world_leaves(from, *lr); return true; }
    if (auto* wl = std::get_if<proto::WorldLeaves>(&m)) { if (!s.host) on_world_leaves(*wl); return true; }
    if (auto* r = std::get_if<proto::JoinReady>(&m)) {
        if (!s.host) return true;
        auto& pj = s.peers[from];
        if (game::in_battle()) {
            // Instantané pris avant ce combat, que le client n'a pas joué : il en recevra un nouveau à la fin (sinon il
            // manquerait les effets du combat : ennemi vaincu, butin, expérience).
            pj = PeerJoin{JoinState::Pending, s.now, false};
            bump("snapshots_outdated");
            log().write("coop_join_outdated", JsonObj().add("player", static_cast<int>(from)).add("why", "battle started meanwhile"));
            return true;
        }
        if (pj.state != JoinState::Joined) { pj.state = JoinState::Joined; bump("joined_players"); }
        log().write("coop_peer_joined", JsonObj().add("player", static_cast<int>(from)).add("xfer", r->xfer).add("level", r->level));
        // Le nouveau venu apprend qui pilote.
        s.net.send_to(from, proto::PilotChange{s.pilot});
        return true;
    }
    if (auto* p = std::get_if<proto::PilotState>(&m)) {
        if (s.host) {
            if (p->pilot != s.pilot || p->pilot != from) return true;   // seul le pilote publie sa position
            s.net.relay(m, from, false);
        }
        if (p->pilot == s.me) return true;
        follow_stats_rx(*p);
        // Position de l'ancien niveau arrivée après le MapChange (non fiable, peut doubler le message fiable) : ignorée
        // jusqu'à la première position du pilote dans le nouveau niveau (30 s au plus).
        if (zone_fix() && !s.expect_level.empty()) {
            if (p->level == s.expect_level || s.now - s.expect_level_at > 30.0) s.expect_level.clear();
            else { bump("pilot_states_old_level"); return true; }
        }
        if (!s.last_pilot || p->seq > s.last_pilot->seq || p->pilot != s.last_pilot->pilot) { s.last_pilot = *p; s.last_pilot_at = s.now; }
        follow_push(*p);
        if (mt.open && mt.pilot_at < 0 && p->level == mt.level && p->pilot == s.pilot) mt.pilot_at = wnow();   // pilote arrivé
        return true;
    }
    if (auto* pr = std::get_if<proto::PilotRequest>(&m)) {
        if (!s.host) return true;
        if (pr->player != from) return true;
        set_pilot(from, "request");
        s.net.publish(proto::PilotChange{from}, true);
        return true;
    }
    if (auto* pc = std::get_if<proto::PilotChange>(&m)) { if (!s.host) set_pilot(pc->pilot, "host"); return true; }
    // Client pas encore entré (instantané en cours) : les événements d'exploration ne le concernent pas encore. Sauf le
    // dernier MapChange, gardé pour après l'arrivée (pilote parti dans un autre niveau pendant la jonction).
    if (!s.host && !sharing() && (std::holds_alternative<proto::WorldInteract>(m) || std::holds_alternative<proto::DialogueChoice>(m) ||
                                  std::holds_alternative<proto::PilotState>(m) || std::holds_alternative<proto::FlagSet>(m)))
        return true;
    // Drapeau persistant du pilote (D38) : relayé par l'hôte depuis le pilote seulement, comme WorldInteract.
    if (auto* fl = std::get_if<proto::FlagSet>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        on_flag_set(*fl);
        return true;
    }
    if (auto* wi = std::get_if<proto::WorldInteract>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        s.interactions.push_back(*wi);
        touch();
        return true;
    }
    if (auto* dc = std::get_if<proto::DialogueChoice>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        if (dc->hover == 1) { s.choice_hover_want = dc->index; return true; }   // survol seulement : surbrillance, pas de choix
        if (dc->hover == 2) { s.merchant_want = dc->index; if (s.merchant_seen_at < 0) s.merchant_seen_at = s.now; return true; }   // article du marchand sélectionné par le pilote
        if (dc->hover == 3) { s.merchant_close = true; s.merchant_want = -1; s.merchant_buy = -1; s.merchant_seen_at = -1; s.merchant_open_tried = false; return true; }   // le pilote a fermé la fenêtre
        if (dc->hover == 4) { bump("merchant_buys_seen"); s.merchant_buy = dc->index; return true; }   // achat du pilote, rejoué ici
        if (dc->hover) return true;
        s.choices.emplace_back(*dc, s.now);
        touch();
        return true;
    }
    if (auto* mc = std::get_if<proto::MapChange>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        on_map_change(*mc);
        return true;
    }
    if (auto* ns = std::get_if<proto::NarrativeState>(&m)) {
        if (s.host) { if (ns->player != from) return true; s.net.relay(m, from, true); }
        if (ns->active && !s.remote_narr[ns->player]) s.remote_narr_since[ns->player] = s.now;
        if (!ns->active && s.remote_narr[ns->player]) { sc.remote_end_at[ns->player] = s.now; s.remote_narr_end[ns->player] = s.now; }
        s.remote_narr[ns->player] = ns->active;
        presence::set_narrative(ns->player, ns->active, ns->kind == proto::NarrativeKind::Cinematic, s.now);
        log().write("coop_narrative_remote", JsonObj().add("player", static_cast<int>(ns->player)).add("active", ns->active).add("seq", ns->seq));
        return true;
    }
    if (std::get_if<proto::BarrierRelease>(&m)) return true;
    if (auto* ss = std::get_if<proto::SceneStart>(&m)) {
        if (s.host) { if (ss->player != from || from != s.pilot) return true; s.net.relay(m, from, true); }
        log().write("coop_scene_remote", JsonObj().add("player", static_cast<int>(ss->player)).add("scene", ss->scene).add("sequence", ss->sequence)
                                             .add("cause", static_cast<int>(ss->cause)).add("cause_path", ss->cause_path).add("action", ss->action));
        if (ss->player == s.pilot && ss->player != s.me && sharing()) { sc.pending = *ss; sc.pending_at = s.now; sc.scene_rx_at = s.now; bump("scenes_received"); }
        touch();
        return true;
    }
    if (auto* sk = std::get_if<proto::SceneSkip>(&m)) {
        if (s.host) { if (sk->player != from || from != s.pilot) return true; s.net.relay(m, from, true); }
        log().write("coop_scene_skip_remote", JsonObj().add("player", static_cast<int>(sk->player)).add("scene", sk->scene).add("local_scene", sc.local_scene));
        if (sk->player == s.pilot && sk->player != s.me && sharing()) {
            sc.skip_at = s.now; sc.next_skip_try = s.now; sc.skip_tries = 0; sc.skip_seen_playing = false;
            bump("scene_skips_received");
        }
        return true;
    }
    if (auto* ps = std::get_if<proto::PauseState>(&m)) {
        if (s.host) { if (ps->player != from) return true; s.net.relay(m, from, true); }
        s.remote_pause[ps->player] = ps->paused;
        presence::set_menu(ps->player, ps->paused, s.now);
        log().write("coop_pause_remote_state", JsonObj().add("player", static_cast<int>(ps->player)).add("paused", ps->paused).add("in_battle", ps->in_battle));
        return true;
    }
    return false;
}

void tick(double now)
{
    if (!s.active) return;
    if (s.ended) return;
    s.now = now;
    { prof::Scope p("map"); map_tick(); }
    if (!s.host || !is_pilot()) pending_map_tick();
    if (s.host) host_join_tick();
    else client_join_tick();
    key_tick();
    if (is_pilot()) { sprint_release(); pilot_tick(); }
    else if (!scene_test_no_follow()) { prof::Scope p("follow"); follow_tick(); }
    mount::tick(s.now);   // pion fantôme rendu si le suivi s'est arrêté
    follow_stats_flush(false);
    {   // Mouvement en cours (pilote vu d'ici, ou pion local) : pas de rechargement d'instantané (world_resync_tick).
        // Pilote sans nouvelles (perte, pointe) : considéré encore en mouvement 5 s.
        const bool pilot_moving = !fc.buf.empty() && s.now - fc.last_arrival < 5.0 && std::hypot(fc.buf.back().vel[0], fc.buf.back().vel[1]) > 50;
        const bool local_moving_now = s.local_speed_at >= 0 && s.now - s.local_speed_at < 0.5 && s.local_speed > 50;
        if (pilot_moving || local_moving_now) s.last_motion_at = s.now;
    }
    replay_tick();
    scene_tick();
    { prof::Scope p("explore_poll"); poll_tick(); }
    world_hash_tick();
    world_resync_tick();
    flags_tick();
    if (!s.host) world_repair_tick();
    if (!s.host) perturb_world_tick();
}

bool join_requested() { return s.want_join; }

bool joined() { return !s.want_join || s.cj == S::ClientJoin::Joined; }

int joined_players()
{
    int n = 0;
    for (auto& [p, pj] : s.peers) if (pj.state == JoinState::Joined) ++n;
    return n;
}

uint8_t pilot() { return s.pilot; }

bool is_pilot() { return s.pilot == s.me; }

bool shared() { return sharing(); }

void request_pilot()
{
    if (is_pilot()) return;
    log().write("coop_pilot_request", JsonObj().add("player", static_cast<int>(s.me)));
    if (s.host) { set_pilot(s.me, "local request"); s.net.publish(proto::PilotChange{s.me}, true); }
    else s.net.publish(proto::PilotRequest{s.me}, true);
}

bool narrative_active() { return s.local_narr; }

bool choice_pending() { return s.choice_pending; }

bool sequence_playing() { return s.sequence_playing; }

double last_event_age() { return s.now - s.last_event_at; }

bool remote_paused() { return s.remote_paused_game; }

double follow_error() { return s.follow_error; }

bool motion_recent() { return s.active && !s.ended && s.now - s.last_motion_at < 0.5; }

bool local_moving() { return s.active && !s.ended && s.local_speed_at >= 0 && s.now - s.local_speed_at < 1.0 && s.local_speed > 150; }

bool calm(double secs) { return s.active && !s.ended && quiet(secs); }

std::string take_world_enemy()
{
    // Seulement un ennemi du monde qui vient de lancer ce combat (pas un combat scripté : boss, séquence).
    std::string e = s.now - s.last_enemy_at < 20.0 ? s.last_enemy : std::string();
    s.last_enemy.clear();
    s.last_enemy_at = -1;
    return e;
}

int last_world_engage() { return s.last_engage; }

} // namespace e33::explore
