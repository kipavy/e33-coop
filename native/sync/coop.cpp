// Session coop : état, réseau, messages, options, démarrage (D8-D14, D18).
#include "coop_internal.hpp"

using RC::Unreal::UObject;
using RC::Unreal::UFunction;

namespace e33::coop::detail {

State s;
Heartbeat hb;

// --- Utilitaires ------------------------------------------------------------------------

std::wstring arg(const wchar_t* key) { return cmdline::arg(key); }   // en test : options du scénario seulement

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

void send_to(Peer& p, const proto::Message& m, bool reliable) { if (p.link) p.link->send(m, reliable); }
// Pair qui participe à la partie : accepté, et entré (instantané chargé) s'il a demandé à rejoindre. Un joueur qui
// charge encore l'instantané (ou attend la fin d'un combat pour entrer) ne reçoit pas les messages de jeu.
bool in_game(const Peer& p) { return p.accepted && p.drop.empty() && (s.role == Role::Client || explore::peer_in_game(p.player_id)); }
void broadcast(const proto::Message& m) { for (auto& p : s.peers) if (in_game(p)) send_to(p, m); }
// Hôte : à tous les clients sauf l'émetteur ; client : à l'hôte.
void publish(const proto::Message& m, const Peer* origin, bool reliable)
{
    for (auto& p : s.peers) if (in_game(p) && &p != origin) send_to(p, m, reliable);
}

std::string player_name(uint8_t id)
{
    if (id == s.my_player_id && s.role != Role::Solo) return ui::text("you");
    auto it = s.roster.find(id);
    if (it != s.roster.end() && !it->second.empty()) return it->second;
    return id == 0 ? ui::text("host") : "Joueur " + std::to_string(id + 1);
}

// Joueur qui contrôle un personnage hors combat (builds, D13) et par défaut en combat : -e33owners (nom), sinon le
// k-ième personnage de l'équipe va au k-ième joueur (hôte, puis clients par numéro), l'hôte garde le reste.
uint8_t character_owner(const std::string& name)
{
    if (auto it = s.owner_overrides.find(name); it != s.owner_overrides.end()) return it->second;
    if (!s.owner_overrides.empty()) return 0;
    const auto party = game::party_names();
    const auto pos = std::find(party.begin(), party.end(), name);
    if (pos == party.end()) return 0;
    std::vector<uint8_t> players{0};
    for (auto& [id, n] : s.roster) if (id != 0) players.push_back(id);
    std::sort(players.begin() + 1, players.end());
    const auto idx = static_cast<size_t>(pos - party.begin());
    return idx < players.size() ? players[idx] : 0;
}

// Répartition lisible de l'équipe (« Maelle : Killian · Verso : TOTOFFF »), montrée à chacun après un changement.
std::string owners_summary()
{
    std::string out;
    for (auto& c : game::party_names()) out += (out.empty() ? "" : " · ") + c + " : " + player_name(character_owner(c));
    return out;
}

// Bannière « Équipe : … » seulement quand la répartition a changé (avant : à chaque Owners reçu, donc à chaque arrivée
// ou rechargement du client, jugé parasite par Killian le 2026-10-23).
void toast_owners_if_changed()
{
    static std::string last;
    const std::string sum = owners_summary();
    if (sum.empty() || sum == last) return;   // vide : partie pas encore chargée (arrivée du client)
    last = sum;
    ui::toast("owners_summary", {sum}, 8.0);
}

// Hôte : répartition actuelle des personnages de l'équipe, envoyée aux clients (ils l'appliquent telle quelle).
proto::Owners character_owner_map()
{
    proto::Owners ow;
    ow.battle_id = 0;
    for (auto& [name, player] : s.owner_overrides) ow.owners.push_back({0, name, player});
    return ow;
}

void cycle_owner(size_t index)
{
    const auto party = game::party_names();
    if (party.empty()) return;
    std::map<std::string, uint8_t> current;
    for (auto& c : party) current[c] = character_owner(c);
    const std::string ch = party[index % party.size()];
    std::vector<uint8_t> players{0};
    for (auto& [id, n] : s.roster) if (id != 0) players.push_back(id);
    std::sort(players.begin() + 1, players.end());
    const auto pos = std::find(players.begin(), players.end(), current[ch]);
    const uint8_t next = players[pos == players.end() ? 0 : static_cast<size_t>(pos - players.begin() + 1) % players.size()];
    current[ch] = next;
    s.owner_overrides.clear();
    for (auto& [c, p] : current) s.owner_overrides[c] = p;
    broadcast(character_owner_map());
    bump("owner_reassignments");
    ui::toast("owner_changed", {ch, player_name(next)});
    toast_owners_if_changed();
    log().write("coop_owner_reassigned", JsonObj().add("character", ch).add("player", static_cast<int>(next)));
}

// --- Battement « occupé » --------------------------------------------------------------------------
// Pendant un chargement de niveau, le thread de jeu ne tourne plus pendant 30 à 60 s : un thread à part envoie alors
// à chaque pair un paquet « occupé » (net::Link::busy_packet), pour qu'il ne nous croie pas partis.

int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count(); }

void heartbeat_targets()
{
    std::lock_guard lk(hb.mu);
    hb.targets.clear();
    for (auto& p : s.peers) if (p.link && p.drop.empty()) hb.targets.emplace_back(p.link->peer(), p.link->epoch());
}

void heartbeat_start()
{
    if (hb.started.exchange(true)) return;
    std::thread([] {
        for (;;) {
            Sleep(250);
            const int64_t last = hb.last_tick_ms.load();
            if (last == 0 || steady_ms() - last < 750) continue;   // thread de jeu vivant : rien à faire
            // Diagnostic (essai à deux PC du 2026-10-22 : journal du client coupé net) : un gel du thread de jeu est journalisé
            // d'ici ; un journal qui s'arrête sans « game_thread_stall » ni « mod_unload » = processus tué ou planté.
            if (const int64_t age = steady_ms() - last; age >= 5000 && hb.stall_of.load() != last) {
                hb.stall_of = last;
                log().write("game_thread_stall", JsonObj().add("ms", age));
            }
            std::vector<std::pair<std::string, uint32_t>> t;
            { std::lock_guard lk(hb.mu); t = hb.targets; }
            for (auto& [peer, epoch] : t) {
                const auto pkt = net::Link::busy_packet(epoch);
                s.transport->send(peer, pkt.data(), pkt.size());
            }
        }
    }).detach();
}

uint8_t owner_of_spot(int spot)
{
    auto it = s.battle.owner_by_spot.find(spot);
    return it == s.battle.owner_by_spot.end() ? 0 : it->second;
}
bool controls(int spot) { return owner_of_spot(spot) == s.my_player_id; }

proto::Unit actor_unit(UObject* actor) { return sync::unit_of(actor); }

// Visée libre : zone touchée (BP_FreeAimTargetPart_C, ex. point faible bleu) et sa position. Vu en vrai le 2026-10-24 :
// un tir rejoué au même point relatif au combattant manquait la petite boule (animation décalée d'un PC à l'autre).
int free_aim_part_index(UObject* actor, UObject* part)
{
    if (!actor || !part) return -1;
    const auto parts = ue::get_object_array(actor, STR("FreeAimTargetParts"));
    for (size_t i = 0; i < parts.size() && i < 0xFF; ++i) if (parts[i] == part) return static_cast<int>(i);
    return -1;
}

UObject* free_aim_part_at(UObject* actor, int index)
{
    const auto parts = ue::get_object_array(actor, STR("FreeAimTargetParts"));
    return index >= 0 && index < static_cast<int>(parts.size()) ? parts[static_cast<size_t>(index)] : nullptr;
}

bool free_aim_part_location(UObject* part, double out[3])
{
    ue::Call c(part, STR("K2_GetComponentLocation"));
    if (!c || !c.invoke() || c.param_size(STR("ReturnValue")) != 24) return false;
    const auto* v = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    return true;
}

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

void wire_link(net::Link& l)
{
    const bool host = s.role == Role::Host;
    l.on_invalid = [](const std::string& why) { bump("invalid_messages"); log().write("net_invalid", JsonObj().add("why", why)); };
    l.on_sent = [host](const proto::Message& m, bool) { s.recorder.write(host ? "h2c" : "c2h", m); log().write("net_send", JsonObj().raw("msg", proto::to_json(m))); };
    l.on_delivered = [host](const proto::Message& m) { s.recorder.write(host ? "c2h" : "h2c", m); log().write("net_recv", JsonObj().raw("msg", proto::to_json(m))); };
}

// Retire les pairs marqués (départ, silence, redémarrage, refus) : l'hôte reprend leurs héros.
void sweep_peers()
{
    bool changed = false;
    for (size_t i = 0; i < s.peers.size();) {
        Peer& p = s.peers[i];
        if (p.drop.empty() || (p.drop_at >= 0 && s.now < p.drop_at)) { ++i; continue; }
        if (p.link) p.link->update(s.now);   // derniers messages (Reject, Bye)
        const bool was_player = p.accepted;
        const uint8_t id = p.player_id;
        const std::string name = p.name, why = p.drop;
        if (p.link && p.link->remote_epoch()) s.tombstones[{p.link->peer(), p.link->remote_epoch()}] = s.now + 60.0;
        log().write("session_peer_removed", JsonObj().add("peer", p.link ? p.link->peer() : "").add("player_id", static_cast<int>(id)).add("why", why));
        s.peers.erase(s.peers.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        if (was_player && s.role == Role::Host) on_player_left(id, name, why);
    }
    if (changed) heartbeat_targets();
}

void pump_network()
{
    if (!s.transport || s.ended) return;
    net::Datagram d;
    while (s.transport->poll(d)) {
        Peer* peer = nullptr;
        for (auto& p : s.peers) if (p.link && p.link->peer() == d.peer) peer = &p;
        if (peer && !peer->drop.empty()) {   // en cours de retrait (refus) : il finit d'acquitter, rien de plus
            peer->link->on_datagram(d.data.data(), d.data.size(), s.now);
            continue;
        }
        if (s.role == Role::Host) {
            // Même adresse, nouvelle époque : le client a redémarré (nouveau processus, Steam : même compte). L'ancien
            // joueur est parti ; le nouveau se présente comme n'importe quel arrivant (Hello, puis instantané).
            const uint32_t ep = net::Link::packet_epoch(d.data.data(), d.data.size());
            if (peer && ep && peer->link->remote_epoch() && ep != peer->link->remote_epoch()) {
                peer->drop = "restarted";
                bump("peer_restarts");
                peer = nullptr;
                sweep_peers();
            }
            if (!peer && ep) {   // derniers paquets d'un pair qui vient d'être retiré (même processus) : ignorés
                auto ts = s.tombstones.find({d.peer, ep});
                if (ts != s.tombstones.end() && s.now < ts->second) continue;
            }
            if (!peer) {
                const uint16_t pv = net::Link::packet_protocol(d.data.data(), d.data.size());
                if (pv == 0 && d.data.size() == 8 && std::memcmp(d.data.data(), "E33S", 4) == 0) { bump("steam_selftest_received"); continue; }
                if (pv == 0) { log().write("net_ignored", JsonObj().add("peer", d.peer).add("why", "not an E33Coop packet")); continue; }
                if (pv != proto::kProtocolVersion) {
                    // Autre version du mod : on répond avec notre version (son jeu affiche l'écart), sans créer de pair.
                    const auto pkt = net::Link::version_packet(1);
                    s.transport->send(d.peer, pkt.data(), pkt.size());
                    if (s.version_rejected.insert(d.peer).second) {
                        bump("sessions_rejected");
                        const std::string who = s.steam ? net::steam::friend_name(net::steam::id_of(d.peer)) : std::string();
                        update::on_version_mismatch(pv > proto::kProtocolVersion, who.empty() ? ui::text("a_player") : who, "", false, 0);
                        log().write("session_reject", JsonObj().add("peer", d.peer).add("reason", static_cast<int>(proto::RejectReason::ProtocolMismatch))
                                                         .add("detail", "protocole hôte " + std::to_string(proto::kProtocolVersion) + " / client " + std::to_string(pv)));
                    }
                    continue;
                }
                Peer np;
                np.link = std::make_unique<net::Link>(*s.transport, d.peer);
                np.created_at = s.now;
                wire_link(*np.link);
                s.peers.push_back(std::move(np));
                peer = &s.peers.back();
                heartbeat_targets();
                log().write("net_peer", JsonObj().add("peer", d.peer));
            }
        }
        if (!peer) { log().write("net_ignored", JsonObj().add("peer", d.peer).add("why", "unknown peer")); continue; }
        peer->link->on_datagram(d.data.data(), d.data.size(), s.now);
    }
    for (size_t i = 0; i < s.peers.size(); ++i) {
        proto::Message m;
        while (s.peers[i].link && s.peers[i].drop.empty() && s.peers[i].link->pop(m)) handle(s.peers[i], m);
        if (s.ended) return;
        Peer& p = s.peers[i];
        if (!p.link) continue;
        p.link->update(s.now);
        // Silence : le pair est parti (jeu fermé, réseau coupé). Un pair qui n'a jamais répondu est un intrus ou une
        // adresse erronée : retiré au bout du même délai.
        if (p.drop.empty() && (p.link->timed_out(s.now, kLinkTimeout, kBusyMax) || (p.link->last_heard() < 0 && s.now - p.created_at > (s.role == Role::Host ? kHandshakeTimeout : kClientHandshakeTimeout)))) {
            if (s.role == Role::Host) {
                p.drop = "timeout";
                bump("peer_timeouts");
            } else {
                end_session(p.link->last_heard() < 0 ? "host_unreachable" : "host_lost", "");
                return;
            }
        }
        if (s.role == Role::Host && !p.accepted && p.drop.empty() && s.now - p.created_at > kHandshakeTimeout) p.drop = "no hello";
    }
    sweep_peers();
}

// Horloge du mod (s.now) : avance de dt à chaque image. Une attente bloquante (wait_for) l'a déjà avancée du temps
// attendu, et le dt de l'image suivante contient ce même temps : il ne compte qu'une fois. Avant, il comptait deux fois :
// essai à deux PC du 2026-10-22, attente de tour de 15 s chez le client -> horloge avancée de 30 s -> lien jugé muet
// 15 s (le dernier paquet de l'hôte datait de la fin de l'attente) -> « Connexion à l'hôte perdue », hôte bien vivant.
void advance_clock(double dt)
{
    s.now = std::max(s.now, s.tick_now + dt);
    s.tick_now = s.now;
}

// Valeur maximale d'un compteur (durées les plus longues : attentes, images).
void bump_max(const char* name, int v) { int& c = s.counters[name]; c = std::max(c, v); }

// Parade appuyée pendant une attente figée (D32, essai à deux PC du 2026-10-24, 18:27) : sur une attaque de zone, le
// jeu règle le coup du héros de l'autre joueur (attente de son résultat, image figée ~60-150 ms) puis, dans la même
// image, celui du héros d'ici. Un appui pendant l'attente n'est lu par le jeu qu'à l'image suivante : la parade du héros
// d'ici était jugée ratée. On lit donc la touche de parade nous-mêmes pendant l'attente (E au clavier, RB à la manette),
// et le coup du héros d'ici qui suit lance sa parade (même chemin que l'imposition d'un résultat).
// -e33waitparry=0 : ancien comportement.
bool wait_parry_enabled()
{
    static const bool v = arg(L"e33waitparry") != L"0";
    return v;
}

bool parry_key_down()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return false;
    if (GetAsyncKeyState('E') & 0x8000) return true;
    struct Pad { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    using GetState = DWORD(WINAPI*)(DWORD, Pad*);
    static GetState get = [] {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        return m ? reinterpret_cast<GetState>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    if (!get) return false;
    for (DWORD i = 0; i < 4; ++i) {
        Pad p{};
        if (get(i, &p) == 0 && (p.buttons & 0x0200)) return true;   // XINPUT_GAMEPAD_RIGHT_SHOULDER (RB)
    }
    return false;
}

// Messages de jeu communs (hôte et client) : stockés pour les points de synchro ; l'hôte relaie aux autres.
bool handle_battle_common(Peer& p, proto::Message& m)
{
    auto& b = s.battle;
    // Hôte : preuve positive que ce joueur est dans ce tour chez lui (il agit, ouvre ses menus, vise, pare : en plus de
    // son StateHash du tour). Un joueur qui réfléchit, fouille ses menus ou lit une description n'est jamais « bloqué ».
    if (s.role == Role::Host) {
        auto sign = [&](uint32_t bid, uint32_t turn) {
            if (bid != b.id) return;
            b.acks[turn].insert(p.player_id);
            if (turn >= b.turn) b.unresponsive.erase(p.player_id);
        };
        if (auto* x = std::get_if<proto::ActionCommand>(&m)) sign(x->battle_id, x->turn);
        else if (auto* x1 = std::get_if<proto::ComboResult>(&m)) sign(x1->battle_id, x1->turn);
        else if (auto* x2 = std::get_if<proto::FreeAimEvent>(&m)) sign(x2->battle_id, x2->turn);
        else if (auto* x3 = std::get_if<proto::IntentPreview>(&m)) sign(x3->battle_id, x3->turn);
        else if (auto* x4 = std::get_if<proto::MenuState>(&m)) sign(x4->battle_id, x4->turn);
        else if (auto* x5 = std::get_if<proto::AimView>(&m)) sign(x5->battle_id, x5->turn);
        else if (auto* x6 = std::get_if<proto::DefenseResult>(&m)) sign(x6->battle_id, x6->turn);
    }
    if (auto* ac = std::get_if<proto::ActionCommand>(&m)) {
        if (ac->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(ac->actor_spot) != p.player_id) {
            log().write("net_rejected_msg", JsonObj().add("why", "action for a hero this player does not own").raw("msg", proto::to_json(m)));
            return true;
        }
        if (b.forced.count({ac->turn, ac->actor_spot})) {
            // Minuteur écoulé : l'hôte a déjà imposé l'action de ce tour (la commande du joueur s'est croisée avec la sienne).
            bump("turn_timer_races");
            log().write("coop_forced_action_conflict", JsonObj().raw("msg", proto::to_json(m)), b.turn);
            return true;
        }
        if (ac->forced) b.forced.insert({ac->turn, ac->actor_spot});
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
    if (auto* fa = std::get_if<proto::FreeAimEvent>(&m)) {
        if (fa->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(fa->spot) != p.player_id) return true;
        const std::pair<uint32_t, int> key{fa->turn, fa->spot};
        if (fa->kind == proto::FreeAimKind::Start) {
            b.fa_events[key].clear();
            b.fa_arrival[key] = s.now;
            // Nouvelle visée du même héros dans le même tour (il est sorti puis revenu) : le rejeu repart de zéro.
            if (s.fa_rep.key == key && s.fa_rep.started) {
                if (!s.fa_rep.done) { ue::Call c(game::battle_manager(), STR("CancelFreeAimFromUI")); if (c) c.invoke(); }
                s.fa_rep = FreeAimReplay{key};
            }
        }
        b.fa_events[key].push_back(*fa);
        if (s.role == Role::Host) publish(*fa, &p, fa->kind != proto::FreeAimKind::Aim);
        return true;
    }
    // Aperçu de l'action en préparation et caméra de visée libre d'un autre joueur (affichage seulement).
    if (auto* ip = std::get_if<proto::IntentPreview>(&m)) {
        if (ip->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(ip->spot) != p.player_id) return true;
        intent::receive(*ip);
        if (s.role == Role::Host) publish(*ip, &p);
        return true;
    }
    if (auto* ms = std::get_if<proto::MenuState>(&m)) {
        if (ms->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(ms->spot) != p.player_id) return true;
        intent::receive(*ms);
        if (s.role == Role::Host) publish(*ms, &p);
        return true;
    }
    if (auto* av = std::get_if<proto::AimView>(&m)) {
        if (av->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(av->spot) != p.player_id) return true;
        intent::receive(*av);
        if (s.role == Role::Host) publish(*av, &p, false);
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
        if (sh->battle_id == b.id) {
            // Hôte : ce joueur a commencé ce tour chez lui (il répond de nouveau s'il était bloqué).
            if (s.role == Role::Host) {
                b.acks[sh->turn].insert(p.player_id);
                if (sh->turn >= b.turn && b.unresponsive.erase(p.player_id))
                    log().write("coop_player_responsive", JsonObj().add("player", static_cast<int>(p.player_id)), b.turn);
            }
            b.remote_hash[sh->turn] = sh->hash;
            check_hash(sh->turn);
        }
        return true;
    }
    if (auto* ping = std::get_if<proto::Ping>(&m)) { send_to(p, proto::Pong{ping->t_us}, false); return true; }
    return false;
}

uint8_t free_player_id()
{
    for (uint8_t id = 1; id <= kMaxClients; ++id) {
        bool used = false;
        for (auto& p : s.peers) if (p.accepted && p.drop.empty() && p.player_id == id) used = true;
        if (!used) return id;
    }
    return 0xFF;
}

void send_roster()
{
    proto::Roster r;
    r.players.push_back({0, s.my_name});
    for (auto& p : s.peers) if (p.accepted && p.drop.empty()) r.players.push_back({p.player_id, p.name});
    s.roster.clear();
    for (auto& e : r.players) s.roster[e.player] = e.name;
    for (auto& p : s.peers) if (p.accepted && p.drop.empty()) send_to(p, r);
}

void reject(Peer& p, proto::RejectReason reason, const std::string& detail)
{
    send_to(p, proto::Reject{reason, detail});
    p.drop = "rejected";
    p.drop_at = s.now + 2.0;   // le temps que le Reject parte (renvoyé tant qu'il n'est pas acquitté)
    bump("sessions_rejected");
    log().write("session_reject", JsonObj().add("peer", p.link->peer()).add("reason", static_cast<int>(reason)).add("detail", detail));
}

void handle(Peer& p, proto::Message& m)
{
    if (s.role == Role::Host) {
        if (auto* h = std::get_if<proto::Hello>(&m)) {
            if (p.accepted) return;   // doublon
            proto::RejectReason reason = proto::RejectReason::Other;
            std::string diff;
            if (h->protocol != proto::kProtocolVersion) { reason = proto::RejectReason::ProtocolMismatch; diff = "protocole hôte " + std::to_string(proto::kProtocolVersion) + " / client " + std::to_string(h->protocol); }
            else if (h->game_build != s.game_build) { reason = proto::RejectReason::GameBuildMismatch; diff = "build du jeu hôte " + std::to_string(s.game_build) + " / client " + std::to_string(h->game_build); }
            else if (h->mod_version != kModVersion) { reason = proto::RejectReason::ModVersionMismatch; diff = std::string("version du mod hôte ") + kModVersion + " / client " + h->mod_version; }
            else if (h->gameplay_mods != s.gameplay_mods) {
                reason = proto::RejectReason::ModListMismatch;
                diff = "mods de gameplay différents :";
                for (auto& x : s.gameplay_mods) if (std::find(h->gameplay_mods.begin(), h->gameplay_mods.end(), x) == h->gameplay_mods.end()) diff += " hôte seul=" + x;
                for (auto& x : h->gameplay_mods) if (std::find(s.gameplay_mods.begin(), s.gameplay_mods.end(), x) == s.gameplay_mods.end()) diff += " client seul=" + x;
            }
            if (diff.empty() && free_player_id() == 0xFF) { reason = proto::RejectReason::SessionFull; diff = "partie complète (3 joueurs)"; }
            if (!diff.empty()) {
                reject(p, reason, diff);
                // Écart de version du mod : le côté le plus ancien est invité à mettre à jour (native/sync/update).
                if (reason == proto::RejectReason::ModVersionMismatch)
                    update::on_version_mismatch(release::compare_versions(kModVersion, h->mod_version) < 0,
                                                h->player_name.empty() ? ui::text("a_player") : h->player_name, h->mod_version, false, 0);
                return;
            }
            p.accepted = true;
            p.player_id = free_player_id();
            p.wants_join = h->join;
            p.name = h->player_name.empty() ? "Joueur " + std::to_string(p.player_id + 1) : h->player_name;
            send_to(p, proto::Welcome{p.player_id, s.session_id});
            explore::on_peer_accepted(p.player_id, h->join);
            send_roster();
            bump("sessions_accepted");
            if (!s.owner_overrides.empty()) send_to(p, character_owner_map());
            ui::toast("player_joined", {p.name});
            log().write("session_join", JsonObj().add("player", p.name).add("player_id", static_cast<int>(p.player_id)).add("join", h->join));
            return;
        }
        if (!p.accepted) return;
        if (std::get_if<proto::Bye>(&m)) {
            if (p.drop.empty()) p.drop = "bye";
            bump("peer_byes");
            return;
        }
        if (auto* br = std::get_if<proto::BattleReady>(&m)) {
            if (br->battle_id == s.battle.id) s.battle.ready.insert(p.player_id);
            log().write("coop_battle_ready", JsonObj().add("player", static_cast<int>(p.player_id)).add("battle", br->battle_id));
            return;
        }
        // Demande de combat du client meneur (BattleStart sans n°, D34) : le même ennemi est lancé ici ; l'avis précoce
        // et le BattleStart partent ensuite comme pour un combat de l'hôte.
        if (auto* rq = std::get_if<proto::BattleStart>(&m); rq && rq->battle_id == 0) {
            std::string why, err;
            if (p.player_id != explore::pilot()) why = "not_pilot";
            else if (s.battle.active || game::in_battle()) why = "in_battle";
            else if (!game::explore_pawn() || game::map_change_in_progress()) why = "not_ready";
            else if (UObject* enemy = game::find_world_enemy(rq->world_enemy); !enemy) why = "enemy_not_found";
            else if (!game::start_world_battle(enemy, L"", rq->engage, err)) why = "start_failed: " + err;
            bump(why.empty() ? "battle_requests_started" : "battle_requests_refused");
            log().write("coop_battle_request_received", JsonObj().add("player", static_cast<int>(p.player_id)).add("enemy", rq->world_enemy)
                                                            .add("engage", static_cast<int>(rq->engage)).add("started", why.empty()).add("why", why));
            return;
        }
        if (builds::handle(p.player_id, m)) return;
        if (explore::handle(p.player_id, m)) return;
        if (handle_battle_common(p, m)) return;
        return;
    }
    // Client
    if (auto* w = std::get_if<proto::Welcome>(&m)) {
        s.accepted = true; s.my_player_id = w->player_id; s.session_id = w->session_id;
        explore::set_player_id(w->player_id);
        builds::set_player_id(w->player_id);
        log().write("session_welcome", JsonObj().add("player_id", static_cast<int>(w->player_id)));
        return;
    }
    if (auto* r = std::get_if<proto::Reject>(&m)) {
        s.rejected = true;
        log().write("session_rejected", JsonObj().add("reason", static_cast<int>(r->reason)).add("detail", r->detail));
        // Version de l'hôte : données du lobby Steam (lues avant de le quitter), sinon le détail (« … hôte X / client Y »).
        std::string hv = s.steam && s.lobby.id ? net::steam::lobby_data(s.lobby.id, "mod") : std::string();
        if (hv.empty()) {
            const auto a = r->detail.find("hôte "), b = r->detail.find(" /");
            if (a != std::string::npos && b != std::string::npos && b > a) hv = r->detail.substr(a + 6, b - a - 6);
        }
        const uint64_t owner = s.lobby.owner;
        end_session("rejected", r->reason == proto::RejectReason::SessionFull ? ui::text("reject_full") : r->detail);
        if (r->reason == proto::RejectReason::ModVersionMismatch)
            update::on_version_mismatch(release::compare_versions(kModVersion, hv) < 0, player_name(0), hv, true, owner);
        return;
    }
    if (auto* bye = std::get_if<proto::Bye>(&m)) {
        log().write("session_host_bye", JsonObj().add("reason", bye->reason));
        end_session("host_left", bye->reason);
        return;
    }
    if (auto* ro = std::get_if<proto::Roster>(&m)) {
        s.roster.clear();
        for (auto& e : ro->players) s.roster[e.player] = e.name;
        return;
    }
    if (auto* ow = std::get_if<proto::Owners>(&m)) {
        if (ow->battle_id == 0) {   // répartition des personnages hors combat (réattribution par l'hôte, F6)
            s.owner_overrides.clear();
            for (auto& o : ow->owners) s.owner_overrides[o.character] = o.player_id;
            bump("owner_maps_received");
            toast_owners_if_changed();
            log().write("coop_character_owners", JsonObj().raw("msg", proto::to_json(m)));
            return;
        }
        if (ow->battle_id == s.battle.id) {
            for (auto& o : ow->owners) s.battle.owner_by_spot[o.spot] = o.player_id;
            bump("owner_updates");
            log().write("coop_owners", JsonObj().raw("msg", proto::to_json(m)));
        }
        return;
    }
    if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
        // Le combat ne démarre qu'une fois le monde local prêt (voir tick) : l'hôte peut être en avance.
        if (bs->battle_id == 0) {   // avis précoce (StartBattle de l'hôte) : ennemi, équipe, engagement ; le vrai suit
            s.early_start = *bs;
            s.early_start_at = s.now;
            log().write("coop_battle_notice_received", JsonObj().add("enemy", bs->world_enemy).add("engage", static_cast<int>(bs->engage)));
            return;
        }
        s.pending_start = *bs;
        s.last_start = *bs;
        s.battle = Battle{};   // remis à zéro dès maintenant : les messages suivants de l'hôte s'y accumulent
        s.battle.id = bs->battle_id;
        s.battle.start_rx_at = s.now;
        for (auto& o : bs->owners) s.battle.owner_by_spot[o.spot] = o.player_id;
        s.battle.rng_seed = bs->rng_seed;
        // Mise en place déjà commencée ici avec la graine de l'avis précoce : ce doit être la même (D30).
        if (s.setup.open && s.setup.seeded && s.setup.seed != bs->rng_seed) {
            bump("setup_seed_mismatches");
            log().write("coop_setup_seed_mismatch", JsonObj().add("setup", std::to_string(s.setup.seed)).add("host", std::to_string(bs->rng_seed)).add("source", s.setup.source));
        }
        s.battle.turn_timer_s = bs->turn_timer_s;
        s.battle.duo_parry = bs->duo_parry != 0;   // réglage de l'hôte, jamais le nôtre : même calcul partout
        log().write("coop_battle_start_received", JsonObj().add("battle", bs->battle_id).add("encounter", bs->encounter));
        return;
    }
    if (builds::handle(0, m)) return;
    if (explore::handle(0, m)) return;
    if (handle_battle_common(p, m)) return;
    if (auto* ts = std::get_if<proto::TurnStart>(&m)) {
        if (ts->battle_id == s.battle.id) s.battle.host_turns[ts->turn] = *ts;
    } else if (auto* ss = std::get_if<proto::StateSync>(&m)) {
        if (ss->battle_id == s.battle.id) s.battle.host_sync[ss->turn] = *ss;
    } else if (auto* dw = std::get_if<proto::Draws>(&m)) {
        if (dw->battle_id == s.battle.id)
            for (auto& d : dw->draws) s.battle.host_draws[dw->turn][(static_cast<uint64_t>(d.key) << 16) | d.rank] = d.value;
    } else if (auto* be = std::get_if<proto::BattleEnd>(&m)) {
        if (be->battle_id == s.battle.id) { s.battle.host_end = be->result; s.battle.host_end_at = s.now; }
        log().write("coop_battle_end_received", JsonObj().add("battle", be->battle_id).add("result", static_cast<int>(be->result)));
        // Combat fini chez l'hôte avant d'avoir pu être lancé ici (monde pas prêt, lancement impossible) : il ne l'est plus,
        // sinon il partirait plus tard, hors de propos (l'hôte est passé à autre chose).
        if (s.pending_start && s.pending_start->battle_id == be->battle_id) {
            s.pending_start.reset();
            bump("battle_starts_dropped");
            log().write("coop_battle_start_dropped", JsonObj().add("battle", be->battle_id).add("why", "host battle ended before it started here"));
            if (!s.battle.active) s.battle.finalized = true;   // jamais commencé ici : rien à clore dans le jeu
        }
    } else if (auto* rw = std::get_if<proto::BattleRewards>(&m)) {
        if (rw->battle_id == s.battle.id) s.battle.host_rewards = *rw;
    } else if (auto* dc = std::get_if<proto::DefeatChoice>(&m)) {
        defeat_on_host_choice(*dc);
    }
}

// --- Indicateurs, départs, fin de session (phase 5) --------------------------------------------

// « Tour de X » / « À vous de jouer » au début du tour d'un héros ; effacé pendant les tours ennemis.
void turn_indicator(const proto::Unit& u)
{
    if (u.kind != proto::ActorKind::Hero) { ui::clear_status("turn"); return; }
    const uint8_t owner = owner_of_spot(u.spot);
    if (owner == s.my_player_id) ui::set_status("turn", "your_turn");
    else ui::set_status("turn", "turn_of", {player_name(owner)});
}

// Hôte : un joueur est parti (Bye, silence, redémarrage). L'hôte reprend ses héros, combat en cours compris ; le joueur
// pourra revenir par le même chemin que « rejoindre » (instantané au prochain point sûr).
void on_player_left(uint8_t player, const std::string& name, const std::string& why)
{
    bump("players_left");
    explore::on_peer_left(player);
    auto& b = s.battle;
    proto::Owners ow;
    ow.battle_id = b.id;
    int taken = 0;
    for (auto& [spot, owner] : b.owner_by_spot) {
        if (owner != player) continue;
        owner = 0;
        ++taken;
    }
    if (b.active && taken > 0) {
        for (auto& [spot, owner] : b.owner_by_spot) {
            UObject* h = game::find_unit(true, spot);
            ow.owners.push_back({static_cast<uint8_t>(spot), h ? game::char_class(h) : std::string(), owner});
        }
        broadcast(ow);
        s.takeover = true;
        bump("heroes_taken_over", taken);
    }
    b.ready.erase(player);
    b.unresponsive.erase(player);
    send_roster();
    ui::toast(why == "bye" ? "player_left" : "player_lost", {name});
    log().write("session_leave", JsonObj().add("player_id", static_cast<int>(player)).add("name", name).add("why", why).add("heroes_taken", taken)
                                       .add("battle_active", b.active));
}

// Client : la session est finie (hôte parti ou injoignable, refus). Message clair, entrées rendues, puis retour au menu
// principal : la partie chargée est celle de l'hôte (emplacement coop), elle n'a plus de sens sans lui.
void end_session(const std::string& key, const std::string& detail)
{
    if (s.ended) return;
    s.ended = true;
    s.end_key = key;
    if (s.steam) net::steam::leave_lobby(s.lobby);
    s.ended_at = s.now;
    bump("session_ended");
    set_local_input(true, "session end");
    // Hôte : ennemis rendus passifs pendant que le client menait, rendus au jeu (la partie continue en solo).
    if (s.role == Role::Host) {
        const int n = game::restore_world_enemies();
        if (n > 0) log().write("coop_world_enemies_restored", JsonObj().add("count", n).add("why", "session end"));
    }
    explore::on_session_end();
    ui::clear_status("turn");
    ui::clear_status("wait");
    ui::clear_status("peer");
    if (key == "rejected") ui::set_status("session", "rejected", {detail});
    else ui::set_status("session", key);
    log().write("session_end", JsonObj().add("key", key).add("detail", detail).add("text", ui::statuses().count("session") ? ui::statuses().at("session") : ""));
}

// Retour au menu principal par le joueur (menu pause) : la session se termine pour lui. Hôte : tout le monde est prévenu
// (Bye) ; client : l'hôte reprend ses héros.
void subscribe_session()
{
    HookBus::get().pre(game::path::fn(game::path::CtrlWorld, L"BackToTitleMenu"), [](HookCtx&, UFunction*) {
        if (s.role != Role::Solo) coopsave::on_quit_to_title();   // hôte d'une copie : dernière copie
        if (s.role == Role::Solo || s.ended) return;
        log().write("session_quit_to_title", JsonObj().add("role", s.role == Role::Host ? "host" : "client"));
        shutdown(s.role == Role::Host ? "host quit to title" : "client quit to title");
        s.ended = true;
        s.title_requested = true;
        s.ended_at = s.now;
    });
}
// Client : lien vers l'hôte (UDP : -e33connect ; Steam : propriétaire du lobby) et présentation (Hello).
void connect_to_host(const std::string& addr)
{
    s.connect_to = addr;
    Peer host;
    host.link = std::make_unique<net::Link>(*s.transport, s.connect_to);
    host.created_at = s.now;
    host.accepted = true;
    host.player_id = 0;
    wire_link(*host.link);
    // Hôte d'une autre version de protocole : il nous répond avec sa version, on l'affiche.
    host.link->on_protocol_mismatch = [](uint16_t v) {
        const bool first = !s.ended;
        const std::string hv = s.steam && s.lobby.id ? net::steam::lobby_data(s.lobby.id, "mod") : std::string();
        const uint64_t owner = s.lobby.owner;
        end_session("rejected", ui::text("reject_protocol", {std::to_string(v), std::to_string(proto::kProtocolVersion)}));
        if (first) update::on_version_mismatch(v > proto::kProtocolVersion, player_name(0), hv, true, owner);
    };
    s.peers.push_back(std::move(host));
    proto::Hello h;
    h.game_build = s.game_build;
    h.mod_version = kModVersion;
    h.gameplay_mods = s.gameplay_mods;
    h.player_name = s.my_name;
    h.instance_id = GetTickCount64();
    h.join = s.want_join;
    send_to(s.peers.front(), h);
    heartbeat_targets();
    if (s.want_join) ui::set_status("session", "joining");
}

// Options changées en jeu (options.cpp) : réglages d'hôte relus ; l'attente sans blocage seulement hors combat.
void apply_options()
{
    if (!s.options_dirty) return;
    s.duo_parry = arg(L"e33duoparry") != L"0";
    const std::wstring t = arg(L"e33turntimer");
    s.turn_timer_s = static_cast<uint16_t>(t.empty() ? 0 : std::wcstoul(t.c_str(), nullptr, 10));
    if (s.battle.active) return;
    s.turn_dep = arg(L"e33turndep") != L"0";
    s.turn_align = arg(L"e33turnalign") != L"0";
    s.options_dirty = false;
    log().write("coop_options", JsonObj().add("duo_parry", s.duo_parry).add("turn_timer_s", static_cast<int>(s.turn_timer_s)).add("turndep", s.turn_dep)
                                   .add("turnalign", s.turn_align));
}

} // namespace e33::coop::detail

namespace e33::coop {

void reassign_character(size_t index) { if (s.role == Role::Host) cycle_owner(index); }
bool neutral() { return s.neutral && s.role == Role::Solo; }
bool ready() { return s.neutral || s.role != Role::Solo; }
void reload_options() { s.options_dirty = true; }

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
// Essai -e33turndep : début de tour retenu tant que l'état de l'hôte n'est pas là (le joueur n'a pas encore le menu ;
// le testeur, qui agit directement, attend aussi).
bool holding() { return s.battle.hold || s.battle.dep_active || s.battle.dep_finish_pending; }
bool session_ended() { return s.ended; }
int defeat_prompt_kind() { return s.defeat.retry_prompted ? 0 : 1; }
bool action_forced(int spot) { return s.battle.forced.count({s.battle.turn, spot}) > 0; }
int remote_turn_open()
{
    if (s.role == Role::Solo || !s.battle.active || s.battle.ending) return -1;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur) || ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return -1;
    const int spot = game::spot(cur);
    if (controls(spot) || s.battle.executed.count({s.battle.turn, spot})) return -1;
    return spot;
}
std::string name_of(uint8_t player) { return player_name(player); }
uint8_t my_player() { return s.my_player_id; }
uint8_t spot_owner(int spot) { return owner_of_spot(spot); }
uint8_t hero_owner(const std::string& name)
{
    if (s.battle.active)
        for (UObject* h : game::players())
            if (game::hardcoded_name(h) == name) return owner_of_spot(game::spot(h));
    return character_owner(name);
}
std::vector<std::pair<uint8_t, std::string>> players()
{
    std::vector<std::pair<uint8_t, std::string>> out;
    for (auto& [id, n] : s.roster) out.emplace_back(id, n.empty() ? player_name(id) : n);
    if (out.empty() || out.front().first != 0) out.insert(out.begin(), {uint8_t{0}, s.role == Role::Host ? s.my_name : player_name(0)});
    return out;
}
bool steam_transport() { return s.steam; }
uint64_t host_lobby()
{
    return s.role == Role::Host && s.steam && s.lobby.state == net::steam::LobbyState::Ready ? s.lobby.id : 0;
}
bool host_lobby_has(uint64_t steam_id)
{
    if (!host_lobby()) return false;
    for (uint64_t m : net::steam::lobby_members(s.lobby)) if (m == steam_id) return true;
    return false;
}
void shutdown(const char* reason)
{
    if (s.role == Role::Solo) return;
    for (auto& p : s.peers) if (p.accepted) send_to(p, proto::Bye{reason ? reason : ""});
    for (int i = 0; i < 3; ++i) {
        for (auto& p : s.peers) if (p.link) p.link->update(s.now + 0.3 * (i + 1));
        Sleep(30);
    }
    if (s.steam) net::steam::leave_lobby(s.lobby);
    log().write("session_shutdown", JsonObj().add("reason", reason ? reason : ""));
}
void set_local_aim(const double* point)
{
    if (point) s.local_aim = std::array<double, 3>{point[0], point[1], point[2]};
    else s.local_aim.reset();
}
const std::map<std::string, int>& counters() { return s.counters; }
const char* mod_version() { return kModVersion; }
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

} // namespace e33::coop

namespace e33::coop::detail {

// Réglages lus une fois au démarrage (indépendants du rôle).
void read_settings()
{
    for (const char* c : {"desyncs", "raw_hash_diffs", "hashes_compared", "gameplay_draws", "draw_mismatches", "draw_count_diffs", "draw_errors", "invalid_messages",
                          "session_ended", "players_left", "heroes_taken_over", "peer_timeouts", "sessions_rejected", "battle_ready_timeouts", "turn_timer_expired",
                          "defense_timeouts", "defense_waits_owner_ahead", "combo_timeouts", "turn_wait_timeouts", "actor_mismatches", "end_mismatches", "rewards_mismatches",
                          "repair_failures", "defense_verdict_mismatches", "combo_verdict_mismatches", "remote_actions",
                          "setup_draws_replaced", "setup_draws_natural", "start_mech_diffs", "raw_mech_diffs"})
        s.counters[c] = 0;

    s.game_build = read_build_id();
    s.gameplay_mods = read_gameplay_mods();
    s.session_id = GetTickCount();
    if (const std::wstring seed = arg(L"e33seed"); !seed.empty()) s.fixed_seed = std::wcstoull(seed.c_str(), nullptr, 0);
    s.setup_draws = arg(L"e33setupdraws") != L"0";
    s.defeat_sync = arg(L"e33defeatsync") != L"0";
    s.defeat_abandon_resync = arg(L"e33defeatabandon") == L"resync";
    if (const std::wstring d = arg(L"e33actiondelay"); !d.empty()) s.action_delay_s = std::wcstod(d.c_str(), nullptr);
    if (const std::wstring p = arg(L"e33perturb"); !p.empty()) s.perturb_turn = static_cast<uint32_t>(std::wcstoul(p.c_str(), nullptr, 10));
    if (const std::wstring p = arg(L"e33perturbend"); !p.empty()) s.perturb_end_turn = static_cast<uint32_t>(std::wcstoul(p.c_str(), nullptr, 10));
    if (arg(L"e33lockinput") == L"0") s.lock_input = false;
    if (const std::wstring l = arg(L"e33replaylag"); !l.empty()) s.replay_lag_s = std::wcstod(l.c_str(), nullptr);
    s.turn_dep = arg(L"e33turndep") != L"0";   // attente sans figer le jeu, par défaut depuis le 2026-10-22 (-e33turndep=0 : attente bloquante)
    if (const std::wstring t = arg(L"e33turntimer"); !t.empty()) s.turn_timer_s = static_cast<uint16_t>(std::wcstoul(t.c_str(), nullptr, 10));
    s.duo_parry = arg(L"e33duoparry") != L"0";
    if (const std::wstring v = arg(L"e33readyidle"); !v.empty()) s.ready_idle_s = std::wcstod(v.c_str(), nullptr);
    if (const std::wstring v = arg(L"e33stuckturn"); !v.empty()) s.stuck_turn_s = std::wcstod(v.c_str(), nullptr);
    s.turn_align = arg(L"e33turnalign") != L"0";   // démarrage synchronisé des tours ennemis, par défaut depuis le 2026-10-24 (D28)
    s.my_name = ue::to_utf8(arg(L"e33name"));
    s.name_from_arg = !s.my_name.empty();
    ui::init();
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
        s.owner_overrides_arg = s.owner_overrides;
    }
    if (const std::wstring l = arg(L"e33lobby"); !l.empty()) s.lobby_wanted = std::wcstoull(l.c_str(), nullptr, 10);
    if (arg(L"e33steamlobby") == L"0") s.steam_lobby = false;
    if (arg(L"e33steamselftest") == L"1") s.steam_selftest = true;
}

void coopsave_init(bool host)
{
    // Mode neutre : F10 au menu principal fait de ce joueur l'hôte de la copie (become_host).
    std::function<bool()> become;
    if (s.neutral) become = [] {
        if (s.role == Role::Host) return true;
        if (s.role != Role::Solo) return false;
        start_session(Role::Host, "F10 copy");
        return s.role == Role::Host;
    };
    coopsave::init(host, [](const char* c, int n) { s.counters[c] += n; }, become);
}

// Rôle décidé (option de lancement, ou en jeu en mode neutre) : modules initialisés pour ce rôle, hooks abonnés une
// fois. En mode neutre, les hooks du coop ne sont abonnés qu'ici (partie solo sans session : rien d'accroché).
void start_session(Role r, const char* why)
{
    s.role = r;
    s.my_player_id = 0;
    s.runtime_session = s.neutral;
    if (r == Role::Client && s.neutral) s.want_join = true;   // en jeu, un client rejoint toujours la partie de l'hôte
    if (!s.name_from_arg && (s.my_name.empty() || s.my_name == "Hôte" || s.my_name == "Joueur")) {
        const std::string pn = s.steam && net::steam::loaded() ? net::steam::persona_name() : std::string();
        s.my_name = !pn.empty() ? pn : r == Role::Host ? "Hôte" : "Joueur";
    }
    static bool recorder_open = false;
    if (!recorder_open) {
        recorder_open = true;
        s.recorder.open(ue::to_utf8((std::filesystem::path(launch_args().run_dir) / (r == Role::Host ? L"recording-host.jsonl" : L"recording-client.jsonl")).wstring()));
    }
    log().set_role(r == Role::Host ? "host" : "client");

    std::string mods = "[";
    for (auto& m : s.gameplay_mods) { if (mods.size() > 1) mods += ","; mods += "\"" + json_escape(m) + "\""; }
    log().write("coop_init", JsonObj().add("role", r == Role::Host ? "host" : "client").add("local", s.transport ? s.transport->local_address() : std::string("steam (pending)"))
                                 .add("game_build", s.game_build).add("mod_version", kModVersion).add("duo_parry", s.duo_parry).raw("gameplay_mods", mods + "]")
                                 .add("why", why ? why : "").add("runtime", s.runtime_session));

    if (r == Role::Client && !s.steam) connect_to_host(ue::to_utf8(arg(L"e33connect")));
    explore::Net net;
    net.publish = [](const proto::Message& m, bool reliable) { publish(m, nullptr, reliable); };
    net.send_to = [](uint8_t player, const proto::Message& m) { for (auto& p : s.peers) if (p.accepted && p.player_id == player) send_to(p, m); };
    net.relay = [](const proto::Message& m, uint8_t origin, bool reliable) { for (auto& p : s.peers) if (p.accepted && p.player_id != origin) send_to(p, m, reliable); };
    net.bump = [](const char* c, int n) { s.counters[c] += n; };
    net.name_of = [](uint8_t id) { return player_name(id); };
    net.battle_engaged = [] { return s.pending_start.has_value() || (s.battle.active && !s.battle.finalized); };
    explore::init(r == Role::Host, s.want_join, net);
    {
        builds::Net bn;
        bn.publish = [](const proto::Message& m) { publish(m); };
        bn.send_to = [](uint8_t player, const proto::Message& m) { for (auto& p : s.peers) if (p.accepted && p.player_id == player) send_to(p, m); };
        bn.relay = [](const proto::Message& m, uint8_t origin) { for (auto& p : s.peers) if (in_game(p) && p.player_id != origin) send_to(p, m); };
        bn.bump = [](const char* c, int n) { s.counters[c] += n; };
        bn.name_of = [](uint8_t id) { return player_name(id); };
        bn.owner_of = [](const std::string& n) { return character_owner(n); };
        bn.sharing = [] { if (s.role == Role::Client) return explore::joined() && s.accepted; for (auto& p : s.peers) if (in_game(p)) return true; return false; };
        bn.last_event_age = [] { return explore::last_event_age(); };
        bn.request_resync = [](const std::string& why) { publish(proto::ResyncRequest{why}); };
        builds::init(r == Role::Host, bn);
    }
    if (s.neutral) coopsave::set_host(r == Role::Host);   // déjà initialisé au démarrage (F10 peut appeler d'ici)
    else coopsave_init(r == Role::Host);
    {
        intent::Net in;
        in.publish = [](const proto::Message& m, bool reliable) { publish(m, nullptr, reliable); };
        in.bump = [](const char* c, int n) { s.counters[c] += n; };
        in.name_of = [](uint8_t id) { return player_name(id); };
        in.owner_of_spot = [](int spot) { return owner_of_spot(spot); };
        in.controls = [](int spot) { return controls(spot); };
        in.action_known = [](uint32_t turn, int spot) { return s.battle.commands.count({turn, spot}) > 0 || s.battle.executed.count({turn, spot}) > 0; };
        in.local_aiming = [] { return s.fa_own.active; };
        in.battle = [] { return intent::BattleRef{s.battle.id, s.battle.turn, s.battle.active}; };
        in.remote_aim_active = [] { return s.battle.active && s.fa_rep.started && !s.fa_rep.done && s.fa_rep.key.first == s.battle.turn; };
        in.paused = [] { return s.battle.hold || explore::remote_paused() || game::pause_menu_visible(); };
        intent::init(in);
    }
    if (!s.hooks_subscribed) {
        s.hooks_subscribed = true;
        subscribe();
        intent::subscribe();
        explore::subscribe();
        if (!s.neutral) {   // mode neutre : déjà abonnés au démarrage
            coopsave::subscribe();
            subscribe_session();
            if (r == Role::Client && s.steam) ui::joinbutton::subscribe();   // bouton « Rejoindre <ami> » du menu principal
            ui::joinbutton::subscribe_version();   // version du mod au menu principal
        } else {
            HookBus::get().try_register();   // en pleine partie : classes déjà chargées, accrochées tout de suite
        }
    }
}

} // namespace e33::coop::detail

namespace e33::coop {

void init()
{
    std::wstring r = arg(L"e33role");
    // Test à distance sans options de lancement : client Steam qui rejoint la partie de l'hôte de confiance.
    const bool remote_default = r.empty() && !cmdline::test_mode() && remote::client_enabled();
    if (remote_default) r = L"client";
    Role role = Role::Solo;
    if (r == L"host") role = Role::Host;
    else if (r == L"client") role = Role::Client;
    // En test sans rôle : solo pur, comme avant (aucune lecture, aucun hook).
    if (role == Role::Solo && cmdline::test_mode()) return;

    read_settings();
    update::init();
    if (role == Role::Solo) {
        // Partie normale sans option -e33role : prêt au coop par Steam ; le rôle se décide en jeu (D18).
        s.neutral = true;
        s.steam = true;
        s.rejoin_host = update::take_rejoin();
        s.rejoin_until = s.rejoin_host ? 180.0 : 0;
        coopsave_init(false);
        coopsave::subscribe();
        subscribe_session();
        ui::joinbutton::subscribe();
        ui::joinbutton::subscribe_version();   // version du mod au menu principal
        log().write("coop_neutral", JsonObj().add("mod_version", kModVersion).add("rejoin", std::to_string(s.rejoin_host)));
        return;
    }
    if (role == Role::Client && (arg(L"e33join") == L"1" || remote_default)) s.want_join = true;
    std::string err;
    const std::wstring port_s = arg(L"e33port");
    const uint16_t port = static_cast<uint16_t>(port_s.empty() ? (role == Role::Host ? 7777 : 0) : std::stoi(port_s));
    s.steam = arg(L"e33transport") == L"steam" || remote_default;
    if (!s.steam) {
        s.transport = net::make_udp(port, err);
        if (!s.transport) { log().write("coop_error", JsonObj().add("what", err)); return; }
    }
    // Steam : transport et lobby créés au premier tick (rien dans les 10 premières secondes, voir dllmain), steam_tick.
    start_session(role, "launch option");
}

} // namespace e33::coop
