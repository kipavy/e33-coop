// Tick du mode coop (game thread) et diagnostics par image.
#include "coop_internal.hpp"

namespace e33::coop {

// Diagnostic (2026-10-24 : parade du client sans effet) : à chaque image en combat, apparition / disparition des
// contextes de défense, de blocage et de visée libre, et du drapeau IsDefenseInputBound. Comparer hôte et client.
static void defense_context_watch()
{
    static int logs = 0;
    static int last[4] = {-2, -2, -2, -2};   // -1 absent, sinon priorité ; dernier : drapeau bound
    if (!s.battle.active || s.role == Role::Solo) { last[0] = last[1] = last[2] = last[3] = -2; return; }
    UObject* sub = game::input_subsystem();
    if (!sub) return;
    static const wchar_t* names[] = {L"IMC_BattleDefense", L"IMC_Battle_InputBlocker", L"IMC_Battle_FreeAim"};
    static UObject* imcs[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (!imcs[i]) imcs[i] = ue::find_object((std::wstring(L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/") + names[i] + L"." + names[i]).c_str());
        if (!imcs[i]) continue;
        ue::Call has(sub, STR("HasMappingContext"));
        if (!has) return;
        has.set_object(STR("MappingContext"), imcs[i]).invoke();
        const int now = has.out_bool(STR("ReturnValue")) ? static_cast<int>(has.out_int(STR("OutFoundPriority"))) : -1;
        if (now == last[i]) continue;
        last[i] = now;
        if (++logs > 400) continue;
        log().write("coop_imc_change", JsonObj().add("imc", ue::to_utf8(names[i])).add("active", now >= 0).add("priority", now)
                                           .add("input_enabled", s.input_enabled), s.battle.turn);
        // Contexte de défense ouvert : ses touches (une fois), widgets CommonUI activés et type d'entrée (manette ?).
        if (i == 0 && now >= 0) {
            static bool mapped = false;
            JsonObj o;
            if (!mapped) { mapped = true; o.add("mappings", game::imc_mappings(imcs[0])); }
            log().write("coop_defense_ui_state", o.add("input_type", game::common_input_type()).add("widgets", game::active_common_widgets())
                                                    .add("rb", game::active_contexts_with_key("Gamepad_RightShoulder"))
                                                    .add("rt", game::active_contexts_with_key("Gamepad_RightTrigger"))
                                                    .add("e", game::active_contexts_with_key("E")), s.battle.turn);
        }
    }
    UObject* ctrl = game::controller();
    const int bound = ctrl ? (ue::get_bool(ctrl, STR("IsDefenseInputBound")) ? 1 : 0) : -1;
    if (bound != last[3]) {
        last[3] = bound;
        if (++logs <= 400) log().write("coop_imc_change", JsonObj().add("imc", "IsDefenseInputBound").add("active", bound == 1).add("input_enabled", s.input_enabled), s.battle.turn);
    }
}

// Activité des autres joueurs (écrans d'attente) : chargement (battements « occupé ») et silence du lien, par pair
// direct. Ligne d'information quand un joueur charge sans qu'on l'attende (« TOTOFF charge votre partie… 12 s »).
static void peer_activity_tick()
{
    std::vector<uint8_t> ids;
    for (auto& p : s.peers) {
        if (!p.link || !p.accepted || !p.drop.empty()) continue;
        const double heard = p.link->last_heard();
        presence::set_link(p.player_id, p.link->busy(s.now), heard >= 0 ? s.now - heard : 0.0, s.now);
        ids.push_back(p.player_id);
    }
    // Client pas encore entré : son écran d'attente est déjà la ligne « session ».
    const bool show = !ui::statuses().count("wait") && (s.role == Role::Host || explore::joined());
    ui::set_status_text("peer", "peer_loading", show ? presence::loading_text(ids, s.now, player_name) : std::string());
}

// Diagnostic (essai à deux PC du 2026-10-23) : jeu relancé par un script (relaunch.ps1, installateur) -> aucun salon
// d'ami vu pendant 10 min, alors qu'un lancement à la main le trouve en quelques secondes. Au menu, toutes les 30 s
// tant qu'aucun salon n'est trouvé, puis une fois quand un salon l'est : amis, GetFriendGamePlayed de l'hôte de
// confiance et de l'ami du rejoin.txt, état de persona ; processus (parents, ligne de commande, variables Steam*) au
// premier enregistrement et à celui du salon trouvé, pour comparer lancement à la main et lancement par script.
static void lobby_diag_tick(bool found)
{
    static double next = 0;
    static bool found_logged = false, process_logged = false;
    if (found ? found_logged : s.now < next) return;
    next = s.now + 30.0;
    if (found) found_logged = true;
    std::vector<uint64_t> focus{remote::trusted_host()};
    if (s.rejoin_host && s.rejoin_host != focus.front()) focus.push_back(s.rejoin_host);
    JsonObj o;
    o.add("found", found).add("t", s.now).raw("steam", net::steam::friends_diag_json(focus));
    if (!process_logged || found) o.raw("process", net::steam::process_diag_json());
    process_logged = true;
    log().write("coop_lobby_diag", o);
}

// Client Steam au menu principal : bouton « Rejoindre <ami> » quand un ami a ouvert une partie coop (liste d'amis
// relue toutes les 5 s, seulement au menu). Le clic passe par le même chemin que F7 (steam_tick).
static void join_button_tick()
{
    static double next_menu_check = 0, next_scan = 0;
    static bool at_menu = false;
    static std::string friend_name;
    const bool eligible = (s.role == Role::Client || (s.role == Role::Solo && s.neutral)) && s.steam && s.steam_lobby && s.transport && s.peers.empty() &&
                          s.lobby.state == net::steam::LobbyState::None && !s.lobby_wanted;
    if (!eligible) { friend_name.clear(); at_menu = false; (void)ui::joinbutton::take_click(); }   // pas de clic en attente
    else if (s.now >= next_menu_check) {
        next_menu_check = s.now + 0.5;   // FindFirstOf : pas à chaque image
        at_menu = game::main_menu() != nullptr;
        if (!at_menu) friend_name.clear();
        else if (s.now >= next_scan) {
            next_scan = s.now + 5.0;
            // Mode neutre : aussi la partie d'un ami d'une autre version (le refus dira qui doit mettre à jour).
            auto lobbies = net::steam::friend_lobbies(kModVersion, s.neutral);
            std::stable_sort(lobbies.begin(), lobbies.end(), [](const auto& a, const auto& b) { return a.compatible && !b.compatible; });
            const std::string n = lobbies.empty() ? std::string() : lobbies.front().name;
            if (n != friend_name) log().write("coop_friend_lobby", JsonObj().add("friend", n).add("count", static_cast<int64_t>(lobbies.size())));
            friend_name = n;
            lobby_diag_tick(!lobbies.empty());
        }
    }
    ui::joinbutton::tick(s.now, friend_name);
    // Plan B : bouton impossible à placer dans le menu du jeu -> carte d'invitation (touche F7).
    if (!friend_name.empty() && ui::joinbutton::failed()) ui::set_status("invite", "friend_invite", {friend_name});
    else ui::clear_status("invite");
}

// Mode neutre (aucun rôle) : Steam prêt, bouton « Rejoindre », F7 / F10, copie coop. Le rôle décidé, tick() prend la suite.
static void neutral_tick(double dt)
{
    s.game_thread = GetCurrentThreadId();
    advance_clock(dt);
    hb.last_tick_ms = steady_ms();
    steam_tick();
    if (s.role != Role::Solo) return;
    join_button_tick();
    ui::tick(s.now);
    coopsave::tick(s.now, true);
    apply_options();
    if (s.session_msg_until >= 0 && s.now > s.session_msg_until) { s.session_msg_until = -1; ui::clear_status("session"); }
}

// À-coups d'image hors combat (2026-10-22 : « freeze » chez le client quand l'hôte court, sans journal en exploration).
// Une ligne par à-coup (> 80 ms, au plus une par seconde) avec le travail du mod dans l'image en cause (étapes de
// l'image précédente, contenue dans ce dt), et un résumé toutes les 30 s (pas une ligne par image).
struct ExploreFrames {
    double since = -1;
    int frames = 0;
    double dt_sum = 0, dt_max = 0;
    int h50 = 0, h80 = 0, h150 = 0, h300 = 0;
    double mod_sum = 0, mod_max = 0;
    std::vector<std::pair<const char*, double>> step_max;
    double last_line = -10;
    int suppressed = 0;
};
static ExploreFrames ef;

static std::string steps_json(const prof::Frame& f, double min_ms, size_t max)
{
    auto v = f.steps;
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    JsonObj o;
    size_t n = 0;
    for (auto& [name, ms] : v) {
        if (ms < min_ms || n++ >= max) break;
        o.add(name, std::round(ms * 100.0) / 100.0);
    }
    return o.str();
}

static void explore_frames_tick(double dt)
{
    if (s.battle.active) { ef = ExploreFrames{}; return; }   // combat : frame_hitch (plus haut)
    const prof::Frame& last = prof::last();
    if (ef.since < 0) ef.since = s.now;
    ++ef.frames;
    ef.dt_sum += dt;
    ef.dt_max = std::max(ef.dt_max, dt);
    if (dt > 0.05) ++ef.h50;
    if (dt > 0.08) ++ef.h80;
    if (dt > 0.15) ++ef.h150;
    if (dt > 0.30) ++ef.h300;
    ef.mod_sum += last.total_ms;
    ef.mod_max = std::max(ef.mod_max, last.total_ms);
    for (auto& [name, ms] : last.steps) {
        bool found = false;
        for (auto& [n, m] : ef.step_max) if (n == name) { m = std::max(m, ms); found = true; break; }
        if (!found) ef.step_max.emplace_back(name, ms);
    }
    // Images > 50 ms pendant un mouvement (suivi, D23) ; « mod » : le travail du mod en fait au moins la moitié.
    if (dt > 0.05 && dt < 3.0 && explore::motion_recent() && !game::map_change_in_progress()) {
        bump("explore_hitches_moving");
        if (last.total_ms > 500.0 * dt) bump("explore_hitches_mod");
    }
    if (dt > 0.08 && dt < 3.0) {   // au-delà : chargement (écran figé attendu)
        bump("explore_frame_hitches");
        if (s.now - ef.last_line < 1.0) ++ef.suppressed;
        else if (!game::map_change_in_progress()) {
            ef.last_line = s.now;
            log().write("frame_hitch_explore", JsonObj().add("dt_ms", static_cast<int64_t>(dt * 1000.0))
                                                   .add("mod_ms", std::round(last.total_ms * 100.0) / 100.0)
                                                   .raw("steps", steps_json(last, 0.2, 8))
                                                   .add("heavy", prof::last_heavy())
                                                   .add("pilot", explore::is_pilot())
                                                   .add("follow_error", static_cast<int64_t>(explore::follow_error()))
                                                   .add("suppressed", ef.suppressed));
            ef.suppressed = 0;
        }
    }
    if (s.now - ef.since >= 30.0) {
        prof::Frame mx;
        mx.steps = ef.step_max;
        log().write("explore_frame_stats", JsonObj().add("s", std::round((s.now - ef.since) * 10.0) / 10.0).add("frames", ef.frames)
                                               .add("dt_avg_ms", std::round(ef.dt_sum / ef.frames * 10000.0) / 10.0)
                                               .add("dt_max_ms", static_cast<int64_t>(ef.dt_max * 1000.0))
                                               .add("over_50ms", ef.h50).add("over_80ms", ef.h80).add("over_150ms", ef.h150).add("over_300ms", ef.h300)
                                               .add("mod_avg_ms", std::round(ef.mod_sum / ef.frames * 1000.0) / 1000.0)
                                               .add("mod_max_ms", std::round(ef.mod_max * 100.0) / 100.0)
                                               .raw("step_max_ms", steps_json(mx, 0.5, 12))
                                               .add("pilot", explore::is_pilot()));
        const double keep_line = ef.last_line;
        ef = ExploreFrames{};
        ef.last_line = keep_line;
    }
}

void tick(double dt)
{
    if (s.role == Role::Solo) { if (s.neutral) neutral_tick(dt); return; }
    s.game_thread = GetCurrentThreadId();
    advance_clock(dt);
    if (const int64_t st = hb.stall_of.exchange(0); st) {
        log().write("game_thread_resumed", JsonObj().add("stalled_ms", steady_ms() - st).add("battle", s.battle.active));
        if (s.battle.active) bump("game_thread_stalls_battle");   // gel de plus de 5 s en plein combat (hors chargement)
    }
    hb.last_tick_ms = steady_ms();
    heartbeat_start();
    if (!s.ended) explore_frames_tick(dt);
    { prof::Scope p("steam"); steam_tick(); }
    { prof::Scope p("join_button"); join_button_tick(); }
    { prof::Scope p("ui"); ui::tick(s.now); }
    { prof::Scope p("coopsave"); coopsave::tick(s.now, !s.ended); }   // copie coop (D15) : aussi après la fin de session (retour au menu)
    apply_options();
    if (s.ended) {
        // Client : session finie. Message affiché, puis retour au menu principal (la partie chargée est celle de l'hôte).
        if (!s.title_requested && s.now - s.ended_at > 6.0 && game::controller() && !game::main_menu()) {
            s.title_requested = true;
            const bool ok = game::back_to_title();
            log().write("session_back_to_title", JsonObj().add("ok", ok));
        }
        if (s.title_requested && game::main_menu() && s.now - s.ended_at > 20.0) ui::clear_status("session");
        // Session ouverte en jeu : de retour au menu principal, le joueur redevient « prêt au coop » (mode neutre).
        if (s.runtime_session && s.now - s.ended_at > 3.0 && s.now >= s.next_place_check) {
            s.next_place_check = s.now + 0.5;
            if (!game::controller() && game::main_menu()) back_to_neutral(s.end_key.empty() ? "quit to title" : s.end_key.c_str());
        }
        return;
    }
    // À-coups d'image en combat (> 50 ms) : un appui de QTE peut sembler « raté d'emblée » si le jeu saccade.
    if (s.battle.active) bump_max("battle_frame_max_ms", static_cast<int>(dt * 1000.0));   // image la plus longue en combat
    if (s.battle.active && dt > 0.05) {
        bump("frame_hitches");
        log().write("frame_hitch", JsonObj().add("dt_ms", static_cast<int64_t>(dt * 1000.0)).add("in_combo_window", !s.battle.windows.empty()), s.battle.turn);
    }
    { prof::Scope p("net"); pump_network(); }
    if (s.ended) return;
    // Client : les ennemis de la carte ne détectent, ne poursuivent ni n'engagent plus personne ici. Le combat ne part
    // que de l'hôte (BattleStart), avec son équipe et son engagement. Vu le 2026-10-21 : l'ennemi du client fonçait sur
    // le pion suiveur et lançait le combat 3,5 s avant l'attaque surprise de l'hôte (« pris par surprise » chez le client).
    // Toutes les 3 s (FindAllOf parcourt tous les objets), seule tâche lourde de son image ; pion pris par le contrôleur
    // (avant : deux FindFirstOf de plus, pion du monde et menu principal, à chaque passage).
    // Quand le client mène (F8), c'est l'inverse : ses ennemis restent actifs (son pion engage, D34 demande le combat à
    // l'hôte) et ceux de l'hôte deviennent passifs. Vu le 2026-10-25 : le pion suiveur de l'hôte touchait un monstre
    // que le client n'avait pas touché -> combat imposé. Ennemis rendus au jeu dès que ce joueur redevient meneur.
    {
        const bool lead_here = explore::shared() && explore::is_pilot();
        const bool want_passive = s.role == Role::Client ? !lead_here : (explore::shared() && !explore::is_pilot());
        static bool was_passive = false;
        if (!want_passive && was_passive && game::explore_pawn() && !game::map_change_in_progress()) {
            was_passive = false;
            const int n = game::restore_world_enemies();
            log().write("coop_world_enemies_restored", JsonObj().add("count", n).add("pilot", explore::is_pilot()));
        }
        if (static double next_passive = 0; want_passive && s.now >= next_passive && prof::claim_heavy("passive_enemies")) {
            next_passive = s.now + 3.0;
            prof::Scope p("passive_enemies");
            if (game::explore_pawn() && !game::in_battle() && !game::map_change_in_progress()) {
                was_passive = true;
                const int n = game::passive_world_enemies();
                static int last_n = -1;
                if (n != last_n) { last_n = n; log().write("coop_world_enemies_passive", JsonObj().add("count", n).add("pilot", explore::is_pilot())); }
            }
        }
    }
    // Rappel des touches du mod tant que le menu pause est ouvert.
    {
        const bool show = game::pause_menu_visible() && !owners_ui::shows_key_hints();   // panneau Coop : il porte déjà les touches
        if (show != s.keys_shown) {
            s.keys_shown = show;
            if (show) ui::set_status("keys", s.role == Role::Host ? "keys_host" : "keys_client");
            else ui::clear_status("keys");
        }
    }
    // Hôte, hors combat, fenêtre du jeu au premier plan : F5 choisit un personnage de l'équipe, F6 le donne au joueur
    // suivant (menu de réattribution du brief, version touches). Diffusé aux clients ; vaut pour les builds et les
    // combats suivants. Avant : F6 seul passait aussi au personnage suivant à chaque appui (peu pratique, Killian).
    {
        static bool f5_down = false, f6_down = false;
        static size_t selected = SIZE_MAX;   // aucun : le premier F5 choisit le premier personnage
        const bool d5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        const bool d6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        const bool press5 = d5 && !f5_down, press6 = d6 && !f6_down;
        DWORD pid = 0;
        if (press5 || press6) GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool ok = (press5 || press6) && pid == GetCurrentProcessId() && s.role == Role::Host && !s.battle.active;
        if (const auto party = ok ? game::party_names() : std::vector<std::string>{}; !party.empty()) {
            if (press5 || selected == SIZE_MAX) {   // F6 sans choix préalable : choisit d'abord
                selected = (selected + 1) % party.size();
                const std::string& ch = party[selected];
                ui::toast("owner_selected", {ch, player_name(character_owner(ch))});
            } else {
                cycle_owner(selected % party.size());
            }
        }
        f5_down = d5;
        f6_down = d6;
    }
    // Héros repris à un joueur parti : si c'est le tour de l'un d'eux, le joueur de l'hôte le joue (entrées rendues).
    if (s.takeover && s.battle.active) {
        s.takeover = false;
        UObject* cur = game::current_character();
        if (cur && game::is_player(cur) && controls(game::spot(cur)) && !s.battle.executed.count({s.battle.turn, game::spot(cur)})) {
            set_local_input(true, "takeover", true);
            turn_indicator(actor_unit(cur));
            log().write("coop_takeover_turn", JsonObj().add("spot", game::spot(cur)), s.battle.turn);
        }
    }
    // Démarrage coordonné : jeu en pause au premier tour tant qu'un joueur de la partie n'a pas commencé ce combat.
    if (auto& b = s.battle; b.hold) {
        // Qui on attend et pourquoi (chargement, cinématique, menu, reconnexion ; sinon « entre en combat »).
        std::vector<uint8_t> who;
        for (auto& p : s.peers) if (in_game(p) && !b.ready.count(p.player_id)) who.push_back(p.player_id);
        // Inactivité continue de tous les joueurs attendus : rien ne les retient, ils devraient déjà être là.
        bool idle = !who.empty();
        for (uint8_t w : who) if (presence::activity(w, s.now) != presence::Activity::Ready) idle = false;
        b.hold_idle_s = idle ? b.hold_idle_s + dt : 0.0;
        const bool expired = s.now - b.hold_since > kReadyTimeout;
        const bool idle_out = b.hold_idle_s >= s.ready_idle_s;
        if (who.empty() || expired || idle_out) {
            b.hold = false;
            explore::hold_pause("battle_start", false);
            ui::clear_status("wait");
            if (expired || idle_out) {
                // Jamais figé pour toujours : le combat commence sans eux ; ils suivront en arrivant (BattleStart reçu).
                bump("battle_ready_timeouts");
                if (idle_out && !expired) bump("battle_ready_idle_timeouts");
                std::string names;
                for (uint8_t w : who) names += (names.empty() ? "" : ", ") + player_name(w);
                ui::toast("battle_started_without", {names}, 8.0);
            }
            bump("battle_holds_released");
            bump_max("battle_hold_max_ms", static_cast<int>((s.now - b.hold_since) * 1000.0));
            log().write("coop_battle_hold_release", JsonObj().add("waited_s", s.now - b.hold_since).add("timeout", expired).add("idle_timeout", idle_out)
                                                         .add("idle_s", b.hold_idle_s), b.turn);
        } else {
            if (!explore::pause_held("battle_start")) {
                explore::hold_pause("battle_start", true);
                bump("battle_holds");
                log().write("coop_battle_hold", JsonObj(), b.turn);
            }
            if (s.now - std::max(b.hold_since, b.hold_logged_at) >= 15.0) {
                b.hold_logged_at = s.now;
                std::string ids, acts;
                for (uint8_t w : who) {
                    if (!ids.empty()) { ids += ","; acts += ","; }
                    ids += std::to_string(w);
                    acts += presence::activity_name(presence::activity(w, s.now));
                }
                log().write("coop_battle_hold_waiting", JsonObj().add("waited_s", s.now - b.hold_since).add("players", ids).add("activities", acts)
                                                             .add("idle_s", b.hold_idle_s), b.turn);
            }
            ui::set_status_text("wait", "waiting_players", presence::wait_text(who, s.now, player_name, "wait_battle_prep"));
        }
    }
    // Diagnostic (duel du prologue, 2026-10-22) : la barrière narrative ne doit jamais tenir pendant un combat coop engagé.
    {
        const bool engaged = s.pending_start.has_value() || (s.battle.active && !s.battle.finalized);
        if (engaged && explore::pause_held("barrier")) {
            if (s.barrier_battle_since < 0) s.barrier_battle_since = s.now;
            else if (s.barrier_battle_since > 0 && s.now - s.barrier_battle_since > 2.0) {
                s.barrier_battle_since = 0;   // une fois par épisode
                bump("barrier_holds_in_battle");
                log().write("coop_barrier_in_battle", JsonObj().add("pending_start", s.pending_start.has_value()).add("active", s.battle.active), s.battle.turn);
            }
        } else {
            s.barrier_battle_since = -1;
        }
    }
    peer_activity_tick();
    defense_context_watch();
    if (s.battle.active && !s.battle.hold && !explore::remote_paused() && !game::pause_menu_visible()) s.battle.timer_elapsed += dt;
    if (s.battle.active && !s.input_enabled) enforce_input_block();
    if (s.role == Role::Client) start_pending_battle();
    { prof::Scope p("explore"); explore::tick(s.now); }
    { prof::Scope p("builds"); builds::tick(s.now); }
    run_remote_action();
    // Aperçu de ce que préparent les joueurs, caméra de la visée libre (envoyée par le propriétaire, rejouée ailleurs).
    { prof::Scope p("intent"); intent::tick(s.now); }
    if (s.fa_own.active && s.battle.active)
        intent::aim_owner_tick(s.now, s.fa_own.spot, static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count()));
    {
        const auto& r = s.fa_rep;
        intent::AimReplay ar;
        const bool replaying = s.battle.active && r.started && !r.done && r.key.first == s.battle.turn;
        if (replaying) {
            ar.hero = game::find_unit(true, r.key.second);
            ar.spot = r.key.second;
            ar.elapsed_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - r.t0).count());
            ar.have_point = r.have_override;
            std::copy(r.point.begin(), r.point.end(), ar.point);
        }
        intent::aim_camera_tick(s.now, dt, replaying ? &ar : nullptr);
    }
    // Visée libre d'un héros d'ici ouverte sans que StartFreeAimFromUI soit vu (combat pas encore actif pour le mod :
    // essai du 2026-10-25, 01:11, combat déjà engagé chez le client, visée ouverte pendant les 4,6 s d'attente du
    // BattleStart -> aucun tir publié, dégâts annulés par la réparation du tour suivant). On part de l'état du jeu.
    if (s.battle.active && !s.fa_own.active && s.battle.turn > 0) {
        UObject* cur = game::current_character();
        const bool replaying_here = s.fa_rep.started && !s.fa_rep.done;
        const bool aiming_here = cur && ue::get_bool(ue::get_object(cur, STR("BP_FreeAimShooterBattleComponent")), STR("IsAiming"));
        const std::pair<uint32_t, int> key{s.battle.turn, cur ? game::spot(cur) : -1};
        if (!aiming_here && s.fa_own_ended == key) s.fa_own_ended = {0, -1};   // visée vraiment refermée : une nouvelle peut suivre
        if (cur && aiming_here && !replaying_here && s.fa_own_ended != key && game::is_player(cur) && controls(key.second)) {
            s.fa_own = FreeAimOwner{};
            s.fa_own.active = true;
            s.fa_own.seen_aiming = true;
            s.fa_own.spot = game::spot(cur);
            s.fa_own.t0 = SteadyClock::now();
            proto::FreeAimEvent e;
            e.battle_id = s.battle.id;
            e.turn = s.battle.turn;
            e.spot = static_cast<uint8_t>(s.fa_own.spot);
            e.kind = proto::FreeAimKind::Start;
            publish(e);
            bump("free_aims_published");
            bump("free_aim_late_starts");
            log().write("coop_free_aim_late_start", JsonObj().add("spot", s.fa_own.spot), s.battle.turn);
        }
    }
    // Visée libre d'un héros d'ici quittée par un autre chemin que CancelFreeAimFromUI / EndFreeAim (touche
    // d'annulation de la visée : vu le 2026-10-21, l'autre joueur restait en visée et l'action suivante attendait).
    if (s.fa_own.active) {
        const bool aiming = ue::get_bool(ue::get_object(game::find_unit(true, s.fa_own.spot), STR("BP_FreeAimShooterBattleComponent")), STR("IsAiming"));
        if (aiming) s.fa_own.seen_aiming = true;
        else if (s.fa_own.seen_aiming) {
            proto::FreeAimEvent e;
            e.battle_id = s.battle.id;
            e.turn = s.battle.turn;
            e.spot = static_cast<uint8_t>(s.fa_own.spot);
            e.kind = proto::FreeAimKind::Cancel;
            e.t_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count());
            e.x = s.fa_own.point[0]; e.y = s.fa_own.point[1]; e.z = s.fa_own.point[2];
            publish(e);
            s.fa_own.active = false;
            s.fa_own_ended = {s.battle.turn, s.fa_own.spot};
            bump("free_aim_exits_detected");
            bump("free_aim_cancels_published");
            log().write("coop_free_aim_local_exit", JsonObj().add("spot", s.fa_own.spot), s.battle.turn);
        }
    }
    // Diagnostic (TOTOFFF vise mais ne tire pas, à la manette) : touches de la visée et contexte qui reçoit RT / clic.
    if (s.fa_diag_at > 0 && s.now >= s.fa_diag_at) {
        s.fa_diag_at = -1;
        static bool mapped = false;
        UObject* fa_imc = ue::find_object(L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_FreeAim.IMC_Battle_FreeAim");
        JsonObj o;
        if (!mapped) { mapped = true; o.add("mappings", game::imc_mappings(fa_imc)); }
        o.add("active", s.fa_own.active).add("input_enabled", s.input_enabled)
         .add("rt_top", game::top_context_for_key("Gamepad_RightTrigger")).add("rt", game::active_contexts_with_key("Gamepad_RightTrigger"))
         .add("rb", game::active_contexts_with_key("Gamepad_RightShoulder")).add("lmb_top", game::top_context_for_key("LeftMouseButton"))
         .add("input_type", game::common_input_type()).add("widgets", game::active_common_widgets());
        // Tir (RT) lié aussi par un autre contexte actif (ex. sprint de l'exploration) : il peut ne jamais arriver.
        const std::string rt = game::active_contexts_with_key("Gamepad_RightTrigger");
        bump(rt.find(",") != std::string::npos ? "free_aim_rt_shared" : "free_aim_rt_alone");
        log().write("coop_free_aim_inputs", o, s.battle.turn);
    }
    // Visée libre d'un héros d'ici : point visé envoyé ~20 fois par seconde (non fiable, pour l'affichage chez les autres).
    if (s.fa_own.active && s.fa_own.have_point && s.now >= s.fa_own.next_aim_s) {
        s.fa_own.next_aim_s = s.now + 0.05;
        proto::FreeAimEvent e;
        e.battle_id = s.battle.id;
        e.turn = s.battle.turn;
        e.spot = static_cast<uint8_t>(s.fa_own.spot);
        e.kind = proto::FreeAimKind::Aim;
        e.t_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count());
        e.x = s.fa_own.point[0]; e.y = s.fa_own.point[1]; e.z = s.fa_own.point[2];
        publish(e, nullptr, false);
    }
    // Essai -e33turndep=1 : vérification différée faite et dépendance résolue dès que l'état de l'hôte est là.
    if (auto& b = s.battle; b.dep_active || b.dep_finish_pending) {
        const bool ready = b.host_sync.count(b.dep_turn) || b.host_end >= 0 || !b.active;
        const bool expired = s.now - b.dep_since > 15.0;
        if (ready || expired) {
            // D'abord la vérification (acteur, état, réparation), puis le jeu reprend le tour.
            if (b.dep_finish_pending && ready && b.active && b.turn == b.dep_turn && b.dep_actor) client_turn_finish(b.dep_actor);
            b.dep_finish_pending = false;
            b.dep_start = {};
            if (expired && !ready) { bump("turn_dep_timeouts"); log().write("coop_turn_dep_timeout", JsonObj().add("turn", b.dep_turn), b.dep_turn); }
            if (b.dep_active) {
                bump("turn_deps_held");
                dep_solve(ready ? "host_state" : "timeout");
                // SolveDependency conclut déjà l'écoute quand c'était la dernière dépendance ; ses drapeaux (IsListening,
                // HasTriggered) ne sont pas encore à jour juste après : un CheckDependencies immédiat relançait la suite du tour
                // une 2e fois (l'ennemi tirait deux fois sa cible, tours 21 et 28 de p3_party, 2026-10-22). Pas de vérification forcée (voir plus bas).
                b.dep_recheck_at = s.now + 0.5;
            }
        }
    }
    // Diagnostic seulement : 0,5 s après la résolution, les drapeaux disent parfois « écoute toujours » alors que la suite
    // du tour est déjà partie ; un CheckDependencies forcé la relançait une 2e fois (tours 21 et 28 de p3_party).
    if (auto& b = s.battle; b.dep_recheck_at >= 0 && s.now >= b.dep_recheck_at) {
        b.dep_recheck_at = -1;
        UObject* deps = b.active && !b.dep_active && b.turn == b.dep_turn ? turn_deps() : nullptr;
        if (deps && ue::get_bool(deps, STR("IsListening")) && !ue::get_bool(deps, STR("HasTriggered"))) {
            bump("turn_deps_still_listening");
            log().write("coop_turn_dep_still_listening", JsonObj().add("turn", b.dep_turn), b.turn);
        }
    }
    if (s.role == Role::Host) align_tick();   // D28 : tour ennemi relâché quand les clients l'ont commencé
    client_force_end_tick();
    defeat_tick();
    // Fin sans victoire : clôture dès que le résultat de l'hôte est là (8 s au plus).
    if (auto& b = s.battle; b.end_flow_at >= 0 && !b.finalized && (s.role == Role::Host || b.host_end >= 0 || s.now - b.end_flow_at > 8.0))
        finalize_battle("end_flow");
    // Combat fini chez l'hôte sans fin locale (client bloqué, ou carte rechargée en suivant l'hôte) : clôture quand
    // même, sinon l'état de ce combat déborde sur le suivant (vu le 2026-10-20 : 2e combat compté comme le tour 2 du 1er).
    if (auto& b = s.battle; s.role == Role::Client && b.active && !b.finalized && b.host_end_at >= 0 && b.end_flow_at < 0 &&
                            (!game::in_battle() || (b.host_end != game::EndVictory && s.now - b.host_end_at > 15.0))) {
        bump("battles_closed_by_host_end");
        finalize_battle("host_end");
    }
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
