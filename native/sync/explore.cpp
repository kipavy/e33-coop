#include "explore.hpp"

#include "builds.hpp"
#include "coopsave.hpp"
#include "presence.hpp"

#include <algorithm>
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
    double level_mismatch_since = -1;
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
    int perturb_gold = 0;                         // -e33perturbworld=N (test)
    double cj_joined_at = 0;
    double waited_since = -1;                     // joueur en retard : les autres ont fini depuis
} s;

void bump(const char* c, int n = 1) { if (s.net.bump) s.net.bump(c, n); }

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
    ui::set_status("session", "new_game_follow");
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
        // Seulement sur une position fraîche : un pilote qui charge n'envoie plus rien (sa dernière zone est périmée).
        if (s.now - s.last_pilot_at > 1.5) { s.level_mismatch_since = -1; return; }
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
    if (d > kTeleportDistance || std::fabs(dz) > 400) {
        const double pos[3] = {tx, ty, tz};
        game::teleport_pawn(pawn, pos, p.yaw);
        bump("follower_teleports");
        log().write("coop_follower_teleport", JsonObj().add("distance", d).add("dz", dz));
        return;
    }
    // Arrêt seulement quand le pilote s'arrête vraiment et qu'on est arrivé ; reprise dès qu'il bouge ou s'éloigne
    // (avant : un seul seuil à 60 u, donc arrêt-redémarrage plusieurs fois par seconde quand il court).
    const double sp = std::hypot(p.vx, p.vy);
    if (!s.follow_moving && (d > 120 || sp > 80)) { s.follow_moving = true; bump("follow_starts"); }
    else if (s.follow_moving && d < 40 && sp < 30) s.follow_moving = false;
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

void on_map_change(const proto::MapChange& mc)
{
    touch();
    std::string dest, tag;
    if (game::map_change_in_progress() && game::map_destination(dest, tag) && dest == mc.level) {
        log().write("coop_map_change_already", JsonObj().add("level", mc.level));
        return;
    }
    if (!game::map_change_in_progress() && game::current_level() == mc.level) {
        log().write("coop_map_change_same_level", JsonObj().add("level", mc.level));
        return;
    }
    // Dernière position du pilote = ancienne zone : oubliée, sinon le suiveur arrivé avant lui y retournait au bout de 8 s.
    s.last_pilot.reset();
    s.level_mismatch_since = -1;
    s.replaying = true;
    const bool ok = game::change_map(ue::from_utf8(mc.level), ue::from_utf8(mc.spawn_tag));
    s.replaying = false;
    bump(ok ? "map_changes_followed" : "map_change_errors");
    log().write("coop_map_change_followed", JsonObj().add("level", mc.level).add("spawn", mc.spawn_tag).add("ok", ok));
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
        if (narr) s.local_kind = cine ? proto::NarrativeKind::Cinematic : proto::NarrativeKind::Dialogue;
        ++s.narr_seq;
        s.net.publish(proto::NarrativeState{s.me, s.local_kind, narr, s.narr_seq}, true);
        bump(narr ? "narratives_started" : "narratives_finished");
        log().write("coop_narrative", JsonObj().add("active", narr).add("kind", static_cast<int>(s.local_kind)).add("seq", s.narr_seq));
    }
    // Barrière : fini ici, pas chez les autres -> jeu en pause jusqu'à ce que tout le monde ait fini. Aussi quand la
    // cinématique enchaîne sur un combat (boss) : personne ne commence le combat avant les autres.
    const bool hold = !narr && others_in_narrative();
    if (hold && !s.barrier_paused) {
        s.pause_reasons.insert("barrier");
        s.barrier_paused = true;
        s.barrier_since = s.now;
        bump("barrier_waits");
        log().write("coop_barrier_hold", JsonObj());
    } else if (s.barrier_paused && (!hold || s.now - s.barrier_since > kBarrierTimeout)) {
        if (hold) { bump("barrier_timeouts"); s.remote_narr.clear(); }
        s.pause_reasons.erase("barrier");
        s.barrier_paused = false;
        ui::clear_status("wait");
        bump("barrier_releases");
        log().write("coop_barrier_release", JsonObj().add("waited_s", s.now - s.barrier_since).add("timeout", hold));
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

std::vector<proto::HashPart> world_parts(std::map<std::string, reflect::Leaves>* leaves = nullptr)
{
    std::vector<proto::HashPart> out;
    UObject* gi = game::gi();
    if (!gi) return out;
    for (const auto& p : kWorldParts) {
        reflect::Leaves l = reflect::field(gi, p.field);
        out.push_back({p.name, reflect::hash(l)});
        if (leaves) (*leaves)[p.name] = std::move(l);
    }
    return out;
}

void touch() { s.last_event_at = s.now; }

bool quiet(double secs)
{
    return safe_point() && s.interactions.empty() && s.choices.empty() && !s.choice_pending && s.now - s.last_event_at >= secs;
}

void world_hash_tick()
{
    if (game::in_battle() || game::map_change_in_progress() || s.local_narr) touch();
    if (s.now < s.next_world_check) return;
    s.next_world_check = s.now + (s.host ? 5.0 : 1.0);   // hôte : lecture de tout l'état, pas à chaque seconde
    if (s.host) {
        if (!sharing() || !quiet(8.0)) return;
        std::map<std::string, reflect::Leaves> leaves;
        auto parts = world_parts(&leaves);
        if (parts.empty()) return;
        uint64_t all = 0;
        for (auto& p : parts) all = all * 31 + p.value;
        if (all == s.last_world_sent && s.now - s.last_world_sent_at < 60.0) return;
        s.last_world_sent = all;
        s.last_world_sent_at = s.now;
        proto::WorldHash wh{++s.world_seq, game::current_level(), parts};
        s.net.publish(wh, true);
        bump("world_hashes_sent");
        std::string sizes = "{";
        for (auto& [k, l] : leaves) { if (sizes.size() > 1) sizes += ","; sizes += "\"" + k + "\":" + std::to_string(l.size()); }
        log().write("coop_world_hash", JsonObj().raw("msg", proto::to_json(wh)).raw("leaves", sizes + "}"));
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
    if (differ.empty()) {
        s.world_mismatch_streak = 0;
        log().write("coop_world_hash_ok", JsonObj().add("seq", host.seq));
        uint64_t all = 0;
        for (auto& p : host.parts) all = all * 31 + p.value;
        coopsave::on_world_ok(all, s.net.name_of ? s.net.name_of(0) : std::string());   // état vérifié : copie coop (D15)
        return;
    }
    ++s.world_mismatch_streak;
    bump("world_hash_diffs");
    std::string sizes = "{";
    for (auto& [k, l] : leaves) { if (sizes.size() > 1) sizes += ","; sizes += "\"" + k + "\":" + std::to_string(l.size()); }
    // Détail local des parties en écart (l'hôte journalise sa taille de chaque partie à l'envoi).
    std::string detail = "{";
    for (auto& [k, l] : leaves) {
        if (differ.find(k) == std::string::npos) continue;
        if (detail.size() > 1) detail += ",";
        std::string first = "[";
        for (size_t i = 0; i < l.size() && i < 12; ++i) { if (first.size() > 1) first += ","; first += JsonObj().add("p", l[i].first).add("v", l[i].second).str(); }
        detail += "\"" + k + "\":" + first + "]";
    }
    log().write("coop_world_hash_diff", JsonObj().add("seq", host.seq).add("parts", differ).add("streak", s.world_mismatch_streak)
                                           .raw("leaves", sizes + "}").raw("mine", detail + "}"));
    if (s.world_mismatch_streak >= 2) {
        s.world_mismatch_streak = 0;
        bump("world_resyncs");
        ui::set_status("session", "resyncing");
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
    s.pilot = s.me;            // plus de pilote distant : le déplacement local est rendu (movement_lock_tick)
    movement_lock_tick();
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
    if (std::get_if<proto::ResyncRequest>(&m)) {
        if (!s.host) return true;
        // Le client recharge un instantané : il ne participe plus jusqu'à son JoinReady.
        s.peers[from] = PeerJoin{JoinState::Pending, s.now, false};
        bump("resyncs_requested");
        log().write("coop_resync_requested", JsonObj().add("player", static_cast<int>(from)));
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
        s.remote_narr[ns->player] = ns->active;
        presence::set_narrative(ns->player, ns->active, ns->kind == proto::NarrativeKind::Cinematic, s.now);
        log().write("coop_narrative_remote", JsonObj().add("player", static_cast<int>(ns->player)).add("active", ns->active).add("seq", ns->seq));
        return true;
    }
    if (std::get_if<proto::BarrierRelease>(&m)) return true;
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
    if (s.host) host_join_tick();
    else client_join_tick();
    key_tick();
    if (is_pilot()) pilot_tick();
    else follow_tick();
    replay_tick();
    poll_tick();
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
        if (n > 0) ui::toast("new_game_shared", {std::to_string(n)});
        log().write("coop_new_game", JsonObj().add("players_following", n));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:OnInitialCinematicCompleted", [](HookCtx&, UFunction*) {
        if (!s.intro) return;
        s.intro = false;
        bump("intros_finished");
        log().write("coop_intro_finished", JsonObj().add("after_s", s.now - s.intro_since));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:ChangeMap", [](HookCtx&, UFunction*) {
        if (s.replaying || !s.active || !is_pilot() || !sharing()) return;   // pas pendant le chargement de l'instantané
        std::string level, tag;
        if (!game::map_destination(level, tag)) return;
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
