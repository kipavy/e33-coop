#include "explore.hpp"

#include "builds.hpp"
#include "coopsave.hpp"
#include "mount.hpp"
#include "presence.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string_view>
#include <vector>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/prof.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/reflect.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"

#include <Unreal/Hooks.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
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

// resync : client entré qui a demandé un nouvel instantané ; il reste dans la partie (combats compris) jusqu'à l'envoi.
struct PeerJoin { JoinState state = JoinState::None; double since = 0; bool wait_sent = false; bool resync = false; };

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
    double cj_calm_since = -1;                    // client : début du calme (ni dialogue ni cinématique) avant de charger
    int cj_load_tries = 0;                        // client : chargements demandés pour cet instantané
    double cj_next_diag = 0;                      // client : prochain journal de l'attente du monde
    int join_stuck_test = 0;                      // test : -e33joinstuck=N, monde vu « pas prêt » pendant les N premiers chargements
    int cj_loads_total = 0;                       // client : chargements demandés dans la session (tous instantanés)
    double next_char_fix = 0;                     // suiveur : prochain essai de copie du personnage affiché
    // Pilote
    uint8_t pilot = 0;
    uint32_t pilot_seq = 0;
    double next_pilot_send = 0;
    std::optional<proto::PilotState> last_pilot;  // suiveur
    double last_pilot_at = 0;
    double follow_error = -1;
    // Sprint du suiveur (2026-10-22 : le pilote sprinte à ~720 u/s, le suiveur restait en course à ~350 u/s et était
    // téléporté toutes les ~2,2 s à 800 u d'écart : les « freeze » vus quand l'hôte court). Suivi : FollowCtl (D23).
    bool follow_sprint = false;                   // sprint demandé au pion suiveur
    bool sprint_touched = false;                  // le mod a déjà changé l'allure de ce pion
    int sprint_method = 0;                        // 2 SetDesiredGait (0 : échec)
    int gait_before = -1;                         // allure voulue avant le sprint (rendue ensuite)
    double sprint_since = -1;
    bool sprint_logged = false;
    // Vitesse du pion local (pilote ou suiveur), pour reporter les tâches lourdes pendant le mouvement
    double local_speed = 0;
    double local_speed_at = -1;
    double last_motion_at = -100;                 // pilote ou pion local en mouvement (> 50 u/s) pour la dernière fois
    // Client : resynchro (rechargement de 5-6 s) attendue jusqu'à l'arrêt du mouvement (essai du 2026-10-22 : deux
    // rechargements en plein sprint de l'hôte, vus comme des « freeze »).
    std::string resync_deferred;                  // parties en écart (vide : aucune demande en attente)
    double resync_deferred_since = -1;
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
    int choice_hover_sent = -1, choice_hover_want = -1, choice_hover_shown = -1;   // choix survolé (pilote / voulu / affiché ici)
    int choice_diag = 0;
    bool merchant_open = false, merchant_locked = false, merchant_close = false;   // fenêtre du marchand (pilote : ouverte ; suiveur : miroir posé, fermeture demandée)
    int merchant_sent = -1, merchant_want = -1, merchant_shown = -1;
    int merchant_diag = 0;
    int merchant_buy = -1;            // suiveur : achat du pilote à rejouer (index de l'article)
    double merchant_seen_at = -1;     // suiveur : premier message du marchand du pilote (sélection), pour ouvrir la fenêtre
    bool merchant_open_tried = false; // suiveur : ouverture de secours déjà tentée pour cette visite
    bool sequence_playing = false;                // OnSequenceStarted reçu, pas encore OnSequenceFinished
    // Narration (barrière) et pause
    double next_poll = 0;
    bool local_narr = false;
    proto::NarrativeKind local_kind = proto::NarrativeKind::Dialogue;
    uint32_t narr_seq = 0;
    std::map<uint8_t, bool> remote_narr;
    std::map<uint8_t, double> remote_narr_since;  // début de la narration en cours de chaque autre joueur
    std::map<uint8_t, double> remote_narr_end;    // fin de la dernière narration de chaque autre joueur
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

// Personnage affiché en exploration, par pion (hook de LoadAndActivateCharacter) : gardé hors session (le pion
// charge son personnage avant l'ouverture de la partie coop).
std::map<UObject*, std::string> g_char_of;

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
    double sprint_s = 0, sprint_gait_ok_s = 0, starved_s = 0, last_t = -1;
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

void bump(const char* c, int n = 1) { if (s.net.bump) s.net.bump(c, n); }
void bump_max(const char* name, int& cur, int v);   // compteur « maximum » (plus bas)
void mt_follow(double err, bool teleported, int started, int stopped);   // mesures d'arrivée (plus bas)
// Corrections des changements de niveau (2026-10-23) : -e33zonefix=0 les coupe (mesure « avant » des scénarios p9).
bool zone_fix() { static const bool v = cmdline::arg(L"e33zonefix") != L"0"; return v; }

// Les événements d'exploration ne concernent que les joueurs entrés dans la partie (hôte : au moins un client
// entré ; client : instantané chargé, ou session sans « rejoindre » des tests de combat).
bool sharing();
void touch();   // événement d'exploration : la partie n'est pas « calme » (hash d'état)
void send_world_resync(std::string differ);

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
        // Instantané jamais envoyé pendant la narration du client ni dans les 3 s qui suivent : le 2026-10-23 (essai à
        // deux PC), deux chargements demandés dans la seconde de la fin d'un dialogue sont restés sur l'écran noir.
        auto end = s.remote_narr_end.find(player);
        // Ni pendant sa pause (chargement bloqué menu pause ouvert, 2026-10-24).
        const bool client_calm = !s.remote_narr[player] && !s.remote_pause[player] && (end == s.remote_narr_end.end() || s.now - end->second >= 3.0);
        if (pj.state == JoinState::Joined && pj.resync) {
            // Client entré qui veut une resynchro : il joue avec nous jusqu'à l'envoi (aucun combat joué seul chez lui).
            if (safe_point() && client_calm) { pj.resync = false; send_snapshot(player); }
            continue;
        }
        if (pj.state != JoinState::Pending && pj.state != JoinState::Waiting) continue;
        // Client encore dans l'intro de sa nouvelle partie : on attend sa fin (l'instantané la couperait).
        if (safe_point() && client_calm) { send_snapshot(player); continue; }
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
        const bool retry = s.cj_load_tries > 0;
        // Un nouvel essai passe outre un « chargement en cours » resté levé (c'est peut-être lui qui bloque).
        if (!game::gi() || (!retry && game::map_change_in_progress())) return;
        if (!game::main_menu() && !game::controller()) return;
        // Pas de chargement pendant un dialogue / une cinématique ni juste après : le 2026-10-23 (essai à deux PC),
        // un chargement demandé dans la même seconde que la fin d'un dialogue est resté sur l'écran noir.
        // Calme de 2 s exigé, 20 s d'attente au plus après l'instantané.
        if (!game::main_menu() && (game::in_dialogue() || game::playing_cinematic() || game::pause_menu_visible())) s.cj_calm_since = -1;
        else if (s.cj_calm_since < 0) s.cj_calm_since = s.now;
        if (s.now - s.cj_since < 20.0 && (s.cj_calm_since < 0 || s.now - s.cj_calm_since < 2.0)) return;
        if (!game::main_menu() && game::pause_menu_visible()) { s.cj_since = s.now; return; }   // pause : on attend sa fermeture, sans limite
        std::string err;
        if (!game::load_save_slot(kCoopSlot, err)) {
            bump("join_errors");
            log().write("coop_join_error", JsonObj().add("what", "load: " + err));
            s.cj = S::ClientJoin::None;
            return;
        }
        ++s.cj_load_tries;
        ++s.cj_loads_total;
        if (retry) bump("join_load_retries");
        s.cj = S::ClientJoin::WaitWorld;
        s.cj_since = s.now;
        s.cj_next_diag = s.now + 10.0;
        s.world_stable = 0;
        log().write("coop_join_loading", JsonObj().add("slot", ue::to_utf8(kCoopSlot)).add("try", s.cj_load_tries));
        return;
    }
    if (s.cj == S::ClientJoin::WaitWorld) {
        if (s.now - s.cj_since < 5) return;   // le chargement ne commence pas tout de suite
        const bool ready = world_ready() && s.cj_loads_total > s.join_stuck_test;
        s.world_stable = ready ? s.world_stable + kPollPeriod : 0;
        if (!ready && s.now >= s.cj_next_diag) {
            // Ce qui manque (journal toutes les 10 s) : aucune attente muette.
            s.cj_next_diag = s.now + 10.0;
            log().write("coop_join_wait_world", JsonObj().add("s", s.now - s.cj_since).add("try", s.cj_load_tries)
                                                    .add("controller", game::controller() != nullptr).add("loading", game::map_change_in_progress())
                                                    .add("pawn", game::explore_pawn() != nullptr).add("battle", game::in_battle())
                                                    .add("dialogue", game::in_dialogue()).add("cinematic", game::playing_cinematic())
                                                    .add("main_menu", game::main_menu()));
        }
        if (!ready && s.now - s.cj_since > 45.0) {
            if (s.cj_load_tries < 2) {
                // Monde jamais prêt 45 s après le chargement : on recharge l'instantané (déjà sur le disque).
                log().write("coop_join_load_stuck", JsonObj().add("try", s.cj_load_tries).add("action", "reload"));
                s.cj = S::ClientJoin::Loading;
                s.cj_since = s.now;
                s.cj_calm_since = -1;
                return;
            }
            // Toujours bloqué : on redemande un instantané à l'hôte (nouvel envoi, nouveaux essais).
            bump("join_load_stuck");
            log().write("coop_join_load_stuck", JsonObj().add("try", s.cj_load_tries).add("action", "resync"));
            s.cj = S::ClientJoin::None;
            s.last_chunk_at = s.now;
            s.net.publish(proto::ResyncRequest{"join load stuck"}, true);
            return;
        }
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
    s.cj_calm_since = -1;
    s.cj_load_tries = 0;
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
    m.t_ms = 1 + static_cast<uint32_t>(std::fmod(s.now * 1000.0, 4.0e9));   // horloge du pilote (tampon du suiveur)
    if (auto it = g_char_of.find(pawn); it != g_char_of.end()) m.chr = it->second;   // personnage affiché
    s.net.publish(m, false);
}

// --- Suivi fluide du pilote (2026-10-23, « freeze » vus par le client quand l'hôte court, D23) -------------
// Avant : cible extrapolée depuis la dernière position reçue + lissage exponentiel, échelle d'entrée approximative,
// sprint par SprintAction (sans effet sur le pion suiveur : repli SetDesiredGait après 0,6 s, retard de ~200 u jamais
// rattrapé puisque le suiveur n'allait pas plus vite que le pilote), téléportation fixe à 800 u.
// Maintenant :
//  - tampon d'interpolation : chaque position est placée à son instant d'envoi (PilotState.t_ms, horloge du pilote ;
//    à défaut l'instant de réception). Le pion vise la trajectoire du pilote telle qu'elle était kRenderDelay avant la
//    position la plus récente arrivée « à l'heure » (transit minimal) : courbe d'Hermite entre deux positions, sans la
//    gigue du réseau ; prolongée au plus kExtrapolateMax si une position tarde (pointe réseau) ;
//  - commande en vitesse : vitesse du pilote à cet instant + rattrapage proportionnel à l'écart. En course : échelle
//    d'entrée = vitesse voulue / MaxWalkSpeed. En sprint : allure voulue Sprint (SetDesiredGait, directement), entrée
//    pleine (l'ALS n'accorde le sprint qu'au-delà de 0,9) et MaxWalkSpeed = vitesse voulue (jusqu'à 1,2 x le sprint
//    pour rattraper un retard), rendue à l'ALS à la sortie du sprint ;
//  - téléportation seulement si elle se justifie : trou de données ou saut du pilote (« gap »), écart > 800 u qui ne
//    se résorbe pas en 1 s ou > 1600 u, pion bloqué 2 s, écart vertical > 400 u pendant 0,5 s ;
//  - mesures de fluidité en compteurs (follow_metrics), vérifiées par les scénarios p8_*.
constexpr double kRenderDelay = 0.10;        // retard de rendu au-delà du transit minimal (horloge du pilote)
constexpr double kRenderDelayNoClock = 0.12; // sans horloge (ancien enregistrement) : instants de réception
constexpr double kExtrapolateMax = 0.5;      // position en retard (pointe réseau) : trajectoire prolongée au plus d'autant
constexpr double kFollowGain = 2.0;          // rattrapage : vitesse ajoutée par unité d'écart (1/s)
constexpr double kCatchUpMax = 1.2;          // vitesse maximale = 1,2 x sprint pendant un rattrapage

struct PilotSample { double t = 0; double pos[3]{}; double vel[3]{}; double yaw = 0; };

struct FollowCtl {
    std::deque<PilotSample> buf;            // positions du pilote, triées par instant d'envoi
    uint8_t pilot = 0xFF;
    std::string level;
    bool clock = false;                     // instants = horloge du pilote (sinon réception)
    double off = 0;                         // réception - envoi, minimal (s)
    int off_high = 0;
    double last_arrival = -1;
    double discontinuity_at = -100;         // trou de données / saut du pilote : téléportation « gap » permise
    double teleport_at = -100;
    // Conduite
    bool moving = false;
    bool sprint = false;
    double sprint_low_since = -1;
    double sprint_set = 0;                  // vitesse de sprint écrite (lissée)
    double ctl_last = -1;
    double next_gait_assert = 0;
    float base_max = 0;                     // MaxWalkSpeed avant la première écriture du mod
    bool max_written = false;
    game::GaitSpeeds speeds{};
    bool speeds_ok = false;
    bool speeds_logged = false;
    double speeds_at = -100;
    double far_since = -1, far_err0 = 0;
    double dz_since = -1;
    double stuck_since = -1;
    double follow_since = -1;               // pion conduit depuis
    // Mesures
    double rv_move_since = -1;              // vitesse rendue du pilote > 150 depuis
    double rv_fast_since = -1;              // > 300 depuis
    double stall_since = -1; bool stall_counted = false;
    double frozen_since = -1;
    double dip_since = -1; bool dip_counted = false;
    double rest_since = -1;
    int cls = -2; double cls_since = 0;     // classe d'allure du pilote rendu (marche, course, sprint) et depuis quand
    double last_drop_at = -10;
    double last_event_log = -10;
    std::deque<std::array<double, 3>> hist; // (instant, vitesse du pion, vitesse rendue du pilote) sur 0,3 s
} fc;

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

// Position et vitesse du pilote à l'instant r (horloge du tampon). starved : r au-delà de la dernière position ;
// stale : au-delà de la prolongation permise (pilote considéré arrêté là où on l'a prolongé).
struct Rendered { double pos[3]{}; double vel[3]{}; double yaw = 0; bool starved = false; bool stale = false; };

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
        // Tir du pilote sur un objet du monde : même coup ici (FreeAimHit avec notre pion, au centre de l'objet).
        if (wi.actor.rfind("freeaim:", 0) == 0) {
            const std::string path = wi.actor.substr(8);
            UObject* target = ue::find_object(ue::from_utf8(path).c_str());
            bool ok = false;
            if (target) {
                ue::Call c(target, STR("FreeAimHit"));
                double loc[3]{};
                game::location(target, loc);
                if (c && c.param_size(STR("HitLocation")) == sizeof(loc)) {
                    s.replaying = true;
                    ok = c.set_object(STR("PlayerCharacterWorld"), game::world_pawn()).set_raw(STR("HitLocation"), loc).set_bool(STR("IsDisabled"), false).invoke();
                    s.replaying = false;
                }
            }
            bump(ok ? "freeaim_hits_replayed" : "freeaim_hit_errors"); touch();
            log().write("coop_freeaim_world_replayed", JsonObj().add("actor", path).add("found", target != nullptr).add("ok", ok));
            return;
        }
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
    double ticked_s = 0;             // temps de jeu réel après l'arrivée (images < 0,5 s : sans les gels de chargement)
};
MapTrans mt;
// Horloge murale (s) : un chargement bloque le thread de jeu, les mesures de transition se font en temps réel.
double wnow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool load_in = false;                 // chargement de niveau en cours ici (IsMapChangeInProgress)
double load_fall_at = -1;              // fin du dernier chargement (IsMapChangeInProgress retombé)
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

// Choix de dialogue survolé par le pilote (demande de Killian, 2026-10-24) : le suiveur voit la même option en
// surbrillance avant le choix. Options = WBP_Dialogue_ChoiceOption_C visibles, rangées par leur place dans leur parent.
std::vector<UObject*> choice_options()
{
    std::vector<UObject*> all, out;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("WBP_Dialogue_ChoiceOption_C"), all);
    std::vector<std::pair<int, UObject*>> v;
    for (UObject* o : all) {
        if (!o) continue;
        ue::Call vis(o, STR("IsVisible"));
        if (!vis || !vis.invoke() || !vis.out_bool(STR("ReturnValue"))) continue;
        ue::Call par(o, STR("GetParent"));
        UObject* parent = par && par.invoke() ? par.out_object(STR("ReturnValue")) : nullptr;
        int idx = static_cast<int>(v.size()) + 100;
        if (parent) { ue::Call ci(parent, STR("GetChildIndex")); if (ci) { ci.set_object(STR("Content"), o); if (ci.invoke()) idx = static_cast<int>(ci.out_int(STR("ReturnValue"), idx)); } }
        v.emplace_back(idx, o);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [i, o] : v) out.push_back(o);
    return out;
}

bool widget_flag(UObject* w, const wchar_t* fn)
{
    ue::Call c(w, fn);
    return c && c.invoke() && c.out_bool(STR("ReturnValue"));
}


// Marchand (essai à deux PC du 2026-10-24, demande de Killian) : le choix « Acheter » du pilote est rejoué chez le
// suiveur, qui avait alors sa propre fenêtre du marchand, entrées libres (achats séparés, combat du marchand lancé chez
// lui). Désormais la fenêtre du suiveur est un miroir : ses boutons (acheter, combattre, partir) sont désactivés, il voit
// l'article sélectionné par le pilote (DialogueChoice.hover 2 = index de l'article), et elle se ferme quand le pilote
// ferme la sienne (hover 3). Les achats du pilote lui arrivent par la synchro de l'inventaire et de l'or (D13).
UObject* merchant_window()
{
    UObject* win = ue::get_object(ue::get_object(game::controller(), STR("BP_DialogueSystemComponent")), STR("MerchantWindow"));
    if (!win) return nullptr;
    ue::Call c(win, STR("IsActivated"));
    return c && c.invoke() && c.out_bool(STR("ReturnValue")) ? win : nullptr;
}

std::vector<UObject*> merchant_entries(UObject* win)
{
    ue::Call c(ue::get_object(win, STR("DynamicEntryBox_Items")), STR("GetAllEntries"));
    if (!c || !c.invoke()) return {};
    return c.out_object_array(STR("ReturnValue"));
}

void merchant_tick()
{
    static double next = 0;
    if (s.now < next || !sharing()) return;
    next = s.now + 0.1;
    UObject* win = merchant_window();
    if (is_pilot()) {
        if (!win) {
            if (s.merchant_open) { s.merchant_open = false; s.net.publish(proto::DialogueChoice{++s.seq, 0, 3}, true); log().write("coop_merchant_closed", JsonObj()); }
            return;
        }
        if (!s.merchant_open) { s.merchant_open = true; s.merchant_sent = -1; s.merchant_diag = 0; log().write("coop_merchant_opened", JsonObj()); }
        UObject* focused = ue::get_object(win, STR("FocusedMerchantItem"));
        const auto entries = merchant_entries(win);
        int idx = -1;
        for (size_t i = 0; i < entries.size(); ++i) if (entries[i] == focused) idx = static_cast<int>(i);
        // FocusedMerchantItem ne désigne pas forcément l'entrée (rien envoyé en 0.6.23) : focus ou survol de l'entrée.
        for (size_t i = 0; i < entries.size() && idx < 0; ++i)
            if (widget_flag(entries[i], STR("HasAnyUserFocus")) || widget_flag(entries[i], STR("HasFocusedDescendants")) || widget_flag(entries[i], STR("IsHovered"))) idx = static_cast<int>(i);
        static double diag_next = 0;
        if (s.merchant_diag < 8 && s.now >= diag_next && ++s.merchant_diag)
            log().write("coop_merchant_focus_diag", JsonObj().add("entries", static_cast<int64_t>(entries.size())).add("focused", focused ? ue::short_name(focused->GetClassPrivate()) : std::string("null"))
                                                       .add("entry_class", entries.empty() ? std::string() : ue::short_name(entries[0]->GetClassPrivate())).add("idx", idx));
        if (s.merchant_diag <= 8 && s.now >= diag_next) diag_next = s.now + 2.0;
        if (idx >= 0 && idx != s.merchant_sent && idx <= 255) {
            s.merchant_sent = idx;
            s.net.publish(proto::DialogueChoice{++s.seq, idx, 2}, true);
            bump("merchant_focus_published");
        }
        return;
    }
    if (!win) {
        s.merchant_locked = false; s.merchant_shown = -1;
        // Fenêtre absente alors que le pilote est chez le marchand depuis 6 s (ex. dialogue relancé après le combat du
        // marchand, pas rejoué ici, 2026-10-24) : ouverte directement sur le marchand le plus proche.
        if (s.merchant_seen_at >= 0 && !s.merchant_open_tried && s.now - s.merchant_seen_at > 6.0 && !s.choice_pending && s.choices.empty()) {
            s.merchant_open_tried = true;
            std::vector<UObject*> comps;
            RC::Unreal::UObjectGlobals::FindAllOf(STR("BP_MerchantComponent_C"), comps);
            double me[3]{}, best = 1e30;
            UObject* merchant = nullptr;
            const bool have_me = game::location(game::world_pawn(), me);
            for (UObject* c : comps) {
                UObject* owner = ue::component_owner(c);
                double p[3]{};
                if (!owner || !game::location(owner, p)) continue;
                const double d = have_me ? (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]) : 0;
                if (d < best) { best = d; merchant = owner; }
            }
            bool ok = false;
            if (merchant) {
                ue::Call c(ue::get_object(game::controller(), STR("BP_DialogueSystemComponent")), STR("OpenMerchantWindow"));
                if (c && c.set_object(STR("Merchant"), merchant).invoke()) ok = c.out_bool(STR("IsOpened"));
            }
            bump(ok ? "merchant_windows_opened" : "merchant_open_errors");
            log().write("coop_follower_merchant_open", JsonObj().add("ok", ok).add("merchants", static_cast<int64_t>(comps.size()))
                                                         .add("merchant", merchant ? ue::short_name(merchant) : std::string("null")).add("dist", std::sqrt(best)));
        }
        return;
    }
    if (!s.merchant_locked) {
        s.merchant_locked = true;
        s.merchant_open_tried = true;   // fenêtre ouverte par le dialogue : pas d'ouverture de secours
        s.merchant_close = false;
        for (const wchar_t* b : {STR("BuyItemButton"), STR("FightMerchantButton"), STR("LeaveButton")}) {
            ue::Call c(ue::get_object(win, b), STR("SetIsInteractionEnabled"));
            if (c) c.set_bool(STR("bInIsInteractionEnabled"), false).invoke();
        }
        bump("merchant_mirrors");
        log().write("coop_follower_merchant_mirror", JsonObj().add("entries", static_cast<int64_t>(merchant_entries(win).size())));
        ui::toast("pilot_at_merchant");
    }
    if (s.merchant_close) {
        s.merchant_close = false;
        UObject* leave = ue::get_object(win, STR("LeaveButton"));
        bool ok = false;
        { ue::Call c(win, STR("BndEvt__WBP_Merchant_Panel_LeaveButton_K2Node_ComponentBoundEvent_5_CommonButtonBaseClicked__DelegateSignature"));
          if (c && leave) ok = c.set_object(STR("Button"), leave).invoke(); }
        if (!ok) { ue::Call d(win, STR("DeactivateWidget")); ok = d && d.invoke(); }
        bump(ok ? "merchant_windows_closed" : "merchant_close_errors");
        log().write("coop_follower_merchant_closed", JsonObj().add("ok", ok));
        return;
    }
    const auto entries = merchant_entries(win);
    // Boutons du suiveur désactivés à chaque passage : le jeu les réactive à chaque changement d'article (0.6.28 :
    // TOTOFFF a pu acheter lui-même pendant que Killian menait).
    for (const wchar_t* b : {STR("BuyItemButton"), STR("FightMerchantButton"), STR("LeaveButton")}) {
        ue::Call c(ue::get_object(win, b), STR("SetIsInteractionEnabled"));
        if (c) c.set_bool(STR("bInIsInteractionEnabled"), false).invoke();
    }
    auto read_gold = [] { auto* g = ue::value_ptr<int32_t>(game::gi(), STR("Gold")); return g ? static_cast<int64_t>(*g) : int64_t(-1); };
    static int64_t last_gold = -1;   // or vu au passage précédent (achat fait par le suiveur lui-même)
    // Achat du pilote rejoué ici par le bouton Acheter du panneau (même chemin que chez le pilote : achat, or et stock
    // affichés à jour), l'article du pilote étant sélectionné. Repli : TryBuyItem direct (0.6.28 : or changé, mais
    // fenêtre pas redessinée). Or et inventaire communs (D13).
    if (s.merchant_buy >= 0 && s.merchant_buy < static_cast<int>(entries.size())) {
        UObject* row = entries[static_cast<size_t>(s.merchant_buy)];
        s.merchant_buy = -1;
        if (ue::get_object(win, STR("FocusedMerchantItem")) != row) { ue::Call p(win, STR("OnMerchantItemFocused")); if (p) p.set_object(STR("Widget"), row).invoke(); }
        const int64_t gold_before = read_gold();
        std::string how = "button";
        bool ok = false;
        s.replaying = true;
        if (ue::get_object(win, STR("FocusedMerchantItem")) == row) {
            ue::Call c(win, STR("BndEvt__WBP_Merchant_Panel_BuyItemButton_K2Node_ComponentBoundEvent_3_CommonButtonBaseClicked__DelegateSignature"));
            UObject* buy = ue::get_object(win, STR("BuyItemButton"));
            ok = c && buy && c.set_object(STR("Button"), buy).invoke() && read_gold() != gold_before;
        }
        if (!ok) {
            how = "direct";
            using namespace RC::Unreal;
            auto* sp = CastField<FStructProperty>(ue::prop(row, STR("MerchantItem")));
            void* item = sp ? sp->ContainerPtrToValuePtr<void>(row) : nullptr;
            FProperty* name_p = sp ? ue::struct_member(sp->GetStruct(), STR("MerchantItemRowName")) : nullptr;
            ue::Call c(ue::get_object(win, STR("MerchantComponent")), STR("TryBuyItem"));
            if (c && name_p && c.param_size(STR("MerchantItemRowName")) == static_cast<size_t>(name_p->GetSize()))
                ok = c.set_raw(STR("MerchantItemRowName"), name_p->ContainerPtrToValuePtr<void>(item)).invoke();
            for (UObject* r : entries) { ue::Call u(r, STR("UpdateContent")); if (u) u.invoke(); }
        }
        s.replaying = false;
        // Or affiché : valeur réelle (le panneau peut garder l'ancienne).
        if (UObject* ga = ue::get_object(win, STR("WBP_GoldAmount"))) { ue::Call g(ga, STR("SetCurrentValue")); if (g) g.set_int(STR("Value"), read_gold()).invoke(); }
        bump(ok ? "merchant_buys_replayed" : "merchant_buy_errors");
        log().write("coop_follower_merchant_buy", JsonObj().add("ok", ok).add("how", how).add("gold_before", gold_before).add("gold_after", read_gold()));
        s.merchant_shown = -1;   // lignes peut-être reconstruites : sélection reposée
        last_gold = read_gold();
        return;
    }
    // Achat fait par le suiveur lui-même malgré tout (or qui baisse sans achat du pilote) : signalé.
    if (const int64_t g = read_gold(); last_gold >= 0 && g < last_gold && !s.replaying) {
        bump("merchant_follower_spent");
        log().write("coop_follower_merchant_spent", JsonObj().add("gold_before", last_gold).add("gold_after", g));
    }
    last_gold = read_gold();
    // Sélection du pilote : seule sa ligne accepte le focus (le suiveur ne peut plus naviguer), focus sur son bouton
    // (surbrillance), panneau mis à jour (nom, stock, détails) s'il ne suit pas.
    if (s.merchant_want >= 0 && s.merchant_want < static_cast<int>(entries.size())) {
        UObject* row = entries[static_cast<size_t>(s.merchant_want)];
        UObject* focused = ue::get_object(win, STR("FocusedMerchantItem"));
        if (s.merchant_want != s.merchant_shown || (focused && focused != row)) {
            for (size_t i = 0; i < entries.size(); ++i) {
                ue::Call fc(ue::get_object(entries[i], STR("WBP_BaseButton")), STR("SetIsFocusable"));
                if (fc) fc.set_bool(STR("bInIsFocusable"), entries[i] == row).invoke();
            }
            UObject* button = ue::get_object(row, STR("WBP_BaseButton"));
            { ue::Call c(button ? button : row, STR("SetFocus")); if (c) c.invoke(); }
            const bool by_focus = ue::get_object(win, STR("FocusedMerchantItem")) == row;
            if (!by_focus) { ue::Call p(win, STR("OnMerchantItemFocused")); if (p) p.set_object(STR("Widget"), row).invoke(); }
            static int logged = 0;
            if (logged++ < 20)
                log().write("coop_follower_merchant_focus", JsonObj().add("idx", s.merchant_want).add("by_focus", by_focus).add("snapback", s.merchant_want == s.merchant_shown)
                                                              .add("panel_ok", ue::get_object(win, STR("FocusedMerchantItem")) == row));
            s.merchant_shown = s.merchant_want;
        }
    }
}
void choice_hover_tick()
{
    static double next = 0;
    if (!s.choice_pending || !sharing() || s.now < next) return;
    next = s.now + 0.1;
    const auto opts = choice_options();
    if (opts.empty()) return;
    if (s.choice_diag++ == 0) {
        std::string names;
        for (UObject* o : opts) names += ue::short_name(o) + (widget_flag(o, STR("HasFocusedDescendants")) || widget_flag(o, STR("HasAnyUserFocus")) ? "(f)" : "") +
                                         (widget_flag(o, STR("IsHovered")) ? "(h)" : "") + " ";
        log().write("coop_choice_options", JsonObj().add("count", static_cast<int64_t>(opts.size())).add("options", names).add("pilot", is_pilot()));
    }
    if (is_pilot()) {
        int idx = -1;
        for (size_t i = 0; i < opts.size() && idx < 0; ++i)
            if (widget_flag(opts[i], STR("HasAnyUserFocus")) || widget_flag(opts[i], STR("HasFocusedDescendants")) || widget_flag(opts[i], STR("IsHovered"))) idx = static_cast<int>(i);
        if (idx >= 0 && idx != s.choice_hover_sent) {
            s.choice_hover_sent = idx;
            s.net.publish(proto::DialogueChoice{++s.seq, idx, 1}, true);
            bump("choice_hovers_published");
        }
        return;
    }
    const int want = s.choice_hover_want;
    if (want < 0 || want == s.choice_hover_shown || want >= static_cast<int>(opts.size())) return;
    // Focus de l'interface sur l'option voulue : surbrillance du jeu. Les entrées de choix du suiveur sont coupées
    // (contexte de choix retiré), il ne peut pas valider.
    ue::Call c(opts[static_cast<size_t>(want)], STR("SetFocus"));
    const bool ok = c && c.invoke();
    s.choice_hover_shown = want;
    bump(ok ? "choice_hovers_shown" : "choice_hover_errors");
    if (s.choice_diag < 40) { ++s.choice_diag; log().write("coop_choice_hover", JsonObj().add("index", want).add("ok", ok)); }
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
    std::string cine_action;                      // action de jeu qui a appelé TriggerCinematic (pile d'appels)
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
DWORD g_explore_thread = 0;   // fil du jeu (relevé au tick d'exploration)

// Crochet global de ProcessEvent (2026-10-24, pont de la carte du monde activé par Killian, pas chez TOTOFFF) :
//  1. activation (OnInteractionRequested) d'un objet BP_GPE_* hors de kInteractables (pont…) : publiée et rejouée
//     comme les autres interactions ;
//  2. diagnostic : fonctions appelées en exploration sur les objets de tir / peinture / GPE (noms distincts, plafonnés),
//     pour rejouer ensuite les tirs du pilote (sphères de peinture). Classe jugée intéressante une fois par UClass.
void gpe_process_event(RC::Unreal::UObject* ctx, RC::Unreal::UFunction* fn, void* parms)
{
    using namespace RC::Unreal;
    if (!ctx || !fn || GetCurrentThreadId() != g_explore_thread) return;
    static std::unordered_map<UClass*, int> kind;   // 0 rien, 1 GPE à rejouer, 2 trace seulement
    UClass* cls = ctx->GetClassPrivate();
    auto it = kind.find(cls);
    if (it == kind.end()) {
        const std::string n = ue::short_name(cls);
        int k = 0;
        if (n.rfind("BP_GPE_", 0) == 0 && n != "BP_GPE_Rope_C") k = 1;
        else if (n.find("Paint") != std::string::npos || n.find("FreeAim") != std::string::npos || n.find("Projectile") != std::string::npos ||
                 n.find("Shoot") != std::string::npos || n.find("Target") != std::string::npos || n.find("GPE") != std::string::npos) k = 2;
        it = kind.emplace(cls, k).first;
    }
    if (it->second == 0 || !s.active || !sharing()) return;
    static const FName interact(STR("OnInteractionRequested"), FNAME_Find);
    if (it->second == 1 && fn->GetNamePrivate() == interact && !s.replaying) {
        const std::string path = object_path(ctx);
        if (!is_pilot()) { bump("local_actions_on_follower"); log().write("coop_follower_interaction", JsonObj().add("actor", path)); return; }
        s.net.publish(proto::WorldInteract{++s.seq, path, false}, true);
        bump("interactions_published"); bump("gpe_interactions_published"); touch();
        sc.interaction = path; sc.interaction_at = s.now;
        log().write("coop_interaction", JsonObj().add("actor", path).add("instant", false).add("gpe", true));
        return;
    }
    static std::unordered_set<std::string> seen;
    if (seen.size() >= 300 || !is_pilot() || game::in_battle()) return;
    const std::string key = ue::short_name(cls) + ":" + ue::to_utf8(fn->GetName());
    if (seen.insert(key).second) log().write("coop_pe_trace", JsonObj().add("fn", key).add("actor", ue::short_name(ctx)));
}

void gpe_pe_hook(RC::Unreal::Hook::TCallbackIterationData<void>&, RC::Unreal::UObject* ctx, RC::Unreal::UFunction* fn, void* parms)
{
    gpe_process_event(ctx, fn, parms);
}

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

// Pilote : une cinématique (TriggerCinematic) ou un dialogue (ActivateDialogue, D24) commence ici -> SceneStart (cause la
// plus récente : zone, sinon interaction, sinon game action). Dialogue : seq = chemin de l'acteur BP_Dialogue_C.
void on_local_scene(proto::NarrativeKind kind, const std::string& id, const std::string& seq)
{
    const bool dlg = kind == proto::NarrativeKind::Dialogue;
    const std::string key = scene_key(id, seq);
    if (dlg && sc.local_scene == key && s.now - sc.local_scene_at < 5.0) return;   // même dialogue réactivé (étapes)
    sc.local_scene = key;
    sc.local_scene_at = s.now;
    proto::SceneStart m;
    m.scene = id; m.sequence = seq; m.kind = kind;
    auto recent = [](double at) { return at >= 0 && s.now - at < kSceneCauseWindow; };
    if (recent(sc.zone_at) && (!recent(sc.interaction_at) || sc.zone_at >= sc.interaction_at)) {
        m.cause = proto::SceneCause::Zone; m.cause_path = sc.zone; m.action = sc.zone_action;
        // Action la plus récente exécutée depuis la zone (ex. cinématique dans une action conditionnelle) : le suiveur
        // l'exécute si sa zone ne lance rien (conditions différentes, camp du 2026-10-25).
        if (recent(sc.action_at) && sc.action_at >= sc.zone_at) m.action = sc.action;
        if (kind == proto::NarrativeKind::Cinematic && !sc.cine_action.empty()) m.action = sc.cine_action;
    } else if (recent(sc.interaction_at)) {
        m.cause = proto::SceneCause::Interaction; m.cause_path = sc.interaction;
        if (recent(sc.action_at)) m.action = sc.action;
    } else if (recent(sc.action_at)) {
        m.cause = proto::SceneCause::Action; m.action = sc.action;
    }
    // Pas en combat (dialogues des combats : joués par chacun dans son combat).
    const bool publish = s.active && !s.ended && is_pilot() && sharing() && !s.replaying && !game::in_battle();
    log().write(dlg ? "coop_dialogue_trigger" : "coop_cinematic_trigger", JsonObj().add("scene", id).add("sequence", seq).add("cause", static_cast<int>(m.cause))
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
    bump(dlg ? "dialogues_published" : "scenes_published");
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
    // Même zone déjà exécutée ici (son action suit son cours, ex. dialogue après une cinématique) : pas de double lecture.
    // Sauf si rien n'a commencé ici depuis (2026-10-25, camp après le boss Goblu : action conditionnelle de la zone
    // exécutée chez les deux, cinématique lancée seulement chez l'hôte, TOTOFFF resté au camp à attendre).
    if (m.cause == proto::SceneCause::Zone && !m.cause_path.empty() && sc.zone == m.cause_path && s.now - sc.zone_at < 120) {
        if (sc.local_scene_at >= sc.zone_at) return drop("zone_already_run");
        if (s.now - sc.pending_at < 3.0) return;
        UObject* action = m.action.empty() ? nullptr : ue::find_object(ue::from_utf8(m.action).c_str());
        ue::Call ex(ue::get_object(game::controller(), STR("BP_GameActionExecutorComponent")), STR("ExecuteGameAction"));
        bool ran = false;
        if (action && ex) {
            s.replaying = true;
            ran = ex.set_object(STR("GameAction"), action).invoke();
            s.replaying = false;
        }
        bump(ran ? "scene_zone_forced" : "scene_zone_force_errors");
        log().write("coop_scene_zone_forced", JsonObj().add("scene", key).add("action", m.action).add("found", action != nullptr).add("ran", ran));
        sc.acted_scene = key; sc.acted_at = s.now; sc.acted_method = 4; sc.acted_checked = false;
        sc.pending.reset();
        return;
    }
    if (!world_ready()) { if (s.now - sc.pending_at > 15) drop("world_not_ready"); return; }
    const bool dlg = m.kind == proto::NarrativeKind::Dialogue;
    // Interaction : rejouée ici par WorldInteract, la scène doit suivre d'elle-même. Cinématique : repli après 3 s ;
    // dialogue : rien de plus (une téléportation ne relance pas le dialogue d'un PNJ).
    if (m.cause == proto::SceneCause::Interaction) {
        if (dlg) return drop("interaction_replayed");
        if (s.now - sc.pending_at < 3.0) return;
    }
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
            if (playing || sc.local_scene_at >= sc.pending_at || game::playing_cinematic() || game::in_dialogue()) method = 1;
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
    if (dlg) bump("dialogue_syncs");
    log().write("coop_scene_sync", JsonObj().add("scene", key).add("dialogue", dlg).add("cause", static_cast<int>(m.cause)).add("method", method)
                                       .add("after_s", s.now - sc.pending_at));
    sc.acted_scene = key; sc.acted_at = s.now; sc.acted_method = method; sc.acted_checked = false;
    sc.pending.reset();
}

void poll_tick()
{
    if (s.now < s.next_poll) return;
    s.next_poll = s.now + kPollPeriod;
    if (!game::controller()) return;
    g_explore_thread = GetCurrentThreadId();
    movement_lock_tick();
    pilot_stuck_tick();
    choice_hover_tick();
    merchant_tick();
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
    {"gold", L"Gold"}, {"inventory2", L"Inventory"},   // inventory2 : nouvelle empreinte (minuscules, sans tenues), 2026-10-23
     {"interacted2", L"InteractedObjects"}, {"battled", L"BattledEnemies"},
    {"flags", L"NamedIDsStates"}, {"gpe", L"GPE_States"}, {"cinematics", L"InteractedCinematics"}, {"dialogs", L"InteractedDialogs"},
};

// Inventaire résumé : quantité totale par objet et niveau de chaque équipement (comme les ressources communes, D13),
// trié par nom. Avant : réflexion brute de GI.Inventory (~11 700 feuilles : ordre des piles, données statiques
// recopiées, pointeurs d'objets) ; l'hôte en avait 11 741 et le client 11 342 juste après avoir chargé l'instantané de
// l'hôte (2026-10-22) : écart permanent, donc nouvel instantané (rechargement de 5-6 s) toutes les 1 à 2 min.
// Noms en minuscules : le jeu écrit parfois le même objet avec une casse différente selon le PC (« CritCHanceOnWeak »
// / « CritChanceOnWeak »). Tenues et coiffures (Skin*, Face*) ignorées : elles viennent des éditions / DLC du compte,
// pas de la partie (l'hôte en a, le client non : resynchros sans fin, essai à deux PC du 2026-10-23).
reflect::Leaves inventory_leaves()
{
    std::map<std::string, int64_t> eco;
    game::economy_sample(eco);
    std::map<std::string, int64_t> low;
    for (auto& [k, v] : eco) {
        if (k == "gold" || v == 0) continue;
        std::string n = k;
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n.rfind("item:skin", 0) == 0 || n.rfind("item:face", 0) == 0) continue;
        low[n] += v;
    }
    reflect::Leaves l;
    for (auto& [k, v] : low) l.emplace_back(k, std::to_string(v));
    return l;
}

// Objets utilisés sans les zones de passage ni les séquences de niveau : le pilote les déclenche en passant, le
// suiveur pas toujours (zones sans cinématique ni dialogue, non rejouées) ; écart permanent, donc une resynchro toutes
// les 10 à 20 s d'exploration (essai à deux PC du 2026-10-24). Le compte (« InteractedObjects#… ») est retiré aussi.
reflect::Leaves interacted_leaves(reflect::Leaves in)
{
    reflect::Leaves out;
    for (auto& kv : in) {
        const std::string& k = kv.first;
        if (k.find('#') != std::string::npos) continue;
        if (k.find("_BP_GameActionTriggerZone") != std::string::npos || k.find("ObjectID_LevelSequence_") != std::string::npos) continue;
        out.push_back(std::move(kv));
    }
    return out;
}

std::vector<proto::HashPart> world_parts(std::map<std::string, reflect::Leaves>* leaves = nullptr)
{
    std::vector<proto::HashPart> out;
    UObject* gi = game::gi();
    if (!gi) return out;
    prof::Scope prof_scope("world_parts");
    s.last_world_compute = s.now;
    for (const auto& p : kWorldParts) {
        reflect::Leaves l = std::string_view(p.name) == "inventory2" ? inventory_leaves() : reflect::field(gi, p.field);
        if (std::string_view(p.name) == "interacted2") l = interacted_leaves(std::move(l));
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
        for (auto& p : s.last_world_parts) if (p.name == "inventory2") prev_inventory = p.value;
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
        for (auto& p : parts) if (p.name == "inventory2" && p.value != prev_inventory) o.add("inventory", leaves_line(leaves["inventory2"]));
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
    if (differ.find("inventory") != std::string::npos) diff_log.add("inventory", leaves_line(leaves["inventory2"]));   // à comparer au coop_world_hash de l'hôte
    log().write("coop_world_hash_diff", diff_log);
    if (s.world_mismatch_streak >= 2) {
        s.world_mismatch_streak = 0;
        bump("world_resyncs");
        // Diagnostic : toutes les feuilles locales des parties en écart (l'hôte journalise les siennes à la réception de
        // la demande, coop_world_leaves) : comparer les deux journaux hors ligne.
        for (const std::string& part : parts)
            if (auto it = leaves.find(part); it != leaves.end()) log_world_leaves(part, it->second, host.seq);
        s.resync_parts = std::set<std::string>(parts.begin(), parts.end());
        // Rechargement (5-6 s d'écran figé) jamais en plein mouvement : attendu jusqu'à l'arrêt (world_resync_tick).
        // Ni en dialogue, cinématique ou combat : l'instantané arriverait à leur fin (chargement bloqué, 2026-10-23).
        if (s.now - s.last_motion_at < 1.5 || !resync_calm()) {
            if (s.resync_deferred.empty()) {
                s.resync_deferred_since = s.now;
                bump("resyncs_deferred_moving");
                log().write("coop_world_resync_deferred", JsonObj().add("parts", differ));
            }
            s.resync_deferred = differ;
            return;
        }
        send_world_resync(differ);
    }
}

void send_world_resync(std::string differ)   // copie : l'appelant peut passer s.resync_deferred
{
    s.resync_deferred.clear();
    if (s.now - s.last_motion_at < 0.5) bump("resyncs_while_moving");
    ui::set_status("session", "resyncing");
    s.resync_at = s.now;
    s.net.publish(proto::ResyncRequest{"world state differs: " + differ}, true);
    log().write("coop_world_resync", JsonObj().add("parts", differ).add("deferred_s", s.resync_deferred_since >= 0 ? s.now - s.resync_deferred_since : 0.0));
    s.resync_deferred_since = -1;
}

// Client : resynchro attendue -> envoyée après 1,5 s sans mouvement (pilote et pion), ou au bout de 120 s.
void world_resync_tick()
{
    if (s.host || s.resync_deferred.empty()) return;
    if (s.cj != S::ClientJoin::Joined && s.want_join) { s.resync_deferred.clear(); s.resync_deferred_since = -1; return; }
    if ((s.now - s.last_motion_at >= 1.5 && resync_calm()) || s.now - s.resync_deferred_since > 120.0) send_world_resync(s.resync_deferred);
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
                          "join_load_retries", "join_load_stuck", "snapshot_timeouts"})
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
    // Personnage affiché en exploration (changement de personnage, chargement du pion) : noté pour le PilotState.
    bus.pre(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_World.BP_jRPG_Character_World_C:LoadAndActivateCharacter",
            [](HookCtx& ctx, UFunction* f) {
                const std::string name = ue::read_param_fname(ctx, f, STR("CharacterName"));
                if (name.empty() || name == "None") return;
                if (g_char_of.size() > 64) g_char_of.clear();   // pions des niveaux quittés
                g_char_of[ctx.Context] = name;
            });
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
    // Achat du pilote chez le marchand : signalé au suiveur (DialogueChoice.hover 4), qui voit l'article dans son miroir.
    bus.post(L"/Game/Gameplay/Inventory/Merchant/BP_MerchantComponent.BP_MerchantComponent_C:TryBuyItem", [](HookCtx&, UFunction*) {
        if (!s.active || !sharing() || !is_pilot()) return;
        s.net.publish(proto::DialogueChoice{++s.seq, std::max(0, s.merchant_sent), 4}, true);
        bump("merchant_buys_published");
        log().write("coop_merchant_buy", JsonObj().add("index", s.merchant_sent));
    });
    // Tirs du pilote en exploration (point 4 de la liste de Killian : sphères de peinture, cages, boutons) : le projectile
    // qui touche un objet du monde cible du tir (interface BPI_FreeAimTarget_World : FreeAimHit) est publié en
    // WorldInteract « freeaim: » ; le suiveur appelle FreeAimHit sur le même objet. Les combattants du monde (ennemis,
    // pions) sont exclus : un tir sur un ennemi lance un combat, qui suit son propre chemin.
    bus.post(L"/Game/Gameplay/WorldSkills/BP_FreeAimProjectile_Base.BP_FreeAimProjectile_Base_C:OnCollision", [](HookCtx& ctx, UFunction* f) {
        if (!s.active || !sharing() || s.replaying || game::in_battle()) return;
        UObject* target = ue::read_param_object(ctx, f, STR("OtherActor"));
        static int logged = 0;
        const bool hittable = target && ue::Call(target, STR("FreeAimHit")) && !ue::is_a(target, STR("BP_jRPG_Enemy_World_Base_Seamless_C")) &&
                              !ue::is_a(target, STR("BP_jRPG_Character_World_C"));
        if (logged < 40 && ++logged)
            log().write("coop_freeaim_world_hit", JsonObj().add("target", target ? ue::short_name(target) : std::string("null"))
                                                     .add("class", target ? ue::short_name(target->GetClassPrivate()) : std::string()).add("hittable", hittable).add("pilot", is_pilot()));
        if (!hittable || !is_pilot()) return;
        const std::string path = object_path(target);
        s.net.publish(proto::WorldInteract{++s.seq, "freeaim:" + path, false}, true);
        bump("freeaim_hits_published"); touch();
    });
    RC::Unreal::Hook::RegisterProcessEventPreCallback(&gpe_pe_hook, {false, false, STR("E33Coop"), STR("ExploreGPE")});
    bus.pre(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:OnChoiceMade", [](HookCtx& ctx, UFunction* f) {
        s.choice_pending = false;
        s.choice_hover_want = -1;
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
        s.choice_hover_sent = s.choice_hover_shown = -1;   // le survol voulu reçu avant la question ici est gardé
        s.choice_diag = 0;
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
        // Action de jeu qui lance cette cinématique (pile d'appels) : le suiveur l'exécute si sa zone ne lance rien.
        sc.cine_action.clear();
        std::string frames;
        RC::Unreal::FFrame* fr = ctx.TheStack.PreviousFrame();
        for (int depth = 0; fr && depth < 8; ++depth) {
            UObject* o = fr->Object();
            const std::string cls = o ? ue::short_name(o->GetClassPrivate()) : std::string();
            if (frames.size() < 400) frames += cls + ";";
            if (sc.cine_action.empty() && o && cls.find("GameAction") != std::string::npos && cls.find("Component") == std::string::npos)
                sc.cine_action = object_path(o);
            RC::Unreal::FFrame* prev = fr->PreviousFrame();
            fr = prev == fr ? nullptr : prev;
        }
        log().write("coop_cinematic_caller", JsonObj().add("action", sc.cine_action).add("frames", frames));
        on_local_scene(proto::NarrativeKind::Cinematic, id, seq);
    });
    // Dialogue (D24) : ActivateDialogue(Dialogue) du composant de dialogue, quelle que soit sa cause (PNJ, zone, action).
    bus.pre(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:ActivateDialogue", [](HookCtx& ctx, UFunction* f) {
        UObject* d = ue::read_param_object(ctx, f, STR("Dialogue"));
        if (!d) return;
        on_local_scene(proto::NarrativeKind::Dialogue, "", object_path(d));
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
