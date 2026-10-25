// Combat côté client : lancement du combat de l'hôte, rejeu de la visée libre et des actions des autres joueurs.
#include "coop_internal.hpp"

namespace e33::coop {

// Client : lance le combat reçu de l'hôte quand le monde local est chargé et stable depuis quelques secondes.
void start_pending_battle()
{
    if (!s.pending_start || s.now < s.next_trigger_try) return;
    constexpr double kStep = 0.5;   // FindFirstOf parcourt tous les objets : pas à chaque image
    s.next_trigger_try = s.now + kStep;
    // Pas d'appel au jeu (ProcessEvent sur l'instance de jeu) avant qu'un pion d'exploration existe : au démarrage
    // (écran d'avertissement), ces appels ont figé le jeu entier (4 runs clients sur ~20, 2026-10-19).
    if (!game::world_pawn()) { s.world_ready_s = 0; return; }
    const bool ready = game::controller() && !game::map_change_in_progress() && !game::main_menu();
    // Menu du jeu ouvert ici (équipe, pictos, pause : jeu en pause) : lancer le combat dedans a figé le jeu de TOTOFFF
    // (2026-10-24, Lune déformée dans l'écran des pictos). On attend sa fermeture, avec un message.
    // Écrans de menu CommonUI actifs (l'inventaire ne lève peut-être pas IsPauseMenuVisible : jeu de TOTOFFF figé
    // encore en 0.6.20, 21:15). Liste relevée pour le diagnostic.
    const std::string widgets = game::active_common_widgets();
    bool menu_widget = false;
    for (const char* k : {"Menu", "Inventory", "Character", "Equip", "Picto", "Lumina", "Skill", "Shop", "Merchant", "Journal", "Outfit", "Weapon"})
        menu_widget = menu_widget || widgets.find(k) != std::string::npos;
    // Filet : un écran « menu » qui resterait actif sans en être un (nom trompeur) ne bloque pas le combat plus de 90 s.
    if (menu_widget && !game::pause_menu_visible()) { if (s.menu_widget_since < 0) s.menu_widget_since = s.now; }
    else s.menu_widget_since = -1;
    const bool widget_timeout = s.menu_widget_since >= 0 && s.now - s.menu_widget_since > 90.0;
    const bool in_menu = game::pause_menu_visible() || (menu_widget && !widget_timeout);   // pas IsGamePaused : la barrière narrative met aussi le jeu en pause
    if (in_menu && s.now >= s.menu_wait_next_log) {
        s.menu_wait_next_log = s.now + 15.0;
        log().write("coop_battle_wait_menu_widgets", JsonObj().add("pause_menu", game::pause_menu_visible()).add("widgets", widgets));
    }
    if (in_menu) {
        ui::set_status("battle_menu", "battle_close_menu");
        if (!s.menu_wait_logged) { s.menu_wait_logged = true; bump("battle_starts_waiting_menu"); log().write("coop_battle_wait_menu", JsonObj()); }
        s.world_ready_s = 0;
        return;
    }
    if (s.menu_wait_logged) { s.menu_wait_logged = false; ui::clear_status("battle_menu"); }
    s.world_ready_s = ready ? s.world_ready_s + kStep : 0;
    // Combat déjà engagé ici (notre pion a suivi le pilote) : le monde est chargé et le jeu donne déjà la main au
    // joueur. Attendre 5 s de plus laissait le mod inactif pendant que le joueur agissait (essai du 2026-10-25, 01:11 :
    // tirs en visée libre jamais publiés). On démarre dès que le contrôleur est là.
    const bool engaged_here = game::in_battle() && game::controller() && !game::map_change_in_progress();
    if (s.world_ready_s < 5.0 && !engaged_here) return;
    log().write("coop_battle_start_widgets", JsonObj().add("widgets", widgets).add("widget_timeout", widget_timeout));
    const proto::BattleStart bs = *s.pending_start;
    std::string err;
    bool started = game::in_battle();
    const bool already = started;
    if (started) {
        // Notre pion a déjà engagé un combat (il suit le pilote) : ce doit être le même.
        std::string enc, gid;
        int engage = 0;
        if (game::battle_params(enc, gid, engage) && enc != bs.encounter) {
            bump("battle_encounter_mismatches");
            log().write("coop_battle_encounter_mismatch", JsonObj().add("local", enc).add("host", bs.encounter));
        }
    } else if (!bs.world_enemy.empty()) {
        if (UObject* enemy = game::find_world_enemy(bs.world_enemy)) started = game::start_world_battle(enemy, ue::from_utf8(bs.encounter), bs.engage, err);
        else err = "world enemy not found: " + bs.world_enemy;
        if (!started) log().write("coop_error", JsonObj().add("what", "start_world_battle: " + err));
        else bump("world_battles");
    }
    if (!started && !game::trigger_battle(ue::from_utf8(bs.encounter), party_wide(bs.party), err)) {
        log().write("coop_error", JsonObj().add("what", "trigger_battle: " + err));
        s.next_trigger_try = s.now + 2.0;
        return;
    }
    s.pending_start.reset();
    s.battle.active = true;
    log().write("coop_battle_start", JsonObj().add("battle", bs.battle_id).add("encounter", bs.encounter).add("already_engaged", already));
    // Combat engagé ici avant le BattleStart (notre pion suit le pilote) : son premier tour a pu commencer sans le mod.
    // Sans rattrapage, le BattleReady ne part jamais et l'hôte reste figé en attente (vu le 2026-10-20, 2e combat).
    if (UObject* cur = game::current_character(); already && cur && s.battle.turn == 0 && ue::get_bool(cur, STR("IsTurnActive"))) {
        bump("turn_start_catchups");
        // Vérification différée : la photo du vrai début de ce tour, reportée sur l'état de l'hôte (shift_host_state).
        const bool snap = s.early_turn && s.early_turn_actor == cur && s.now - s.early_turn_at < 60.0;
        if (snap) {
            s.battle.dep_start = *s.early_turn;
            s.battle.dep_start.battle_id = s.battle.id;
            s.battle.dep_start.turn = 1;
        }
        log().write("coop_turn_start_catchup", JsonObj().add("actor", game::char_class(cur)).add("turn_start_snapshot", snap)
                                                   .add("snapshot_age_s", snap ? s.now - s.early_turn_at : -1.0));
        s.early_turn.reset();
        on_turn_start(cur);
    }
}

// Visée libre d'un héros d'un autre joueur : rejouée au même rythme que chez lui (décalée de replay_lag_s).
// Renvoie true tant qu'elle est en cours (l'action du tour attend).
bool replay_free_aim(const std::pair<uint32_t, int>& key, UObject* hero)
{
    auto& b = s.battle;
    auto ev = b.fa_events.find(key);
    if (ev == b.fa_events.end() || ev->second.empty()) return false;
    auto& r = s.fa_rep;
    if (r.key != key) r = FreeAimReplay{key};
    if (r.done) return false;
    if (!r.started) {
        if (s.now - b.turn_started_at < s.action_delay_s || s.now - b.fa_arrival[key] < s.replay_lag_s) return true;
        if (!intent::mirror_release(s.now)) return true;   // menus rejoués refermés d'abord (état du début du tour)
        game::ModCall mod;
        ue::Call c(game::battle_manager(), STR("StartFreeAimFromUI"));
        if (c) c.invoke();
        // Le joueur d'ici ne doit ni orienter la caméra ni tirer pour ce héros (vu par Killian le 2026-10-19).
        // DisableFreeAimControls coupe la souris/le stick et le bouton de tir ; nos tirs (OnFreeAimShootAction) et
        // l'annulation (CancelFreeAimFromUI) passent toujours (vérifié en jeu le 2026-10-19).
        ue::Call off(game::battle_manager(), STR("DisableFreeAimControls"));
        if (off) { off.invoke(); bump("free_aim_controls_locked"); }
        r.started = true;
        r.t0 = SteadyClock::now();
        bump("free_aims_replayed");
        log().write("coop_free_aim_start", JsonObj().add("spot", key.second), b.turn);
        return true;
    }
    const auto elapsed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - r.t0).count());
    const auto& events = ev->second;
    // Point visé (affichage) : le plus récent déjà atteint.
    const proto::FreeAimEvent* aim = nullptr;
    for (auto& e : events) if (e.kind == proto::FreeAimKind::Aim && e.t_ms <= elapsed && (!aim || e.t_ms >= aim->t_ms)) aim = &e;
    if (aim && aim->target.spot == proto::kAutoSpot) { r.point = {aim->x, aim->y, aim->z}; r.have_override = true; }
    else if (aim) {   // point relatif à un combattant (faux client)
        double a[3];
        if (game::location(game::find_unit(aim->target.kind == proto::ActorKind::Hero, aim->target.spot), a)) {
            r.point = {a[0] + aim->x, a[1] + aim->y, a[2] + aim->z};
            r.have_override = true;
        }
    }
    // Événements fiables, dans l'ordre.
    game::ModCall mod;   // tirs et fin de la visée rejoués : appels du mod, pas des entrées d'ici
    while (r.next < events.size() && events[r.next].t_ms <= elapsed) {
        const auto& e = events[r.next++];
        if (e.kind == proto::FreeAimKind::Fire) {
            ue::Call c(ue::get_object(hero, STR("BP_FreeAimShooterBattleComponent")), STR("OnFreeAimShootAction"));
            if (c) c.invoke();
        } else if (e.kind == proto::FreeAimKind::Cancel || e.kind == proto::FreeAimKind::End) {
            // Fin chez le propriétaire : si la visée est encore ouverte ici, on l'annule pareil.
            if (ue::get_bool(ue::get_object(hero, STR("BP_FreeAimShooterBattleComponent")), STR("IsAiming"))) {
                ue::Call c(game::battle_manager(), STR("CancelFreeAimFromUI"));
                if (c) c.invoke();
            }
            r.done = true;
            r.have_override = false;
            bump("free_aim_replays_ended");
            log().write("coop_free_aim_end", JsonObj().add("spot", key.second).add("kind", static_cast<int>(e.kind)).add("raycasts", static_cast<int>(r.raycasts)), b.turn);
            return true;
        }
    }
    // Filet : le propriétaire a déjà choisi une autre action et tous ses événements de visée sont joués, sans fin
    // reçue (ancienne version, chemin de sortie inconnu) : on ferme la visée ici pour ne pas bloquer son action.
    if (r.next >= events.size() && b.commands.count(key)) {
        if (ue::get_bool(ue::get_object(hero, STR("BP_FreeAimShooterBattleComponent")), STR("IsAiming"))) {
            ue::Call c(game::battle_manager(), STR("CancelFreeAimFromUI"));
            if (c) c.invoke();
        }
        r.done = true;
        r.have_override = false;
        bump("free_aim_closed_by_action");
        bump("free_aim_replays_ended");
        log().write("coop_free_aim_end", JsonObj().add("spot", key.second).add("kind", -1).add("raycasts", static_cast<int>(r.raycasts)), b.turn);
    }
    return true;
}

// Tour d'un héros contrôlé par un autre joueur : exécuter sa commande (reçue de lui ou relayée par l'hôte).
void run_remote_action()
{
    auto& b = s.battle;
    if (!b.active || b.ending) return;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur)) return;
    const int spot = game::spot(cur);
    const std::pair<uint32_t, int> key{b.turn, spot};
    // Héros d'ici : le joueur agit lui-même, sauf action imposée par l'hôte (minuteur de tour écoulé).
    if (controls(spot) && !b.forced.count(key)) return;
    if (b.executed.count(key) || b.hold) return;
    if (replay_free_aim(key, cur)) return;   // visée libre du propriétaire d'abord (elle ne termine pas le tour)
    // Ordre des tours décalé d'un cran entre les deux jeux (engagement différent, mort d'un seul côté) : la commande de
    // ce héros est rangée sous le tour voisin et n'était jamais trouvée (essai du 2026-10-25, combats 7 et 9 : 30 s
    // d'attente puis abandon). Une commande non jouée du même héros au tour précédent ou suivant est la sienne.
    if (!b.commands.count(key)) {
        for (const uint32_t t : {b.turn - 1, b.turn + 1}) {
            const std::pair<uint32_t, int> other{t, spot};
            auto o = b.commands.find(other);
            if (t == 0 || o == b.commands.end() || b.executed.count(other) || b.forced.count(other)) continue;
            proto::ActionCommand ac = o->second;
            ac.turn = b.turn;
            b.commands[key] = ac;
            b.command_time[key] = b.command_time[other];
            b.commands.erase(other);
            bump("commands_turn_shifted");
            log().write("coop_command_turn_shift", JsonObj().add("spot", spot).add("from_turn", static_cast<int64_t>(t)), b.turn);
            break;
        }
    }
    auto it = b.commands.find(key);
    if (it == b.commands.end()) {
        // Hôte : attaque automatique imposée au héros d'un autre joueur et diffusée (il la jouera en arrivant à ce tour).
        auto force_attack = [&] {
            proto::ActionCommand ac;
            ac.battle_id = b.id;
            ac.turn = b.turn;
            ac.actor_spot = static_cast<uint8_t>(spot);
            ac.action_type = game::ActAttack;
            ac.targets = {{proto::ActorKind::Enemy, proto::kAutoSpot}};
            ac.forced = true;
            b.forced.insert(key);
            b.commands[key] = ac;
            b.command_time[key] = s.now - s.replay_lag_s;   // pas de décalage : personne n'attend ce QTE
            broadcast(ac);
        };
        const bool running = !game::pause_menu_visible() && !explore::remote_paused();
        // Minuteur de tour (option, désactivé par défaut) : l'hôte impose une attaque automatique au héros du joueur muet.
        if (s.role == Role::Host && b.turn_timer_s > 0 && running) {
            const double left = b.turn_timer_s - b.timer_elapsed;
            ui::set_status("turn", "turn_timer", {player_name(owner_of_spot(spot)), std::to_string(static_cast<int>(std::max(0.0, std::ceil(left))))});
            if (left <= 0) {
                force_attack();
                bump("turn_timer_expired");
                ui::toast("turn_forced", {player_name(owner_of_spot(spot))});
                log().write("coop_turn_timer_expired", JsonObj().add("spot", spot).add("timer_s", static_cast<int>(b.turn_timer_s)), b.turn);
                return;
            }
        }
        // Tour bloqué (sans minuteur) : preuve positive que le jeu du propriétaire n'est pas dans ce tour : lien vivant,
        // mais aucun signe de ce tour venu de lui (ni StateHash, ni menu, aperçu, visée, action) depuis stuck_turn_s de
        // temps de jeu (pauses exclues), sans raison visible (chargement, reconnexion : on attend). Un joueur qui réfléchit,
        // fouille ses menus ou lit une description a déjà envoyé son StateHash au début du tour : jamais concerné, même
        // longtemps. Attaque automatique, et ses défenses / QTE ne sont plus attendus tant qu'il ne répond pas.
        if (s.role == Role::Host && running && !controls(spot)) {
            const uint8_t owner = owner_of_spot(spot);
            const auto act = presence::activity(owner, s.now);
            const bool reached = b.acks.count(b.turn) && b.acks[b.turn].count(owner);
            if (!reached && b.timer_elapsed >= s.stuck_turn_s && act != presence::Activity::Loading && act != presence::Activity::Joining &&
                act != presence::Activity::Reconnecting) {
                force_attack();
                b.unresponsive.insert(owner);
                bump("turn_stuck_forced");
                ui::toast("turn_forced_stuck", {player_name(owner)});
                log().write("coop_turn_stuck", JsonObj().add("spot", spot).add("player", static_cast<int>(owner)).add("waited_s", b.timer_elapsed)
                                                   .add("activity", presence::activity_name(act)), b.turn);
                return;
            }
        }
        if (!b.action_wait_logged && s.now - b.turn_started_at > 30.0) {
            b.action_wait_logged = true;
            bump("action_waits_long");
            log().write("coop_action_wait", JsonObj().add("spot", spot).add("waited_s", s.now - b.turn_started_at), b.turn);
        }
        return;
    }
    if (s.now - b.turn_started_at < s.action_delay_s) return;
    if (s.now - b.command_time[key] < s.replay_lag_s) return;
    if (explore::remote_paused() || game::pause_menu_visible()) return;   // jeu en pause : l'action attend la reprise
    // Menus du propriétaire rejoués ici (miroir) : refermés d'abord, l'action part du même état qu'avant le miroir.
    // Clé de l'action : fermeture dans la même image que l'exécution (D29).
    if (!controls(spot) && !intent::mirror_release(s.now, std::to_string(it->second.action_type) + "|" + it->second.secondary)) return;
    const proto::ActionCommand cmd = it->second;
    b.executed.insert(key);
    if (controls(spot)) set_local_input(false, "forced action");   // action imposée sur un héros d'ici : le joueur ne joue plus ce tour
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
        const double ap = ue::get_double(game::stats(cur), STR("CurrentAP"));
        const bool no_ap = cmd.action_type == game::ActSkill && game::skill_info(cur, secondary, targeting, cost) && ap < cost;
        if (no_item || no_ap) {
            bump("remote_action_invalid");
            log().write("coop_remote_action_invalid", JsonObj().add("spot", spot).add("secondary", cmd.secondary).add("no_item", no_item).add("no_ap", no_ap)
                                                         .add("ap", ap).add("cost", cost).add("hero", game::char_class(cur)), b.turn);
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

} // namespace e33::coop
