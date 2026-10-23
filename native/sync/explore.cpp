#include "explore.hpp"

#include "builds.hpp"
#include "coopsave.hpp"
#include "mount.hpp"
#include "presence.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/prof.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/reflect.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

using RC::Unreal::UObject;
using RC::Unreal::UFunction;

namespace e33::explore {

namespace {

constexpr const wchar_t* kHostSnapshotSlot = L"e33coop/snapshot";   // hôte : instantané envoyé
constexpr const wchar_t* kCoopSlot = L"e33coop/host";               // client : emplacement coop (jamais ses saves solo)
constexpr double kPilotPeriod = 0.05;                               // PilotState ~20 par seconde
constexpr double kPollPeriod = 0.2;                                 // dialogues, cinématiques, menu pause
constexpr double kTeleportDistance = 800;                           // écart au-delà duquel le suiveur est téléporté
constexpr double kBarrierTimeout = 600;   // les cinématiques durent jusqu'à ~5 min (intro de Renoir : 295 s)
constexpr double kIntroMax = 900;         // nouvelle partie : intro comptée comme cinématique au plus 15 min (filet si sa fin n'est pas vue)
// Cinématique ou dialogue commencé chez un autre joueur, pas ici : délai avant la pause de la barrière, pour que le
// suiveur atteigne le déclencheur et commence la même scène (sinon il reste figé en dehors, ne la déclenche qu'après
// la fin chez l'autre, et c'est l'autre qui attend ensuite : scènes « en décalé », essai à deux PC du 2026-10-22).
constexpr double kBarrierGrace = 6.0;

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

// InGame : client accepté sans « rejoindre » (tests de combat) : il participe d'emblée, sans instantané.
enum class JoinState { None, Pending, Waiting, Sent, Joined, InGame };

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
    // Suivi lissé (2026-10-21 : arrêts-redémarrages visibles quand l'hôte court, positions Steam irrégulières)
    bool follow_moving = false;
    bool follow_have_target = false;
    double follow_target[3]{};
    double follow_last_t = -1;
    // Sprint du suiveur (2026-10-22 : le pilote sprinte à ~720 u/s, le suiveur restait en course à ~350 u/s et était
    // téléporté toutes les ~2,2 s à 800 u d'écart : les « freeze » vus quand l'hôte court).
    bool follow_sprint = false;                   // sprint demandé au pion suiveur
    bool sprint_touched = false;                  // le mod a déjà changé l'allure de ce pion
    int sprint_method = 0;                        // 1 SprintAction, 2 SetDesiredGait (repli)
    int gait_before = -1;                         // allure voulue avant le sprint (rendue ensuite)
    double sprint_since = -1;
    double next_gait_check = 0;
    bool sprint_logged = false;
    bool sprint_needs_gait = false;               // SprintAction seul n'a pas suffi : SetDesiredGait aussi
    // Vitesse du pion local (pilote ou suiveur), pour reporter les tâches lourdes pendant le mouvement
    double local_speed = 0;
    double local_speed_at = -1;
    double level_mismatch_since = -1;
    // Changements de niveau (suiveur) : MapChange en attente, point d'apparition du pilote par niveau, niveau attendu
    std::optional<proto::MapChange> pending_map;
    std::map<std::string, std::string> map_spawns;
    std::string expect_level;
    double expect_level_at = -1;
    bool f8_down = false;
    // Interactions, dialogues, transitions (rejoués chez les suiveurs)
    uint32_t seq = 0;
    bool replaying = false;
    std::deque<proto::WorldInteract> interactions;
    std::deque<std::pair<proto::DialogueChoice, double>> choices;
    bool choice_pending = false;                  // question posée (StartChoice), pas encore répondue
    bool sequence_playing = false;                // OnSequenceStarted reçu, pas encore OnSequenceFinished
    // Narration (barrière) et pause
    double next_poll = 0;
    bool local_narr = false;
    proto::NarrativeKind local_kind = proto::NarrativeKind::Dialogue;
    uint32_t narr_seq = 0;
    std::map<uint8_t, bool> remote_narr;
    std::map<uint8_t, double> remote_narr_since;  // début de la narration en cours de chaque autre joueur
    double local_narr_end = -1;                   // fin de la dernière narration d'ici
    bool barrier_paused = false;
    double barrier_since = -1;
    bool local_pause = false;
    std::map<uint8_t, bool> remote_pause;
    bool remote_paused_game = false;
    // Nouvelle partie ensemble : intro (vidéo d'ouverture) en cours ici, comptée comme une cinématique pour la barrière
    bool intro = false;
    double intro_since = -1;
    int intro_sequences_done = 0;                 // cinématiques du prologue terminées depuis le début de l'intro
    double intro_calm_since = -1;                 // pion jouable, sans cinématique, depuis
    // Combat d'exploration
    std::string last_enemy;
    int last_engage = 0;
    double last_enemy_at = -1;
    std::map<UObject*, int> removed_locomotion;   // suiveur : contextes de déplacement retirés -> priorité
    // Pause partagée (raisons) et robustesse (phase 5)
    std::set<std::string> pause_reasons;
    bool pause_applied = false;
    double last_chunk_at = -1;                    // client : réception de l'instantané en cours
    bool ended = false;
    // Hash d'état d'exploration
    double last_event_at = 0;
    double next_world_check = 0;
    uint64_t last_world_sent = 0;
    double last_world_sent_at = -1000;
    uint32_t world_seq = 0;
    std::optional<proto::WorldHash> pending_world;   // client : dernière empreinte de l'hôte, à comparer au calme
    double pending_world_at = 0;
    int world_mismatch_streak = 0;
    std::set<std::string> world_ignored;          // client : parties qu'un instantané n'a pas réparées (plus de resynchro)
    std::set<std::string> resync_parts;           // client : parties en écart de la dernière demande d'instantané
    double resync_at = -1;                        // client : instant de cette demande (-1 : aucune en cours)
    std::map<std::string, int> resync_failures;   // client : instantanés de suite qui n'ont pas réparé cette partie
    int perturb_gold = 0;                         // -e33perturbworld=N (test)
    double cj_joined_at = 0;
    double waited_since = -1;                     // joueur en retard : les autres ont fini depuis
    double last_world_compute = -1000;            // dernière lecture de l'état d'exploration (hôte et client)
    std::vector<proto::HashPart> last_world_parts; // hôte : dernière empreinte envoyée
    uint64_t copy_ok_hash = 0;                    // client : empreinte vérifiée égale à celle de l'hôte, copie coop à faire
    double copy_ok_at = -1;
} s;

// Statistiques du suivi, journalisées toutes les 15 s (coop_follow_stats), pas une ligne par image.
struct FollowStats {
    double since = -1;
    // PilotState reçus (chez le suiveur)
    int rx = 0, rx_same_frame = 0, rx_gap_100 = 0, rx_gap_250 = 0, rx_gap_500 = 0, seq_lost = 0, rx_old = 0;
    double rx_gap_sum = 0, rx_gap_max = 0, last_rx = -1;
    uint32_t last_seq = 0;
    // Conduite du pion
    int ticks = 0, starts = 0, stops = 0, teleports = 0, sprints = 0, sprint_fallbacks = 0;
    double err_sum = 0, err_max = 0, age_max = 0, pilot_speed_max = 0, own_speed_sum = 0, own_speed_max = 0;
    int own_speed_n = 0;
    double sprint_s = 0, sprint_gait_ok_s = 0, last_t = -1;
};
FollowStats fstats;

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
            .add("sprint_fallbacks", f.sprint_fallbacks).add("sprint_method", s.sprint_method));
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

void bump(const char* c, int n = 1) { if (s.net.bump) s.net.bump(c, n); }
void bump_max(const char* name, int& cur, int v);   // compteur « maximum » (plus bas)
void mt_follow(double err, bool teleported, int started, int stopped);   // mesures d'arrivée (plus bas)
// Corrections des changements de niveau (2026-10-23) : -e33zonefix=0 les coupe (mesure « avant » des scénarios p9).
bool zone_fix() { static const bool v = cmdline::arg(L"e33zonefix") != L"0"; return v; }

// Les événements d'exploration ne concernent que les joueurs entrés dans la partie (hôte : au moins un client
// entré ; client : instantané chargé, ou session sans « rejoindre » des tests de combat).
bool sharing();
void touch();   // événement d'exploration : la partie n'est pas « calme » (hash d'état)

bool world_ready()
{
    return game::controller() && !game::map_change_in_progress() && game::explore_pawn() && !game::in_battle();
}

bool safe_point()
{
    return world_ready() && !game::in_dialogue() && !game::playing_cinematic() && !s.intro;
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
    coopsave::on_snapshot_sent(bytes);   // hôte d'une copie : la copie suit
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
        std::ofstream g(std::filesystem::path(launch_args().run_dir) / (L"snapshot-" + std::to_wstring(xfer) + L".sav"), std::ios::binary);
        g.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    s.peers[player].state = JoinState::Sent;
    s.peers[player].since = s.now;
    bump("snapshots_sent");
    log().write("coop_snapshot_sent", JsonObj().add("player", static_cast<int>(player)).add("xfer", xfer).add("size", static_cast<int64_t>(bytes.size()))
                                         .add("hash", hex64(h)).add("level", level));
}

void host_join_tick()
{
    for (auto& [player, pj] : s.peers) presence::set_joining(player, pj.state == JoinState::Sent, s.now);   // charge notre instantané
    for (auto& [player, pj] : s.peers) {
        if (pj.state != JoinState::Pending && pj.state != JoinState::Waiting) continue;
        // Client encore dans l'intro de sa nouvelle partie : on attend sa fin (l'instantané la couperait).
        if (safe_point() && !s.remote_narr[player]) { send_snapshot(player); continue; }
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

// --- Nouvelle partie ensemble ----------------------------------------------------------------------
// L'hôte lance une nouvelle partie pendant qu'un client attend au menu principal : le client lance la sienne au même
// moment (sans aucune sauvegarde), chacun voit l'intro chez lui, la barrière attend le plus lent, puis l'instantané
// de l'hôte remplace la partie locale du client au premier point sûr (même endroit, même moment de l'histoire).

void start_intro()
{
    s.intro = true;
    s.intro_since = s.now;
    s.intro_sequences_done = 0;
    s.intro_calm_since = -1;
}

void follow_new_game()
{
    if (s.cj != S::ClientJoin::None || !game::main_menu()) {
        bump("new_games_ignored");
        log().write("coop_new_game_ignored", JsonObj().add("main_menu", game::main_menu()).add("join_state", static_cast<int>(s.cj)));
        return;
    }
    std::string err;
    if (!game::start_new_game(true, err)) {
        bump("new_game_errors");
        log().write("coop_error", JsonObj().add("what", "start_new_game: " + err));
        return;
    }
    start_intro();
    coopsave::mark_coop_game();   // après StartNewGame (qui lève la garde) : partie de l'hôte, rien dans les saves perso
    bump("new_games_followed");
    // Pas de bannière ici (jugée inutile par Killian, 2026-10-23) : l'intro qui démarre suffit.
    log().write("coop_new_game_followed", JsonObj());
}

void client_join_tick()
{
    if (s.cj == S::ClientJoin::Receiving && s.now - s.last_chunk_at > 60.0) {
        // Instantané interrompu (aucun morceau depuis 60 s) : on le redemande.
        bump("snapshot_timeouts");
        log().write("coop_snapshot_timeout", JsonObj().add("received", s.inbound ? static_cast<int64_t>(s.inbound->received) : -1));
        s.inbound.reset();
        s.cj = S::ClientJoin::None;
        s.last_chunk_at = s.now;
        s.net.publish(proto::ResyncRequest{"snapshot timeout"}, true);
        return;
    }
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
        s.cj_joined_at = s.now;
        s.world_mismatch_streak = 0;
        s.last_event_at = s.now;
        const std::string level = game::current_level();
        s.net.publish(proto::JoinReady{s.joined_xfer, level}, true);
        bump("joins_completed");
        builds::rebaseline();
        ui::clear_status("session");
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
    s.last_chunk_at = s.now;
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
    coopsave::on_host_snapshot(in->data, s.net.name_of ? s.net.name_of(0) : std::string(), in->level);   // copie coop (D15)
    s.joined_xfer = in->xfer;
    s.cj = S::ClientJoin::Loading;
    s.cj_since = s.now;
    in.reset();
}

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
    s.net.publish(m, false);
}

// Sprint du suiveur : demandé comme la touche du joueur (SprintAction), SetDesiredGait en repli si l'allure ne suit pas.
void sprint_start(UObject* pawn)
{
    s.gait_before = s.sprint_touched ? s.gait_before : game::pawn_gait(pawn, true);
    s.sprint_touched = true;
    s.follow_sprint = true;
    s.sprint_since = s.now;
    s.sprint_method = game::pawn_sprint_action(pawn, true) ? 1 : 0;
    if ((!s.sprint_method || s.sprint_needs_gait) && game::set_pawn_desired_gait(pawn, 2)) s.sprint_method = 2;
    s.next_gait_check = s.now + 0.6;
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
    game::pawn_sprint_action(pawn, false);
    if (game::pawn_gait(pawn, true) == 2) game::set_pawn_desired_gait(pawn, s.gait_before >= 0 && s.gait_before != 2 ? s.gait_before : 1);
}

void sprint_stop(UObject* pawn)
{
    if (!s.follow_sprint) return;
    s.follow_sprint = false;
    if (pawn) sprint_off(pawn);
}

// Plus suiveur (pilote, combat, chargement, fin) : sprint rendu au pion s'il est encore là.
void sprint_release()
{
    if (!s.follow_sprint) return;
    sprint_stop(game::in_battle() ? nullptr : game::explore_pawn());
}

void follow_tick()
{
    if (!s.last_pilot || s.last_pilot->pilot != s.pilot) { sprint_release(); return; }
    if (!s.host && s.want_join && s.cj != S::ClientJoin::Joined) return;
    if (game::map_change_in_progress() || game::in_battle()) { s.follow_sprint = false; return; }
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
    // Carte du monde : pilote sur Esquie (vol, nage) non reproductible ici -> pion tenu en fantôme sur le pilote (D19).
    if (mount::follow(pawn, p, s.now - s.last_pilot_at, s.now, s.net.bump)) { s.follow_have_target = false; s.follow_moving = false; return; }
    game::PawnState me;
    if (!game::pawn_state(pawn, me)) return;
    const double own_speed = std::hypot(me.vel[0], me.vel[1]);
    s.local_speed = own_speed;
    s.local_speed_at = s.now;
    const double real_age = std::max(0.0, s.now - s.last_pilot_at);
    const double age = std::min(0.25, real_age);
    const double raw[3] = {p.x + p.vx * age, p.y + p.vy * age, p.z + p.vz * age};
    // Cible lissée (~0,12 s) : les positions reçues par Steam arrivent par à-coups ; un saut net (> 3 m) est pris tel quel.
    const double dt = s.follow_last_t < 0 ? 0.0 : std::clamp(s.now - s.follow_last_t, 0.0, 0.25);
    s.follow_last_t = s.now;
    const double jump = std::hypot(raw[0] - s.follow_target[0], raw[1] - s.follow_target[1]);
    if (!s.follow_have_target || jump > 300) { for (int i = 0; i < 3; ++i) s.follow_target[i] = raw[i]; s.follow_have_target = true; }
    else { const double k = 1.0 - std::exp(-dt / 0.12); for (int i = 0; i < 3; ++i) s.follow_target[i] += (raw[i] - s.follow_target[i]) * k; }
    const double tx = s.follow_target[0], ty = s.follow_target[1], tz = s.follow_target[2];
    const double dx = tx - me.pos[0], dy = ty - me.pos[1], dz = tz - me.pos[2];
    const double d = std::sqrt(dx * dx + dy * dy);
    s.follow_error = std::sqrt(d * d + dz * dz);
    const double sp = std::hypot(p.vx, p.vy);
    // Statistiques (résumé toutes les 15 s)
    auto& f = fstats;
    if (f.since < 0) f.since = s.now;
    const double fdt = f.last_t < 0 ? 0.0 : std::clamp(s.now - f.last_t, 0.0, 0.25);
    f.last_t = s.now;
    ++f.ticks;
    f.err_sum += s.follow_error;
    f.err_max = std::max(f.err_max, s.follow_error);
    f.age_max = std::max(f.age_max, real_age);
    f.pilot_speed_max = std::max(f.pilot_speed_max, sp);
    if (s.follow_moving) { f.own_speed_sum += own_speed; ++f.own_speed_n; }
    f.own_speed_max = std::max(f.own_speed_max, own_speed);
    const bool fresh = real_age < 1.0;   // mesures d'arrivée (changement de niveau) : position récente du pilote seulement
    if (d > kTeleportDistance || std::fabs(dz) > 400) {
        if (fresh) mt_follow(s.follow_error, true, 0, 0);
        const double pos[3] = {tx, ty, tz};
        game::teleport_pawn(pawn, pos, p.yaw);
        bump("follower_teleports");
        ++f.teleports;
        log().write("coop_follower_teleport", JsonObj().add("distance", d).add("dz", dz).add("pilot_speed", sp).add("own_speed", own_speed)
                                                 .add("gait", game::pawn_gait(pawn)).add("sprint", s.follow_sprint).add("age_ms", static_cast<int64_t>(real_age * 1000.0)));
        return;
    }
    // Arrêt seulement quand le pilote s'arrête vraiment et qu'on est arrivé ; reprise dès qu'il bouge ou s'éloigne
    // (avant : un seul seuil à 60 u, donc arrêt-redémarrage plusieurs fois par seconde quand il court).
    int started = 0, stopped = 0;
    if (!s.follow_moving && (d > 120 || sp > 80)) { s.follow_moving = true; bump("follow_starts"); ++f.starts; started = 1; }
    else if (s.follow_moving && d < 40 && sp < 30) { s.follow_moving = false; ++f.stops; stopped = 1; }
    if (fresh) mt_follow(s.follow_error, false, started, stopped);
    // Allure : sprint quand le pilote sprinte (course ~350 u/s, sprint ~720 u/s mesurés le 2026-10-22) ou pour
    // rattraper un retard pendant qu'il court ; rendu quand il ralentit et qu'on l'a rattrapé.
    const bool want_sprint = s.follow_moving && (sp > 450 || (sp > 200 && d > 300));
    const bool drop_sprint = !s.follow_moving || (sp < 400 && d < 150);
    if (want_sprint && !s.follow_sprint) sprint_start(pawn);
    else if (s.follow_sprint && drop_sprint) sprint_stop(pawn);
    if (s.follow_sprint) {
        f.sprint_s += fdt;
        if (own_speed > 500) f.sprint_gait_ok_s += fdt;
    }
    if (s.sprint_touched && s.now >= s.next_gait_check) {
        s.next_gait_check = s.now + 0.5;
        const int gait = game::pawn_gait(pawn);
        if (s.follow_sprint && gait >= 0 && gait != 2 && s.sprint_method == 1 && s.now - s.sprint_since > 0.6 && own_speed < 450) {
            // SprintAction sans effet (pion sans entrée du joueur ?) : allure voulue imposée.
            if (game::set_pawn_desired_gait(pawn, 2)) s.sprint_method = 2;
            ++f.sprint_fallbacks;
            bump("follow_sprint_fallbacks");
            if (!s.sprint_needs_gait)   // une fois : les sprints suivants passent directement par SetDesiredGait
                log().write("coop_follow_sprint_fallback", JsonObj().add("gait", gait).add("own_speed", own_speed).add("ok", s.sprint_method == 2));
            s.sprint_needs_gait = true;
        } else if (!s.follow_sprint && gait == 2) {
            sprint_off(pawn);   // sprint resté après un combat ou un chargement
        }
    }
    if (!s.follow_moving || d < 1) return;
    // Direction : vers la cible, mêlée à la direction du pilote quand on est tout près (pas de demi-tours sur place).
    double ux = dx / d, uy = dy / d;
    if (sp > 30) {
        const double w = std::clamp(1.0 - d / 150.0, 0.0, 1.0);
        ux = ux * (1 - w) + (p.vx / sp) * w;
        uy = uy * (1 - w) + (p.vy / sp) * w;
        const double n = std::hypot(ux, uy);
        if (n > 1e-3) { ux /= n; uy /= n; }
    }
    // Vitesse : celle du pilote (course comprise) plus un rattrapage proportionnel à l'écart.
    const double scale = std::clamp(sp / 500.0 + d / 300.0, 0.25, 1.0);
    game::move_pawn(pawn, ux, uy, scale);
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
            bump(ok ? "interactions_replayed" : "interaction_errors"); touch();
            log().write("coop_interaction_replayed", JsonObj().add("actor", wi.actor).add("ok", ok));
        }
    }
    if (!s.choices.empty()) {
        auto& [dc, at] = s.choices.front();
        if (s.choice_pending) {
            s.replaying = true;
            const bool ok = game::choose_dialogue(dc.index);
            s.replaying = false;
            bump(ok ? "choices_replayed" : "choice_errors"); touch();
            log().write("coop_choice_replayed", JsonObj().add("index", dc.index).add("ok", ok).add("waited_s", s.now - at));
            s.choices.pop_front();
        } else if (s.now - at > 120) {
            bump("choice_timeouts");
            log().write("coop_choice_timeout", JsonObj().add("index", dc.index));
            s.choices.pop_front();
        }
    }
}

// --- Changements de niveau : mesures (2026-10-23) -------------------------------------------------
// Chez le suiveur, pour chaque MapChange du pilote : délai avant le début du chargement d'ici, nombre de chargements,
// durée du chargement d'ici, arrivée par rapport au pilote (sa première position dans le nouveau niveau), écart au pilote
// à la reprise du suivi et temps pour le rattraper, téléportations, arrêts, pause et images lentes du mod juste après
// l'arrivée. Une ligne coop_map_transition par transition, et des compteurs pour les scénarios (« max » en ms).
struct MapTrans {
    bool open = false;
    std::string level, spawn, how;   // how : followed, already (chargement déjà en route), same_level, deferred
    int expect_loads = 1;
    double rx_at = -1;               // MapChange reçu
    double load_at = -1;             // premier chargement d'ici (avant rx_at : pion passé par le même déclencheur)
    int loads = 0;                   // chargements commencés ici pendant la transition
    double arrived_at = -1;          // monde prêt ici, dans le niveau visé
    double pilot_at = -1;            // première position du pilote reçue dans le niveau visé
    double follow_at = -1, err0 = -1, caught_at = -1;   // premier suivi après l'arrivée, son écart, écart < 250 u
    int teleports = 0, starts = 0, stops = 0, mod_slow = 0, slow = 0;
    double paused_s = 0, mod_max_ms = 0;
};
MapTrans mt;
bool load_in = false;                 // chargement de niveau en cours ici (IsMapChangeInProgress)
double load_rise_at = -1;
std::string load_dest;
double prev_now = -1;
std::string tp_level;                 // niveau où l'état des boîtes de passage a été appliqué
bool tp_off = false;                  // boîtes de passage coupées (suiveur)
double tp_at = -1;
int tp_count = -1;
struct MapMax { int delay = 0, late = 0, load = 0, err = 0, catchup = 0, paused = 0, mod = 0; } mmax;
constexpr double kMapWindow = 12.0;   // fenêtre d'observation après l'arrivée (et celle du pilote)

double mt_window_start() { return std::max(mt.arrived_at, mt.pilot_at); }
bool mt_in_window() { return mt.open && mt.arrived_at >= 0 && s.now - mt.arrived_at < 30.0; }

void mt_close(const char* why)
{
    if (!mt.open) return;
    mt.open = false;
    const bool done = mt.arrived_at >= 0;
    auto ms = [](double a, double b) { return a >= 0 && b >= 0 ? static_cast<int64_t>((a - b) * 1000.0) : -1; };
    const int64_t delay = ms(mt.load_at, mt.rx_at), load = ms(mt.arrived_at, mt.load_at), late = ms(mt.arrived_at, mt.pilot_at);
    const int64_t catchup = mt.caught_at >= 0 ? ms(mt.caught_at, mt.follow_at) : (mt.follow_at >= 0 ? ms(s.now, mt.follow_at) : -1);
    log().write("coop_map_transition", JsonObj().add("level", mt.level).add("spawn", mt.spawn).add("how", mt.how).add("why", why).add("arrived", done)
        .add("loads", mt.loads).add("expect_loads", mt.expect_loads).add("delay_ms", delay).add("load_ms", load)
        .add("pilot_ready_ms", ms(mt.pilot_at, mt.rx_at)).add("after_pilot_ms", late)
        .add("err0", static_cast<int64_t>(mt.err0)).add("catchup_ms", catchup).add("teleports", mt.teleports)
        .add("starts", mt.starts).add("stops", mt.stops).add("paused_ms", static_cast<int64_t>(mt.paused_s * 1000.0))
        .add("mod_slow", mt.mod_slow).add("mod_max_ms", std::round(mt.mod_max_ms * 100.0) / 100.0).add("slow_frames", mt.slow));
    bump("map_transitions");
    if (!done) bump("map_transitions_unfinished");
    if (mt.loads > mt.expect_loads) bump("map_loads_extra", mt.loads - mt.expect_loads);
    if (mt.loads < mt.expect_loads) bump("map_loads_missing", mt.expect_loads - mt.loads);
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
    if (done && mt.follow_at < 0) bump("map_arrival_no_follow");
}

// Chaque image (pilote et suiveur) : début et fin des chargements d'ici ; suiveur : arrivée et fenêtre d'observation.
void map_tick()
{
    const double dt = prev_now < 0 ? 0.0 : s.now - prev_now;
    prev_now = s.now;
    const bool in = game::map_change_in_progress();
    if (in && !load_in) {
        load_in = true;
        load_rise_at = s.now;
        std::string tag;
        if (!game::map_destination(load_dest, tag)) load_dest.clear();
        bump("map_loads_local");
        log().write("coop_map_load_start", JsonObj().add("level", load_dest).add("spawn", tag).add("pilot", is_pilot()).add("from", game::current_level()));
        if (mt.open) { ++mt.loads; if (mt.load_at < 0) mt.load_at = s.now; }   // après l'arrivée : chargement de trop
    } else if (!in && load_in) {
        load_in = false;
        log().write("coop_map_load_end", JsonObj().add("level", load_dest).add("load_s", s.now - load_rise_at).add("pilot", is_pilot()));
    }
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
            mt.arrived_at = s.now;
            log().write("coop_map_arrived", JsonObj().add("level", mt.level).add("after_rx_s", s.now - mt.rx_at)
                                                .add("pilot_here", mt.pilot_at >= 0).add("loads", mt.loads));
        } else if (s.now - mt.rx_at > 180.0) mt_close("never_arrived");
        return;
    }
    // Fenêtre d'observation : pause, images lentes (mod > 20 ms, image > 80 ms hors chargement).
    if (!s.pause_reasons.empty() || s.pause_applied) mt.paused_s += dt;
    const double mod_ms = prof::last().total_ms;
    mt.mod_max_ms = std::max(mt.mod_max_ms, mod_ms);
    if (mod_ms > 20.0) ++mt.mod_slow;
    if (dt > 0.08 && dt < 3.0 && !in) ++mt.slow;
    if (mt.pilot_at >= 0 && s.now - mt_window_start() > kMapWindow) mt_close("done");
    else if (s.now - mt.arrived_at > 60.0) mt_close("pilot_absent");
}

// Suiveur : chaque image de suivi après l'arrivée (écart au pilote, téléportation, arrêt / reprise).
void mt_follow(double err, bool teleported, int started, int stopped)
{
    if (!mt_in_window() || mt.pilot_at < 0) return;
    if (mt.follow_at < 0) {
        mt.follow_at = s.now;
        mt.err0 = err;
        log().write("coop_map_follow_resumed", JsonObj().add("level", mt.level).add("err", static_cast<int64_t>(err))
                                                   .add("after_arrival_s", s.now - mt.arrived_at).add("after_pilot_s", s.now - mt.pilot_at));
    }
    if (mt.caught_at < 0 && err < 250) mt.caught_at = s.now;
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
    bump(ok ? "map_changes_followed" : "map_change_errors");
    log().write("coop_map_change_followed", JsonObj().add("level", mc.level).add("spawn", mc.spawn_tag).add("ok", ok).add("how", mt.how)
                                                .add("after_rx_s", s.now - mt.rx_at));
}

// Suiveur : MapChange appliqué tout de suite si possible ; sinon gardé (le dernier seulement) et appliqué dès que le
// monde est prêt ici (pending_map_tick) : chargement d'un autre niveau en cours (pilote reparti avant notre arrivée),
// combat pas encore fini ici, instantané en cours de chargement. Avant : second ChangeMap en plein chargement, ou
// MapChange perdu pendant la jonction (retour par level_resync 8 s plus tard, sans point d'apparition).
void on_map_change(const proto::MapChange& mc)
{
    touch();
    if (is_menu_level(mc.level)) {
        log().write("coop_map_change_ignored", JsonObj().add("level", mc.level).add("why", "menu"));
        return;
    }
    s.map_spawns[mc.level] = mc.spawn_tag;
    s.expect_level = mc.level;
    s.expect_level_at = s.now;
    mt_close("next");
    mt = MapTrans{};
    mt.open = true;
    mt.level = mc.level;
    mt.spawn = mc.spawn_tag;
    mt.rx_at = s.now;
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

// --- Entrées de déplacement d'un suiveur ------------------------------------------------------------
// Le pion d'un suiveur est conduit par le mod (AddMovementInput) : les touches de déplacement locales sont coupées
// en retirant les contextes de locomotion (même méthode qu'en combat, vu le 2026-10-20 : seul RemoveMappingContext
// coupe vraiment une touche). Caméra, menus et dialogues restent libres. Rendus quand on redevient pilote.
constexpr const wchar_t* kLocomotionContexts[] = {
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_Default_ALS.IMC_Locomotion_Default_ALS",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_Crouch.IMC_Locomotion_Crouch",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_CustomMovement.IMC_Locomotion_CustomMovement",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_WorldMapCharacter.IMC_Locomotion_WorldMapCharacter",
    // Phase 5 : interactions du suiveur (touche d'interaction : PNJ, coffres…) et choix de dialogue pendant une question
    // (la réponse vient du pilote). Indices 4 et 5 : voir context_wanted_locked.
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_UI_Exploration.IMC_UI_Exploration",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_UI_Dialogue.IMC_UI_Dialogue",
};

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

// --- Barrière narrative et pause ------------------------------------------------------------------

// Pause du jeu entier tenue par une ou plusieurs raisons. Le jeu peut l'enlever de lui-même (fin de cinématique :
// ResumeGameAfterCinematic) : elle est alors réappliquée, sinon le combat qui suit une cinématique démarre quand même.
void apply_pause()
{
    const bool want = !s.pause_reasons.empty();
    if (want != s.pause_applied) {
        if (game::set_game_paused(want)) s.pause_applied = want;
        return;
    }
    if (want && !game::is_game_paused()) {
        game::set_game_paused(true);
        bump("pause_reasserted");
        log().write("coop_pause_reasserted", JsonObj().add("reasons", static_cast<int64_t>(s.pause_reasons.size())));
    }
}


bool others_in_narrative()
{
    for (auto& [p, a] : s.remote_narr) if (a && p != s.me) return true;
    return false;
}

std::vector<uint8_t> narrative_players()
{
    std::vector<uint8_t> out;
    for (auto& [p, a] : s.remote_narr) if (a && p != s.me) out.push_back(p);
    return out;
}

// Autres joueurs de la partie connus ici (hôte : clients entrés ; client : l'hôte et ceux dont la narration est relayée).
std::vector<uint8_t> known_players()
{
    std::set<uint8_t> ps;
    if (s.host) { for (auto& [p, pj] : s.peers) if (pj.state == JoinState::Joined || pj.state == JoinState::InGame) ps.insert(p); }
    else ps.insert(0);
    for (auto& [p, a] : s.remote_narr) ps.insert(p);
    ps.erase(s.me);
    return std::vector<uint8_t>(ps.begin(), ps.end());
}

// Joueur en retard (cinématique ou dialogue encore en cours ici, les autres ont fini et leur barrière les tient en
// pause) : rappel discret, après 3 s pour ne pas l'afficher quand un autre commence la même cinématique un peu après.
void waited_line_tick(bool narr)
{
    std::vector<uint8_t> waiting;
    if (narr && sharing())
        for (uint8_t p : known_players()) if (auto it = s.remote_narr.find(p); it == s.remote_narr.end() || !it->second) waiting.push_back(p);
    if (waiting.empty()) s.waited_since = -1;
    else if (s.waited_since < 0) s.waited_since = s.now;
    if (!waiting.empty() && s.now - s.waited_since >= 3.0)
        ui::set_status_text("waited", "waited_by", presence::waited_by_text(waiting, s.net.name_of, s.local_kind == proto::NarrativeKind::Cinematic));
    else ui::clear_status("waited");
}

// --- Scènes synchronisées (2026-10-23) -------------------------------------------------------------
// Essai à deux PC du 2026-10-22 : le pilote déclenche une cinématique (zone traversée au bas de la corde) ; le suiveur,
// un peu derrière, n'atteint pas la zone, la barrière le fige, et il ne joue la scène qu'après la fin chez le pilote
// (« en décalé »). Le pilote publie donc la scène qu'il commence (TriggerCinematic) et sa cause ; le suiveur la
// déclenche aussitôt par la même zone (TryExecuteGameActionOnOverlap : état de la zone tenu par le jeu, pas de double
// lecture), sinon téléporte son pion sur la position du pilote au déclenchement (il traverse la même zone).
// La barrière ne sert plus qu'à attendre la fin ; son délai de grâce reste le filet (pair ancien, scène non retrouvée).
struct SceneSync {
    // Dernières causes possibles vues ici (pilote) : zone « au passage », interaction publiée, game action exécutée.
    std::string zone, zone_action; double zone_at = -1;
    std::string interaction; double interaction_at = -1;
    std::string action; double action_at = -1;
    UObject* zone_in_call = nullptr;              // zone dans TryExecuteGameActionOnOverlap (pré -> post)
    uint32_t seq = 0;
    // Dernière cinématique commencée ici
    std::string local_scene; double local_scene_at = -1;
    // Suiveur : scène du pilote à déclencher ici
    std::optional<proto::SceneStart> pending; double pending_at = -1;
    double scene_rx_at = -1;                      // dernier SceneStart reçu (mesure du retard)
    std::string acted_scene; double acted_at = -1; int acted_method = 0; bool acted_checked = true;
    // Suiveur : cinématique passée par le pilote, à passer ici aussi (réessayée : le jeu ignore un passage trop tôt)
    double skip_at = -1, next_skip_try = 0; int skip_tries = 0; bool skip_seen_playing = false;
    double skip_called_at = -1;                   // SkipCinematic appelé ici (passage effectif si la séquence finit juste après)
    // Mesures (tests) : début de la scène ici après le début chez un autre, fin ici après la fin chez lui
    std::map<uint8_t, double> remote_end_at;
    int lag_max_ms = 0, end_late_max_ms = 0, barrier_wait_max_ms = 0;
};
SceneSync sc;

constexpr double kSceneCauseWindow = 60.0;   // cause retenue si vue dans la minute qui précède la cinématique

// Clé d'une scène : identifiant de cinématique, sinon nom de l'asset de séquence.
std::string scene_key(const std::string& id, const std::string& seq) { return !id.empty() && id != "None" ? id : seq; }

// Compteur « maximum » (les compteurs du coop sont des sommes) : relevé de la différence.
void bump_max(const char* name, int& cur, int v)
{
    if (v <= cur) return;
    bump(name, v - cur);
    cur = v;
}

// Action « au passage » d'une zone (GameActionOnOverlap : référence ou action intégrée).
UObject* zone_overlap_action(UObject* zone)
{
    using namespace RC::Unreal;
    auto* wp = CastField<FStructProperty>(ue::prop(zone, STR("GameActionOnOverlap")));
    if (!wp) return nullptr;
    void* w = wp->ContainerPtrToValuePtr<void>(zone);
    for (const wchar_t* m : {STR("GameActionReference"), STR("GameActionInline")})
        if (FProperty* p = ue::struct_member(wp->GetStruct(), m))
            if (UObject* a = *static_cast<UObject**>(p->ContainerPtrToValuePtr<void>(w))) return a;
    return nullptr;
}

// TriggerCinematic(FS_TriggerCinematicVariables) : identifiant de la cinématique et asset de la séquence (chemin souple).
void read_cinematic_params(RC::Unreal::UnrealScriptFunctionCallableContext& ctx, UFunction* f, std::string& id, std::string& seq)
{
    using namespace RC::Unreal;
    auto* sp = CastField<FStructProperty>(ue::struct_member(f, L"TriggerCinematicVariableStruct"));
    void* base = ue::param_address(ctx, f, STR("TriggerCinematicVariableStruct"));
    if (!sp || !base) return;
    UStruct* st = sp->GetStruct();
    if (FProperty* p = ue::struct_member(st, L"Cinematic_LevelSequence_ID"))
        id = ue::to_utf8(static_cast<FName*>(p->ContainerPtrToValuePtr<void>(base))->ToString());
    // TSoftObjectPtr (UE 5.4, 0x28) : FWeakObjectPtr (8) puis FSoftObjectPath { PackageName, AssetName, SubPath }.
    if (FProperty* p = ue::struct_member(st, L"Cinematic_LevelSequence_SequenceToPlay"); p && p->GetSize() == 0x28) {
        auto* raw = static_cast<uint8_t*>(p->ContainerPtrToValuePtr<void>(base));
        const std::string pkg = ue::to_utf8(reinterpret_cast<FName*>(raw + 0x08)->ToString());
        const std::string asset = ue::to_utf8(reinterpret_cast<FName*>(raw + 0x10)->ToString());
        if (!asset.empty() && asset != "None") seq = pkg + "." + asset;
    }
}

// Pilote : la cinématique commence ici -> SceneStart (cause la plus récente : zone, sinon interaction, sinon game action).
void on_local_cinematic(const std::string& id, const std::string& seq)
{
    sc.local_scene = scene_key(id, seq);
    sc.local_scene_at = s.now;
    proto::SceneStart m;
    m.scene = id; m.sequence = seq;
    auto recent = [](double at) { return at >= 0 && s.now - at < kSceneCauseWindow; };
    if (recent(sc.zone_at) && (!recent(sc.interaction_at) || sc.zone_at >= sc.interaction_at)) {
        m.cause = proto::SceneCause::Zone; m.cause_path = sc.zone; m.action = sc.zone_action;
    } else if (recent(sc.interaction_at)) {
        m.cause = proto::SceneCause::Interaction; m.cause_path = sc.interaction;
        if (recent(sc.action_at)) m.action = sc.action;
    } else if (recent(sc.action_at)) {
        m.cause = proto::SceneCause::Action; m.action = sc.action;
    }
    const bool publish = s.active && !s.ended && is_pilot() && sharing() && !s.replaying;
    log().write("coop_cinematic_trigger", JsonObj().add("scene", id).add("sequence", seq).add("cause", static_cast<int>(m.cause))
                                              .add("cause_path", m.cause_path).add("action", m.action).add("published", publish)
                                              .add("zone_age_s", sc.zone_at < 0 ? -1.0 : s.now - sc.zone_at)
                                              .add("interaction_age_s", sc.interaction_at < 0 ? -1.0 : s.now - sc.interaction_at));
    if (!publish) return;
    m.player = s.me;
    m.seq = ++sc.seq;
    if (UObject* pawn = game::explore_pawn()) {
        game::PawnState ps;
        if (game::pawn_state(pawn, ps)) { m.x = ps.pos[0]; m.y = ps.pos[1]; m.z = ps.pos[2]; }
    }
    s.net.publish(m, true);
    bump("scenes_published");
}

// Test (-e33nofollow=1) : pion suiveur laissé sur place, la scène ne peut venir que de la synchro (SceneStart).
bool scene_test_no_follow()
{
    static const bool v = cmdline::arg(L"e33nofollow") == L"1";
    return v;
}

// Suiveur : déclenche ici la scène que le pilote vient de commencer.
void scene_tick()
{
    // Passage de la cinématique demandé par le pilote : tant qu'elle joue ici (au plus 10 s), une tentative par seconde.
    if (sc.skip_at >= 0 && s.now >= sc.next_skip_try) {
        // Séquence en cours (OnSequenceStarted -> OnSequenceFinished) : après sa fin, la transition de sortie suit, on
        // n'y touche plus (une pause de cinématique pendant la transition n'est pas souhaitable).
        const bool playing = s.sequence_playing && game::playing_cinematic();
        if (!playing && !sc.skip_seen_playing && s.now - sc.skip_at < 10.0) {
            sc.next_skip_try = s.now + 0.2;   // pas encore commencée ici : attente
        } else if (playing && s.now - sc.skip_at < 10.0) {
            sc.skip_seen_playing = true;
            sc.next_skip_try = s.now + 1.0;
            ++sc.skip_tries;
            game::skip_cinematic();
            log().write("coop_scene_skip_try", JsonObj().add("try", sc.skip_tries).add("after_s", s.now - sc.skip_at));
        } else {
            bump(sc.skip_seen_playing && !playing ? "scene_skips_ok" : "scene_skips_failed");
            log().write("coop_scene_skip_done", JsonObj().add("tries", sc.skip_tries).add("still_playing", playing).add("after_s", s.now - sc.skip_at));
            sc.skip_at = -1;
        }
    }
    // Vérification (3 s après) : la scène a-t-elle commencé ici ?
    if (!sc.acted_checked && s.now - sc.acted_at > 3.0) {
        sc.acted_checked = true;
        const bool ok = s.local_narr || sc.local_scene_at >= sc.acted_at;
        bump(ok ? "scene_syncs_ok" : "scene_syncs_failed");
        log().write("coop_scene_sync_result", JsonObj().add("scene", sc.acted_scene).add("ok", ok).add("method", sc.acted_method)
                                                  .add("local_scene", sc.local_scene));
    }
    if (!sc.pending) return;
    const proto::SceneStart m = *sc.pending;
    const std::string key = scene_key(m.scene, m.sequence);
    auto drop = [&](const char* why) {
        log().write("coop_scene_sync_skip", JsonObj().add("scene", key).add("why", why));
        bump("scene_syncs_skipped");
        sc.pending.reset();
    };
    if (is_pilot()) return drop("pilot");
    // Déjà dans une scène ici, ou même scène commencée ici récemment (le pion suiveur a traversé la zone) : rien.
    if (s.local_narr || game::playing_cinematic() || game::in_dialogue()) return drop("already_narrative");
    if (!key.empty() && sc.local_scene == key && s.now - sc.local_scene_at < 120) return drop("already_started");
    if (!world_ready()) { if (s.now - sc.pending_at > 15) drop("world_not_ready"); return; }
    // Interaction : rejouée ici par WorldInteract, la scène doit suivre d'elle-même ; repli après 3 s.
    if (m.cause == proto::SceneCause::Interaction && s.now - sc.pending_at < 3.0) return;
    int method = 0;
    if (m.cause == proto::SceneCause::Zone && !m.cause_path.empty()) {
        if (UObject* zone = ue::find_object(ue::from_utf8(m.cause_path).c_str())) {
            const bool done = ue::get_bool(zone, STR("IsBehaviorDone"));
            s.replaying = true;
            ue::Call c(zone, STR("TryExecuteGameActionOnOverlap"));
            const bool called = c && c.invoke();
            s.replaying = false;
            const bool playing = ue::get_bool(zone, STR("IsGameActionOnOverlapPlaying"));
            log().write("coop_scene_zone", JsonObj().add("zone", m.cause_path).add("done_before", done).add("called", called).add("playing", playing)
                                               .add("scene_started", sc.local_scene_at >= sc.pending_at));
            if (playing || sc.local_scene_at >= sc.pending_at || game::playing_cinematic()) method = 1;
            else if (!done) {
                // La zone refuse (conditions d'entrée ?) : son action exécutée directement.
                UObject* action = zone_overlap_action(zone);
                ue::Call ex(ue::get_object(game::controller(), STR("BP_GameActionExecutorComponent")), STR("ExecuteGameAction"));
                if (action && ex) {
                    s.replaying = true;
                    ex.set_object(STR("GameAction"), action).invoke();
                    s.replaying = false;
                    method = 2;
                    log().write("coop_scene_zone_action", JsonObj().add("action", ue::full_name(action)));
                }
            }
        } else log().write("coop_scene_zone_missing", JsonObj().add("zone", m.cause_path));
    }
    if (method == 0 && (m.x != 0 || m.y != 0 || m.z != 0)) {
        // Repli : le pion suiveur rejoint la position du pilote au déclenchement (il traverse la même zone).
        if (UObject* pawn = game::explore_pawn()) {
            const double pos[3] = {m.x, m.y, m.z};
            const double yaw = s.last_pilot ? s.last_pilot->yaw : 0.0;
            if (game::teleport_pawn(pawn, pos, yaw)) method = 3;
        }
    }
    bump(method == 1 ? "scene_syncs_zone" : method == 2 ? "scene_syncs_action" : method == 3 ? "scene_syncs_teleport" : "scene_syncs_none");
    log().write("coop_scene_sync", JsonObj().add("scene", key).add("cause", static_cast<int>(m.cause)).add("method", method)
                                       .add("after_s", s.now - sc.pending_at));
    sc.acted_scene = key; sc.acted_at = s.now; sc.acted_method = method; sc.acted_checked = false;
    sc.pending.reset();
}

void poll_tick()
{
    if (s.now < s.next_poll) return;
    s.next_poll = s.now + kPollPeriod;
    if (!game::controller()) return;
    movement_lock_tick();
    // Narration locale
    if (s.intro && s.now - s.intro_since > kIntroMax) { s.intro = false; log().write("coop_intro_timeout", JsonObj()); }
    // Fin de l'intro : le signal du jeu (OnInitialCinematicCompleted) n'arrive pas (vu le 2026-10-21). Elle finit donc
    // quand le pion est jouable, sans cinématique, 5 s après la fin d'au moins une cinématique du prologue (la vidéo
    // d'ouverture n'est pas une cinématique du jeu : le monde paraît « calme » pendant qu'elle passe).
    if (s.intro) {
        const bool calm = world_ready() && !game::playing_cinematic() && !s.sequence_playing && !game::in_dialogue();
        if (!calm || s.intro_sequences_done == 0) s.intro_calm_since = -1;
        else if (s.intro_calm_since < 0) s.intro_calm_since = s.now;
        else if (s.now - s.intro_calm_since >= 5.0) {
            s.intro = false;
            bump("intros_finished");
            log().write("coop_intro_finished", JsonObj().add("after_s", s.now - s.intro_since).add("sequences", s.intro_sequences_done));
        }
    }
    const bool cine = game::playing_cinematic() || s.intro;
    const bool dlg = !cine && game::in_dialogue();
    const bool narr = cine || dlg;
    if (narr != s.local_narr) {
        s.local_narr = narr;
        if (!narr) s.local_narr_end = s.now;
        if (narr) s.local_kind = cine ? proto::NarrativeKind::Cinematic : proto::NarrativeKind::Dialogue;
        ++s.narr_seq;
        s.net.publish(proto::NarrativeState{s.me, s.local_kind, narr, s.narr_seq}, true);
        bump(narr ? "narratives_started" : "narratives_finished");
        log().write("coop_narrative", JsonObj().add("active", narr).add("kind", static_cast<int>(s.local_kind)).add("seq", s.narr_seq));
        // Mesures : début ici après le début chez un autre joueur (scène en cours chez lui, ou SceneStart reçu juste avant),
        // fin ici après sa fin.
        if (narr && sc.scene_rx_at >= 0 && s.now - sc.scene_rx_at < 30) {
            const double lag = s.now - sc.scene_rx_at;
            log().write("coop_scene_lag", JsonObj().add("from_scene_start", true).add("lag_s", lag));
            bump_max("scene_start_lag_ms_max", sc.lag_max_ms, static_cast<int>(lag * 1000));
            sc.scene_rx_at = -1;
        }
        for (auto& [p, a] : s.remote_narr) {
            if (p == s.me) continue;
            if (narr && a) {
                const double lag = s.now - s.remote_narr_since[p];
                log().write("coop_scene_lag", JsonObj().add("player", static_cast<int>(p)).add("lag_s", lag));
                bump_max("scene_start_lag_ms_max", sc.lag_max_ms, static_cast<int>(lag * 1000));
            } else if (!narr && !a && sc.remote_end_at.count(p) && s.now - sc.remote_end_at[p] < 300) {
                const double late = s.now - sc.remote_end_at[p];
                log().write("coop_scene_end_late", JsonObj().add("player", static_cast<int>(p)).add("late_s", late));
                bump_max("scene_end_late_ms_max", sc.end_late_max_ms, static_cast<int>(late * 1000));
            }
        }
    }
    // Barrière : fini ici, pas chez les autres -> jeu en pause jusqu'à ce que tout le monde ait fini. Aussi quand la
    // cinématique enchaîne sur un combat (boss) : personne ne commence le combat avant les autres.
    // Jamais pendant un combat coop engagé : c'est le démarrage coordonné (BattleReady) qui attend les joueurs. Sinon
    // interblocage, vu le 2026-10-22 (duel du prologue contre Maelle) : le dialogue de l'hôte reste « en cours » pendant
    // tout le duel, la barrière gardait le client en pause, son premier tour ne commençait jamais, pas de BattleReady,
    // et l'hôte restait figé à son tour 1 en attendant le client.
    const bool battle = s.net.battle_engaged && s.net.battle_engaged();
    bool hold = false, finished_here = false;
    double since = s.now;
    if (!narr && !battle && others_in_narrative()) {
        // Fini ici après le début chez les autres (même scène, on a fini avant) : pause tout de suite. Scène des autres
        // pas (encore) jouée ici : délai de grâce pour que le suiveur la déclenche aussi.
        for (auto& [p, a] : s.remote_narr) {
            if (!a || p == s.me) continue;
            auto it = s.remote_narr_since.find(p);
            since = std::min(since, it == s.remote_narr_since.end() ? -1e9 : it->second);
        }
        finished_here = s.local_narr_end >= 0 && s.local_narr_end >= since - 1.0;
        hold = finished_here || s.now - since >= kBarrierGrace || s.barrier_paused;
    }
    if (hold && !s.barrier_paused) {
        s.pause_reasons.insert("barrier");
        s.barrier_paused = true;
        s.barrier_since = s.now;
        bump("barrier_waits");
        if (!finished_here) bump("barrier_waits_unstarted");   // scène des autres pas commencée ici (pause avant la scène)
        // finished_here : même scène, finie ici avant les autres ; sinon scène pas commencée ici après le délai de grâce.
        log().write("coop_barrier_hold", JsonObj().add("finished_here", finished_here).add("others_since_s", since < -1e8 ? -1.0 : s.now - since));
    } else if (s.barrier_paused && (!hold || s.now - s.barrier_since > kBarrierTimeout)) {
        if (hold) { bump("barrier_timeouts"); s.remote_narr.clear(); s.remote_narr_since.clear(); }
        s.pause_reasons.erase("barrier");
        s.barrier_paused = false;
        ui::clear_status("wait");
        bump("barrier_releases");
        if (battle) bump("barrier_releases_battle");
        bump_max("barrier_wait_ms_max", sc.barrier_wait_max_ms, static_cast<int>((s.now - s.barrier_since) * 1000));
        log().write("coop_barrier_release", JsonObj().add("waited_s", s.now - s.barrier_since).add("timeout", hold).add("battle", battle));
    }
    // Qui on attend et pourquoi (cinématique, dialogue, chargement, menu, reconnexion), mis à jour à chaque passage.
    if (s.barrier_paused) {
        const auto who = narrative_players();
        ui::set_status_text("wait", "waiting_players", who.empty() ? ui::text("waiting_players") : presence::wait_text(who, s.now, s.net.name_of));
    }
    waited_line_tick(narr);
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
    int by = -1;
    for (auto& [p, v] : s.remote_pause) if (v && p != s.me) by = p;
    const bool want_remote_pause = by >= 0 && game::in_battle() && !paused;
    if (want_remote_pause) ui::set_status("pause", "paused_by", {s.net.name_of ? s.net.name_of(static_cast<uint8_t>(by)) : std::to_string(by)});
    else ui::clear_status("pause");
    if (want_remote_pause != s.remote_paused_game) {
        if (want_remote_pause) s.pause_reasons.insert("remote_pause"); else s.pause_reasons.erase("remote_pause");
        s.remote_paused_game = want_remote_pause;
        bump(want_remote_pause ? "remote_pauses_applied" : "remote_pauses_released");
        log().write("coop_pause_remote", JsonObj().add("paused", want_remote_pause));
    }
    apply_pause();
}

bool sharing() { return s.host ? joined_players() > 0 : (!s.want_join || s.cj == S::ClientJoin::Joined); }

// --- Hash d'état d'exploration (phase 5) -----------------------------------------------------------
// L'hôte résume son état d'exploration (or, inventaire, objets du monde utilisés, ennemis vaincus, drapeaux de
// progression…) quand la partie est calme (point sûr, aucune interaction depuis quelques secondes). Chaque client
// compare au même point calme ; deux écarts de suite -> nouvel instantané au prochain point sûr (ResyncRequest).
struct WorldPart { const char* name; const wchar_t* field; };
constexpr WorldPart kWorldParts[] = {
    {"gold", L"Gold"}, {"inventory", L"Inventory"}, {"interacted", L"InteractedObjects"}, {"battled", L"BattledEnemies"},
    {"flags", L"NamedIDsStates"}, {"gpe", L"GPE_States"}, {"cinematics", L"InteractedCinematics"}, {"dialogs", L"InteractedDialogs"},
};

// Inventaire résumé : quantité totale par objet et niveau de chaque équipement (comme les ressources communes, D13),
// trié par nom. Avant : réflexion brute de GI.Inventory (~11 700 feuilles : ordre des piles, données statiques
// recopiées, pointeurs d'objets) ; l'hôte en avait 11 741 et le client 11 342 juste après avoir chargé l'instantané de
// l'hôte (2026-10-22) : écart permanent, donc nouvel instantané (rechargement de 5-6 s) toutes les 1 à 2 min.
reflect::Leaves inventory_leaves()
{
    std::map<std::string, int64_t> eco;
    game::economy_sample(eco);
    reflect::Leaves l;
    for (auto& [k, v] : eco)
        if (k != "gold" && v != 0) l.emplace_back(k, std::to_string(v));
    return l;
}

std::vector<proto::HashPart> world_parts(std::map<std::string, reflect::Leaves>* leaves = nullptr)
{
    std::vector<proto::HashPart> out;
    UObject* gi = game::gi();
    if (!gi) return out;
    prof::Scope prof_scope("world_parts");
    s.last_world_compute = s.now;
    for (const auto& p : kWorldParts) {
        reflect::Leaves l = std::string_view(p.name) == "inventory" ? inventory_leaves() : reflect::field(gi, p.field);
        out.push_back({p.name, reflect::hash(l)});
        if (leaves) (*leaves)[p.name] = std::move(l);
    }
    return out;
}

// Inventaire résumé en une ligne de journal ("objet=quantité,…"), pour comparer hôte et client quand il diffère.
std::string leaves_line(const reflect::Leaves& l)
{
    std::string out;
    for (auto& [k, v] : l) { if (!out.empty()) out += ","; out += k + "=" + v; }
    return out;
}

void touch() { s.last_event_at = s.now; }

std::vector<std::string> split_csv(const std::string& csv)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= csv.size()) {
        const size_t b = std::min(csv.find(',', a), csv.size());
        if (b > a) out.push_back(csv.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

// Diagnostic des écarts d'exploration : toutes les feuilles d'une partie (au plus 3000), une ligne par partie.
void log_world_leaves(const std::string& part, const reflect::Leaves& l, uint32_t seq)
{
    std::string arr = "[";
    for (size_t i = 0; i < l.size() && i < 3000; ++i) { if (arr.size() > 1) arr += ","; arr += JsonObj().add("p", l[i].first).add("v", l[i].second).str(); }
    log().write("coop_world_leaves", JsonObj().add("part", part).add("seq", seq).add("count", static_cast<int64_t>(l.size())).raw("leaves", arr + "]"));
}

bool quiet(double secs)
{
    return safe_point() && s.interactions.empty() && s.choices.empty() && !s.choice_pending && s.now - s.last_event_at >= secs;
}

// Client : copie coop après une empreinte égale à celle de l'hôte, pion arrêté (au plus 60 s après la vérification, et
// seulement sans événement depuis : sinon l'état n'est plus celui vérifié).
void copy_tick()
{
    if (s.host || s.copy_ok_at < 0) return;
    if (s.last_event_at > s.copy_ok_at || s.now - s.copy_ok_at > 60.0) { s.copy_ok_at = -1; return; }
    if (local_moving() || !quiet(3.0) || !prof::claim_heavy("coop_copy")) return;
    s.copy_ok_at = -1;
    prof::Scope p("coop_copy");
    coopsave::on_world_ok(s.copy_ok_hash, s.net.name_of ? s.net.name_of(0) : std::string());
}

void world_hash_tick()
{
    if (game::in_battle() || game::map_change_in_progress() || s.local_narr) touch();
    copy_tick();
    if (s.now < s.next_world_check) return;
    // Lecture de l'état : seule tâche lourde de son image, et pas pendant que le pion court (sauf toutes les 30 s au
    // plus) : l'hôte la faisait toutes les 5 s en courant (courir compte comme « calme »). false : réessai bientôt.
    auto may_read = [] {
        if (local_moving() && s.now - s.last_world_compute < 30.0) { s.next_world_check = s.now + 0.5; return false; }
        if (!prof::claim_heavy("world_hash")) { s.next_world_check = s.now; return false; }   // image suivante
        return true;
    };
    s.next_world_check = s.now + (s.host ? 5.0 : 1.0);   // hôte : lecture de tout l'état, pas à chaque seconde
    if (s.host) {
        if (!sharing() || !quiet(8.0)) return;
        if (!may_read()) return;
        std::map<std::string, reflect::Leaves> leaves;
        auto parts = world_parts(&leaves);
        if (parts.empty()) return;
        uint64_t all = 0;
        for (auto& p : parts) all = all * 31 + p.value;
        if (all == s.last_world_sent && s.now - s.last_world_sent_at < 60.0) return;
        uint64_t prev_inventory = 0;
        for (auto& p : s.last_world_parts) if (p.name == "inventory") prev_inventory = p.value;
        s.last_world_sent = all;
        s.last_world_sent_at = s.now;
        proto::WorldHash wh{++s.world_seq, game::current_level(), parts};
        s.net.publish(wh, true);
        bump("world_hashes_sent");
        std::string sizes = "{";
        for (auto& [k, l] : leaves) { if (sizes.size() > 1) sizes += ","; sizes += "\"" + k + "\":" + std::to_string(l.size()); }
        JsonObj o;
        o.raw("msg", proto::to_json(wh)).raw("leaves", sizes + "}");
        // Inventaire résumé quand il change (à comparer à celui d'un client en écart).
        for (auto& p : parts) if (p.name == "inventory" && p.value != prev_inventory) o.add("inventory", leaves_line(leaves["inventory"]));
        s.last_world_parts = parts;
        log().write("coop_world_hash", o);
        return;
    }
    // Client : comparer la dernière empreinte de l'hôte une fois calme ici aussi.
    if (!s.pending_world) return;
    if (s.cj != S::ClientJoin::Joined && s.want_join) { s.pending_world.reset(); return; }
    if (s.now - s.pending_world_at > 45.0) {
        bump("world_hash_skipped");
        log().write("coop_world_hash_skipped", JsonObj().add("seq", s.pending_world->seq).add("why", "never quiet"));
        s.pending_world.reset();
        return;
    }
    if (!quiet(3.0)) return;
    if (s.now - s.pending_world_at < 20.0) { if (!may_read()) return; }
    else (void)prof::claim_heavy("world_hash");   // empreinte reçue depuis 20 s : comparée même en courant
    const proto::WorldHash host = *s.pending_world;
    s.pending_world.reset();
    if (host.level != game::current_level()) { log().write("coop_world_hash_skipped", JsonObj().add("seq", host.seq).add("why", "level")); return; }
    std::map<std::string, reflect::Leaves> leaves;
    const auto mine = world_parts(&leaves);
    std::string differ;
    for (auto& hp : host.parts)
        for (auto& mp : mine)
            if (mp.name == hp.name && mp.value != hp.value) { if (!differ.empty()) differ += ","; differ += mp.name; }
    bump("world_hashes_compared");
    // Première comparaison après un instantané demandé (ResyncRequest) : une partie qui diffère encore juste après
    // avoir rechargé l'état exact de l'hôte, deux fois de suite, ne sera pas réparée par un autre instantané (écart
    // d'inventaire permanent après une nouvelle partie, 2026-10-22 : 5 rechargements en 10 min, chacun en plein jeu).
    // Elle reste journalisée, mais ne demande plus d'instantané.
    std::string ignored;
    if (s.resync_at >= 0 && s.cj_joined_at > s.resync_at) {
        const auto now_differ = split_csv(differ);
        for (const std::string& part : s.resync_parts) {
            if (std::find(now_differ.begin(), now_differ.end(), part) == now_differ.end()) { s.resync_failures.erase(part); continue; }
            const int n = ++s.resync_failures[part];
            log().write("coop_world_resync_failed", JsonObj().add("part", part).add("seq", host.seq).add("times", n));
            if (n >= 2 && s.world_ignored.insert(part).second) {
                bump("world_parts_unfixable");
                log().write("coop_world_part_unfixable", JsonObj().add("part", part).add("seq", host.seq));
            }
        }
        s.resync_at = -1;
        s.resync_parts.clear();
    }
    {
        std::string kept;
        for (const std::string& part : split_csv(differ)) {
            std::string& dst = s.world_ignored.count(part) ? ignored : kept;
            if (!dst.empty()) dst += ",";
            dst += part;
        }
        differ = kept;
    }
    if (differ.empty() && !ignored.empty()) {
        s.world_mismatch_streak = 0;
        bump("world_hash_diffs_ignored");
        log().write("coop_world_hash_ignored", JsonObj().add("seq", host.seq).add("parts", ignored));
        // Le reste est égal : la copie coop (D15) continue d'avancer (sinon un écart permanent la figerait).
    }
    if (differ.empty()) {
        s.world_mismatch_streak = 0;
        log().write("coop_world_hash_ok", JsonObj().add("seq", host.seq));
        uint64_t all = 0;
        for (auto& p : host.parts) all = all * 31 + p.value;
        // État vérifié : copie coop (D15). Écrire une save fige l'image : reportée si le pion court (copy_tick).
        // Pion fantôme (pilote sur Esquie) : position en l'air, sans monture ici : pas de copie coop.
        if (mount::ghosted()) log().write("coop_copy_skipped", JsonObj().add("why", "follower_ghost"));
        else { s.copy_ok_hash = all; s.copy_ok_at = s.now; }
        return;
    }
    ++s.world_mismatch_streak;
    bump("world_hash_diffs");
    std::string sizes = "{";
    for (auto& [k, l] : leaves) { if (sizes.size() > 1) sizes += ","; sizes += "\"" + k + "\":" + std::to_string(l.size()); }
    // Détail local des parties en écart (l'hôte journalise sa taille de chaque partie à l'envoi).
    std::string detail = "{";
    const auto parts = split_csv(differ);
    for (auto& [k, l] : leaves) {
        if (std::find(parts.begin(), parts.end(), k) == parts.end()) continue;
        if (detail.size() > 1) detail += ",";
        std::string first = "[";
        for (size_t i = 0; i < l.size() && i < 12; ++i) { if (first.size() > 1) first += ","; first += JsonObj().add("p", l[i].first).add("v", l[i].second).str(); }
        detail += "\"" + k + "\":" + first + "]";
    }
    JsonObj diff_log;
    diff_log.add("seq", host.seq).add("parts", differ).add("ignored", ignored).add("streak", s.world_mismatch_streak).raw("leaves", sizes + "}").raw("mine", detail + "}");
    if (differ.find("inventory") != std::string::npos) diff_log.add("inventory", leaves_line(leaves["inventory"]));   // à comparer au coop_world_hash de l'hôte
    log().write("coop_world_hash_diff", diff_log);
    if (s.world_mismatch_streak >= 2) {
        s.world_mismatch_streak = 0;
        bump("world_resyncs");
        ui::set_status("session", "resyncing");
        // Diagnostic : toutes les feuilles locales des parties en écart (l'hôte journalise les siennes à la réception de
        // la demande, coop_world_leaves) : comparer les deux journaux hors ligne.
        for (const std::string& part : parts)
            if (auto it = leaves.find(part); it != leaves.end()) log_world_leaves(part, it->second, host.seq);
        s.resync_parts = std::set<std::string>(parts.begin(), parts.end());
        s.resync_at = s.now;
        s.net.publish(proto::ResyncRequest{"world state differs: " + differ}, true);
        log().write("coop_world_resync", JsonObj().add("parts", differ));
    }
}

// Test de resynchro (-e33perturbworld=N) : une fois entré, le client fausse son or de N (l'écart doit être détecté,
// puis réparé par un nouvel instantané).
void perturb_world_tick()
{
    if (s.perturb_gold == 0 || s.cj != S::ClientJoin::Joined || s.now - s.cj_joined_at < 2.0) return;
    if (auto* gold = ue::value_ptr<int32_t>(game::gi(), STR("Gold"))) {
        *gold += s.perturb_gold;
        bump("world_perturbations");
        log().write("coop_world_perturb", JsonObj().add("gold_delta", s.perturb_gold).add("gold", *gold));
    }
    s.perturb_gold = 0;
}

} // namespace

// --- API --------------------------------------------------------------------------------------------

void init(bool host, bool want_join, const Net& net)
{
    {
        if (const std::wstring v = cmdline::arg(L"e33perturbworld"); !v.empty()) s.perturb_gold = _wtoi(v.c_str());
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
                          "interaction_errors", "choice_errors", "choice_timeouts", "map_change_errors", "local_actions_on_follower"})
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
        // Le client recharge un instantané : il ne participe plus jusqu'à son JoinReady.
        s.peers[from] = PeerJoin{JoinState::Pending, s.now, false};
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
        if (mt.open && mt.pilot_at < 0 && p->level == mt.level && p->pilot == s.pilot) mt.pilot_at = s.now;   // pilote arrivé
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
                                  std::holds_alternative<proto::PilotState>(m)))
        return true;
    if (auto* wi = std::get_if<proto::WorldInteract>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
        s.interactions.push_back(*wi);
        touch();
        return true;
    }
    if (auto* dc = std::get_if<proto::DialogueChoice>(&m)) {
        if (s.host) { if (from != s.pilot) return true; s.net.relay(m, from, true); }
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
        if (!ns->active && s.remote_narr[ns->player]) sc.remote_end_at[ns->player] = s.now;
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
    replay_tick();
    scene_tick();
    { prof::Scope p("explore_poll"); poll_tick(); }
    world_hash_tick();
    if (!s.host) perturb_world_tick();
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
            bump("interactions_published"); touch();
            sc.interaction = path; sc.interaction_at = s.now;   // cause possible d'une cinématique (SceneStart)
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
        bump("choices_published"); touch();
        log().write("coop_choice", JsonObj().add("index", index));
    });
    // Question de dialogue : CurrentChoices reste vide pendant la question (vu le 2026-10-20), on suit StartChoice.
    bus.post(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:StartChoice", [](HookCtx&, UFunction*) {
        s.choice_pending = true;
        log().write("coop_choice_asked", JsonObj());
    });
    bus.post(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceStarted", [](HookCtx&, UFunction*) {
        s.sequence_playing = true;
        log().write("coop_sequence_started", JsonObj());
    });
    bus.post(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceFinished", [](HookCtx&, UFunction*) {
        s.sequence_playing = false;
        if (s.intro) ++s.intro_sequences_done;
        log().write("coop_sequence_finished", JsonObj());
    });
    // Scènes synchronisées : causes vues ici (zone « au passage », game action), début et passage d'une cinématique.
    bus.pre(L"/Game/LevelTools/MovementZone/BP_GameActionTriggerZone.BP_GameActionTriggerZone_C:TryExecuteGameActionOnOverlap", [](HookCtx& ctx, UFunction*) {
        sc.zone_in_call = ctx.Context;
    });
    bus.post(L"/Game/LevelTools/MovementZone/BP_GameActionTriggerZone.BP_GameActionTriggerZone_C:TryExecuteGameActionOnOverlap", [](HookCtx&, UFunction*) {
        sc.zone_in_call = nullptr;
    });
    bus.pre(L"/Game/Gameplay/GameActionsSystem/BP_GameActionExecutorComponent.BP_GameActionExecutorComponent_C:ExecuteGameActionWrapperFromTriggerZone", [](HookCtx&, UFunction*) {
        if (!sc.zone_in_call) return;
        UObject* action = zone_overlap_action(sc.zone_in_call);
        sc.zone = object_path(sc.zone_in_call);
        sc.zone_action = action ? object_path(action) : std::string();
        sc.zone_at = s.now;
        log().write("coop_zone_executed", JsonObj().add("zone", sc.zone).add("action", sc.zone_action).add("replaying", s.replaying));
    });
    bus.pre(L"/Game/Gameplay/GameActionsSystem/BP_GameActionExecutorComponent.BP_GameActionExecutorComponent_C:ExecuteGameAction", [](HookCtx& ctx, UFunction* f) {
        UObject* a = ue::read_param_object(ctx, f, STR("GameAction"));
        if (!a) return;
        sc.action = object_path(a);
        sc.action_at = s.now;
    });
    bus.pre(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:TriggerCinematic", [](HookCtx& ctx, UFunction* f) {
        std::string id, seq;
        read_cinematic_params(ctx, f, id, seq);
        on_local_cinematic(id, seq);
    });
    // Passage : le jeu ignore parfois SkipCinematic (trop tôt, scène impossible à passer) ; on ne publie qu'un passage
    // effectif, vu à la fin de la séquence peu après l'appel (OnSequenceFinished, avant la transition de sortie).
    bus.pre(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:SkipCinematic", [](HookCtx&, UFunction*) {
        sc.skip_called_at = s.now;
        log().write("coop_skip_called", JsonObj().add("playing", game::playing_cinematic()).add("remote", sc.skip_at >= 0));
    });
    bus.post(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceFinished", [](HookCtx&, UFunction*) {
        if (sc.skip_called_at < 0 || s.now - sc.skip_called_at > 5.0) return;
        sc.skip_called_at = -1;
        if (!s.active || s.ended || !is_pilot() || !sharing() || sc.skip_at >= 0) return;
        s.net.publish(proto::SceneSkip{s.me, ++sc.seq, sc.local_scene}, true);
        bump("scene_skips_published");
        log().write("coop_scene_skip", JsonObj().add("scene", sc.local_scene));
    });
    bus.post(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:EndDialogue", [](HookCtx&, UFunction*) { s.choice_pending = false; });
    // Nouvelle partie ensemble : l'hôte lance une nouvelle partie -> les clients qui attendent au menu suivent.
    bus.pre(L"/Game/Gameplay/Save/BP_SaveManager.BP_SaveManager_C:StartNewGame", [](HookCtx&, UFunction*) {
        if (!s.active || s.ended) return;
        start_intro();
        if (!s.host) return;   // client : c'est la nôtre (follow_new_game)
        int n = 0;
        for (auto& [player, pj] : s.peers) {
            if (pj.state != JoinState::Pending && pj.state != JoinState::Waiting) continue;
            s.net.send_to(player, proto::JoinWait{"new_game"});
            pj.state = JoinState::Waiting;
            pj.wait_sent = true;
            ++n;
        }
        bump("new_games_announced");
        log().write("coop_new_game", JsonObj().add("players_following", n));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:OnInitialCinematicCompleted", [](HookCtx&, UFunction*) {
        if (!s.intro) return;
        s.intro = false;
        bump("intros_finished");
        log().write("coop_intro_finished", JsonObj().add("after_s", s.now - s.intro_since));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:ChangeMap", [](HookCtx&, UFunction*) {
        if (s.replaying || !s.active || s.ended || !is_pilot() || !sharing()) return;   // pas pendant le chargement de l'instantané
        std::string level, tag;
        if (!game::map_destination(level, tag) || is_menu_level(level)) return;   // retour au menu : pas une transition
        s.net.publish(proto::MapChange{++s.seq, level, tag}, true);
        bump("map_changes_published"); touch();
        log().write("coop_map_change", JsonObj().add("level", level).add("spawn", tag));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle", [](HookCtx& ctx, UFunction* f) {
        s.last_enemy = ue::short_name(ctx.Context);
        s.last_engage = static_cast<int>(ue::read_param_int(ctx, f, STR("EngagementType")).value_or(0));
        s.last_enemy_at = s.now;
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
bool sequence_playing() { return s.sequence_playing; }
double last_event_age() { return s.now - s.last_event_at; }
bool remote_paused() { return s.remote_paused_game; }
double follow_error() { return s.follow_error; }
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
