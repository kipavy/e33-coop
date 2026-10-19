#include "coop.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

using RC::Unreal::UObject;
using RC::Unreal::UFunction;

namespace e33::coop {

namespace {

constexpr const char* kModVersion = "0.2.0";

struct Peer {
    std::unique_ptr<net::Link> link;
    uint8_t player_id = 0;
    bool accepted = false;
    std::string name;
};

struct Battle {
    uint32_t id = 0;
    uint32_t turn = 0;
    bool active = false;
    std::map<int, uint8_t> owner_by_spot;               // emplacement de héros -> player_id
    std::map<uint8_t, uint16_t> hit_counter;            // coups reçus par joueur (hôte : demandes ; client : résultats)
    std::map<uint64_t, proto::DefenseResult> defense_results;   // hôte : (player<<16|hit) -> résultat reçu
    std::map<uint32_t, uint64_t> local_hash;            // tour -> hash local
    std::map<uint32_t, uint64_t> remote_hash;           // tour -> hash de l'hôte (client) / du client (hôte)
    std::optional<proto::ActionCommand> pending_command;   // hôte : commande reçue pour le tour courant
    std::vector<proto::ActionResult> pending_results;   // client : résultats à appliquer
    uint32_t host_turn_seen = 0;                        // client : dernier TurnStart reçu de l'hôte
    std::map<UObject*, uint16_t> local_defense_hit;     // client : coup en cours par composant de stats
    std::map<UObject*, uint16_t> local_jump_hit;        // client : coup sautable en cours
    uint16_t draw_index = 0;                            // tirages d'IA ennemie dans le tour courant
    std::map<uint64_t, double> host_draws;              // client : (tour<<16|rang) -> valeur tirée par l'hôte
    int traced_draws = 0;                               // diagnostic : tirages loggés dans ce combat
    std::vector<UObject*> enemies_cache;                // ennemis au dernier début de tour (comparaison de pointeurs seulement)
};

struct State {
    Role role = Role::Solo;
    std::unique_ptr<net::Transport> transport;
    std::vector<Peer> peers;                             // hôte : clients ; client : [hôte]
    net::Recorder recorder;
    uint8_t my_player_id = 0;                            // hôte = 0
    uint32_t session_id = 0;
    uint32_t game_build = 0;
    std::vector<std::string> gameplay_mods;
    std::string connect_to;
    bool accepted = false;
    bool rejected = false;
    int invalid = 0;
    int desyncs = 0;
    double now = 0;
    Battle battle;
    uint32_t next_battle_id = 1;
    std::map<int, uint8_t> owner_overrides;              // -e33owners=0:1,1:0  (emplacement:joueur)
    DWORD game_thread = 0;                               // fixé par tick()
    std::optional<proto::BattleStart> pending_start;     // client : combat de l'hôte, lancé une fois le monde prêt
    double world_ready_s = 0;                            // client : durée depuis laquelle le monde est stable
    double next_trigger_try = 0;
    int off_thread_draws = 0;                            // tirages hors thread de jeu (ignorés)
    int hp_overrides = 0;                                // client : PV locaux corrigés par ceux de l hôte (écart réel)
    int hashes_compared = 0;
};

State s;

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

int hero_spot(UObject* ch) { return static_cast<int>(ue::get_int(ch, STR("BattleSpotIndex"), -1)); }

int enemy_index(UObject* ch)
{
    auto es = game::enemies();
    auto it = std::find(es.begin(), es.end(), ch);
    return it == es.end() ? -1 : static_cast<int>(it - es.begin());
}

// Version sans appel au jeu (pour les hooks très fréquents) : liste figée au début du tour.
int cached_enemy_index(UObject* ch)
{
    const auto& es = s.battle.enemies_cache;
    auto it = std::find(es.begin(), es.end(), ch);
    return it == es.end() ? -1 : static_cast<int>(it - es.begin());
}

uint8_t owner_of_spot(int spot)
{
    auto it = s.battle.owner_by_spot.find(spot);
    return it == s.battle.owner_by_spot.end() ? 0 : it->second;
}

uint64_t state_hash(UObject* actor)
{
    wire::Fnv64 h;
    auto add_char = [&](UObject* c) {
        UObject* st = game::stats(c);
        h.i64(std::llround(ue::get_double(st, STR("CurrentHP"))));
        h.i64(std::llround(ue::get_double(st, STR("CurrentAP"))));
        h.i64(ue::get_int(st, STR("CurrentShieldPoints")));
        h.i64(game::is_dead(c) ? 1 : 0);
    };
    auto ps = game::players();
    std::sort(ps.begin(), ps.end(), [](UObject* a, UObject* b) { return hero_spot(a) < hero_spot(b); });
    for (UObject* c : ps) add_char(c);
    for (UObject* c : game::enemies()) add_char(c);
    h.str(game::char_class(actor));
    return h.value();
}

std::string state_detail()
{
    std::string out = "[";
    auto add = [&](UObject* c) {
        if (out.size() > 1) out += ",";
        UObject* st = game::stats(c);
        out += JsonObj().add("c", game::char_class(c)).add("hp", ue::get_double(st, STR("CurrentHP"))).add("ap", ue::get_double(st, STR("CurrentAP")))
                   .add("shield", ue::get_int(st, STR("CurrentShieldPoints"))).add("dead", game::is_dead(c)).str();
    };
    for (UObject* c : game::players()) add(c);
    for (UObject* c : game::enemies()) add(c);
    return out + "]";
}

void check_hash(uint32_t turn)
{
    auto l = s.battle.local_hash.find(turn);
    auto r = s.battle.remote_hash.find(turn);
    if (l == s.battle.local_hash.end() || r == s.battle.remote_hash.end()) return;
    ++s.hashes_compared;
    char a[24], b[24];
    std::snprintf(a, sizeof a, "%016llx", static_cast<unsigned long long>(l->second));
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(r->second));
    if (l->second != r->second) {
        ++s.desyncs;
        log().write("desync", JsonObj().add("battle", s.battle.id).add("turn", turn).add("local", std::string_view(a)).add("remote", std::string_view(b)).raw("state", state_detail()), turn);
    } else {
        log().write("hash_ok", JsonObj().add("battle", s.battle.id).add("hash", std::string_view(a)), turn);
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
            pp->link->on_invalid = [](const std::string& why) { ++s.invalid; log().write("net_invalid", JsonObj().add("why", why)); };
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
        if (auto* ac = std::get_if<proto::ActionCommand>(&m)) {
            if (ac->battle_id != s.battle.id || owner_of_spot(ac->actor_spot) != p.player_id) {
                log().write("net_rejected_msg", JsonObj().add("why", "action for a hero this player does not own").raw("msg", proto::to_json(m)));
                return;
            }
            s.battle.pending_command = *ac;
        } else if (auto* dr = std::get_if<proto::DefenseResult>(&m)) {
            if (dr->battle_id == s.battle.id) s.battle.defense_results[(static_cast<uint64_t>(p.player_id) << 16) | dr->hit] = *dr;
        } else if (auto* sh = std::get_if<proto::StateHash>(&m)) {
            if (sh->battle_id == s.battle.id) { s.battle.remote_hash[sh->turn] = sh->hash; check_hash(sh->turn); }
        } else if (auto* ping = std::get_if<proto::Ping>(&m)) {
            send_to(p, proto::Pong{ping->t_us}, false);
        } else if (std::get_if<proto::Bye>(&m)) {
            log().write("session_leave", JsonObj().add("player_id", static_cast<int>(p.player_id)));
            p.accepted = false;
        }
        return;
    }
    // Client
    if (auto* w = std::get_if<proto::Welcome>(&m)) {
        s.accepted = true; s.my_player_id = w->player_id; s.session_id = w->session_id;
        log().write("session_welcome", JsonObj().add("player_id", static_cast<int>(w->player_id)));
    } else if (auto* r = std::get_if<proto::Reject>(&m)) {
        s.rejected = true;
        log().write("session_rejected", JsonObj().add("reason", static_cast<int>(r->reason)).add("detail", r->detail));
    } else if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
        // Le combat ne démarre qu'une fois le monde local prêt (voir tick) : l'hôte peut être en avance
        // (client encore au menu ou en chargement).
        s.pending_start = *bs;
        s.battle = Battle{};   // remis à zéro dès maintenant : les messages suivants de l'hôte s'y accumulent
        s.battle.id = bs->battle_id;
        for (auto& o : bs->owners) s.battle.owner_by_spot[o.spot] = o.player_id;
        log().write("coop_battle_start_received", JsonObj().add("battle", bs->battle_id).add("encounter", bs->encounter));
    } else if (auto* ts = std::get_if<proto::TurnStart>(&m)) {
        s.battle.host_turn_seen = std::max(s.battle.host_turn_seen, ts->turn);
    } else if (auto* ar = std::get_if<proto::ActionResult>(&m)) {
        s.battle.pending_results.push_back(*ar);
    } else if (auto* sh = std::get_if<proto::StateHash>(&m)) {
        s.battle.remote_hash[sh->turn] = sh->hash;
        check_hash(sh->turn);
    } else if (auto* ad = std::get_if<proto::AiDraws>(&m)) {
        for (size_t i = 0; i < ad->values.size(); ++i)
            s.battle.host_draws[(static_cast<uint64_t>(ad->turn) << 16) | (ad->first + i)] = ad->values[i];
    } else if (std::get_if<proto::BattleEnd>(&m)) {
        log().write("coop_battle_end_received", JsonObj().add("battle", s.battle.id));
    } else if (auto* ping = std::get_if<proto::Ping>(&m)) {
        send_to(p, proto::Pong{ping->t_us}, false);
    }
}

// Client : applique les PV calculés par l'hôte.
void apply_results()
{
    if (s.battle.pending_results.empty()) return;
    auto ps = game::players();
    auto es = game::enemies();
    for (auto& ar : s.battle.pending_results) {
        for (auto& h : ar.hits) {
            UObject* c = nullptr;
            if (h.kind == proto::ActorKind::Enemy && h.index < es.size()) c = es[h.index];
            if (h.kind == proto::ActorKind::Hero) for (UObject* x : ps) if (hero_spot(x) == h.index) c = x;
            UObject* st = game::stats(c);
            if (auto* hp = ue::value_ptr<double>(st, STR("CurrentHP"))) {
                if (std::fabs(*hp - h.hp_after) > 0.5) ++s.hp_overrides;
                if (std::fabs(*hp - h.hp_after) > 0.5) log().write("coop_hp_override", JsonObj().add("c", game::char_class(c)).add("local", *hp).add("host", h.hp_after), ar.turn);
                *hp = h.hp_after;
            }
        }
    }
    s.battle.pending_results.clear();
}

// Hôte : résultat de l'action qui vient de se terminer = PV de tous les combattants.
proto::ActionResult snapshot_result(UObject* actor)
{
    proto::ActionResult ar;
    ar.battle_id = s.battle.id;
    ar.turn = s.battle.turn;
    const bool hero = game::is_player(actor);
    ar.kind = hero ? proto::ActorKind::Hero : proto::ActorKind::Enemy;
    ar.actor_index = static_cast<uint8_t>(hero ? hero_spot(actor) : enemy_index(actor));
    for (UObject* c : game::players()) ar.hits.push_back({proto::ActorKind::Hero, static_cast<uint8_t>(hero_spot(c)), 0, false, ue::get_double(game::stats(c), STR("CurrentHP"))});
    auto es = game::enemies();
    for (size_t i = 0; i < es.size(); ++i) ar.hits.push_back({proto::ActorKind::Enemy, static_cast<uint8_t>(i), 0, false, ue::get_double(game::stats(es[i]), STR("CurrentHP"))});
    return ar;
}

// --- Hooks -------------------------------------------------------------------------------

bool on_game_thread() { return s.game_thread != 0 && GetCurrentThreadId() == s.game_thread; }

// Appelant Blueprint d'une fonction native hookée. Le cadre fourni par UE4SS est celui de la fonction
// elle-même (objet = CDO de KismetMathLibrary, nœud = RandomFloatInRange, vérifié en jeu le 2026-10-19) :
// on remonte PreviousFrame jusqu'au premier objet qui n'est pas ce CDO.
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

// Diagnostic des tirages aléatoires : chaîne des cadres de script (objet / fonction BP).
// 5 premiers appels par fonction (combat ou non), puis 600 par combat au plus.
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
        frames += JsonObj().add("o", ue::full_name(f->Object())).add("n", ue::short_name(f->Node())).add("enemy", cached_enemy_index(f->Object())).str();
        RC::Unreal::FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    log().write("rng_trace", JsonObj().add("fn", fname).add("battle", s.battle.active).raw("frames", frames + "]")
                                 .add("has_result", ctx.RESULT_DECL != nullptr), s.battle.turn);
}

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();

    // Début de tour : hôte -> TurnStart + StateHash ; client -> hash local + attente de l'hôte.
    bus.post(fn(game::path::BattleChar, L"OnTurnStart"), [](HookCtx& ctx, UFunction*) {
        UObject* actor = ctx.Context;
        if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;   // contre : pas un tour
        if (s.role == Role::Host) {
            if (!s.battle.active) {
                s.battle = Battle{};
                s.battle.id = s.next_battle_id++;
                s.battle.active = true;
                proto::BattleStart bs;
                bs.battle_id = s.battle.id;
                bs.encounter = ue::to_utf8(game::last_encounter());
                auto ps = game::players();
                for (UObject* c : ps) {
                    const int spot = hero_spot(c);
                    uint8_t owner = 0;
                    auto ov = s.owner_overrides.find(spot);
                    if (ov != s.owner_overrides.end()) owner = ov->second;
                    else if (!s.peers.empty() && s.peers.front().accepted && spot == 0) owner = s.peers.front().player_id;   // défaut PoC
                    s.battle.owner_by_spot[spot] = owner;
                    bs.owners.push_back({static_cast<uint8_t>(spot), game::char_class(c), owner});
                }
                broadcast(bs);
            }
            ++s.battle.turn;
            s.battle.draw_index = 0;
            s.battle.enemies_cache = game::enemies();
            const bool hero = game::is_player(actor);
            broadcast(proto::TurnStart{s.battle.id, s.battle.turn, hero ? proto::ActorKind::Hero : proto::ActorKind::Enemy,
                                       static_cast<uint8_t>(hero ? hero_spot(actor) : enemy_index(actor)), game::char_class(actor)});
            const uint64_t h = state_hash(actor);
            s.battle.local_hash[s.battle.turn] = h;
            broadcast(proto::StateHash{s.battle.id, s.battle.turn, h});
            check_hash(s.battle.turn);
            s.battle.pending_command.reset();
        } else if (s.role == Role::Client && s.battle.active) {
            ++s.battle.turn;
            s.battle.draw_index = 0;
            s.battle.enemies_cache = game::enemies();
            // l'hôte rythme les tours : attendre son TurnStart et ses résultats pour ce tour
            wait_for([&] { return s.battle.host_turn_seen >= s.battle.turn; }, 10.0);
            // Hash avant correction : c'est lui qui dit si la simulation locale a suivi celle de l'hôte.
            const uint64_t raw = state_hash(actor);
            apply_results();
            const uint64_t h = state_hash(actor);
            if (raw != h) log().write("coop_raw_hash_differs", JsonObj().add("battle", s.battle.id).raw("state_after_override", state_detail()), s.battle.turn);
            s.battle.local_hash[s.battle.turn] = h;
            for (auto& p : s.peers) send_to(p, proto::StateHash{s.battle.id, s.battle.turn, h});
            check_hash(s.battle.turn);
        }
    });

    // Fin de tour (hôte) : diffuser les PV résultants.
    bus.post(fn(game::path::BattleChar, L"OnTurnEnded"), [](HookCtx& ctx, UFunction*) {
        if (s.role != Role::Host || !s.battle.active) return;
        broadcast(snapshot_result(ctx.Context));
    });

    // Client : capter l'action choisie localement pour un héros qu'on contrôle.
    bus.pre(fn(game::path::BattleMgr, L"RequestActionExecution"), [](HookCtx&, UFunction*) {
        if (s.role != Role::Client || !s.battle.active) return;
        UObject* cur = game::current_character();
        if (!cur || !game::is_player(cur) || owner_of_spot(hero_spot(cur)) != s.my_player_id) return;
        proto::ActionCommand ac;
        ac.battle_id = s.battle.id;
        ac.turn = s.battle.turn;
        ac.actor_spot = static_cast<uint8_t>(hero_spot(cur));
        UObject* bm = game::battle_manager();
        ac.action_type = static_cast<uint8_t>(ue::get_int(bm, STR("SelectedAction"), 1));
        ac.combo_hits = static_cast<uint8_t>(std::clamp<int64_t>(ue::get_int(bm, STR("DEBUG_ComboAutoSuccessCount"), 0), 0, 255));
        ac.targets = {0};
        for (auto& p : s.peers) send_to(p, ac);
    });

    // Défense distante (hôte) : demande au propriétaire du héros, attend son résultat, l'applique.
    auto remote_defense = [](UObject* defender, bool jumpable) {
        const int spot = hero_spot(defender);
        const uint8_t owner = owner_of_spot(spot);
        const uint16_t hit = ++s.battle.hit_counter[owner];
        for (auto& p : s.peers)
            if (p.player_id == owner) send_to(p, proto::DefenseRequest{s.battle.id, s.battle.turn, hit, 0, {static_cast<uint8_t>(spot)}, 3000});
        const uint64_t key = (static_cast<uint64_t>(owner) << 16) | hit;
        const ULONGLONG t0 = GetTickCount64();
        const bool got = wait_for([&] { return s.battle.defense_results.count(key) > 0; }, 3.0);
        const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
        proto::Defense res = proto::Defense::Fail;
        if (got) { res = s.battle.defense_results[key].result; s.battle.defense_results.erase(key); }
        bool started = false;
        if (jumpable && res != proto::Defense::Fail) started = game::start_jump(defender);
        else if (!jumpable && (res == proto::Defense::Parry || res == proto::Defense::GradientParry)) started = game::start_parry(defender);
        log().write("coop_remote_defense", JsonObj().add("player", static_cast<int>(owner)).add("hit", static_cast<int>(hit)).add("jumpable", jumpable).add("result", static_cast<int>(res))
                                               .add("received", got).add("waited_ms", waited_ms).add("started", started), s.battle.turn);
    };
    bus.pre(fn(game::path::Stats, L"ReceiveDamageFromObject"), [remote_defense](HookCtx& ctx, UFunction* f) {
        if (!s.battle.active || s.role == Role::Solo) return;
        UObject* defender = ue::component_owner(ctx.Context);
        if (!defender || !game::is_player(defender)) return;
        if (!game::incoming_attack_jumpable(ue::read_param_object(ctx, f, STR("DamageBuilder")))) return;
        if (s.role == Role::Host) {
            if (owner_of_spot(hero_spot(defender)) != 0) remote_defense(defender, true);
        } else if (owner_of_spot(hero_spot(defender)) == s.my_player_id) {
            s.battle.local_jump_hit[ctx.Context] = ++s.battle.hit_counter[s.my_player_id];
        }
    });
    bus.post(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, UFunction* f) {
        if (s.role != Role::Client || !s.battle.active) return;
        auto it = s.battle.local_jump_hit.find(ctx.Context);
        if (it == s.battle.local_jump_hit.end()) return;
        const uint16_t hit = it->second;
        s.battle.local_jump_hit.erase(it);
        const bool dodged = ue::read_out_bool(ctx, f, STR("DodgeSuccess")).value_or(false);
        for (auto& p : s.peers) send_to(p, proto::DefenseResult{s.battle.id, s.battle.turn, hit, dodged ? proto::Defense::Jump : proto::Defense::Fail});
    });

    // Défense : TryParryHitFromCharacter sur le composant de stats du défenseur.
    bus.pre(fn(game::path::Stats, L"TryParryHitFromCharacter"), [remote_defense](HookCtx& ctx, UFunction*) {
        if (s.role != Role::Host || !s.battle.active) return;
        UObject* defender = ue::component_owner(ctx.Context);
        if (!defender || !game::is_player(defender)) return;
        if (owner_of_spot(hero_spot(defender)) == 0) return;   // héros de l'hôte : défendu localement
        remote_defense(defender, false);
    });

    bus.post(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, UFunction* f) {
        if (s.role != Role::Host || !s.battle.active) return;
        UObject* defender = ue::component_owner(ctx.Context);
        if (!defender || !game::is_player(defender) || owner_of_spot(hero_spot(defender)) == 0) return;
        const auto v = ue::read_out_bool(ctx, f, STR("Successful"));
        log().write("coop_remote_parry_verdict", JsonObj().add("successful", v.value_or(false)).add("known", v.has_value()), s.battle.turn);
    });

    // Client : défense résolue localement -> résultat envoyé à l'hôte.
    bus.pre(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, UFunction*) {
        if (s.role != Role::Client || !s.battle.active) return;
        UObject* defender = ue::component_owner(ctx.Context);
        if (!defender || !game::is_player(defender) || owner_of_spot(hero_spot(defender)) != s.my_player_id) return;
        s.battle.local_defense_hit[ctx.Context] = ++s.battle.hit_counter[s.my_player_id];
    });
    bus.post(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, UFunction* f) {
        if (s.role != Role::Client || !s.battle.active) return;
        auto it = s.battle.local_defense_hit.find(ctx.Context);
        if (it == s.battle.local_defense_hit.end()) return;
        const uint16_t hit = it->second;
        s.battle.local_defense_hit.erase(it);
        const bool ok = ue::read_out_bool(ctx, f, STR("Successful")).value_or(false);
        for (auto& p : s.peers) send_to(p, proto::DefenseResult{s.battle.id, s.battle.turn, hit, ok ? proto::Defense::Parry : proto::Defense::Fail});
    });

    // Diagnostic (temporaire) : ciblage aléatoire, lecture seule.
    bus.post(L"/Script/Engine.KismetArrayLibrary:Array_Random", [](HookCtx& ctx, UFunction*) {
        if (s.role != Role::Solo && on_game_thread()) trace_random(ctx, "Array_Random");
    });

    // Tirages aléatoires de l'IA ennemie (D9) : fonctions natives de KismetMathLibrary appelées depuis un ennemi.
    struct RandomFn { const wchar_t* name; int kind; };   // 0 = int32, 1 = double, 2 = bool
    static const RandomFn random_fns[] = {
        {L"RandomIntegerInRange", 0}, {L"RandomInteger", 0}, {L"RandomFloat", 1},
        {L"RandomFloatInRange", 1}, {L"RandomBool", 2}, {L"RandomBoolWithWeight", 2},
    };
    for (const auto& rf : random_fns) {
        const int kind = rf.kind;
        const std::string fname = ue::to_utf8(rf.name);
        bus.post(std::wstring(L"/Script/Engine.KismetMathLibrary:") + rf.name, [kind, fname](HookCtx& ctx, UFunction*) {
            if (s.role == Role::Solo) return;
            // Threads d'animation, chargement… : jamais un choix d'IA, et aucun accès au jeu permis ici.
            if (!on_game_thread()) { ++s.off_thread_draws; return; }
            trace_random(ctx, fname);
            if (!s.battle.active || !ctx.RESULT_DECL) return;
            UObject* caller = script_caller(ctx);
            if (!caller) return;
            const int ei = cached_enemy_index(caller);
            if (ei < 0) return;   // seulement les tirages faits par un ennemi (choix de l'IA)
            const uint16_t idx = s.battle.draw_index++;
            double v = kind == 0 ? static_cast<double>(*static_cast<int32_t*>(ctx.RESULT_DECL))
                     : kind == 1 ? *static_cast<double*>(ctx.RESULT_DECL)
                                 : (*static_cast<uint8_t*>(ctx.RESULT_DECL) ? 1.0 : 0.0);
            if (s.role == Role::Host) {
                broadcast(proto::AiDraws{s.battle.id, s.battle.turn, static_cast<uint8_t>(ei), idx, {v}});
                log().write("coop_ai_draw", JsonObj().add("fn", fname).add("enemy", ei).add("index", static_cast<int>(idx)).add("value", v), s.battle.turn);
                return;
            }
            const uint64_t key = (static_cast<uint64_t>(s.battle.turn) << 16) | idx;
            const bool got = wait_for([&] { return s.battle.host_draws.count(key) > 0; }, 3.0);
            if (!got) { log().write("coop_ai_draw_missing", JsonObj().add("fn", fname).add("index", static_cast<int>(idx)).add("local", v), s.battle.turn); return; }
            const double hv = s.battle.host_draws[key];
            if (kind == 0) *static_cast<int32_t*>(ctx.RESULT_DECL) = static_cast<int32_t>(hv);
            else if (kind == 1) *static_cast<double*>(ctx.RESULT_DECL) = hv;
            else *static_cast<uint8_t*>(ctx.RESULT_DECL) = hv != 0.0 ? 1 : 0;
            log().write("coop_ai_draw_replayed", JsonObj().add("fn", fname).add("index", static_cast<int>(idx)).add("local", v).add("host", hv), s.battle.turn);
        });
    }

    bus.pre(fn(game::path::BattleMgr, L"StartBattleEndFlow"), [](HookCtx& ctx, UFunction* f) {
        if (!s.battle.active) return;
        const int result = static_cast<int>(ue::read_param_int(ctx, f, STR("BattleEndResult")).value_or(-1));
        if (s.role == Role::Host) broadcast(proto::BattleEnd{s.battle.id, static_cast<uint8_t>(std::max(result, 0))});
        log().write("coop_battle_end", JsonObj().add("battle", s.battle.id).add("result", result).add("desyncs", s.desyncs).add("off_thread_draws", s.off_thread_draws));
        s.battle.active = false;
    });
}

} // namespace

Role role() { return s.role; }
bool active() { return s.role != Role::Solo; }
int desync_count() { return s.desyncs; }
int connected_players()
{
    if (s.role == Role::Client) return s.accepted && !s.rejected ? 1 : 0;
    int n = 0;
    for (auto& p : s.peers) if (p.accepted) ++n;
    return n;
}
int invalid_count() { return s.invalid; }
int hp_override_count() { return s.hp_overrides; }
int hashes_compared() { return s.hashes_compared; }
bool session_ok() { return s.role == Role::Solo || (s.role == Role::Host ? true : (s.accepted && !s.rejected)); }

bool controls_spot(int spot)
{
    if (s.role == Role::Solo) return true;
    if (!s.battle.active) return s.role == Role::Host;
    return owner_of_spot(spot) == s.my_player_id;
}

void init()
{
    const std::wstring r = arg(L"e33role");
    if (r == L"host") s.role = Role::Host;
    else if (r == L"client") s.role = Role::Client;
    else return;

    s.game_build = read_build_id();
    s.gameplay_mods = read_gameplay_mods();
    s.session_id = GetTickCount();
    for (const std::wstring& tok : {arg(L"e33owners")}) {
        std::wstringstream ss(tok);
        std::wstring part;
        while (std::getline(ss, part, L',')) {
            const auto c = part.find(L':');
            if (c != std::wstring::npos) s.owner_overrides[std::stoi(part.substr(0, c))] = static_cast<uint8_t>(std::stoi(part.substr(c + 1)));
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
        host.link->on_invalid = [](const std::string& why) { ++s.invalid; log().write("net_invalid", JsonObj().add("why", why)); };
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
    if (!game::in_battle() && !game::trigger_battle(ue::from_utf8(bs.encounter), err)) {
        log().write("coop_error", JsonObj().add("what", "trigger_battle: " + err));
        s.next_trigger_try = s.now + 2.0;
        return;
    }
    s.pending_start.reset();
    s.battle.active = true;
    log().write("coop_battle_start", JsonObj().add("battle", bs.battle_id).add("encounter", bs.encounter));
}

void tick(double dt)
{
    if (s.role == Role::Solo) return;
    s.game_thread = GetCurrentThreadId();
    s.now += dt;
    pump_network();
    if (s.role == Role::Client) start_pending_battle();
    // Hôte : exécuter la commande reçue d'un client pour son héros.
    if (s.role == Role::Host && s.battle.active && s.battle.pending_command) {
        UObject* cur = game::current_character();
        if (cur && game::is_player(cur) && hero_spot(cur) == s.battle.pending_command->actor_spot) {
            auto cmd = *s.battle.pending_command;
            s.battle.pending_command.reset();
            std::vector<UObject*> targets;
            auto es = game::enemies();
            for (auto t : cmd.targets) if (t < es.size()) targets.push_back(es[t]);
            if (targets.empty() && !es.empty()) targets.push_back(es.front());
            std::string err;
            if (!game::perform_action(cmd.action_type, targets, cmd.combo_hits, err)) log().write("coop_error", JsonObj().add("what", "perform_action: " + err));
            else log().write("coop_remote_action", JsonObj().add("spot", static_cast<int>(cmd.actor_spot)).add("action", static_cast<int>(cmd.action_type)).add("combo_hits", static_cast<int>(cmd.combo_hits)), s.battle.turn);
        }
    }
}

} // namespace e33::coop
