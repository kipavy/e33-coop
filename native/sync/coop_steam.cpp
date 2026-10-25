// Transport Steam (phase 6) et mode neutre (D18).
#include "coop_internal.hpp"

namespace e33::coop::detail {

// --- Transport Steam (phase 6) ---------------------------------------------------------------------
// Rien de Steam avant le premier tick (le mod reste inerte les 10 premières secondes). Hôte : lobby « amis seulement »
// créé d'office, F7 ouvre la fenêtre d'invitation de l'overlay ; les membres du lobby sont « ouverts » (punch) pour
// que Steam accepte leurs messages. Client : lobby donné par -e33lobby, par Steam (+connect_lobby, invitation acceptée
// jeu fermé), par une invitation acceptée pendant que le jeu tourne, ou F7 au menu principal (lobby coop d'un ami) ;
// une fois dedans, connexion au propriétaire du lobby (son SteamID) comme en UDP.

bool key_pressed(int vk, bool& was_down)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool pressed = down && !was_down && pid == GetCurrentProcessId();
    was_down = down;
    return pressed;
}

void steam_tick()
{
    if (!s.steam) return;
    if (!s.transport) {
        std::string err;
        s.transport = net::steam::make_transport(err);
        if (!s.transport) {
            if (!s.steam_error_shown) {
                s.steam_error_shown = true;
                bump("steam_errors");
                ui::toast("steam_unavailable", {err}, 12.0);
                log().write("coop_steam_error", JsonObj().add("what", err));
            }
            return;
        }
        if (!s.name_from_arg) {
            const std::string pn = net::steam::persona_name();
            if (!pn.empty()) s.my_name = pn;
        }
        log().write("coop_steam_ready", JsonObj().add("steam_id", std::to_string(net::steam::my_id())).add("name", s.my_name)
                                           .add("command_line_lobby", std::to_string(net::steam::command_line_lobby())));
    }
    if (s.steam_selftest && s.now >= s.next_punch) {
        // Auto-test : un message P2P à son propre SteamID par seconde (compté à la réception dans pump_network).
        s.next_punch = s.now + 1.0;
        uint8_t pkt[8] = {'E', '3', '3', 'S'};
        const uint32_t seq = ++s.selftest_seq;
        std::memcpy(pkt + 4, &seq, 4);
        const bool ok = s.transport->send(net::steam::peer_of(net::steam::my_id()), pkt, sizeof pkt);
        bump(ok ? "steam_selftest_sent" : "steam_selftest_send_errors");
    }
    if (!s.steam_lobby) return;
    net::steam::poll_lobby(s.lobby);
    if (s.lobby.state == net::steam::LobbyState::Failed && !s.lobby_error_shown) {
        s.lobby_error_shown = true;
        bump("steam_errors");
        ui::toast("steam_lobby_failed", {s.lobby.error}, 10.0);
        log().write("coop_steam_lobby_failed", JsonObj().add("what", s.lobby.error));
    }
    const bool f7 = key_pressed(VK_F7, s.f7_down);
    if (s.role == Role::Solo) { neutral_steam(f7); return; }   // mode neutre : rôle décidé ici
    if (s.role == Role::Host) {
        if (s.lobby.state == net::steam::LobbyState::None) net::steam::create_lobby(s.lobby, kModVersion, proto::kProtocolVersion);
        if (s.lobby.state == net::steam::LobbyState::Ready && !s.lobby_announced) {
            s.lobby_announced = true;
            ui::toast("session_open_steam", {}, 10.0);
            log().write("coop_steam_lobby", JsonObj().add("lobby", std::to_string(s.lobby.id)));
        }
        if (s.invite_on_ready && s.lobby.state == net::steam::LobbyState::Ready) {   // ouvert par F7 : l'invitation suit
            s.invite_on_ready = false;
            net::steam::invite_dialog(s.lobby);
            bump("steam_invite_dialogs");
        }
        if (f7) {
            if (s.lobby.state == net::steam::LobbyState::Ready) { net::steam::invite_dialog(s.lobby); bump("steam_invite_dialogs"); }
            else if (s.lobby.state == net::steam::LobbyState::Failed) { s.lobby = {}; s.lobby_error_shown = false; }   // nouvel essai
        }
        if (s.now >= s.next_punch && s.lobby.state == net::steam::LobbyState::Ready) {
            s.next_punch = s.now + 1.0;
            const uint64_t me = net::steam::my_id();
            for (uint64_t m : net::steam::lobby_members(s.lobby)) if (m && m != me) net::steam::punch(*s.transport, m);
        }
        (void)net::steam::take_join_request();   // l'hôte n'en a pas l'usage
        return;
    }
    // Client
    if (!s.peers.empty()) {
        if (s.now >= s.next_punch && s.lobby.owner) { s.next_punch = s.now + 1.0; net::steam::punch(*s.transport, s.lobby.owner); }
        return;
    }
    if (s.lobby.state == net::steam::LobbyState::None) {
        uint64_t id = s.lobby_wanted;
        if (!id) id = net::steam::command_line_lobby();
        if (!id) id = net::steam::take_join_request();
        const bool clicked = ui::joinbutton::take_click();   // bouton « Rejoindre <ami> » du menu principal
        if (!id && (f7 || clicked)) {
            if (clicked) log().write("coop_join_button_clicked", JsonObj());
            id = net::steam::find_friend_lobby(kModVersion);
            if (!id) ui::toast("steam_no_friend_lobby", {}, 8.0);
        }
        if (!id) id = remote::auto_join_lobby();   // test à distance : lobby de l'hôte de confiance, au menu principal
        if (id) {
            s.lobby_wanted = 0;
            net::steam::join_lobby(s.lobby, id);
            ui::set_status("session", "joining");
            log().write("coop_steam_join", JsonObj().add("lobby", std::to_string(id)));
        }
        return;
    }
    if (s.lobby.state == net::steam::LobbyState::Failed) {
        s.lobby = {};   // F7 ou une nouvelle invitation pour réessayer
        s.lobby_error_shown = false;
        ui::clear_status("session");
        if (s.runtime_session) back_to_neutral("lobby failed");
        return;
    }
    if (s.lobby.state == net::steam::LobbyState::Ready && s.lobby.owner) {
        net::steam::punch(*s.transport, s.lobby.owner);
        connect_to_host(net::steam::peer_of(s.lobby.owner));
        log().write("coop_steam_connect", JsonObj().add("host", std::to_string(s.lobby.owner)));
    }
}

// --- Mode neutre (D18) -----------------------------------------------------------------------------------------------
// Partie normale sans -e33role : personne n'a de rôle. Au menu principal, une invitation acceptée, +connect_lobby, F7 ou
// le bouton « Rejoindre <ami> » font de ce joueur un client ; dans une partie chargée, F7 l'ouvre au coop (hôte : lobby
// créé alors, fenêtre d'invitation dès qu'il est prêt) ; F10 au menu héberge la copie coop (coopsave). Fin de session
// (retour au menu principal) : retour au mode neutre.
void neutral_steam(bool f7)
{
    if (s.now >= s.next_place_check) {
        s.next_place_check = s.now + 0.5;   // FindFirstOf : pas à chaque image
        const bool menu = game::main_menu() != nullptr && !game::controller();
        const bool world = !menu && game::controller() && game::world_pawn() && !game::map_change_in_progress();
        if (world && !s.ready_hint_shown && !game::in_battle()) {   // partie chargée : rappel de F7, une fois par lancement
            s.ready_hint_shown = true;
            ui::toast("coop_ready_hint", {}, 8.0);
        }
        // Rappel de F7 une seule fois par lancement du jeu (avant : à chaque partie chargée, jugé parasite).
        s.at_menu = menu;
        s.in_world = world;
    }
    uint64_t id = 0;
    const char* why = "";
    if (!s.cmdline_lobby_used) {   // une seule fois : -e33lobby, ou « +connect_lobby » (invitation acceptée jeu fermé)
        s.cmdline_lobby_used = true;
        id = s.lobby_wanted ? s.lobby_wanted : net::steam::command_line_lobby();
        s.lobby_wanted = 0;
        why = "command line lobby";
    }
    if (const uint64_t req = net::steam::take_join_request()) {
        // En pleine partie : rien n'est quitté sans le joueur (sauvegardes) ; la partie de l'ami sera rejointe au menu.
        if (s.in_world) { s.pending_lobby = req; ui::toast("invite_in_game", {}, 10.0); log().write("coop_invite_pending", JsonObj().add("lobby", std::to_string(req))); }
        else { id = req; why = "steam invite"; }
    }
    if (!id && s.pending_lobby && s.at_menu) { id = s.pending_lobby; s.pending_lobby = 0; why = "pending invite"; }
    if (!id && s.rejoin_host && s.at_menu && s.now >= s.next_rejoin_try) {   // relance après une mise à jour
        s.next_rejoin_try = s.now + 3.0;
        if (s.now > s.rejoin_until) s.rejoin_host = 0;
        else if (const uint64_t l = net::steam::friend_lobby(s.rejoin_host)) { id = l; why = "rejoin after update"; s.rejoin_host = 0; }
    }
    const bool clicked = ui::joinbutton::take_click();
    if (!id && ((f7 && s.at_menu) || clicked)) {
        why = clicked ? "join button" : "F7 menu";
        if (clicked) log().write("coop_join_button_clicked", JsonObj());
        id = net::steam::find_friend_lobby(kModVersion);
        if (!id) {   // partie d'un ami avec une autre version : refus clair, puis proposition de mise à jour
            const auto any = net::steam::friend_lobbies(kModVersion, true);
            if (!any.empty()) id = any.front().lobby;
        }
        if (!id && clicked) ui::toast("steam_no_friend_lobby", {}, 8.0);
        if (!id && !clicked) {
            // F7 au menu sans partie d'ami : ouvrir la sienne dès le menu. L'ami rejoint ici, puis la partie lancée
            // (nouvelle partie ensemble avec l'intro, ou sauvegarde chargée) est partagée, comme avec l'ancien -e33role=host.
            start_session(Role::Host, "F7 menu");
            s.invite_on_ready = true;
            bump("coop_opened");
            bump("coop_opened_at_menu");
            ui::toast("coop_opened_menu", {}, 10.0);
            log().write("coop_opened_at_menu", JsonObj());
            return;
        }
    }
    if (id) {
        start_session(Role::Client, why);
        net::steam::join_lobby(s.lobby, id);
        ui::set_status("session", "joining");
        log().write("coop_steam_join", JsonObj().add("lobby", std::to_string(id)).add("why", why));
        return;
    }
    if (f7 && s.in_world) {
        if (game::in_battle()) { ui::toast("open_not_in_battle", {}, 6.0); return; }
        start_session(Role::Host, "F7");
        s.invite_on_ready = true;
        bump("coop_opened");
    }
}

// Session ouverte en jeu terminée, joueur revenu au menu principal : tout ce qui dépend du rôle est remis à zéro. Les
// hooks restent abonnés (ils ne font rien sans rôle) ; le transport Steam est gardé.
void back_to_neutral(const char* why)
{
    log().write("coop_back_to_neutral", JsonObj().add("why", why ? why : "").add("role", s.role == Role::Host ? "host" : s.role == Role::Client ? "client" : "solo"));
    if (s.steam) net::steam::leave_lobby(s.lobby);
    s.peers.clear();
    heartbeat_targets();
    explore::reset();
    builds::reset();
    presence::reset();
    coopsave::set_host(false);
    s.role = Role::Solo;
    s.runtime_session = false;
    s.my_player_id = 0;
    s.session_id = GetTickCount();
    s.connect_to.clear();
    s.accepted = s.rejected = false;
    s.battle = Battle{};
    s.owner_overrides = s.owner_overrides_arg;
    s.pending_start.reset();
    s.last_start.reset();
    s.early_start.reset();
    s.early_start_at = -1;
    s.world_ready_s = 0;
    s.next_trigger_try = 0;
    s.imposing = false;
    s.want_join = false;
    s.input_enabled = true;
    s.input_sub = nullptr;
    s.removed_contexts.clear();
    s.fa_own = FreeAimOwner{};
    s.fa_own_ended = {0, -1};
    s.fa_rep = FreeAimReplay{};
    s.local_aim.reset();
    s.roster.clear();
    s.ended = false;
    s.end_key.clear();
    s.ended_at = -1;
    s.title_requested = false;
    s.hello_at = 0;
    s.takeover = false;
    s.version_rejected.clear();
    s.tombstones.clear();
    s.lobby = {};
    s.lobby_announced = s.lobby_error_shown = false;
    s.next_punch = 0;
    s.invite_on_ready = false;
    s.keys_shown = false;
    s.in_world = false;
    for (const char* slot : {"turn", "pause", "wait", "waited", "peer", "keys", "invite"}) ui::clear_status(slot);
    if (ui::statuses().count("session")) s.session_msg_until = s.now + 12.0;   // message de fin encore lisible un moment
    bump("back_to_neutral");
}

} // namespace e33::coop::detail
