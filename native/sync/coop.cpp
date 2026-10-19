#include "coop.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../net/link.hpp"
#include "../net/recorder.hpp"
#include "../net/transport.hpp"
#include "../protocol/messages.hpp"
#include "state.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

using RC::Unreal::UObject;
using RC::Unreal::UFunction;

namespace e33::coop {

namespace {

constexpr const char* kModVersion = "0.3.0";
constexpr double kDefenseTimeout = 3.0;
constexpr double kComboTimeout = 3.0;

struct Peer {
    std::unique_ptr<net::Link> link;
    uint8_t player_id = 0;
    bool accepted = false;
    std::string name;
};

using TurnSpotSeq = std::tuple<uint32_t, int, uint16_t>;   // (tour, emplacement, n°)

using SteadyClock = std::chrono::steady_clock;

struct Window {                                            // fenêtre de QTE offensif en cours (par timeline)
    UObject* hero = nullptr;
    int spot = 0;
    uint16_t seq = 0;
    SteadyClock::time_point start;
    double duration = 0;                                   // secondes (temps réel)
    bool local = false;                                    // héros contrôlé ici : résultat à publier
    bool published = false;
    proto::ComboResult plan;                               // héros d'un autre joueur : résultat de son propriétaire
    bool pressed = false;
};

struct Press { UObject* timeline = nullptr; SteadyClock::time_point at; };

struct LocalHit {                                          // coup reçu par un héros contrôlé ici (résultat à publier)
    int spot = 0;
    uint16_t hit = 0;
    bool jumpable = false;
    bool perfect_dodge = false;
};

struct Battle {
    uint32_t id = 0;
    uint32_t turn = 0;
    bool active = false;
    bool ending = false;                                   // fin de combat commencée (tirages des récompenses encore remplacés)
    bool finalized = false;
    std::map<int, uint8_t> owner_by_spot;                  // emplacement de héros -> player_id
    // Actions des héros
    std::map<std::pair<uint32_t, int>, proto::ActionCommand> commands;   // (tour, emplacement) -> commande reçue
    std::map<std::pair<uint32_t, int>, double> command_time;             // réception (s.now)
    std::set<std::pair<uint32_t, int>> executed;
    bool action_wait_logged = false;
    // QTE offensifs
    std::map<TurnSpotSeq, proto::ComboResult> combos;
    std::map<int, uint16_t> combo_seq;                     // emplacement -> n° de fenêtre dans le tour
    std::map<UObject*, Window> windows;                    // timeline -> fenêtre en cours
    std::vector<Press> presses;                            // appuis à rejouer (héros d'un autre joueur)
    // Défenses
    std::map<TurnSpotSeq, proto::Defense> defenses;
    std::map<int, uint16_t> hit_seq;                       // emplacement -> n° de coup reçu dans le tour
    std::map<UObject*, LocalHit> local_hits;               // composant de stats -> coup en cours (héros d'ici)
    std::map<UObject*, int> imposed_check;                 // composant de stats -> défense imposée (vérification du verdict)
    // État
    std::map<uint32_t, uint64_t> local_hash;
    std::map<uint32_t, uint64_t> remote_hash;
    std::map<uint32_t, proto::StateSync> host_sync;        // client : état de l'hôte au début de chaque tour
    std::map<uint32_t, proto::TurnStart> host_turns;       // client
    double turn_started_at = 0;
    // Tirages (D10)
    uint64_t rng_seed = 0;
    std::map<uint32_t, uint16_t> draw_rank;                // flux -> rang du prochain tirage dans le tour
    std::map<uint32_t, std::string> stream_names;
    std::map<uint32_t, std::map<uint64_t, double>> local_draws;
    std::map<uint32_t, std::map<uint64_t, double>> host_draws;
    int traced_draws = 0;
    std::map<UObject*, int> enemy_spots;                   // ennemi -> emplacement (sans appel au jeu dans les hooks de tirage)
    // Fin
    int local_end = -1;
    int host_end = -1;
    double end_flow_at = -1;                               // fin sans victoire : clôture différée (tick), jamais bloquante dans le flux de fin
    std::optional<proto::BattleRewards> host_rewards;
};

struct State {
    Role role = Role::Solo;
    std::unique_ptr<net::Transport> transport;
    std::vector<Peer> peers;                               // hôte : clients ; client : [hôte]
    net::Recorder recorder;
    uint8_t my_player_id = 0;                              // hôte = 0
    uint32_t session_id = 0;
    uint32_t game_build = 0;
    std::vector<std::string> gameplay_mods;
    std::string connect_to;
    bool accepted = false;
    bool rejected = false;
    double now = 0;
    Battle battle;
    uint32_t next_battle_id = 1;
    std::map<std::string, uint8_t> owner_overrides;        // -e33owners=0:1,Lune:1  (emplacement ou nom : joueur)
    DWORD game_thread = 0;
    std::optional<proto::BattleStart> pending_start;       // client : combat de l'hôte, lancé une fois le monde prêt
    double world_ready_s = 0;
    double next_trigger_try = 0;
    double action_delay_s = 1.5;                           // délai minimal (depuis le début du tour) avant d'exécuter l'action d'un héros d'un autre joueur
    double replay_lag_s = 1.0;                             // décalage minimal derrière son propriétaire (-e33replaylag) : ses QTE sont connus à temps
    std::optional<uint64_t> fixed_seed;                    // hôte : -e33seed=N
    std::vector<std::string> draw_filter;                  // diagnostic : -e33draws=none | sous-chaînes
    std::optional<uint32_t> perturb_turn;                  // client : -e33perturb=N (test de resynchro)
    bool imposing = false;                                 // une défense imposée est en cours (pas une entrée parasite)
    bool lock_input = true;                                // -e33lockinput=0 : laisser les entrées pendant les tours des autres
    bool input_enabled = true;
    std::map<std::string, int> counters;
};

State s;

void bump(const char* name, int n = 1) { s.counters[name] += n; }

// --- Utilitaires ------------------------------------------------------------------------

std::wstring arg(const wchar_t* key)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring out;
    const std::wstring k = std::wstring(L"-") + key + L"=";
    for (int i = 0; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() > k.size() && _wcsnicmp(a.c_str(), k.c_str(), k.size()) == 0) out = a.substr(k.size());
    }
    if (argv) LocalFree(argv);
    return out;
}

std::filesystem::path game_root()
{
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    // …/Expedition 33/Sandfall/Binaries/Win64/SandFall-Win64-Shipping.exe -> …/Expedition 33
    return std::filesystem::path(buf).parent_path().parent_path().parent_path().parent_path();
}

uint32_t read_build_id()
{
    // steamapps/common/<jeu> -> steamapps/appmanifest_1903340.acf
    const auto acf = game_root().parent_path().parent_path() / L"appmanifest_1903340.acf";
    std::ifstream f(acf);
    std::string line;
    while (std::getline(f, line)) {
        const auto p = line.find("\"buildid\"");
        if (p == std::string::npos) continue;
        const auto q1 = line.find('"', p + 9);
        const auto q2 = line.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) return static_cast<uint32_t>(std::strtoul(line.substr(q1 + 1, q2 - q1 - 1).c_str(), nullptr, 10));
    }
    return 0;
}

std::vector<std::string> read_gameplay_mods()
{
    static const char* builtin[] = {"CheatManagerEnablerMod", "ConsoleCommandsMod", "ConsoleEnablerMod", "SplitScreenMod", "LineTraceMod",
                                    "BPML_GenericFunctions", "BPModLoaderMod", "Keybinds", "E33Coop", "E33Explore"};
    std::vector<std::string> mods;
    const auto mods_txt = game_root() / L"Sandfall/Binaries/Win64/ue4ss/Mods/mods.txt";
    std::ifstream f(mods_txt);
    std::string line;
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || line.find(';') == 0) continue;
        std::string name = line.substr(0, colon);
        name.erase(name.find_last_not_of(" \t") + 1);
        if (line.find('1', colon) == std::string::npos) continue;
        if (std::find_if(std::begin(builtin), std::end(builtin), [&](const char* b) { return name == b; }) != std::end(builtin)) continue;
        mods.push_back("ue4ss:" + name);
    }
    std::error_code ec;
    for (const auto* dir : {L"Sandfall/Content/Paks/~mods", L"Sandfall/Content/Paks/LogicMods"}) {
        for (auto& e : std::filesystem::directory_iterator(game_root() / dir, ec))
            if (e.path().extension() == L".pak") mods.push_back("pak:" + e.path().filename().string());
    }
    std::sort(mods.begin(), mods.end());
    return mods;
}

void send_to(Peer& p, const proto::Message& m, bool reliable = true) { if (p.link) p.link->send(m, reliable); }
void broadcast(const proto::Message& m) { for (auto& p : s.peers) if (p.accepted) send_to(p, m); }
// Hôte : à tous les clients sauf l'émetteur ; client : à l'hôte.
void publish(const proto::Message& m, const Peer* origin = nullptr)
{
    for (auto& p : s.peers) if (p.accepted && &p != origin) send_to(p, m);
}

uint8_t owner_of_spot(int spot)
{
    auto it = s.battle.owner_by_spot.find(spot);
    return it == s.battle.owner_by_spot.end() ? 0 : it->second;
}
bool controls(int spot) { return owner_of_spot(spot) == s.my_player_id; }

proto::Unit actor_unit(UObject* actor) { return sync::unit_of(actor); }

void refresh_enemy_spots()
{
    s.battle.enemy_spots.clear();
    for (UObject* e : game::enemies()) s.battle.enemy_spots[e] = game::spot(e);
}
// Sans appel au jeu (hooks très fréquents) : emplacement d'un ennemi connu, -1 sinon.
int cached_enemy_spot(UObject* o)
{
    auto it = s.battle.enemy_spots.find(o);
    return it == s.battle.enemy_spots.end() ? -1 : it->second;
}

std::string hex64(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

void check_hash(uint32_t turn)
{
    auto l = s.battle.local_hash.find(turn);
    auto r = s.battle.remote_hash.find(turn);
    if (l == s.battle.local_hash.end() || r == s.battle.remote_hash.end()) return;
    bump("hashes_compared");
    if (l->second != r->second) {
        bump("desyncs");
        log().write("desync", JsonObj().add("battle", s.battle.id).add("turn", turn).add("local", hex64(l->second)).add("remote", hex64(r->second))
                                  .raw("state", proto::to_json(sync::snapshot(s.battle.id, turn))), turn);
    } else {
        log().write("hash_ok", JsonObj().add("battle", s.battle.id).add("hash", hex64(l->second)), turn);
    }
    s.battle.local_hash.erase(l);
    s.battle.remote_hash.erase(r);
}

// --- Réseau ------------------------------------------------------------------------------

void handle(Peer& p, proto::Message& m);

void pump_network()
{
    if (!s.transport) return;
    net::Datagram d;
    while (s.transport->poll(d)) {
        Peer* peer = nullptr;
        for (auto& p : s.peers) if (p.link && p.link->peer() == d.peer) peer = &p;
        if (!peer && s.role == Role::Host) {
            if (s.peers.size() >= 2) { log().write("net_ignored", JsonObj().add("peer", d.peer).add("why", "session full")); continue; }
            Peer np;
            np.link = std::make_unique<net::Link>(*s.transport, d.peer);
            np.player_id = static_cast<uint8_t>(s.peers.size() + 1);
            s.peers.push_back(std::move(np));
            peer = &s.peers.back();
            Peer* pp = peer;
            pp->link->on_invalid = [](const std::string& why) { bump("invalid_messages"); log().write("net_invalid", JsonObj().add("why", why)); };
            pp->link->on_sent = [](const proto::Message& m, bool) { s.recorder.write("h2c", m); log().write("net_send", JsonObj().raw("msg", proto::to_json(m))); };
            pp->link->on_delivered = [](const proto::Message& m) { s.recorder.write("c2h", m); log().write("net_recv", JsonObj().raw("msg", proto::to_json(m))); };
            log().write("net_peer", JsonObj().add("peer", d.peer).add("player_id", static_cast<int>(pp->player_id)));
        }
        if (!peer) { log().write("net_ignored", JsonObj().add("peer", d.peer).add("why", "unknown peer")); continue; }
        peer->link->on_datagram(d.data.data(), d.data.size(), s.now);
    }
    for (auto& p : s.peers) {
        proto::Message m;
        while (p.link && p.link->pop(m)) handle(p, m);
        if (p.link) p.link->update(s.now);
    }
}

// Pompe le réseau en bloquant le game thread jusqu'à cond() ou expiration (utilisé dans les hooks).
template <class Cond>
bool wait_for(Cond cond, double timeout_s)
{
    const ULONGLONG start = GetTickCount64();
    const double base = s.now;
    while (!cond()) {
        const double elapsed = (GetTickCount64() - start) / 1000.0;
        if (elapsed > timeout_s) return false;
        s.now = base + elapsed;
        pump_network();
        Sleep(0);   // Sleep(1) dure ~15 ms sous Windows : on cède juste la main
    }
    return true;
}

// Messages de jeu communs (hôte et client) : stockés pour les points de synchro ; l'hôte relaie aux autres.
bool handle_battle_common(Peer& p, proto::Message& m)
{
    auto& b = s.battle;
    if (auto* ac = std::get_if<proto::ActionCommand>(&m)) {
        if (ac->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(ac->actor_spot) != p.player_id) {
            log().write("net_rejected_msg", JsonObj().add("why", "action for a hero this player does not own").raw("msg", proto::to_json(m)));
            return true;
        }
        b.commands[{ac->turn, ac->actor_spot}] = *ac;
        b.command_time[{ac->turn, ac->actor_spot}] = s.now;
        // Résultats de QTE déjà connus (faux client) : rangés comme des ComboResult, avec les instants d'appui
        // canoniques du test runner (parfait à 88 % de la fenêtre, réussi à 30 %).
        for (size_t i = 0; i < ac->combo.size(); ++i) {
            const bool ok = (ac->combo[i] & 1) != 0, perfect = (ac->combo[i] & 2) != 0;
            b.combos[{ac->turn, ac->actor_spot, static_cast<uint16_t>(i + 1)}] = {ac->battle_id, ac->turn, ac->actor_spot, static_cast<uint16_t>(i + 1),
                                                                               ok || perfect, perfect, static_cast<uint16_t>(perfect ? 880 : ok ? 300 : proto::kNoPress)};
        }
        if (s.role == Role::Host) publish(*ac, &p);
        return true;
    }
    if (auto* cr = std::get_if<proto::ComboResult>(&m)) {
        if (cr->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(cr->spot) != p.player_id) return true;
        b.combos[{cr->turn, cr->spot, cr->seq}] = *cr;
        if (s.role == Role::Host) publish(*cr, &p);
        return true;
    }
    if (auto* dr = std::get_if<proto::DefenseResult>(&m)) {
        if (dr->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(dr->spot) != p.player_id) return true;
        b.defenses[{dr->turn, dr->spot, dr->hit}] = dr->result;
        if (s.role == Role::Host) publish(*dr, &p);
        return true;
    }
    if (auto* sh = std::get_if<proto::StateHash>(&m)) {
        if (sh->battle_id == b.id) { b.remote_hash[sh->turn] = sh->hash; check_hash(sh->turn); }
        return true;
    }
    if (auto* ping = std::get_if<proto::Ping>(&m)) { send_to(p, proto::Pong{ping->t_us}, false); return true; }
    return false;
}

void handle(Peer& p, proto::Message& m)
{
    if (s.role == Role::Host) {
        if (auto* h = std::get_if<proto::Hello>(&m)) {
            proto::Reject rej;
            std::string diff;
            if (h->protocol != proto::kProtocolVersion) { rej.reason = proto::RejectReason::ProtocolMismatch; diff = "protocole hôte " + std::to_string(proto::kProtocolVersion) + " / client " + std::to_string(h->protocol); }
            else if (h->game_build != s.game_build) { rej.reason = proto::RejectReason::GameBuildMismatch; diff = "build du jeu hôte " + std::to_string(s.game_build) + " / client " + std::to_string(h->game_build); }
            else if (h->mod_version != kModVersion) { rej.reason = proto::RejectReason::ModVersionMismatch; diff = std::string("version du mod hôte ") + kModVersion + " / client " + h->mod_version; }
            else if (h->gameplay_mods != s.gameplay_mods) {
                rej.reason = proto::RejectReason::ModListMismatch;
                diff = "mods de gameplay différents :";
                for (auto& x : s.gameplay_mods) if (std::find(h->gameplay_mods.begin(), h->gameplay_mods.end(), x) == h->gameplay_mods.end()) diff += " hôte seul=" + x;
                for (auto& x : h->gameplay_mods) if (std::find(s.gameplay_mods.begin(), s.gameplay_mods.end(), x) == s.gameplay_mods.end()) diff += " client seul=" + x;
            }
            if (!diff.empty()) {
                rej.detail = diff;
                send_to(p, rej);
                log().write("session_reject", JsonObj().add("peer", p.link->peer()).add("detail", diff));
                return;
            }
            p.accepted = true;
            p.name = h->player_name;
            send_to(p, proto::Welcome{p.player_id, s.session_id});
            log().write("session_join", JsonObj().add("player", h->player_name).add("player_id", static_cast<int>(p.player_id)));
            return;
        }
        if (!p.accepted) return;
        if (handle_battle_common(p, m)) return;
        if (std::get_if<proto::Bye>(&m)) {
            log().write("session_leave", JsonObj().add("player_id", static_cast<int>(p.player_id)));
            p.accepted = false;
        }
        return;
    }
    // Client
    if (auto* w = std::get_if<proto::Welcome>(&m)) {
        s.accepted = true; s.my_player_id = w->player_id; s.session_id = w->session_id;
        log().write("session_welcome", JsonObj().add("player_id", static_cast<int>(w->player_id)));
        return;
    }
    if (auto* r = std::get_if<proto::Reject>(&m)) {
        s.rejected = true;
        log().write("session_rejected", JsonObj().add("reason", static_cast<int>(r->reason)).add("detail", r->detail));
        return;
    }
    if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
        // Le combat ne démarre qu'une fois le monde local prêt (voir tick) : l'hôte peut être en avance.
        s.pending_start = *bs;
        s.battle = Battle{};   // remis à zéro dès maintenant : les messages suivants de l'hôte s'y accumulent
        s.battle.id = bs->battle_id;
        for (auto& o : bs->owners) s.battle.owner_by_spot[o.spot] = o.player_id;
        s.battle.rng_seed = bs->rng_seed;
        log().write("coop_battle_start_received", JsonObj().add("battle", bs->battle_id).add("encounter", bs->encounter));
        return;
    }
    if (handle_battle_common(p, m)) return;
    if (auto* ts = std::get_if<proto::TurnStart>(&m)) {
        if (ts->battle_id == s.battle.id) s.battle.host_turns[ts->turn] = *ts;
    } else if (auto* ss = std::get_if<proto::StateSync>(&m)) {
        if (ss->battle_id == s.battle.id) s.battle.host_sync[ss->turn] = *ss;
    } else if (auto* dw = std::get_if<proto::Draws>(&m)) {
        if (dw->battle_id == s.battle.id)
            for (auto& d : dw->draws) s.battle.host_draws[dw->turn][(static_cast<uint64_t>(d.key) << 16) | d.rank] = d.value;
    } else if (auto* be = std::get_if<proto::BattleEnd>(&m)) {
        if (be->battle_id == s.battle.id) s.battle.host_end = be->result;
        log().write("coop_battle_end_received", JsonObj().add("battle", be->battle_id).add("result", static_cast<int>(be->result)));
    } else if (auto* rw = std::get_if<proto::BattleRewards>(&m)) {
        if (rw->battle_id == s.battle.id) s.battle.host_rewards = *rw;
    }
}

// --- Tirages de gameplay (D10) ------------------------------------------------------------

bool on_game_thread() { return s.game_thread != 0 && GetCurrentThreadId() == s.game_thread; }

// Appelant Blueprint d'une fonction native hookée. Le cadre fourni par UE4SS est celui de la fonction
// elle-même (objet = CDO de KismetMathLibrary, vérifié en jeu le 2026-10-19) : on remonte PreviousFrame
// jusqu'au premier objet qui n'est pas ce CDO.
UObject* script_caller(HookCtx& ctx, UFunction** node_out = nullptr)
{
    RC::Unreal::FFrame* f = &ctx.TheStack;
    for (int depth = 0; f && depth < 4; ++depth) {
        UObject* o = f->Object();
        if (o && o != ctx.Context) {
            if (node_out) *node_out = f->Node();
            return o;
        }
        RC::Unreal::FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    return nullptr;
}

// Diagnostic : chaîne des cadres de script d'un tirage (5 premiers appels par fonction, puis 600 par combat).
void trace_random(HookCtx& ctx, const std::string& fname)
{
    static std::map<std::string, int> seen;
    const bool early = seen[fname]++ < 5;
    const bool in_battle = s.battle.active && s.battle.traced_draws < 600;
    if (!early && !in_battle) return;
    if (in_battle) ++s.battle.traced_draws;
    std::string frames = "[";
    RC::Unreal::FFrame* f = &ctx.TheStack;
    for (int depth = 0; f && depth < 4; ++depth) {
        if (frames.size() > 1) frames += ",";
        frames += JsonObj().add("o", ue::full_name(f->Object())).add("n", ue::short_name(f->Node())).add("enemy", cached_enemy_spot(f->Object())).str();
        RC::Unreal::FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    log().write("rng_trace", JsonObj().add("fn", fname).add("battle", s.battle.active).raw("frames", frames + "]")
                                 .add("has_result", ctx.RESULT_DECL != nullptr), s.battle.turn);
}

enum class RandomKind { IntInRange, Int, Float, FloatInRange, Bool, BoolWeight };
struct RandomFn { const wchar_t* name; RandomKind kind; };

uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Uniforme dans [0, 1), identique sur toutes les instances pour (graine, tour, flux, rang).
double derived_uniform(uint32_t turn, uint32_t key, uint16_t rank)
{
    const uint64_t h = splitmix64(s.battle.rng_seed ^ splitmix64((static_cast<uint64_t>(turn) << 32) | key) ^ splitmix64(0x5EED0000ull + rank));
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

uint32_t fnv32(const std::string& str)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : str) { h ^= c; h *= 16777619u; }
    return h;
}

// Prochain tirage d'un flux : rang dans le tour, valeur uniforme.
double next_uniform(const std::string& stream, uint32_t& key, uint16_t& rank)
{
    key = fnv32(stream);
    rank = s.battle.draw_rank[key]++;
    s.battle.stream_names.emplace(key, stream);
    return derived_uniform(s.battle.turn, key, rank);
}

void record_draw(const std::string& stream, uint32_t key, uint16_t rank, double v, double a, double b)
{
    bump("gameplay_draws");
    s.battle.local_draws[s.battle.turn][(static_cast<uint64_t>(key) << 16) | rank] = v;
    log().write("coop_draw", JsonObj().add("stream", stream).add("rank", static_cast<int>(rank)).add("value", v).add("a", a).add("b", b), s.battle.turn);
    if (s.role == Role::Host) broadcast(proto::Draws{s.battle.id, s.battle.turn, {{key, rank, v}}});
}

// Nom du flux si le tirage vient du gameplay (premier appelant BP), vide sinon.
// Gameplay (traces du 2026-10-19, GAME-INTERNALS §15) : IA d'un ennemi, critiques, Luminas (BP_LU_*), activation de
// buffs, effets de buffs, scripts de compétence, récompenses (RollBattleRewards + système de butin).
std::string gameplay_stream(HookCtx& ctx, const std::string& fname)
{
    UFunction* node = nullptr;
    UObject* caller = script_caller(ctx, &node);
    if (!caller) return {};
    const int es = cached_enemy_spot(caller);
    const std::string cls = game::char_class(caller);
    const std::string fn = ue::short_name(node);
    const bool gameplay = es >= 0 || cls == "BP_BattleDamages_C" || cls.rfind("BP_LU_", 0) == 0 || cls == "AC_jRPG_CharacterBattleStats_C" ||
                          cls.rfind("BP_BattleBuff", 0) == 0 || cls.rfind("BP_LootSystemComponent", 0) == 0 ||
                          (cls == "AC_jRPG_BattleManager_C" && fn == "RollBattleRewards") || ue::is_a(caller, STR("BP_Battle_SkillScript_C"));
    if (!gameplay) return {};
    std::string name = cls + ":" + fn + ":" + fname;
    if (es >= 0) name += "#" + std::to_string(es);   // un flux par ennemi (même classe possible)
    return name;
}

// Remplace la valeur de retour par la valeur dérivée ; l'hôte la publie pour vérification.
void replace_draw(HookCtx& ctx, UFunction* f, RandomKind kind, const std::string& stream)
{
    uint32_t key = 0;
    uint16_t rank = 0;
    const double u = next_uniform(stream, key, rank);
    auto num = [&](const wchar_t* name, double& out) {
        int size = 0;
        void* p = ue::param_address(ctx, f, name, &size);
        if (!p) return false;
        if (kind == RandomKind::IntInRange || kind == RandomKind::Int) out = size == 4 ? *static_cast<int32_t*>(p) : static_cast<double>(*static_cast<int64_t*>(p));
        else out = size == 4 ? *static_cast<float*>(p) : *static_cast<double*>(p);
        return true;
    };
    double v = 0, a = 0, b = 0;
    bool ok = true;
    switch (kind) {
    case RandomKind::IntInRange: {
        ok = num(STR("Min"), a) && num(STR("Max"), b);
        const int64_t range = static_cast<int64_t>(b) - static_cast<int64_t>(a) + 1;   // FMath::RandRange : bornes incluses
        v = a + (range > 0 ? std::min<int64_t>(static_cast<int64_t>(u * range), range - 1) : 0);
        if (ok) *static_cast<int32_t*>(ctx.RESULT_DECL) = static_cast<int32_t>(v);
        break;
    }
    case RandomKind::Int:
        ok = num(STR("Max"), b);
        v = b > 0 ? std::min(std::floor(u * b), b - 1) : 0;
        if (ok) *static_cast<int32_t*>(ctx.RESULT_DECL) = static_cast<int32_t>(v);
        break;
    case RandomKind::Float:
        v = u;
        *static_cast<double*>(ctx.RESULT_DECL) = v;
        break;
    case RandomKind::FloatInRange:
        ok = num(STR("Min"), a) && num(STR("Max"), b);
        v = a + (b - a) * u;
        if (ok) *static_cast<double*>(ctx.RESULT_DECL) = v;
        break;
    case RandomKind::Bool:
        v = u < 0.5 ? 1 : 0;
        *static_cast<uint8_t*>(ctx.RESULT_DECL) = static_cast<uint8_t>(v);
        break;
    case RandomKind::BoolWeight:
        ok = num(STR("Weight"), a);
        v = (a > 0 && u <= a) ? 1 : 0;
        if (ok) *static_cast<uint8_t*>(ctx.RESULT_DECL) = static_cast<uint8_t>(v);
        break;
    }
    if (!ok) {
        bump("draw_errors");
        log().write("coop_draw_param_missing", JsonObj().add("stream", stream), s.battle.turn);
        return;
    }
    record_draw(stream, key, rank, v, a, b);
}

bool draw_allowed(const std::string& stream)
{
    if (s.draw_filter.empty()) return true;
    for (auto& f : s.draw_filter) {
        if (f == "none") return false;
        if (stream.find(f) != std::string::npos) return true;
    }
    return false;
}

double natural_value(HookCtx& ctx, RandomKind kind)
{
    switch (kind) {
    case RandomKind::IntInRange: case RandomKind::Int: return *static_cast<int32_t*>(ctx.RESULT_DECL);
    case RandomKind::Float: case RandomKind::FloatInRange: return *static_cast<double*>(ctx.RESULT_DECL);
    default: return *static_cast<uint8_t*>(ctx.RESULT_DECL) ? 1.0 : 0.0;
    }
}

// Client : compare les tirages de gameplay d'un tour terminé avec ceux de l'hôte (même flux, même rang, même valeur).
void compare_draws(uint32_t turn)
{
    auto& mine = s.battle.local_draws[turn];
    auto& host = s.battle.host_draws[turn];
    auto name = [](uint64_t k) {
        auto it = s.battle.stream_names.find(static_cast<uint32_t>(k >> 16));
        return it != s.battle.stream_names.end() ? it->second : std::to_string(k >> 16);
    };
    for (auto& [k, v] : mine) {
        auto it = host.find(k);
        if (it == host.end() || std::fabs(it->second - v) > 1e-9) {
            bump("draw_mismatches");
            log().write("coop_draw_mismatch", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("local", v)
                                                  .add("host_has", it != host.end()).add("host", it != host.end() ? it->second : 0.0), turn);
        }
    }
    for (auto& [k, v] : host) {
        if (mine.count(k)) continue;
        bump("draw_mismatches");
        log().write("coop_draw_mismatch", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("local_has", false).add("host", v), turn);
    }
    s.battle.local_draws.erase(turn);
    s.battle.host_draws.erase(turn);
}

// --- Synchro des tours ---------------------------------------------------------------------

std::vector<std::wstring> party_wide(const std::vector<std::string>& v)
{
    std::vector<std::wstring> out;
    for (auto& x : v) out.push_back(ue::from_utf8(x));
    return out;
}

// Propriétaire d'un héros : -e33owners (emplacement ou nom interne), sinon règle du brief :
// le k-ième héros (ordre des emplacements) va au k-ième joueur connecté, l'hôte prend le reste.
uint8_t default_owner(int index, int spot, const std::string& name)
{
    if (auto it = s.owner_overrides.find(std::to_string(spot)); it != s.owner_overrides.end()) return it->second;
    if (auto it = s.owner_overrides.find(name); it != s.owner_overrides.end()) return it->second;
    if (!s.owner_overrides.empty()) return 0;
    std::vector<uint8_t> players{0};
    for (auto& p : s.peers) if (p.accepted) players.push_back(p.player_id);
    return index < static_cast<int>(players.size()) ? players[static_cast<size_t>(index)] : 0;
}

void begin_turn_common()
{
    ++s.battle.turn;
    s.battle.draw_rank.clear();
    s.battle.hit_seq.clear();
    s.battle.combo_seq.clear();
    s.battle.action_wait_logged = false;
    refresh_enemy_spots();
    s.battle.turn_started_at = s.now;
}

void host_turn_start(UObject* actor)
{
    auto& b = s.battle;
    if (!b.active) {
        b = Battle{};
        b.id = s.next_battle_id++;
        b.active = true;
        proto::BattleStart bs;
        bs.battle_id = b.id;
        bs.encounter = ue::to_utf8(game::last_encounter());
        for (auto& p : game::last_party()) bs.party.push_back(ue::to_utf8(p));
        b.rng_seed = s.fixed_seed ? *s.fixed_seed : splitmix64((static_cast<uint64_t>(GetTickCount64()) << 20) ^ s.session_id ^ b.id);
        bs.rng_seed = b.rng_seed;
        auto ps = game::players();
        std::sort(ps.begin(), ps.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
        for (size_t i = 0; i < ps.size(); ++i) {
            const int spot = game::spot(ps[i]);
            const uint8_t owner = default_owner(static_cast<int>(i), spot, game::hardcoded_name(ps[i]));
            b.owner_by_spot[spot] = owner;
            bs.owners.push_back({static_cast<uint8_t>(spot), game::char_class(ps[i]), owner});
        }
        broadcast(bs);
        log().write("coop_battle_inventory", JsonObj().raw("inventory", game::battle_inventory_json()));
    }
    begin_turn_common();
    const proto::Unit au = actor_unit(actor);
    broadcast(proto::TurnStart{b.id, b.turn, au.kind, au.spot, game::char_class(actor)});
    const proto::StateSync st = sync::snapshot(b.id, b.turn);
    const uint64_t h = sync::hash(st, au);
    b.local_hash[b.turn] = h;
    broadcast(proto::StateHash{b.id, b.turn, h});
    broadcast(st);
    check_hash(b.turn);
}

// Client : test de resynchro (-e33perturb=N) : fausse volontairement l'état local au début du tour N,
// après le contrôle de ce tour ; l'écart doit être détecté et corrigé au tour suivant.
void perturb_state()
{
    for (UObject* e : game::enemies()) {
        if (game::is_dead_fast(e)) continue;
        UObject* st = game::stats(e);
        ue::set_double(st, STR("CurrentHP"), ue::get_double(st, STR("CurrentHP")) - 1.0);
        break;
    }
    for (UObject* h : game::players()) {
        if (game::is_dead_fast(h)) continue;
        UObject* st = game::stats(h);
        ue::set_double(st, STR("CurrentAP"), ue::get_double(st, STR("CurrentAP")) + 1.0);
        break;
    }
    bump("perturbations");
    log().write("coop_perturb", JsonObj().add("what", "enemy hp -1, hero ap +1"), s.battle.turn);
}

void client_turn_start(UObject* actor)
{
    auto& b = s.battle;
    begin_turn_common();
    // L'hôte rythme les tours : attendre son début de tour (TurnStart, StateHash, StateSync, dans cet ordre).
    if (!wait_for([&] { return b.host_sync.count(b.turn) > 0; }, 15.0)) {
        bump("turn_wait_timeouts");
        log().write("coop_turn_wait_timeout", JsonObj().add("battle", b.id), b.turn);
        return;
    }
    const proto::Unit au = actor_unit(actor);
    if (auto it = b.host_turns.find(b.turn); it != b.host_turns.end() && (it->second.kind != au.kind || it->second.index != au.spot)) {
        bump("actor_mismatches");
        log().write("coop_actor_mismatch", JsonObj().add("local", game::char_class(actor)).add("host", it->second.actor), b.turn);
    }
    // Les tirages de l'hôte pour le tour précédent sont arrivés avant son TurnStart (lien ordonné).
    if (b.turn > 1) compare_draws(b.turn - 1);
    const proto::StateSync& host = b.host_sync[b.turn];
    const proto::StateSync mine = sync::snapshot(b.id, b.turn);
    uint64_t h = sync::hash(mine, au);
    const uint64_t host_hash = sync::hash(host, au);
    if (h != host_hash) {
        // La simulation locale a divergé : log détaillé, puis resynchronisation depuis l'état de l'hôte.
        bump("raw_hash_diffs");
        const sync::RepairStats r = sync::repair(mine, host);
        bump("repairs");
        bump("repair_failures", r.failed);
        const proto::StateSync after = sync::snapshot(b.id, b.turn);
        h = sync::hash(after, au);
        log().write("coop_resync", JsonObj().raw("diff", sync::diff(mine, host)).raw("repaired", sync::repair_json(r))
                                       .add("fixed", h == host_hash).raw("remaining", h == host_hash ? "[]" : sync::diff(after, host)), b.turn);
    }
    b.local_hash[b.turn] = h;
    for (auto& p : s.peers) send_to(p, proto::StateHash{b.id, b.turn, h});
    check_hash(b.turn);
    b.host_sync.erase(b.turn);
    if (s.perturb_turn && *s.perturb_turn == b.turn) perturb_state();
}

// Entrées de combat de cette instance (roue d'actions, ciblage) : coupées pendant le tour du héros d'un autre joueur.
void set_local_input(bool enabled, const char* why)
{
    if (enabled == s.input_enabled) return;
    s.input_enabled = enabled;
    ue::Call c(game::battle_manager(), STR("SetBattleInputEnabled"));
    if (c) c.set_bool(STR("Enabled"), enabled).invoke();
    log().write("coop_input", JsonObj().add("enabled", enabled).add("why", why), s.battle.turn);
}

// --- Fin de combat -------------------------------------------------------------------------

// Récompenses : lecture / écriture de FS_BattleRewards (out-param de RollBattleRewards).
proto::BattleRewards read_rewards(void* st_ptr, RC::Unreal::UStruct* st)
{
    using namespace RC::Unreal;
    proto::BattleRewards r;
    r.battle_id = s.battle.id;
    if (FProperty* p = ue::struct_member(st, STR("ExperienceGained"))) r.xp = *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(st_ptr));
    if (FProperty* p = ue::struct_member(st, STR("GoldAcquired"))) r.gold = *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(st_ptr));
    if (auto* ap = CastField<FArrayProperty>(ue::struct_member(st, STR("RolledLootEntries")))) {
        auto* inner = CastField<FStructProperty>(ap->GetInner());
        auto* arr = static_cast<FScriptArray*>(ap->ContainerPtrToValuePtr<void>(st_ptr));
        if (inner && arr) {
            UStruct* es = inner->GetStruct();
            FProperty* id = ue::struct_member(es, STR("ItemID"));
            FProperty* qty = ue::struct_member(es, STR("Quantity"));
            auto* data = static_cast<uint8_t*>(arr->GetData());
            for (int32 i = 0; i < arr->Num() && id && qty; ++i) {
                void* e = data + static_cast<size_t>(i) * inner->GetSize();
                r.loot.push_back({ue::to_utf8(static_cast<FName*>(id->ContainerPtrToValuePtr<void>(e))->ToString()), *static_cast<int32_t*>(qty->ContainerPtrToValuePtr<void>(e))});
            }
        }
    }
    return r;
}

void finalize_battle(const char* why)
{
    auto& b = s.battle;
    if (b.finalized) return;
    if (s.role == Role::Client) {
        // Jamais d'attente ici : on peut être au milieu d'un coup (mort -> fin de combat) dont l'hôte attend le résultat.
        // La clôture d'une fin sans victoire est donc faite dans le tick, une fois le BattleEnd de l'hôte reçu.
        if (b.host_end != b.local_end) {
            bump("end_mismatches");
            log().write("coop_end_mismatch", JsonObj().add("local", b.local_end).add("host", b.host_end));
        }
        compare_draws(b.turn);
    }
    b.finalized = true;
    b.active = false;
    set_local_input(true, "battle end");
    log().write("coop_battle_finalized", JsonObj().add("battle", b.id).add("why", why).add("desyncs", counter("desyncs")));
}

// --- Hooks ---------------------------------------------------------------------------------

UObject* timeline_hero(UObject* timeline)
{
    UObject* ch = ue::get_object(timeline, STR("BattleCharacter"));
    return ch && game::is_player(ch) ? ch : nullptr;
}

// Impose sur ce héros la défense décidée par son propriétaire, juste avant le test du jeu.
void impose_defense(UObject* hero, proto::Defense d, bool jumpable)
{
    s.imposing = true;
    bool started = true;
    switch (d) {
    case proto::Defense::Fail:
        if (game::in_immunity(hero)) game::cancel_defense(hero);
        break;
    case proto::Defense::Parry:
        game::clear_defense_lock(hero);
        started = game::start_parry(hero);
        break;
    case proto::Defense::GradientParry:
        game::clear_defense_lock(hero);
        started = game::start_gradient_parry(hero);
        break;
    case proto::Defense::Dodge:
    case proto::Defense::PerfectDodge: {
        game::clear_defense_lock(hero);
        started = game::start_dodge(hero);
        if (d == proto::Defense::Dodge) {
            // Esquive simple : début reculé au-delà de la fenêtre de parade (sinon le jeu la juge parfaite).
            const double start = ue::get_double(hero, STR("DefensiveMoveImmunityStartTime"));
            ue::set_double(hero, STR("DefensiveMoveImmunityStartTime"), start - game::parry_immune_duration(hero) - 0.05);
        }
        break;
    }
    case proto::Defense::Jump:
        if (!jumpable) break;
        game::clear_defense_lock(hero);
        started = game::start_jump(hero);
        break;
    }
    s.imposing = false;
    if (!started) bump("defense_start_failed");
}

proto::Defense local_result(const LocalHit& lh, bool parried, bool dodged, bool gradient)
{
    if (lh.jumpable) return dodged ? proto::Defense::Jump : proto::Defense::Fail;
    if (parried) return gradient ? proto::Defense::GradientParry : proto::Defense::Parry;
    if (dodged) return lh.perfect_dodge ? proto::Defense::PerfectDodge : proto::Defense::Dodge;
    return proto::Defense::Fail;
}

// Point de défense d'un héros (avant le test du jeu) : héros d'ici -> le résultat sera publié après le test ;
// héros d'un autre joueur -> attendre son résultat (relayé par l'hôte) et l'imposer.
void defense_point(UObject* stats_comp, UObject* hero, bool jumpable)
{
    auto& b = s.battle;
    const int spot = game::spot(hero);
    const uint16_t hit = ++b.hit_seq[spot];
    if (controls(spot)) {
        b.local_hits[stats_comp] = {spot, hit, jumpable, false};
        return;
    }
    const uint8_t owner = owner_of_spot(spot);
    if (s.role == Role::Host)
        for (auto& p : s.peers)
            if (p.accepted && p.player_id == owner) send_to(p, proto::DefenseRequest{b.id, b.turn, static_cast<uint8_t>(spot), hit, static_cast<uint8_t>(jumpable), 3000});
    const TurnSpotSeq key{b.turn, spot, hit};
    const ULONGLONG t0 = GetTickCount64();
    const bool got = wait_for([&] { return b.defenses.count(key) > 0; }, kDefenseTimeout);
    const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
    proto::Defense res = proto::Defense::Fail;
    if (got) res = b.defenses[key];
    else bump("defense_timeouts");
    impose_defense(hero, res, jumpable);
    b.imposed_check[stats_comp] = static_cast<int>(res);
    bump("defenses_imposed");
    log().write("coop_defense_imposed", JsonObj().add("spot", spot).add("hit", static_cast<int>(hit)).add("owner", static_cast<int>(owner)).add("jumpable", jumpable)
                                         .add("result", static_cast<int>(res)).add("received", got).add("waited_ms", waited_ms), b.turn);
}

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();

    // Début de tour : hôte -> TurnStart + StateHash + StateSync ; client -> contrôle et resynchro éventuelle.
    bus.post(fn(game::path::BattleChar, L"OnTurnStart"), [](HookCtx& ctx, UFunction*) {
        if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;   // contre : pas un tour
        if (s.role == Role::Host) host_turn_start(ctx.Context);
        else if (s.role == Role::Client && s.battle.active) client_turn_start(ctx.Context);
        // Tour du héros d'un autre joueur : pas de choix d'action ici (son action arrive par le réseau).
        if (s.role != Role::Solo && s.battle.active && s.lock_input)
            set_local_input(!game::is_player(ctx.Context) || controls(game::spot(ctx.Context)), "turn start");
    });

    // Passer le tour / fuir (chemins de l'interface, sans RequestActionExecution) : publiés comme les autres actions.
    for (const auto& [fn_name, type] : {std::pair{L"OnSelectSkip", game::ActSkip}, std::pair{L"OnSelectFlee", game::ActFlee}}) {
        const int t = type;
        bus.pre(fn(game::path::BattleMgr, fn_name), [t](HookCtx&, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active) return;
            UObject* cur = game::current_character();
            if (!cur || !game::is_player(cur) || !controls(game::spot(cur))) return;
            proto::ActionCommand ac;
            ac.battle_id = s.battle.id;
            ac.turn = s.battle.turn;
            ac.actor_spot = static_cast<uint8_t>(game::spot(cur));
            ac.action_type = static_cast<uint8_t>(t);
            bump("actions_published");
            publish(ac);
        });
    }

    // Action choisie ici pour un héros contrôlé ici (test runner ou joueur) : publiée telle quelle.
    bus.pre(fn(game::path::BattleMgr, L"RequestActionExecution"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* cur = game::current_character();
        if (!cur || !game::is_player(cur) || !controls(game::spot(cur))) return;
        UObject* bm = game::battle_manager();
        proto::ActionCommand ac;
        ac.battle_id = s.battle.id;
        ac.turn = s.battle.turn;
        ac.actor_spot = static_cast<uint8_t>(game::spot(cur));
        ac.action_type = static_cast<uint8_t>(ue::get_int(bm, STR("SelectedAction"), 1));
        if (ac.action_type == game::ActItem || ac.action_type == game::ActSkill) ac.secondary = ue::get_name_string(bm, STR("SelectedSecondaryAction"));
        if (auto* arr = static_cast<RC::Unreal::FScriptArray*>(ue::param_address(ctx, f, STR("TargetedCharacters")))) {
            auto** data = static_cast<UObject**>(arr->GetData());
            for (int32_t i = 0; i < arr->Num(); ++i) ac.targets.push_back(sync::unit_of(data[i]));
        }
        bump("actions_published");
        publish(ac);
    });

    // QTE offensifs (D11). Imposer « réussi/parfait » en fin de fenêtre ne reproduit pas un vrai appui (TryActivate a
    // d'autres effets : un coup de plus vu le 2026-10-19). Donc : le propriétaire publie son résultat ET l'instant de son
    // appui dans la fenêtre ; les autres instances, qui jouent l'action ~1 s plus tard, rejouent un vrai appui au même
    // instant (« Try Activate Skill Combo »), avec le verdict « parfait » forcé dans IsInPerfectWindow.
    bus.post(fn(game::path::Timeline, L"OnSkillComboWindowStartReached"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* hero = timeline_hero(ctx.Context);
        if (!hero) return;
        auto& b = s.battle;
        Window w;
        w.hero = hero;
        w.spot = game::spot(hero);
        w.seq = ++b.combo_seq[w.spot];
        w.start = SteadyClock::now();
        w.duration = ue::get_double(ctx.Context, STR("CurrentWindowEndRealTime")) - ue::get_double(ctx.Context, STR("CurrentWindowStartRealTime"));
        w.local = controls(w.spot);
        if (!w.local) {
            const TurnSpotSeq key{b.turn, w.spot, w.seq};
            const ULONGLONG t0 = GetTickCount64();
            const bool got = wait_for([&] { return b.combos.count(key) > 0; }, kComboTimeout);
            const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
            if (got) w.plan = b.combos[key];
            else { bump("combo_timeouts"); w.plan = proto::ComboResult{b.id, b.turn, static_cast<uint8_t>(w.spot), w.seq, false, false, proto::kNoPress}; }
            if (w.plan.success && w.plan.press <= 1000)
                b.presses.push_back({ctx.Context, w.start + std::chrono::microseconds(static_cast<int64_t>(w.duration * w.plan.press * 1000.0))});
            log().write("coop_combo_plan", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", w.plan.success).add("perfect", w.plan.perfect)
                                               .add("press", static_cast<int>(w.plan.press)).add("received", got).add("waited_ms", waited_ms), b.turn);
        }
        b.windows[ctx.Context] = w;
    });
    // Héros d'ici : le premier appui réussi est publié tout de suite, avec son instant dans la fenêtre.
    bus.post(fn(game::path::Timeline, L"TryActivate"), [](HookCtx& ctx, UFunction* f) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end()) return;
        Window& w = it->second;
        const bool ok = ue::read_out_bool(ctx, f, STR("Success")).value_or(false);
        const bool perfect = ue::read_out_bool(ctx, f, STR("Perfect")).value_or(false);
        if (!w.local) {
            w.pressed = true;
            if (ok != w.plan.success || perfect != w.plan.perfect) {
                bump("combo_verdict_mismatches");
                log().write("coop_combo_verdict_mismatch", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", ok).add("perfect", perfect), s.battle.turn);
            }
            return;
        }
        if (w.published || !ok) return;
        const double elapsed = std::chrono::duration<double>(SteadyClock::now() - w.start).count();
        const double frac = w.duration > 0 ? std::clamp(elapsed / w.duration, 0.0, 1.0) : 0.5;
        const proto::ComboResult cr{s.battle.id, s.battle.turn, static_cast<uint8_t>(w.spot), w.seq, true, perfect, static_cast<uint16_t>(std::lround(frac * 1000))};
        w.published = true;
        bump("combos_published");
        log().write("coop_combo_local", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", true).add("perfect", perfect).add("press", static_cast<int>(cr.press)), s.battle.turn);
        publish(cr);
    });
    // Verdict « parfait » d'un appui rejoué : celui du propriétaire (la gigue d'image ne doit pas le changer).
    bus.post(fn(game::path::Timeline, L"IsInPerfectWindow"), [](HookCtx& ctx, UFunction* f) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end() || it->second.local) return;
        ue::write_out_bool(ctx, f, STR("ReturnValue"), it->second.plan.perfect);
    });
    bus.pre(fn(game::path::Timeline, L"OnSkillComboWindowEndReached"), [](HookCtx& ctx, UFunction*) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end() || it->second.local) return;
        Window& w = it->second;
        if (w.plan.success && w.plan.press <= 1000 && !w.pressed) {
            // Appui pas encore rejoué (image trop tardive) : maintenant, avant que la fenêtre ne se ferme.
            std::erase_if(s.battle.presses, [&](const Press& p) { return p.timeline == ctx.Context; });
            ue::Call c(w.hero, STR("Try Activate Skill Combo"));
            if (c) c.invoke();
            bump("combo_late_presses");
        }
        if (w.plan.press == proto::kAutoPress) {   // réussite automatique chez le propriétaire : pas d'appui à rejouer
            ue::set_bool(ctx.Context, STR("IsSkillComboSuccessful"), w.plan.success);
            ue::set_bool(ctx.Context, STR("IsSkillComboPerfect"), w.plan.perfect);
        }
        bump("combos_replayed");
    });
    bus.post(fn(game::path::Timeline, L"OnSkillComboWindowEndReached"), [](HookCtx& ctx, UFunction*) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end()) return;
        Window w = it->second;
        s.battle.windows.erase(it);
        const bool ok = ue::get_bool(ctx.Context, STR("IsSkillComboSuccessful"));
        const bool perfect = ue::get_bool(ctx.Context, STR("IsSkillComboPerfect"));
        if (w.local) {
            if (w.published) return;
            // Pas d'appui réussi : échec, ou réussite automatique (option d'accessibilité, mode test).
            const proto::ComboResult cr{s.battle.id, s.battle.turn, static_cast<uint8_t>(w.spot), w.seq, ok || perfect, perfect, ok ? proto::kAutoPress : proto::kNoPress};
            bump("combos_published");
            log().write("coop_combo_local", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", cr.success).add("perfect", perfect).add("press", static_cast<int>(cr.press)), s.battle.turn);
            publish(cr);
            return;
        }
        if (ok != w.plan.success || perfect != w.plan.perfect) {
            bump("combo_verdict_mismatches");
            log().write("coop_combo_verdict_mismatch", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", ok).add("perfect", perfect)
                                                        .add("want_success", w.plan.success).add("want_perfect", w.plan.perfect), s.battle.turn);
        }
    });

    // Défenses : TryParryHitFromCharacter (coups parables) ou ReceiveDamageFromObject (coups sautables).
    bus.pre(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* hero = ue::component_owner(ctx.Context);
        if (!hero || !game::is_player(hero)) return;
        defense_point(ctx.Context, hero, false);
    });
    bus.pre(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* hero = ue::component_owner(ctx.Context);
        if (!hero || !game::is_player(hero)) return;
        if (!game::incoming_attack_jumpable(ue::read_param_object(ctx, f, STR("DamageBuilder")))) return;
        defense_point(ctx.Context, hero, true);
    });
    bus.post(fn(game::path::Stats, L"TryDodgeHitFromCharacter"), [](HookCtx& ctx, UFunction* f) {
        auto it = s.battle.local_hits.find(ctx.Context);
        if (it == s.battle.local_hits.end()) return;
        it->second.perfect_dodge = ue::read_out_bool(ctx, f, STR("IsPerfectDodge")).value_or(false);
    });
    bus.post(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, UFunction* f) {
        auto& b = s.battle;
        const bool parried = ue::read_out_bool(ctx, f, STR("ParrySuccess")).value_or(false);
        const bool dodged = ue::read_out_bool(ctx, f, STR("DodgeSuccess")).value_or(false);
        if (auto it = b.local_hits.find(ctx.Context); it != b.local_hits.end()) {
            const LocalHit lh = it->second;
            b.local_hits.erase(it);
            UObject* hero = ue::component_owner(ctx.Context);
            const proto::Defense res = local_result(lh, parried, dodged, ue::get_bool(hero, STR("IsGradientParrying")));
            bump("defenses_published");
            log().write("coop_defense_local", JsonObj().add("spot", lh.spot).add("hit", static_cast<int>(lh.hit)).add("result", static_cast<int>(res)), b.turn);
            publish(proto::DefenseResult{b.id, b.turn, static_cast<uint8_t>(lh.spot), lh.hit, res});
            return;
        }
        // Héros d'un autre joueur : le verdict du jeu doit être celui imposé.
        if (auto it = b.imposed_check.find(ctx.Context); it != b.imposed_check.end()) {
            const auto want = static_cast<proto::Defense>(it->second);
            b.imposed_check.erase(it);
            const bool want_parry = want == proto::Defense::Parry || want == proto::Defense::GradientParry;
            const bool want_dodge = want == proto::Defense::Dodge || want == proto::Defense::PerfectDodge || want == proto::Defense::Jump;
            if (parried != want_parry || dodged != want_dodge) {
                bump("defense_verdict_mismatches");
                log().write("coop_defense_verdict_mismatch", JsonObj().add("want", static_cast<int>(want)).add("parried", parried).add("dodged", dodged), b.turn);
            }
        }
    });

    // Entrées de défense parasites sur un héros d'un autre joueur (ex. parade d'un joueur sur une attaque de groupe) :
    // la fenêtre est annulée, seul le résultat du propriétaire compte.
    for (const wchar_t* name : {L"TryStartParry", L"TryStartDodge", L"TryStartJump", L"TryStartGradientParry"}) {
        bus.post(fn(game::path::BattleChar, name), [name](HookCtx& ctx, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active || s.imposing) return;
            UObject* hero = ctx.Context;
            if (!game::is_player(hero) || controls(game::spot(hero))) return;
            game::cancel_defense(hero);
            bump("stray_defenses_undone");
            log().write("coop_stray_defense", JsonObj().add("fn", ue::to_utf8(name)).add("spot", game::spot(hero)), s.battle.turn);
        });
    }

    // NE PAS hooker KismetArrayLibrary:Array_Random (custom thunk à paramètres génériques) : le hook UE4SS casse
    // GetRandomAliveHero (GAME-INTERNALS §15). Le ciblage aléatoire est synchronisé plus bas, sur les helpers BP.

    // Tirages de gameplay (D10) : valeur dérivée de la graine de l'hôte, du flux et du rang dans le tour.
    static const RandomFn random_fns[] = {
        {L"RandomIntegerInRange", RandomKind::IntInRange}, {L"RandomInteger", RandomKind::Int}, {L"RandomFloat", RandomKind::Float},
        {L"RandomFloatInRange", RandomKind::FloatInRange}, {L"RandomBool", RandomKind::Bool}, {L"RandomBoolWithWeight", RandomKind::BoolWeight},
    };
    for (const auto& rf : random_fns) {
        const RandomKind kind = rf.kind;
        const std::string fname = ue::to_utf8(rf.name);
        bus.post(std::wstring(L"/Script/Engine.KismetMathLibrary:") + rf.name, [kind, fname](HookCtx& ctx, UFunction* f) {
            if (s.role == Role::Solo) return;
            // Threads d'animation, chargement… : jamais un tirage de gameplay, et aucun accès au jeu permis ici.
            if (!on_game_thread()) { bump("off_thread_draws"); return; }
            trace_random(ctx, fname);
            if (!s.battle.active || s.battle.turn == 0 || !ctx.RESULT_DECL) return;
            const std::string stream = gameplay_stream(ctx, fname);
            if (!stream.empty() && draw_allowed(stream)) replace_draw(ctx, f, kind, stream);
            else if (!stream.empty()) log().write("coop_draw_natural", JsonObj().add("stream", stream).add("value", natural_value(ctx, kind)), s.battle.turn);
        });
    }

    // Ciblage aléatoire des ennemis (Array_Random sur les héros vivants) : remplacé par un tirage dérivé de la graine.
    // GetSpecificHero / GetMiddleHero ne tirent au hasard que si le héros voulu est mort.
    struct TargetFn { const wchar_t* name; const wchar_t* out; const wchar_t* spot_param; };
    static const TargetFn target_fns[] = {
        {L"GetRandomAliveHero", L"Output", nullptr},
        {L"GetSpecificHero", L"Character", L"SpotIndex"},
    };
    for (const auto& tf : target_fns) {
        const TargetFn t = tf;
        bus.post(fn(game::path::Targeting, t.name), [t](HookCtx& ctx, UFunction* f) {
            if (s.role == Role::Solo || !s.battle.active || s.battle.turn == 0) return;
            UObject* natural = ue::read_out_object(ctx, f, t.out);
            if (!natural) return;
            if (t.spot_param) {
                const auto want = ue::read_param_int(ctx, f, t.spot_param);
                if (want && *want == game::spot(natural)) return;   // héros voulu vivant : pas de hasard
            }
            std::vector<UObject*> alive;
            for (UObject* h : game::players()) if (!game::is_dead_fast(h)) alive.push_back(h);
            std::sort(alive.begin(), alive.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
            if (alive.empty()) return;
            int caller_spot = -1;   // ennemi appelant (AcquireTargetRandomHero…), pour un flux par ennemi
            RC::Unreal::FFrame* fr = ctx.TheStack.PreviousFrame();
            for (int depth = 0; fr && caller_spot < 0 && depth < 6; ++depth) {
                caller_spot = cached_enemy_spot(fr->Object());
                RC::Unreal::FFrame* prev = fr->PreviousFrame();
                fr = prev == fr ? nullptr : prev;
            }
            const std::string stream = "Targeting:" + ue::to_utf8(t.name) + "#" + std::to_string(caller_spot);
            uint32_t key = 0;
            uint16_t rank = 0;
            const double u = next_uniform(stream, key, rank);
            UObject* pick = alive[std::min(alive.size() - 1, static_cast<size_t>(u * static_cast<double>(alive.size())))];
            ue::write_out_object(ctx, f, t.out, pick);
            record_draw(stream, key, rank, game::spot(pick), game::spot(natural), static_cast<double>(alive.size()));
            bump("targets_synced");
        });
    }
    bus.post(fn(game::path::Targeting, L"GetRandomCharacterByBuffCritera"), [](HookCtx&, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        bump("unsynced_random_targeting");
        log().write("coop_unsynced_targeting", JsonObj().add("fn", "GetRandomCharacterByBuffCritera"), s.battle.turn);
    });

    // Invocations : les nouveaux ennemis entrent dans le cache (leurs tirages d'IA sont de gameplay).
    for (const wchar_t* name : {L"SummonMinions", L"SummonSingleMinion"})
        bus.post(fn(game::path::BattleChar, name), [](HookCtx&, UFunction*) { if (s.battle.active) refresh_enemy_spots(); });

    // Récompenses : l'hôte publie les siennes ; le client compare (mêmes tirages, D10) et prend celles de l'hôte.
    bus.post(fn(game::path::BattleMgr, L"RollBattleRewards"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        RC::Unreal::FProperty* prop = nullptr;
        void* addr = ue::out_param_address(ctx, f, STR("BattleRewards"), &prop);
        auto* sp = RC::Unreal::CastField<RC::Unreal::FStructProperty>(prop);
        if (!addr || !sp) { bump("rewards_unreadable"); return; }
        proto::BattleRewards mine = read_rewards(addr, sp->GetStruct());
        log().write("coop_rewards", JsonObj().raw("rewards", proto::to_json(mine)), s.battle.turn);
        if (s.role == Role::Host) {
            broadcast(mine);
        } else {
            auto& b = s.battle;
            wait_for([&] { return b.host_rewards.has_value(); }, 5.0);
            bump("rewards_checked");
            bool same = b.host_rewards && b.host_rewards->xp == mine.xp && b.host_rewards->gold == mine.gold && b.host_rewards->loot.size() == mine.loot.size();
            for (size_t i = 0; same && i < mine.loot.size(); ++i)
                same = mine.loot[i].item == b.host_rewards->loot[i].item && mine.loot[i].quantity == b.host_rewards->loot[i].quantity;
            if (!same) {
                bump("rewards_mismatches");
                log().write("coop_rewards_mismatch", JsonObj().raw("local", proto::to_json(mine)).raw("host", b.host_rewards ? proto::to_json(*b.host_rewards) : "null"));
                if (b.host_rewards) {
                    if (auto* p = ue::struct_member(sp->GetStruct(), STR("ExperienceGained"))) *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(addr)) = b.host_rewards->xp;
                    if (auto* p = ue::struct_member(sp->GetStruct(), STR("GoldAcquired"))) *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(addr)) = b.host_rewards->gold;
                }
            }
        }
        finalize_battle("rewards");
    });

    bus.pre(fn(game::path::BattleMgr, L"StartBattleEndFlow"), [](HookCtx& ctx, UFunction* f) {
        if (!s.battle.active || s.battle.ending) return;
        const int result = static_cast<int>(ue::read_param_int(ctx, f, STR("BattleEndResult")).value_or(-1));
        s.battle.ending = true;
        s.battle.local_end = result;
        if (s.role == Role::Host) broadcast(proto::BattleEnd{s.battle.id, static_cast<uint8_t>(std::clamp(result, 0, 3))});
        log().write("coop_battle_end", JsonObj().add("battle", s.battle.id).add("result", result).add("desyncs", counter("desyncs")));
        // Sans victoire, pas de récompenses : la fin est traitée tout de suite.
        if (result != game::EndVictory) s.battle.end_flow_at = s.now;
    });
}

} // namespace

Role role() { return s.role; }
bool active() { return s.role != Role::Solo; }
int connected_players()
{
    if (s.role == Role::Client) return s.accepted && !s.rejected ? 1 : 0;
    int n = 0;
    for (auto& p : s.peers) if (p.accepted) ++n;
    return n;
}
bool session_ok() { return s.role == Role::Solo || (s.role == Role::Host ? true : (s.accepted && !s.rejected)); }
bool battle_finalized() { return s.battle.finalized; }
const std::map<std::string, int>& counters() { return s.counters; }
int counter(const std::string& name)
{
    auto it = s.counters.find(name);
    return it == s.counters.end() ? 0 : it->second;
}

bool controls_spot(int spot)
{
    if (s.role == Role::Solo) return true;
    if (!s.battle.active) return s.role == Role::Host;
    return controls(spot);
}

void init()
{
    const std::wstring r = arg(L"e33role");
    if (r == L"host") s.role = Role::Host;
    else if (r == L"client") s.role = Role::Client;
    else return;

    for (const char* c : {"desyncs", "raw_hash_diffs", "hashes_compared", "gameplay_draws", "draw_mismatches", "draw_errors", "invalid_messages",
                          "defense_timeouts", "combo_timeouts", "turn_wait_timeouts", "actor_mismatches", "end_mismatches", "rewards_mismatches",
                          "repair_failures", "defense_verdict_mismatches", "combo_verdict_mismatches", "remote_actions"})
        s.counters[c] = 0;

    s.game_build = read_build_id();
    s.gameplay_mods = read_gameplay_mods();
    s.session_id = GetTickCount();
    if (const std::wstring seed = arg(L"e33seed"); !seed.empty()) s.fixed_seed = std::wcstoull(seed.c_str(), nullptr, 0);
    if (const std::wstring d = arg(L"e33actiondelay"); !d.empty()) s.action_delay_s = std::wcstod(d.c_str(), nullptr);
    if (const std::wstring p = arg(L"e33perturb"); !p.empty()) s.perturb_turn = static_cast<uint32_t>(std::wcstoul(p.c_str(), nullptr, 10));
    if (arg(L"e33lockinput") == L"0") s.lock_input = false;
    if (const std::wstring l = arg(L"e33replaylag"); !l.empty()) s.replay_lag_s = std::wcstod(l.c_str(), nullptr);
    {
        std::stringstream ds(ue::to_utf8(arg(L"e33draws")));
        std::string part;
        while (std::getline(ds, part, ',')) if (!part.empty()) s.draw_filter.push_back(part);
    }
    {
        std::stringstream os(ue::to_utf8(arg(L"e33owners")));
        std::string part;
        while (std::getline(os, part, ',')) {
            const auto c = part.find(':');
            if (c != std::string::npos) s.owner_overrides[part.substr(0, c)] = static_cast<uint8_t>(std::atoi(part.substr(c + 1).c_str()));
        }
    }
    std::string err;
    const std::wstring port_s = arg(L"e33port");
    const uint16_t port = static_cast<uint16_t>(port_s.empty() ? (s.role == Role::Host ? 7777 : 0) : std::stoi(port_s));
    s.transport = net::make_udp(port, err);
    if (!s.transport) { log().write("coop_error", JsonObj().add("what", err)); s.role = Role::Solo; return; }
    s.recorder.open(ue::to_utf8((std::filesystem::path(launch_args().run_dir) / (s.role == Role::Host ? L"recording-host.jsonl" : L"recording-client.jsonl")).wstring()));
    log().set_role(s.role == Role::Host ? "host" : "client");

    std::string mods = "[";
    for (auto& m : s.gameplay_mods) { if (mods.size() > 1) mods += ","; mods += "\"" + json_escape(m) + "\""; }
    log().write("coop_init", JsonObj().add("role", s.role == Role::Host ? "host" : "client").add("local", s.transport->local_address())
                                 .add("game_build", s.game_build).add("mod_version", kModVersion).raw("gameplay_mods", mods + "]"));

    if (s.role == Role::Client) {
        s.connect_to = ue::to_utf8(arg(L"e33connect"));
        Peer host;
        host.link = std::make_unique<net::Link>(*s.transport, s.connect_to);
        host.accepted = true;
        host.link->on_invalid = [](const std::string& why) { bump("invalid_messages"); log().write("net_invalid", JsonObj().add("why", why)); };
        host.link->on_sent = [](const proto::Message& m, bool) { s.recorder.write("c2h", m); log().write("net_send", JsonObj().raw("msg", proto::to_json(m))); };
        host.link->on_delivered = [](const proto::Message& m) { s.recorder.write("h2c", m); log().write("net_recv", JsonObj().raw("msg", proto::to_json(m))); };
        s.peers.push_back(std::move(host));
        proto::Hello h;
        h.game_build = s.game_build;
        h.mod_version = kModVersion;
        h.gameplay_mods = s.gameplay_mods;
        h.player_name = "client";
        h.instance_id = GetTickCount64();
        send_to(s.peers.front(), h);
    }
    subscribe();
}

// Client : lance le combat reçu de l'hôte quand le monde local est chargé et stable depuis quelques secondes.
void start_pending_battle()
{
    if (!s.pending_start || s.now < s.next_trigger_try) return;
    constexpr double kStep = 0.5;   // FindFirstOf parcourt tous les objets : pas à chaque image
    s.next_trigger_try = s.now + kStep;
    const bool ready = game::controller() && !game::map_change_in_progress() && !game::main_menu();
    s.world_ready_s = ready ? s.world_ready_s + kStep : 0;
    if (s.world_ready_s < 5.0) return;
    const proto::BattleStart bs = *s.pending_start;
    std::string err;
    if (!game::in_battle() && !game::trigger_battle(ue::from_utf8(bs.encounter), party_wide(bs.party), err)) {
        log().write("coop_error", JsonObj().add("what", "trigger_battle: " + err));
        s.next_trigger_try = s.now + 2.0;
        return;
    }
    s.pending_start.reset();
    s.battle.active = true;
    log().write("coop_battle_start", JsonObj().add("battle", bs.battle_id).add("encounter", bs.encounter));
}

// Tour d'un héros contrôlé par un autre joueur : exécuter sa commande (reçue de lui ou relayée par l'hôte).
void run_remote_action()
{
    auto& b = s.battle;
    if (!b.active || b.ending) return;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur)) return;
    const int spot = game::spot(cur);
    if (controls(spot)) return;
    const std::pair<uint32_t, int> key{b.turn, spot};
    if (b.executed.count(key)) return;
    auto it = b.commands.find(key);
    if (it == b.commands.end()) {
        if (!b.action_wait_logged && s.now - b.turn_started_at > 30.0) {
            b.action_wait_logged = true;
            bump("action_waits_long");
            log().write("coop_action_wait", JsonObj().add("spot", spot).add("waited_s", s.now - b.turn_started_at), b.turn);
        }
        return;
    }
    if (s.now - b.turn_started_at < s.action_delay_s) return;
    if (s.now - b.command_time[key] < s.replay_lag_s) return;
    const proto::ActionCommand cmd = it->second;
    b.executed.insert(key);
    std::vector<UObject*> targets;
    bool auto_target = cmd.targets.empty();
    for (auto& t : cmd.targets) {
        if (t.spot == proto::kAutoSpot) { auto_target = true; continue; }
        if (UObject* o = game::find_unit(t.kind == proto::ActorKind::Hero, t.spot)) targets.push_back(o);
    }
    const std::wstring secondary = ue::from_utf8(cmd.secondary);
    // Une commande que le jeu du propriétaire n'aurait pas permise (objet épuisé, PA insuffisants) signale un écart.
    {
        int targeting = -1, cost = 0;
        const bool no_item = cmd.action_type == game::ActItem && game::battle_item_count(secondary) <= 0;
        const bool no_ap = cmd.action_type == game::ActSkill && game::skill_info(cur, secondary, targeting, cost) &&
                           ue::get_double(game::stats(cur), STR("CurrentAP")) < cost;
        if (no_item || no_ap) {
            bump("remote_action_invalid");
            log().write("coop_remote_action_invalid", JsonObj().add("spot", spot).add("secondary", cmd.secondary).add("no_item", no_item).add("no_ap", no_ap), b.turn);
        }
    }
    if (auto_target) targets = game::auto_targets(cur, cmd.action_type, secondary);
    std::string err;
    // Pas de réussite automatique des QTE : chaque fenêtre reçoit le résultat du propriétaire.
    if (!game::perform_action(cmd.action_type, secondary, targets, 0, err)) {
        bump("remote_action_errors");
        log().write("coop_error", JsonObj().add("what", "perform_action: " + err), b.turn);
        return;
    }
    bump("remote_actions");
    std::string ts = "[";
    for (UObject* t : targets) { if (ts.size() > 1) ts += ","; ts += "\"" + game::char_class(t) + "#" + std::to_string(game::spot(t)) + "\""; }
    log().write("coop_remote_action", JsonObj().add("spot", spot).add("action", static_cast<int>(cmd.action_type)).add("secondary", cmd.secondary)
                                         .raw("targets", ts + "]").add("auto_target", auto_target), b.turn);
}

void tick(double dt)
{
    if (s.role == Role::Solo) return;
    s.game_thread = GetCurrentThreadId();
    s.now += dt;
    pump_network();
    if (s.role == Role::Client) start_pending_battle();
    run_remote_action();
    // Fin sans victoire : clôture dès que le résultat de l'hôte est là (8 s au plus).
    if (auto& b = s.battle; b.end_flow_at >= 0 && !b.finalized && (s.role == Role::Host || b.host_end >= 0 || s.now - b.end_flow_at > 8.0))
        finalize_battle("end_flow");
    // Appuis de QTE rejoués pour les héros des autres joueurs (instant de leur propriétaire).
    auto& presses = s.battle.presses;
    for (size_t i = 0; i < presses.size();) {
        if (SteadyClock::now() < presses[i].at) { ++i; continue; }
        const Press p = presses[i];
        presses.erase(presses.begin() + static_cast<std::ptrdiff_t>(i));
        auto it = s.battle.windows.find(p.timeline);
        if (it == s.battle.windows.end() || it->second.pressed) continue;
        ue::Call c(it->second.hero, STR("Try Activate Skill Combo"));
        if (c) c.invoke();
        bump("combo_presses_replayed");
    }
}

} // namespace e33::coop
