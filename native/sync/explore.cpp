#include "explore.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

using RC::Unreal::UObject;
using RC::Unreal::UFunction;

namespace e33::explore {

namespace {

constexpr const wchar_t* kHostSnapshotSlot = L"e33coop/snapshot";   // hôte : instantané envoyé
constexpr const wchar_t* kCoopSlot = L"e33coop/host";               // client : emplacement coop (jamais ses saves solo)
constexpr double kPilotPeriod = 0.05;                               // PilotState ~20 par seconde
constexpr double kPollPeriod = 0.2;                                 // dialogues, cinématiques, menu pause
constexpr double kTeleportDistance = 800;                           // écart au-delà duquel le suiveur est téléporté
constexpr double kBarrierTimeout = 180;

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

enum class JoinState { None, Pending, Waiting, Sent, Joined };

struct PeerJoin { JoinState state = JoinState::None; double since = 0; bool wait_sent = false; };

struct Inbound {                                  // client : instantané en cours de réception
    uint32_t xfer = 0;
    uint32_t size = 0;
    uint64_t hash = 0;
    std::string level;
    std::vector<uint8_t> data;
    uint32_t received = 0;
};

struct S {
    bool host = false;
    bool active = false;
    Net net;
    uint8_t me = 0;
    double now = 0;
    // Rejoindre
    bool want_join = false;
    std::map<uint8_t, PeerJoin> peers;            // hôte
    uint32_t next_xfer = 1;
    std::optional<Inbound> inbound;               // client
    enum class ClientJoin { None, Receiving, Loading, WaitWorld, Joined } cj = ClientJoin::None;
    double cj_since = 0;
    double world_stable = 0;
    uint32_t joined_xfer = 0;
    // Pilote
    uint8_t pilot = 0;
    uint32_t pilot_seq = 0;
    double next_pilot_send = 0;
    std::optional<proto::PilotState> last_pilot;  // suiveur
    double last_pilot_at = 0;
    double follow_error = -1;
    double level_mismatch_since = -1;
    bool f8_down = false;
    // Interactions, dialogues, transitions (rejoués chez les suiveurs)
    uint32_t seq = 0;
    bool replaying = false;
    std::deque<proto::WorldInteract> interactions;
    std::deque<std::pair<proto::DialogueChoice, double>> choices;
    bool choice_pending = false;                  // question posée (StartChoice), pas encore répondue
    // Narration (barrière) et pause
    double next_poll = 0;
    bool local_narr = false;
    proto::NarrativeKind local_kind = proto::NarrativeKind::Dialogue;
    uint32_t narr_seq = 0;
    std::map<uint8_t, bool> remote_narr;
    bool barrier_paused = false;
    double barrier_since = -1;
    bool local_pause = false;
    std::map<uint8_t, bool> remote_pause;
    bool remote_paused_game = false;
    // Combat d'exploration
    std::string last_enemy;
    int last_engage = 0;
} s;

void bump(const char* c, int n = 1) { if (s.net.bump) s.net.bump(c, n); }

// Les événements d'exploration ne concernent que les joueurs entrés dans la partie (hôte : au moins un client
// entré ; client : instantané chargé, ou session sans « rejoindre » des tests de combat).
bool sharing();

bool world_ready()
{
    return game::controller() && !game::map_change_in_progress() && game::explore_pawn() && !game::in_battle();
}

bool safe_point()
{
    return world_ready() && !game::in_dialogue() && !game::playing_cinematic();
}

// --- Rejoindre : hôte -------------------------------------------------------------------------

void send_snapshot(uint8_t player)
{
    std::vector<uint8_t> bytes;
    std::string err;
    if (!game::make_snapshot(kHostSnapshotSlot, bytes, err)) {
        bump("snapshot_errors");
        log().write("coop_snapshot_error", JsonObj().add("what", err));
        return;
    }
    const uint32_t xfer = s.next_xfer++;
    const uint64_t h = fnv64(bytes);
    const std::string level = game::current_level();
    s.net.send_to(player, proto::SaveOffer{xfer, static_cast<uint32_t>(bytes.size()), h, level});
    for (uint32_t off = 0; off < bytes.size(); off += proto::kMaxChunk) {
        proto::SaveChunk c;
        c.xfer = xfer;
        c.offset = off;
        const size_t n = std::min<size_t>(proto::kMaxChunk, bytes.size() - off);
        c.data.assign(bytes.begin() + off, bytes.begin() + off + n);
        s.net.send_to(player, c);
    }
    // Copie dans le dossier du run (fabrication des données de test : jamais versionnée).
    if (!launch_args().run_dir.empty()) {
        std::ofstream f(std::filesystem::path(launch_args().run_dir) / L"snapshot.sav", std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    s.peers[player].state = JoinState::Sent;
    s.peers[player].since = s.now;
    bump("snapshots_sent");
    log().write("coop_snapshot_sent", JsonObj().add("player", static_cast<int>(player)).add("xfer", xfer).add("size", static_cast<int64_t>(bytes.size()))
                                         .add("hash", hex64(h)).add("level", level));
}

void host_join_tick()
{
    for (auto& [player, pj] : s.peers) {
        if (pj.state != JoinState::Pending && pj.state != JoinState::Waiting) continue;
        if (safe_point()) { send_snapshot(player); continue; }
        if (!pj.wait_sent) {
            const char* why = game::in_battle() ? "battle" : game::map_change_in_progress() ? "loading" : "narrative";
            s.net.send_to(player, proto::JoinWait{why});
            pj.wait_sent = true;
            pj.state = JoinState::Waiting;
            log().write("coop_join_wait", JsonObj().add("player", static_cast<int>(player)).add("reason", why));
        }
    }
}

// --- Rejoindre : client -----------------------------------------------------------------------

void client_join_tick()
{
    if (s.cj == S::ClientJoin::Loading) {
        // Charger depuis le menu principal ou depuis une partie en cours (même chemin que les saves de test).
        if (!game::gi() || game::map_change_in_progress()) return;
        if (!game::main_menu() && !game::controller()) return;
        std::string err;
        if (!game::load_save_slot(kCoopSlot, err)) {
            bump("join_errors");
            log().write("coop_join_error", JsonObj().add("what", "load: " + err));
            s.cj = S::ClientJoin::None;
            return;
        }
        s.cj = S::ClientJoin::WaitWorld;
        s.cj_since = s.now;
        s.world_stable = 0;
        log().write("coop_join_loading", JsonObj().add("slot", ue::to_utf8(kCoopSlot)));
        return;
    }
    if (s.cj == S::ClientJoin::WaitWorld) {
        if (s.now - s.cj_since < 5) return;   // le chargement ne commence pas tout de suite
        s.world_stable = world_ready() ? s.world_stable + kPollPeriod : 0;
        if (s.world_stable < 3.0) return;
        s.cj = S::ClientJoin::Joined;
        const std::string level = game::current_level();
        s.net.publish(proto::JoinReady{s.joined_xfer, level}, true);
        bump("joins_completed");
        log().write("coop_joined", JsonObj().add("xfer", s.joined_xfer).add("level", level).add("wait_s", s.now - s.cj_since));
    }
}

void on_chunk(const proto::SaveChunk& c)
{
    auto& in = s.inbound;
    if (!in || c.xfer != in->xfer || c.offset != in->received || c.offset + c.data.size() > in->size) {
        bump("snapshot_chunk_errors");
        log().write("coop_snapshot_chunk_error", JsonObj().add("xfer", c.xfer).add("offset", c.offset));
        return;
    }
    std::copy(c.data.begin(), c.data.end(), in->data.begin() + c.offset);
    in->received += static_cast<uint32_t>(c.data.size());
    if (in->received < in->size) return;
    const uint64_t h = fnv64(in->data);
    if (h != in->hash) {
        bump("snapshot_hash_errors");
        log().write("coop_snapshot_hash_error", JsonObj().add("want", hex64(in->hash)).add("got", hex64(h)));
        in.reset();
        return;
    }
    const std::filesystem::path dir = std::filesystem::path(game::saves_dir()) / L"e33coop";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto tmp = dir / L"host.tmp", dst = dir / L"host.sav";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(in->data.data()), static_cast<std::streamsize>(in->data.size()));
    }
    std::filesystem::rename(tmp, dst, ec);
    if (ec) { bump("join_errors"); log().write("coop_join_error", JsonObj().add("what", "write: " + ec.message())); in.reset(); return; }
    bump("snapshots_received");
    log().write("coop_snapshot_received", JsonObj().add("xfer", in->xfer).add("size", static_cast<int64_t>(in->size)).add("hash", hex64(h)).add("level", in->level));
    s.joined_xfer = in->xfer;
    s.cj = S::ClientJoin::Loading;
    s.cj_since = s.now;
    in.reset();
}

// --- Pilote et suiveurs -------------------------------------------------------------------------

void pilot_tick()
{
    if (s.now < s.next_pilot_send) return;
    s.next_pilot_send = s.now + kPilotPeriod;
    if (s.host && joined_players() == 0) return;
    if (!s.host && s.cj != S::ClientJoin::Joined && s.want_join) return;
    UObject* pawn = game::explore_pawn();
    if (!pawn || game::map_change_in_progress()) return;
    game::PawnState ps;
    if (!game::pawn_state(pawn, ps)) return;
    proto::PilotState m;
    m.pilot = s.me;
    m.seq = ++s.pilot_seq;
    m.level = game::current_level();
    m.x = ps.pos[0]; m.y = ps.pos[1]; m.z = ps.pos[2]; m.yaw = ps.yaw;
    m.vx = ps.vel[0]; m.vy = ps.vel[1]; m.vz = ps.vel[2];
    s.net.publish(m, false);
}

void follow_tick()
{
    if (!s.last_pilot || s.last_pilot->pilot != s.pilot) return;
    if (!s.host && s.want_join && s.cj != S::ClientJoin::Joined) return;
    if (game::map_change_in_progress() || game::in_battle()) return;
    UObject* pawn = game::explore_pawn();
    if (!pawn) return;
    const auto& p = *s.last_pilot;
    const std::string level = game::current_level();
    if (level != p.level) {
        // Niveau différent sans MapChange reçu (perdu, ou arrivé avant nous) : on rejoint le niveau du pilote.
        if (s.level_mismatch_since < 0) s.level_mismatch_since = s.now;
        if (s.now - s.level_mismatch_since > 8.0) {
            s.level_mismatch_since = -1;
            bump("level_resyncs");
            log().write("coop_level_resync", JsonObj().add("mine", level).add("pilot", p.level));
            game::change_map(ue::from_utf8(p.level), L"");
        }
        return;
    }
    s.level_mismatch_since = -1;
    game::PawnState me;
    if (!game::pawn_state(pawn, me)) return;
    const double age = std::min(0.25, std::max(0.0, s.now - s.last_pilot_at));
    const double tx = p.x + p.vx * age, ty = p.y + p.vy * age, tz = p.z + p.vz * age;
    const double dx = tx - me.pos[0], dy = ty - me.pos[1], dz = tz - me.pos[2];
    const double d = std::sqrt(dx * dx + dy * dy);
    s.follow_error = std::sqrt(d * d + dz * dz);
    if (d > kTeleportDistance || std::fabs(dz) > 400) {
        const double pos[3] = {tx, ty, tz};
        game::teleport_pawn(pawn, pos, p.yaw);
        bump("follower_teleports");
        log().write("coop_follower_teleport", JsonObj().add("distance", d).add("dz", dz));
        return;
    }
    if (d > 60) {
        const double scale = std::clamp(d / 250.0, 0.3, 1.0);
        game::move_pawn(pawn, dx / d, dy / d, scale);
    }
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

// --- Interactions, choix, transitions rejoués -----------------------------------------------------

std::string object_path(UObject* o)
{
    // full_name : "Classe /Game/...:PersistentLevel.Nom" -> chemin après le premier espace
    const std::string f = ue::full_name(o);
    const auto sp = f.find(' ');
    return sp == std::string::npos ? f : f.substr(sp + 1);
}

void replay_tick()
{
    if (!s.host && s.want_join && s.cj != S::ClientJoin::Joined) return;
    if (!s.interactions.empty() && world_ready()) {
        const auto wi = s.interactions.front();
        s.interactions.pop_front();
        UObject* target = ue::find_object(ue::from_utf8(wi.actor).c_str());
        if (!target) {
            bump("interactions_missing");
            log().write("coop_interaction_missing", JsonObj().add("actor", wi.actor));
        } else {
            s.replaying = true;
            const bool ok = game::interact(target, wi.instant);
            s.replaying = false;
            bump(ok ? "interactions_replayed" : "interaction_errors");
            log().write("coop_interaction_replayed", JsonObj().add("actor", wi.actor).add("ok", ok));
        }
    }
    if (!s.choices.empty()) {
        auto& [dc, at] = s.choices.front();
        if (s.choice_pending) {
            s.replaying = true;
            const bool ok = game::choose_dialogue(dc.index);
            s.replaying = false;
            bump(ok ? "choices_replayed" : "choice_errors");
            log().write("coop_choice_replayed", JsonObj().add("index", dc.index).add("ok", ok).add("waited_s", s.now - at));
            s.choices.pop_front();
        } else if (s.now - at > 120) {
            bump("choice_timeouts");
            log().write("coop_choice_timeout", JsonObj().add("index", dc.index));
            s.choices.pop_front();
        }
    }
}

void on_map_change(const proto::MapChange& mc)
{
    std::string dest, tag;
    if (game::map_change_in_progress() && game::map_destination(dest, tag) && dest == mc.level) {
        log().write("coop_map_change_already", JsonObj().add("level", mc.level));
        return;
    }
    if (!game::map_change_in_progress() && game::current_level() == mc.level) {
        log().write("coop_map_change_same_level", JsonObj().add("level", mc.level));
        return;
    }
    s.replaying = true;
    const bool ok = game::change_map(ue::from_utf8(mc.level), ue::from_utf8(mc.spawn_tag));
    s.replaying = false;
    bump(ok ? "map_changes_followed" : "map_change_errors");
    log().write("coop_map_change_followed", JsonObj().add("level", mc.level).add("spawn", mc.spawn_tag).add("ok", ok));
}

// --- Barrière narrative et pause ------------------------------------------------------------------

bool others_in_narrative()
{
    for (auto& [p, a] : s.remote_narr) if (a && p != s.me) return true;
    return false;
}

void poll_tick()
{
    if (s.now < s.next_poll) return;
    s.next_poll = s.now + kPollPeriod;
    if (!game::controller()) return;
    // Narration locale
    const bool cine = game::playing_cinematic();
    const bool dlg = !cine && game::in_dialogue();
    const bool narr = cine || dlg;
    if (narr != s.local_narr) {
        s.local_narr = narr;
        if (narr) s.local_kind = cine ? proto::NarrativeKind::Cinematic : proto::NarrativeKind::Dialogue;
        ++s.narr_seq;
        s.net.publish(proto::NarrativeState{s.me, s.local_kind, narr, s.narr_seq}, true);
        bump(narr ? "narratives_started" : "narratives_finished");
        log().write("coop_narrative", JsonObj().add("active", narr).add("kind", static_cast<int>(s.local_kind)).add("seq", s.narr_seq));
    }
    // Barrière : fini ici, pas chez les autres -> jeu en pause jusqu'à ce que tout le monde ait fini.
    const bool hold = !narr && others_in_narrative() && !game::in_battle();
    if (hold && !s.barrier_paused) {
        s.barrier_paused = game::set_game_paused(true);
        s.barrier_since = s.now;
        bump("barrier_waits");
        log().write("coop_barrier_hold", JsonObj());
    } else if (s.barrier_paused && (!hold || s.now - s.barrier_since > kBarrierTimeout)) {
        if (hold) { bump("barrier_timeouts"); s.remote_narr.clear(); }
        game::set_game_paused(false);
        s.barrier_paused = false;
        log().write("coop_barrier_release", JsonObj().add("waited_s", s.now - s.barrier_since).add("timeout", hold));
    }
    // Menu pause local
    const bool paused = game::pause_menu_visible();
    if (paused != s.local_pause) {
        s.local_pause = paused;
        const bool battle = game::in_battle();
        s.net.publish(proto::PauseState{s.me, paused, battle}, true);
        bump(paused ? "pauses_local" : "unpauses_local");
        log().write("coop_pause_local", JsonObj().add("paused", paused).add("in_battle", battle));
    }
    // Pause d'un autre joueur en combat : tout le monde en pause.
    bool any_remote = false;
    for (auto& [p, v] : s.remote_pause) if (v && p != s.me) any_remote = true;
    const bool want_remote_pause = any_remote && game::in_battle() && !paused;
    if (want_remote_pause != s.remote_paused_game) {
        game::set_game_paused(want_remote_pause);
        s.remote_paused_game = want_remote_pause;
        bump(want_remote_pause ? "remote_pauses_applied" : "remote_pauses_released");
        log().write("coop_pause_remote", JsonObj().add("paused", want_remote_pause));
    }
}

bool sharing() { return s.host ? joined_players() > 0 : (!s.want_join || s.cj == S::ClientJoin::Joined); }

} // namespace

// --- API --------------------------------------------------------------------------------------------

void init(bool host, bool want_join, const Net& net)
{
    s.host = host;
    s.active = true;
    s.want_join = !host && want_join;
    s.net = net;
    s.pilot = 0;
    s.me = host ? 0 : 0xFE;   // client : identifiant reçu dans Welcome (pas pilote avant)
    for (const char* c : {"snapshots_sent", "snapshots_received", "joins_completed", "joined_players", "pilot_changes", "follower_teleports",
                          "interactions_published", "interactions_replayed", "interactions_missing", "choices_published", "choices_replayed",
                          "map_changes_published", "map_changes_followed", "narratives_started", "narratives_finished", "barrier_waits",
                          "barrier_timeouts", "snapshot_errors", "snapshot_hash_errors", "snapshot_chunk_errors", "join_errors",
                          "interaction_errors", "choice_errors", "choice_timeouts", "map_change_errors", "local_actions_on_follower"})
        bump(c, 0);
}

void set_player_id(uint8_t id) { s.me = id; }

void on_peer_accepted(uint8_t player, bool wants_join)
{
    if (!s.host) return;
    if (!wants_join) return;
    s.peers[player] = PeerJoin{JoinState::Pending, s.now, false};
    log().write("coop_join_requested", JsonObj().add("player", static_cast<int>(player)));
}

void on_peer_left(uint8_t player)
{
    s.peers.erase(player);
    s.remote_narr.erase(player);
    s.remote_pause.erase(player);
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
        log().write("coop_snapshot_offer", JsonObj().add("xfer", o->xfer).add("size", o->size).add("hash", hex64(o->hash)).add("level", o->level));
        return true;
    }
    if (auto* c = std::get_if<proto::SaveChunk>(&m)) { if (!s.host) on_chunk(*c); return true; }
    if (auto* w = std::get_if<proto::JoinWait>(&m)) { log().write("coop_join_waiting", JsonObj().add("reason", w->reason)); bump("join_waits"); return true; }
    if (auto* r = std::get_if<proto::JoinReady>(&m)) {
        if (!s.host) return true;
        auto& pj = s.peers[from];
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
        if (!s.last_pilot || p->seq > s.last_pilot->seq || p->pilot != s.last_pilot->pilot) { s.last_pilot = *p; s.last_pilot_at = s.now; }
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
    // Client pas encore entré (instantané en cours) : les événements d'exploration ne le concernent pas encore.
    if (!s.host && !sharing() && (std::holds_alternative<proto::WorldInteract>(m) || std::holds_alternative<proto::DialogueChoice>(m) ||
                                  std::holds_alternative<proto::MapChange>(m) || std::holds_alternative<proto::PilotState>(m)))
        return true;
    if (auto* wi = std::get_if<proto::WorldInteract>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        s.interactions.push_back(*wi);
        return true;
    }
    if (auto* dc = std::get_if<proto::DialogueChoice>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        s.choices.emplace_back(*dc, s.now);
        return true;
    }
    if (auto* mc = std::get_if<proto::MapChange>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        on_map_change(*mc);
        return true;
    }
    if (auto* ns = std::get_if<proto::NarrativeState>(&m)) {
        if (s.host) { if (ns->player != from) return true; s.net.relay(m, from, true); }
        s.remote_narr[ns->player] = ns->active;
        log().write("coop_narrative_remote", JsonObj().add("player", static_cast<int>(ns->player)).add("active", ns->active).add("seq", ns->seq));
        return true;
    }
    if (std::get_if<proto::BarrierRelease>(&m)) return true;
    if (auto* ps = std::get_if<proto::PauseState>(&m)) {
        if (s.host) { if (ps->player != from) return true; s.net.relay(m, from, true); }
        s.remote_pause[ps->player] = ps->paused;
        log().write("coop_pause_remote_state", JsonObj().add("player", static_cast<int>(ps->player)).add("paused", ps->paused).add("in_battle", ps->in_battle));
        return true;
    }
    return false;
}

void tick(double now)
{
    if (!s.active) return;
    s.now = now;
    if (s.host) host_join_tick();
    else client_join_tick();
    key_tick();
    if (is_pilot()) pilot_tick();
    else follow_tick();
    replay_tick();
    poll_tick();
}

void subscribe()
{
    auto& bus = HookBus::get();
    // Interactions du pilote avec les objets du monde (implémentations de BPI_Interaction:OnInteractionRequested).
    static const wchar_t* const kInteractables[] = {
        L"/Game/Gameplay/Exploration/Interaction/BP_InteractionObjectBase.BP_InteractionObjectBase_C:",
        L"/Game/Gameplay/GPE/Chests/BP_Chest_Regular.BP_Chest_Regular_C:",
        L"/Game/LevelTools/BP_GrapplePoint.BP_GrapplePoint_C:",
        L"/Game/LevelTools/Climbing/BP_Holds.BP_Holds_C:",
        L"/Game/LevelTools/Journals/BP_DocumentActor.BP_DocumentActor_C:",
        L"/Game/LevelTools/Journals/BP_ReadingCube.BP_ReadingCube_C:",
        L"/Game/LevelTools/LightSafeAreaMecanic/BP_LightSafeArea.BP_LightSafeArea_C:",
        L"/Game/LevelTools/MovementZone/BP_GameActionTriggerZone.BP_GameActionTriggerZone_C:",
        L"/Game/LevelTools/NPCs/BP_jRPG_NPC_Dialog.BP_jRPG_NPC_Dialog_C:",
        L"/Game/LevelTools/Rope/BP_GPE_Rope.BP_GPE_Rope_C:",
        L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:",
        L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_NPC_Base.BP_jRPG_NPC_Base_C:",
        L"/Game/jRPGTemplate/Blueprints/Other/World/BP_jRPG_InteractionObject_Base.BP_jRPG_InteractionObject_Base_C:",
    };
    for (const wchar_t* cls : kInteractables) {
        bus.pre(std::wstring(cls) + L"OnInteractionRequested", [](HookCtx& ctx, UFunction* f) {
            if (s.replaying || !s.active || !sharing()) return;
            const bool instant = ue::read_param_int(ctx, f, STR("Instant")).value_or(0) != 0;
            const std::string path = object_path(ctx.Context);
            if (!is_pilot()) {
                bump("local_actions_on_follower");
                log().write("coop_follower_interaction", JsonObj().add("actor", path));
                return;
            }
            s.net.publish(proto::WorldInteract{++s.seq, path, instant}, true);
            bump("interactions_published");
            log().write("coop_interaction", JsonObj().add("actor", path).add("instant", instant));
        });
    }
    bus.pre(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:OnChoiceMade", [](HookCtx& ctx, UFunction* f) {
        s.choice_pending = false;
        if (s.replaying || !s.active || !sharing()) return;
        const int index = static_cast<int>(ue::read_param_int(ctx, f, STR("ChoiceIndex")).value_or(-1));
        if (!is_pilot()) {
            bump("local_actions_on_follower");
            log().write("coop_follower_choice", JsonObj().add("index", index));
            return;
        }
        if (index < 0) return;
        s.net.publish(proto::DialogueChoice{++s.seq, index}, true);
        bump("choices_published");
        log().write("coop_choice", JsonObj().add("index", index));
    });
    // Question de dialogue : CurrentChoices reste vide pendant la question (vu le 2026-10-20), on suit StartChoice.
    bus.post(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:StartChoice", [](HookCtx&, UFunction*) {
        s.choice_pending = true;
        log().write("coop_choice_asked", JsonObj());
    });
    bus.post(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:EndDialogue", [](HookCtx&, UFunction*) { s.choice_pending = false; });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:ChangeMap", [](HookCtx&, UFunction*) {
        if (s.replaying || !s.active || !is_pilot() || !sharing()) return;   // pas pendant le chargement de l'instantané
        std::string level, tag;
        if (!game::map_destination(level, tag)) return;
        s.net.publish(proto::MapChange{++s.seq, level, tag}, true);
        bump("map_changes_published");
        log().write("coop_map_change", JsonObj().add("level", level).add("spawn", tag));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle", [](HookCtx& ctx, UFunction* f) {
        s.last_enemy = ue::short_name(ctx.Context);
        s.last_engage = static_cast<int>(ue::read_param_int(ctx, f, STR("EngagementType")).value_or(0));
        log().write("coop_world_battle", JsonObj().add("enemy", s.last_enemy).add("engage", s.last_engage));
    });
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
void request_pilot()
{
    if (is_pilot()) return;
    log().write("coop_pilot_request", JsonObj().add("player", static_cast<int>(s.me)));
    if (s.host) { set_pilot(s.me, "local request"); s.net.publish(proto::PilotChange{s.me}, true); }
    else s.net.publish(proto::PilotRequest{s.me}, true);
}
bool narrative_active() { return s.local_narr; }
bool choice_pending() { return s.choice_pending; }
double follow_error() { return s.follow_error; }
const std::string& last_world_enemy() { return s.last_enemy; }
int last_world_engage() { return s.last_engage; }

} // namespace e33::explore
