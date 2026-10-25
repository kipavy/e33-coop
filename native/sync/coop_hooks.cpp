// Abonnements aux fonctions du jeu (hooks) du mode coop.
#include "coop_internal.hpp"

namespace e33::coop::detail {

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();

    // Client : combat lancé par un ennemi de la carte (le nôtre, ou notre pion suiveur qui l'a touché avant que le mod
    // le lance) : même équipe et même engagement que chez l'hôte. Vu le 2026-10-21 : attaque surprise de l'hôte vue
    // comme « pris par surprise » chez le client, et combat avec l'ancienne équipe (Verso seul).
    bus.pre(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle", [](HookCtx& ctx, UFunction* f) {
        if (s.role != Role::Client) return;
        // Le pion suiveur peut engager avant que le mod lance le combat : le jeu se dit « en combat » (transition) et le
        // mod marque le combat lancé, mais StartBattle n'arrive qu'après (vu le 2026-10-21 : ~0,4 s plus tard). On
        // garde donc le dernier BattleStart tant que ce combat n'a pas commencé son premier tour.
        const proto::BattleStart* p = s.pending_start ? &*s.pending_start
                                    : (s.last_start && s.last_start->battle_id == s.battle.id && s.battle.turn == 0 && !s.battle.finalized) ? &*s.last_start
                                    : (s.early_start && s.now - s.early_start_at < 20.0) ? &*s.early_start
                                    : nullptr;
        if (!p) {
            bump("world_battle_without_host_start");
            log().write("coop_world_battle_early", JsonObj().add("enemy", ue::short_name(ctx.Context)));
            // Client meneur (F8) : son pion a touché l'ennemi, l'hôte n'a rien lancé (son pion suiveur peut être loin :
            // 17 s d'écart le 2026-10-24, chacun son combat). Demande à l'hôte de lancer ce combat (D34) :
            // BattleStart sans n° = demande ; l'hôte lance le même ennemi, puis son BattleStart est appliqué ici
            // comme quand notre pion engage en avance.
            if (explore::is_pilot() && s.now - s.battle_request_at > 10.0) {
                proto::BattleStart rq;
                rq.world_enemy = ue::short_name(ctx.Context);
                rq.engage = static_cast<uint8_t>(ue::read_param_int(ctx, f, STR("EngagementType")).value_or(0));
                rq.party = game::party_names();
                for (auto& peer : s.peers) send_to(peer, rq);
                s.battle_request_at = s.now;
                bump("battle_requests_sent");
                log().write("coop_battle_request", JsonObj().add("enemy", rq.world_enemy).add("engage", static_cast<int>(rq.engage)));
            }
            return;
        }
        const proto::BattleStart& bs = *p;
        const std::string enemy = ue::short_name(ctx.Context);
        if (bs.world_enemy.empty() || enemy != bs.world_enemy) {
            bump("world_battle_other_enemy");
            log().write("coop_world_battle_other_enemy", JsonObj().add("local", enemy).add("host", bs.world_enemy));
            return;
        }
        apply_host_party(bs);
        int size = 0;
        if (auto* e = static_cast<uint8_t*>(ue::param_address(ctx, f, STR("EngagementType"), &size)); e && size == 1 && *e != bs.engage) {
            log().write("coop_engage_forced", JsonObj().add("local", static_cast<int>(*e)).add("host", static_cast<int>(bs.engage)));
            *e = bs.engage;
            bump("engages_forced");
        }
    });
    // Hôte : avis précoce dès le lancement d'un combat de la carte (le BattleStart complet ne part qu'au premier tour,
    // après l'animation d'entrée ; le pion suiveur du client touchait l'ennemi ~3 s avant, vu le 2026-10-21).
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle", [](HookCtx& ctx, UFunction* f) {
        if (s.role != Role::Host || s.battle.active) return;
        proto::BattleStart n;
        n.battle_id = 0;
        n.world_enemy = ue::short_name(ctx.Context);
        n.engage = static_cast<uint8_t>(ue::read_param_int(ctx, f, STR("EngagementType")).value_or(0));
        n.party = game::party_names();
        host_setup_open(true);   // graine du combat dès l'avis : le pion suiveur du client peut engager avant le BattleStart (D30)
        n.rng_seed = s.setup.seed;
        broadcast(n);
        bump("battle_notices_sent");
        log().write("coop_battle_notice", JsonObj().add("enemy", n.world_enemy).add("engage", static_cast<int>(n.engage)));
    });

    // Lancement d'un combat, quel que soit le chemin (ennemi de la carte, game action, dialogue) : fenêtre des tirages
    // de la mise en place (D30), avant le BeginPlay des héros (roue de Monoco) et l'équipement de début de combat.
    bus.pre(fn(game::path::BattleMgr, L"StartBattleNEW"), [](HookCtx&, UFunction*) {
        if (s.role == Role::Host) host_setup_open(false);
        else if (s.role == Role::Client) client_setup_open();
    });

    // Client : le personnage qui joue est celui de l'hôte, pas celui que le jeu local a calculé. L'ordre dépend de
    // l'initiative, de l'engagement (attaque surprise) et des buffs de vitesse, qui peuvent différer : le 2026-10-20
    // (Steam, 2 PC), l'hôte a ouvert avec Maelle et le client avec Verso, puis chacun attendait l'autre. On attend
    // donc le TurnStart de l'hôte et on remplace le paramètre Character avant que le tour ne commence.
    bus.pre(fn(game::path::BattleMgr, L"StartCharacterTurn"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Host && s.turn_align && !s.battle.ending) {   // D28 : qui joue le prochain tour
            ++s.battle.sct_seq;
            auto** slot = static_cast<UObject**>(ue::param_address(ctx, f, STR("Character")));
            s.battle.next_enemy = slot && *slot && !game::is_player(*slot);
            return;
        }
        if (s.role != Role::Client || s.battle.ending) return;
        ++s.battle.sct_seq;
        if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
        // Premier tour d'un combat engagé ici avant que le mod le lance (pion suiveur) : le combat n'est pas encore
        // « actif », mais l'hôte l'a annoncé (avis précoce ou BattleStart). Vu le 2026-10-21 : sans ça, le client
        // jouait Verso au tour 1 pendant que l'hôte jouait Maelle.
        const bool first = !s.battle.active;
        if (first && !s.pending_start && !(s.early_start && s.now - s.early_start_at < 30.0)) return;
        auto& b = s.battle;
        const uint32_t next = first ? 1 : b.turn + 1;
        // Essai (-e33turndep=1) : TurnStart de l'hôte pas encore là -> au lieu de figer le jeu, on enregistre une
        // dépendance de début de tour ; le jeu attend en continuant d'animer, et le tick la résout à l'arrivée.
        if (!first && s.turn_dep && !b.strict && !b.host_turns.count(next) && b.host_end < 0) {
            bump("actor_checks_deferred");   // vérifié au début du tour (OnTurnStart), sans figer le jeu ici
            return;
        }
        if (!wait_for("turn_actor", [&] {
                if (first) return (s.pending_start || b.active) && b.host_turns.count(1) > 0;
                return b.host_turns.count(next) > 0 || b.host_end >= 0;
            }, 15.0)) {
            bump("actor_wait_timeouts");
            log().write("coop_actor_wait_timeout", JsonObj().add("battle", b.id).add("first", first), next);
            return;
        }
        auto it = b.host_turns.find(next);
        if (it == b.host_turns.end()) return;   // combat fini chez l'hôte
        auto** slot = static_cast<UObject**>(ue::param_address(ctx, f, STR("Character")));
        if (!slot) return;
        UObject* local = *slot;
        const proto::Unit want{it->second.kind, it->second.index};
        if (local && actor_unit(local).kind == want.kind && actor_unit(local).spot == want.spot) return;
        UObject* host_actor = game::find_unit(want.kind == proto::ActorKind::Hero, want.spot);
        if (!host_actor || game::is_dead_fast(host_actor)) {
            bump("actor_force_failures");
            log().write("coop_actor_force_failed", JsonObj().add("local", local ? game::char_class(local) : "").add("host", it->second.actor), next);
            return;
        }
        *slot = host_actor;
        bump("actors_forced");
        log().write("coop_actor_forced", JsonObj().add("local", local ? game::char_class(local) : "").add("host", it->second.actor), next);
    });

    // Diagnostic D28 (hôte, en combat) : déroulé du début de tour (écoutes des dépendances, phases, auditeurs).
    {
        auto diag = [](const char* what) {
            return [what](HookCtx& ctx, UFunction*) {
                if (s.role != Role::Host || !s.battle.active || s.battle.ending) return;
                UObject* deps = turn_deps();
                log().write("coop_turn_flow", JsonObj().add("what", what).add("turn", s.battle.turn).add("self", ue::short_name(ctx.Context))
                                                  .add("is_turn_deps", ctx.Context == deps)
                                                  .add("listening", deps ? ue::get_bool(deps, STR("IsListening")) : false)
                                                  .add("triggered", deps ? ue::get_bool(deps, STR("HasTriggered")) : false)
                                                  .add("align_active", s.battle.align_active), s.battle.turn);
            };
        };
        bus.pre(L"/Game/Gameplay/BP_DependencyRefCounter.BP_DependencyRefCounter_C:StartListenToDependencies", diag("dep_listen"));
        bus.pre(fn(game::path::BattleMgr, L"StartCharacterTurn_Phase2"), diag("phase2"));
        bus.pre(fn(game::path::BattleMgr, L"ProcessNextTurnStartListener"), diag("next_listener"));
    }
    // Amélioration d'arme faite par ce joueur (Curateur du camp) : envoyée aussitôt (builds, D13).
    bus.pre(L"/Game/Gameplay/Pictos/BP_ItemUpgradeSystem.BP_ItemUpgradeSystem_C:UpgradeWeaponFromInstance", [](HookCtx&, UFunction*) { builds::on_upgrade_pre(); });
    bus.post(L"/Game/Gameplay/Pictos/BP_ItemUpgradeSystem.BP_ItemUpgradeSystem_C:UpgradeWeaponFromInstance", [](HookCtx&, UFunction*) { builds::on_upgrade_post(); });
    // Objet utilisé hors combat par ce joueur (élixir de chroma, objet donné à un personnage) : effet rejoué chez les autres.
    bus.pre(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:HealParty", [](HookCtx&, UFunction*) { builds::on_item_use_pre("heal_party"); });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Controller_World.BP_jRPG_Controller_World_C:HealParty", [](HookCtx&, UFunction*) { builds::on_item_use_post(); });
    bus.pre(L"/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_InventoryManager.AC_jRPG_InventoryManager_C:UseItemOnCharacter", [](HookCtx& ctx, UFunction* f) {
        const std::string item = ue::read_param_fname(ctx, f, STR("ItemToUse")), ch = ue::read_param_fname(ctx, f, STR("CharacterHardcodedName"));
        if (!item.empty() && !ch.empty()) builds::on_item_use_pre(item + "@" + ch);
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Components/AC_jRPG_InventoryManager.AC_jRPG_InventoryManager_C:UseItemOnCharacter", [](HookCtx&, UFunction*) { builds::on_item_use_post(); });
    // D28 : le jeu ne retient le début de tour que si un « auditeur » (BattleListeners : effets, pictos…) s'inscrit
    // comme dépendance : pour chacun, ProcessNextTurnStartListener remet le compteur à zéro (Reset), appelle son
    // OnCharacterTurnStart, puis écoute s'il y a une dépendance (StartListenToDependencies). Sans auditeur inscrit
    // (tours 9 et 16 du Lancelier Alpha, 2026-10-24), aucune écoute : une inscription faite plus tôt était effacée par
    // Reset. Inscription juste après le premier Reset du tour ennemi : le jeu écoute alors et attend notre résolution.
    // Client (même raison) : tour ennemi commencé ici avant l'état de l'hôte (vérification différée, -e33turndep) et que le jeu
    // n'aurait pas retenu faute d'auditeur : il le retient maintenant jusqu'à l'état de l'hôte, jeu animé. Sinon le client
    // jouait le tour en avance et figeait son image en attendant les défenses des héros de l'hôte (sens inverse de D28).
    bus.post(L"/Game/Gameplay/BP_DependencyRefCounter.BP_DependencyRefCounter_C:Reset", [](HookCtx& ctx, UFunction*) {
        auto& b = s.battle;
        if (!b.active || b.ending) return;
        if (s.role == Role::Host) {
            if (!s.turn_align || b.align_active || b.align_turn == b.turn || !b.cur_enemy || b.ots_seq != b.sct_seq) return;
            if (ctx.Context != turn_deps()) return;
            align_begin(b.turn, "reset");
            return;
        }
        if (s.role != Role::Client || !s.turn_dep || !s.turn_align || b.strict || b.dep_active) return;
        if (!b.dep_finish_pending || b.dep_turn != b.turn || b.host_sync.count(b.turn) || b.host_end >= 0) return;
        if (!b.dep_actor || game::is_player(b.dep_actor)) return;   // tours ennemis seulement (menu d'un héros jamais retardé)
        if (ctx.Context != turn_deps() || ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
        ue::Call reg(ctx.Context, STR("Register Dependency"));
        if (!reg) return;
        reg.set_object(STR("Dependency"), game::save_manager()).invoke();
        b.dep_active = true;
        bump("turn_deps_registered");
        bump("turn_deps_on_reset");
        log().write("coop_turn_dep", JsonObj().add("turn", b.turn).add("after_turn_start", true).add("how", "reset"), b.turn);
    });
    // Client (-e33turndep, par défaut) : le jeu commence à attendre les dépendances du début de tour -> inscription du mod
    // si l'état de l'hôte pour ce tour n'est pas encore arrivé (résolue par le tick à son arrivée). Hôte : voir D28.
    bus.pre(L"/Game/Gameplay/BP_DependencyRefCounter.BP_DependencyRefCounter_C:StartListenToDependencies", [](HookCtx& ctx, UFunction*) {
        // Hôte, démarrage synchronisé (D28) : tour ennemi retenu tant qu'un client qui a des héros ne l'a pas commencé chez lui.
        if (s.role == Role::Host) {
            auto& b = s.battle;
            if (!s.turn_align || !b.active || b.ending || ctx.Context != turn_deps()) return;
            if (b.hold || b.align_active) return;
            if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
            const bool after = b.ots_seq == b.sct_seq;   // OnTurnStart de ce tour déjà passé ?
            const uint32_t turn = after ? b.turn : b.turn + 1;
            if (turn <= 1 || !(after ? b.cur_enemy : b.next_enemy) || align_missing(turn).empty()) return;
            // OnTurnStart pas encore passé : l'état de ce tour (TurnStart, StateSync) n'est pas parti, le client ne peut pas
            // commencer le tour ; le retenir ici ferait attendre chacun l'autre jusqu'au délai. Pas d'alignement.
            if (!after) {
                bump("turn_align_skipped_early");
                log().write("coop_turn_align_skipped", JsonObj().add("turn", turn).add("why", "before_turn_start"), turn);
                return;
            }
            align_begin(turn, "listen");
            return;
        }
        if (s.role != Role::Client || !s.turn_dep || !s.battle.active || s.battle.strict || s.battle.ending) return;
        if (ctx.Context != turn_deps() || ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
        auto& b = s.battle;
        b.listen_seq = b.sct_seq;
        // Le OnTurnStart de ce tour est-il déjà passé ? (l'ordre varie selon les tours)
        const uint32_t turn = b.ots_seq == b.sct_seq ? b.turn : b.turn + 1;
        if (turn <= 1 || b.dep_active || b.host_sync.count(turn) || b.host_end >= 0) return;
        // OnTurnStart déjà passé et tour déjà vérifié (état de l'hôte arrivé avant, puis consommé) : rien à attendre.
        // Sinon la dépendance attendait un état déjà retiré et retenait le tour jusqu'au suivant (p6_turndep, tour 4).
        if (turn == b.turn && !(b.dep_finish_pending && b.dep_turn == turn)) return;
        ue::Call reg(ctx.Context, STR("Register Dependency"));
        if (!reg) return;
        reg.set_object(STR("Dependency"), game::save_manager()).invoke();
        b.dep_active = true;
        b.dep_turn = turn;
        if (!b.dep_finish_pending) b.dep_since = s.now;
        bump("turn_deps_registered");
        log().write("coop_turn_dep", JsonObj().add("turn", turn).add("after_turn_start", b.ots_seq == b.sct_seq), turn);
    });

    // Début de tour : hôte -> TurnStart + StateHash + StateSync ; client -> contrôle et resynchro éventuelle.
    bus.post(fn(game::path::BattleChar, L"OnTurnStart"), [](HookCtx& ctx, UFunction*) {
        if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;   // contre : pas un tour
        on_turn_start(ctx.Context);
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
        // Compétence sur un allié choisie à la main (ex. « Puissant » de Gustave sur Lune, essai du 2026-10-24) : le jeu
        // passe une liste vide, les cibles sont dans le gestionnaire de ciblage. Sans elles, l'autre joueur prenait une
        // cible automatique (le premier ennemi) : buff sur l'Évêque chez lui, sur Lune ici, puis combat qui diverge.
        if (ac.targets.empty() && ac.action_type != game::ActSkip && ac.action_type != game::ActFlee) {
            UObject* tm = ue::get_object(game::controller(), STR("AC_jRPG_TargetingManager"));
            for (UObject* t : ue::get_object_array(tm, STR("TargetedCharacters"))) if (t) ac.targets.push_back(sync::unit_of(t));
            bump(ac.targets.empty() ? "action_targets_empty" : "action_targets_from_targeting");
            log().write("coop_action_targets_fallback", JsonObj().add("action", static_cast<int>(ac.action_type)).add("secondary", ac.secondary)
                                                           .add("targets", static_cast<int64_t>(ac.targets.size())), s.battle.turn);
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
            const uint8_t owner = owner_of_spot(w.spot);
            // Pas d'attente (image figée) quand aucun résultat ne viendra à temps : action imposée par l'hôte (minuteur ou
            // tour bloqué : le propriétaire ne l'a pas jouée), propriétaire qui ne répond plus, combat fini chez l'hôte.
            const bool forced = b.forced.count({b.turn, w.spot}) > 0;
            const bool skip = forced || (s.role == Role::Host && b.unresponsive.count(owner) > 0);
            const bool got = b.combos.count(key) > 0 ||
                             (!skip && wait_for("combo", [&] { return b.combos.count(key) > 0 || owner_of_spot(w.spot) != owner || (s.role == Role::Client && b.host_end >= 0); },
                                                kComboTimeout) &&
                              b.combos.count(key) > 0);
            const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
            if (got) w.plan = b.combos[key];
            else {
                bump(skip ? "combo_waits_skipped" : (s.role == Role::Client && b.host_end >= 0) ? "combo_waits_battle_over" : "combo_timeouts");
                w.plan = proto::ComboResult{b.id, b.turn, static_cast<uint8_t>(w.spot), w.seq, false, false, proto::kNoPress};
            }
            if (w.plan.success && w.plan.press <= 1000)
                b.presses.push_back({ctx.Context, w.start + std::chrono::microseconds(static_cast<int64_t>(w.duration * w.plan.press * 1000.0))});
            log().write("coop_combo_plan", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", w.plan.success).add("perfect", w.plan.perfect)
                                               .add("press", static_cast<int>(w.plan.press)).add("received", got).add("waited_ms", waited_ms), b.turn);
        }
        b.windows[ctx.Context] = w;
    });
    // Diagnostic : tout appui QTE qui atteint le gestionnaire de combat, avec les fenêtres ouvertes à cet instant.
    bus.pre(fn(game::path::BattleMgr, L"OnComboPressed"), [](HookCtx&, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        std::string open = "[";
        for (const auto& [tl, w] : s.battle.windows) {
            if (open.size() > 1) open += ",";
            open += "{\"spot\":" + std::to_string(w.spot) + ",\"seq\":" + std::to_string(w.seq) + ",\"local\":" + (w.local ? "true" : "false")
                  + ",\"elapsed_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - w.start).count())
                  + ",\"window_ms\":" + std::to_string(static_cast<int64_t>(w.duration * 1000)) + "}";
        }
        log().write("coop_combo_pressed", JsonObj().raw("open", open + "]").add("input_enabled", s.input_enabled), s.battle.turn);
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
        const double elapsed = std::chrono::duration<double>(SteadyClock::now() - w.start).count();
        // Diagnostic (QTE « raté d'emblée » à la manette, 2026-10-20) : chaque appui d'ici, même rejeté.
        log().write("coop_combo_try", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("ok", ok).add("perfect", perfect)
                                          .add("elapsed_ms", static_cast<int64_t>(elapsed * 1000)).add("window_ms", static_cast<int64_t>(w.duration * 1000)), s.battle.turn);
        if (w.published || !ok) return;
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

    // Visée libre (D11). Le tir part vers le point que renvoie BM.ComputeFreeAim3rdPersonTargetLocation au moment du
    // rayon du projectile (vu en jeu le 2026-10-19). Propriétaire : on publie le début, le point visé (~20 fois/s, pour
    // l'affichage), chaque tir (instant) et son point exact, puis l'annulation. Ailleurs : rejoué avec le même décalage
    // que ses actions, en imposant ce point visé.
    bus.post(fn(game::path::BattleMgr, L"ComputeFreeAim3rdPersonTargetLocation"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        auto* loc = static_cast<double*>(ue::out_param_address(ctx, f, STR("HitLocation")));
        if (!loc) return;
        if (s.fa_rep.started && !s.fa_rep.done && s.fa_rep.have_override) {
            loc[0] = s.fa_rep.point[0]; loc[1] = s.fa_rep.point[1]; loc[2] = s.fa_rep.point[2];
            return;
        }
        if (!s.fa_own.active) return;
        if (s.local_aim) { loc[0] = (*s.local_aim)[0]; loc[1] = (*s.local_aim)[1]; loc[2] = (*s.local_aim)[2]; }   // « joueur » simulé (tests)
        s.fa_own.point = {loc[0], loc[1], loc[2]};
        s.fa_own.have_point = true;
        if (s.fa_own.in_raycast) { s.fa_own.shot_point = s.fa_own.point; s.fa_own.raycast_point = true; }
    });
    auto fa_event = [](proto::FreeAimKind kind, uint16_t seq, const std::array<double, 3>& p) {
        proto::FreeAimEvent e;
        e.battle_id = s.battle.id;
        e.turn = s.battle.turn;
        e.spot = static_cast<uint8_t>(s.fa_own.spot);
        e.kind = kind;
        e.seq = seq;
        e.t_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count());
        e.x = p[0]; e.y = p[1]; e.z = p[2];
        return e;
    };
    bus.pre(fn(game::path::BattleMgr, L"StartFreeAimFromUI"), [fa_event](HookCtx&, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* cur = game::current_character();
        if (!cur || !game::is_player(cur) || !controls(game::spot(cur))) return;
        s.fa_own = FreeAimOwner{};
        s.fa_own.active = true;
        s.fa_own.spot = game::spot(cur);
        s.fa_own.t0 = SteadyClock::now();
        s.fa_diag_at = s.now + 0.5;
        bump("free_aims_published");
        publish(fa_event(proto::FreeAimKind::Start, 0, {}));
    });
    // Diagnostic (2026-10-24 : parade du client sans effet) : appui reçu par le contrôleur (action d'entrée), puis par
    // le gestionnaire de combat, avant TryStartParry / TryStartDodge (journalisés plus haut).
    for (const wchar_t* ev : {L"InpActEvt_IA_Battle_Defense_Parry_K2Node_EnhancedInputActionEvent_1",
                              L"InpActEvt_IA_Battle_Defense_Dodge_K2Node_EnhancedInputActionEvent_0"}) {
        const std::string name = ue::to_utf8(ev).substr(28, 5);   // « Parry » / « Dodge »
        bus.pre(fn(game::path::CtrlWorld, ev), [name](HookCtx&, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active || ++s.defense_path_logs > 120) return;
            log().write("coop_defense_input_event", JsonObj().add("action", name), s.battle.turn);
        });
    }
    for (const wchar_t* f : {L"OnParryAction", L"OnDodgeAction"}) {
        const std::string name = ue::to_utf8(f);
        bus.pre(fn(game::path::BattleMgr, f), [name](HookCtx&, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active || ++s.defense_path_logs > 120) return;
            log().write("coop_defense_bm_action", JsonObj().add("fn", name), s.battle.turn);
        });
    }
    // Diagnostic (2026-10-24 : TOTOFFF vise mais ne tire pas, à la manette) : bouton de tir vu par le héros.
    bus.pre(fn(game::path::BattleChar, L"InpActEvt_IA_Battle_FreeAim_Shoot_K2Node_EnhancedInputActionEvent_6"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active || ++s.shoot_input_logs > 60) return;
        UObject* fa = ue::get_object(ctx.Context, STR("BP_FreeAimShooterBattleComponent"));
        log().write("coop_free_aim_shoot_input", JsonObj().add("spot", game::spot(ctx.Context)).add("mine", controls(game::spot(ctx.Context)))
                                                    .add("aiming", ue::get_bool(fa, STR("IsAiming"))).add("own_active", s.fa_own.active), s.battle.turn);
    });
    bus.pre(fn(game::FreeAimComp, L"OnFreeAimShootAction"), [fa_event](HookCtx& ctx, UFunction*) {
        if (s.role != Role::Solo && s.battle.active && s.fa_own.active && ++s.shoot_input_logs <= 60)
            log().write("coop_free_aim_shoot_action", JsonObj().add("spot", s.fa_own.spot).add("mine", ue::component_owner(ctx.Context) == game::find_unit(true, s.fa_own.spot)), s.battle.turn);
        if (!s.fa_own.active || ue::component_owner(ctx.Context) != game::find_unit(true, s.fa_own.spot)) return;
        publish(fa_event(proto::FreeAimKind::Fire, ++s.fa_own.fires, s.fa_own.point));
    });
    bus.pre(fn(game::FreeAimProjectile, L"TriggerRaycast"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* caster = ue::get_object(ctx.Context, STR("Caster"));
        if (!caster || !game::is_player(caster)) return;
        if (s.fa_own.active && game::spot(caster) == s.fa_own.spot) {
            s.fa_own.in_raycast = true;
            s.fa_own.raycast_point = false;
            s.fa_own.shot_hit = nullptr;
            s.fa_own.shot_part = nullptr;
            return;
        }
        // Rejeu : point exact du tir du propriétaire (attente bornée ; avec le décalage, il est déjà là).
        auto& r = s.fa_rep;
        if (!r.started || r.done || game::spot(caster) != r.key.second) return;
        const uint16_t seq = ++r.raycasts;
        std::optional<proto::FreeAimEvent> shot;   // copie : le vecteur grandit pendant l'attente
        wait_for("free_aim_shot", [&] {
            for (auto& e : s.battle.fa_events[r.key]) if (e.kind == proto::FreeAimKind::Shot && e.seq == seq) { shot = e; return true; }
            return s.role == Role::Client && s.battle.host_end >= 0;   // combat fini chez l'hôte : plus aucun tir ne viendra
        }, 2.0);
        if (!shot) { bump("free_aim_timeouts"); return; }
        std::array<double, 3> p{shot->x, shot->y, shot->z};
        if (shot->target.spot != proto::kAutoSpot) {   // point relatif au combattant touché chez le propriétaire (ou visé par le faux client)
            UObject* unit = game::find_unit(shot->target.kind == proto::ActorKind::Hero, shot->target.spot);
            double a[3];
            // Zone touchée chez le propriétaire (point faible…) : visée là où elle est ici. Sinon (zone absente ici, ancien
            // message) : point relatif au combattant (approché si le point était relatif à la zone).
            const bool on_part = shot->part != 0xFF && free_aim_part_location(free_aim_part_at(unit, shot->part), a);
            if (on_part) bump("free_aim_part_aims");
            else if (shot->part != 0xFF) {
                bump("free_aim_part_missing");
                log().write("coop_free_aim_part_missing", JsonObj().add("seq", static_cast<int>(seq)).add("part", static_cast<int>(shot->part)), s.battle.turn);
            }
            // Zone : visée en son centre (un point de sa surface, visé depuis un endroit un peu différent, la frôlait sans la
            // toucher : essai du 2026-10-24, 16:21) ; les dégâts ne dépendent que de la zone touchée.
            if (on_part) p = {a[0], a[1], a[2]};
            else if (game::location(unit, a)) p = {a[0] + p[0], a[1] + p[1], a[2] + p[2]};
            else {
                bump("free_aim_anchor_missing");
                log().write("coop_free_aim_anchor_missing", JsonObj().add("seq", static_cast<int>(seq)).add("kind", static_cast<int>(shot->target.kind))
                                                                .add("target_spot", static_cast<int>(shot->target.spot)), s.battle.turn);
            }
        }
        r.point = p;
        r.have_override = true;
        r.in_raycast = true;
        r.want = shot->target;
        r.hit_spot = -1;
        bump("free_aim_shots_replayed");
        log().write("coop_free_aim_shot", JsonObj().add("spot", r.key.second).add("seq", static_cast<int>(seq)).add("x", p[0]).add("y", p[1]).add("z", p[2]), s.battle.turn);
    });
    bus.post(fn(game::FreeAimProjectile, L"TriggerRaycast"), [fa_event](HookCtx& ctx, UFunction*) {
        // Tir rejoué : le combattant touché chez le propriétaire l'a-t-il été ici ? Un tir manqué ici seulement laisse
        // l'ennemi plus vivant qu'ailleurs (essai à deux PC du 2026-10-22 : tirs 7, 9 et 14 sur 15 manqués chez le
        // client, Lancelier vivant ici et mort chez l'hôte). Diagnostic ; l'état de l'hôte corrige au tour suivant.
        if (auto& r = s.fa_rep; r.in_raycast) {
            UObject* caster = ue::get_object(ctx.Context, STR("Caster"));
            if (caster && game::is_player(caster) && game::spot(caster) == r.key.second) {
                r.in_raycast = false;
                if (r.want.spot != proto::kAutoSpot && (r.hit_spot < 0 || r.hit_spot != r.want.spot)) {
                    ++r.misses;
                    bump("free_aim_replay_misses");
                    log().write("coop_free_aim_replay_miss", JsonObj().add("spot", r.key.second).add("seq", static_cast<int>(r.raycasts))
                                                                 .add("owner_hit", static_cast<int>(r.want.spot)).add("hit", r.hit_spot)
                                                                 .add("x", r.point[0]).add("y", r.point[1]).add("z", r.point[2]), s.battle.turn);
                }
            }
        }
        if (!s.fa_own.in_raycast) return;
        UObject* caster = ue::get_object(ctx.Context, STR("Caster"));
        if (!caster || game::spot(caster) != s.fa_own.spot) return;
        s.fa_own.in_raycast = false;
        const auto p = s.fa_own.raycast_point ? s.fa_own.shot_point : s.fa_own.point;
        proto::FreeAimEvent e = fa_event(proto::FreeAimKind::Shot, ++s.fa_own.shots, p);
        // Tir qui a touché un combattant : point envoyé relatif à lui (même format que le faux client ; une version
        // précédente retrouve le même point absolu). Le rejeu vise ce combattant même s'il s'est un peu déplacé, et sait
        // qui devait être touché (tir manqué détecté).
        int hit_spot = -1;
        if (UObject* hit = s.fa_own.shot_hit) {
            double a[3];
            if (game::location(hit, a)) {
                e.target = actor_unit(hit);
                hit_spot = e.target.spot;
                // Zone touchée connue (point faible…) : point relatif à elle, le rejeu la vise là où elle est chez lui.
                const int pi = free_aim_part_index(hit, s.fa_own.shot_part);
                double pl[3];
                if (pi >= 0 && free_aim_part_location(s.fa_own.shot_part, pl)) {
                    e.part = static_cast<uint8_t>(pi);
                    a[0] = pl[0]; a[1] = pl[1]; a[2] = pl[2];
                }
                e.x -= a[0]; e.y -= a[1]; e.z -= a[2];
                if (++s.fa_part_logs <= 40)
                    log().write("coop_free_aim_part", JsonObj().add("seq", static_cast<int>(s.fa_own.shots)).add("index", pi).add("sent", e.part != 0xFF)
                                                         .add("part", s.fa_own.shot_part ? ue::short_name(s.fa_own.shot_part) : std::string())
                                                         .add("type", s.fa_own.shot_part ? static_cast<int>(ue::get_int(s.fa_own.shot_part, STR("BodyPartType"), -1)) : -1),
                                s.battle.turn);
            }
        }
        publish(e);
        log().write("coop_free_aim_local_shot", JsonObj().add("seq", static_cast<int>(s.fa_own.shots)).add("x", p[0]).add("y", p[1]).add("z", p[2])
                                                 .add("from_raycast", s.fa_own.raycast_point).add("hit_spot", hit_spot), s.battle.turn);
    });
    bus.post(fn(game::FreeAimComp, L"OnFreeAimProjectileHit"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* target = ue::read_param_object(ctx, f, STR("Character"));
        if (s.fa_own.in_raycast && !s.fa_own.shot_hit) { s.fa_own.shot_hit = target; s.fa_own.shot_part = ue::read_param_object(ctx, f, STR("TargetBodyPartBP")); }
        if (s.fa_rep.in_raycast && s.fa_rep.hit_spot < 0 && target) s.fa_rep.hit_spot = game::spot(target);
        bump("free_aim_hits");
        log().write("coop_free_aim_hit", JsonObj().add("target", game::char_class(target)).add("spot", game::spot(target))
                                          .add("body_part", ue::read_param_int(ctx, f, STR("BodyPart")).value_or(-1)), s.battle.turn);
    });
    for (const wchar_t* name : {L"CancelFreeAimFromUI"}) {
        bus.pre(fn(game::path::BattleMgr, name), [fa_event](HookCtx&, UFunction*) {
            if (!s.fa_own.active) return;
            publish(fa_event(proto::FreeAimKind::Cancel, 0, s.fa_own.point));
            bump("free_aim_cancels_published");
            s.fa_own.active = false;
            s.fa_own_ended = {s.battle.turn, s.fa_own.spot};
        });
    }
    bus.post(fn(game::FreeAimComp, L"EndFreeAim"), [fa_event](HookCtx& ctx, UFunction*) {
        if (!s.fa_own.active || ue::component_owner(ctx.Context) != game::find_unit(true, s.fa_own.spot)) return;
        publish(fa_event(proto::FreeAimKind::End, 0, s.fa_own.point));
        s.fa_own.active = false;
        s.fa_own_ended = {s.battle.turn, s.fa_own.spot};
    });

    // Défenses : TryParryHitFromCharacter (coups parables) ou ReceiveDamageFromObject (coups sautables).
    bus.pre(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* hero = ue::component_owner(ctx.Context);
        if (!hero || !game::is_player(hero)) return;
        defense_point(ctx.Context, hero, false);
    });
    // Diagnostic D39 : cible de chaque coup d'un héros sur un ennemi (TryParryHitFromCharacter est appelé sur les stats de
    // l'ennemi à chaque coup, compétences comprises : 9 coups du ThunderFall du 25/10 = 9 appels). Lecture seule.
    bus.post(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active || s.battle.turn == 0) return;
        const int es = cached_enemy_spot(ue::component_owner(ctx.Context));
        if (es < 0) return;
        UObject* attacker = ue::component_owner(ue::read_param_object(ctx, f, STR("AttackingCharacter")));
        if (!attacker || !game::is_player(attacker)) { bump("hero_hits_unattributed"); return; }
        record_hit_target(game::spot(attacker), es);
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
            b.turn_defenses.push_back({lh.spot, res});
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
            if (!game::is_player(hero)) return;
            if (controls(game::spot(hero))) {
                // Diagnostic (2026-10-24, TOTOFFF à la manette : esquive OK, parade sans animation) : appui du
                // propriétaire vu par le jeu, et blocage des défenses du combat.
                if (++s.defense_try_logs <= 60)
                    log().write("coop_defense_try", JsonObj().add("fn", ue::to_utf8(name)).add("spot", game::spot(hero))
                                                       .add("block_all", ue::get_bool(game::battle_manager(), STR("BlockAllDefensiveMoves"))), s.battle.turn);
                return;
            }
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
            if (s.repairing) { bump("repair_draws_skipped"); return; }
            trace_random(ctx, fname);
            if (!ctx.RESULT_DECL) return;
            // Avant le tour 1 : mise en place du combat (D30).
            if (!s.battle.active || s.battle.turn == 0) { offturn_draw(ctx, f, kind, fname); return; }
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
    // Diagnostic D39 : seul helper BP du dump qui tire un objet au hasard dans un tableau quelconque (Array_Random sur une
    // copie sans « Object »). Candidat (peu probable : le ThunderFall du 25/10 frappe deux fois de suite le même ennemi)
    // pour les rebonds des compétences ; journalisé avec ses appelants, jamais modifié.
    bus.post(L"/Game/Gameplay/GenericFunctionLibrary.GenericFunctionLibrary_C:GetRandomOtherObjectFromArray", [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active || !on_game_thread()) return;
        bump("random_object_picks");
        if (counter("random_object_picks") > 60) return;
        UObject* pick = ue::read_out_object(ctx, f, STR("OutItem"));
        log().write("coop_random_object_pick", JsonObj().add("pick", game::char_class(pick)).add("enemy", cached_enemy_spot(pick))
                                                   .raw("frames", trace_frames(ctx.TheStack.PreviousFrame(), 5)), s.battle.turn);
    });
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
            const ULONGLONG t0 = GetTickCount64();
            wait_for("rewards", [&] { return b.host_rewards.has_value(); }, 5.0);
            // Fin de combat de l'hôte arrivée entre-temps : ses récompenses suivent de peu (~2 s, p3_freeaim 2026-10-22).
            if (!b.host_rewards && b.host_end >= 0) wait_for("rewards_after_end", [&] { return b.host_rewards.has_value(); }, 4.0);
            bump_max("rewards_wait_max_ms", static_cast<int>(GetTickCount64() - t0));
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
                    if (write_loot(addr, sp->GetStruct(), b.host_rewards->loot)) bump("rewards_loot_rewritten");
                    else bump("rewards_loot_unrepaired");
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
        if (result == game::EndDefeat) defeat_arm();
    });

    // Défaite (D33). Réponse donnée à une fenêtre de l'écran de défaite (k = 0 « Réessayer / Abandonner », 1 équipe de
    // réserve ; tous les chemins : fenêtre, mod) : l'hôte la publie ; chez un client, une réponse qui ne vient pas du mod
    // est une fuite (comptée), remplacée par celle de l'hôte si elle est connue.
    for (int k = 0; k < 2; ++k) {
        const wchar_t* name = k == 0 ? L"OnRetryEncounterResponse" : L"OnReserveTeamResponse";
        const wchar_t* param = k == 0 ? L"ShouldRetry" : L"ShouldSendReserve";
        bus.pre(fn(game::path::BattleMgr, name), [k, param](HookCtx& ctx, UFunction* f) {
            auto* p = static_cast<uint8_t*>(ue::param_address(ctx, f, param));
            const int yes = p ? (*p != 0 ? 1 : 0) : -1;
            auto& d = s.defeat;
            log().write("coop_defeat_response", JsonObj().add("prompt", k).add("yes", yes).add("by_mod", d.applying || game::mod_call_depth() > 0)
                                                    .add("host_choice", d.host_choice[k]).add("local_allowed", d.local_allowed).add("battle", s.battle.id));
            if (s.role == Role::Host) {
                if (!s.defeat_sync || yes < 0) return;
                broadcast(proto::DefeatChoice{s.battle.id, static_cast<uint8_t>(k), static_cast<uint8_t>(yes)});
                bump("defeat_choices_published");
                log().write("coop_defeat_choice_sent", JsonObj().add("battle", s.battle.id).add("prompt", k).add("yes", yes).add("players", connected_players()));
                return;
            }
            if (s.role != Role::Client || !s.defeat_sync) return;
            d.responded[k] = true;
            d.next_poll = 0;
            if (p && d.host_choice[k] >= 0 && yes != d.host_choice[k] && !d.local_allowed) {
                *p = static_cast<uint8_t>(d.host_choice[k]);
                bump("defeat_choices_rewritten");
                log().write("coop_defeat_choice_rewritten", JsonObj().add("prompt", k).add("local", yes).add("host", d.host_choice[k]));
            }
        });
    }
    // Client : clic du joueur d'ici dans une fenêtre de l'écran de défaite (pas le mod, pas le repli « hôte muet ») : fuite.
    bus.pre(L"/Game/UI/Widgets/InGame_Menu/Modal/WBP_ModalPopup.WBP_ModalPopup_C:NotifyUserResponse", [](HookCtx& ctx, UFunction*) {
        auto& d = s.defeat;
        if (s.role != Role::Client || !s.defeat_sync || !d.armed || d.applying || d.local_allowed || game::mod_call_depth() > 0) return;
        if (ue::get_object(ctx.Context, STR("ParentActivatableWidget")) != game::defeat_screen()) return;
        bump("defeat_local_choices");
        log().write("coop_defeat_local_choice", JsonObj().add("prompt", d.prompt_kind).add("modal", ue::short_name(ctx.Context)));
    });
    // « Réessayer / Abandonner » ouverte : la prochaine fenêtre de choix est celle-là (client : refermée à l'image suivante).
    bus.post(std::wstring(game::kGameOverScreenClass) + L":PromptBattleRetry", [](HookCtx&, UFunction*) {
        s.defeat.retry_prompted = true;
        s.defeat.next_poll = 0;
    });
    // Diagnostic : déroulé de la fin de combat perdue (fonctions du jeu, premier paramètre), 80 lignes au plus par défaite.
    {
        const std::wstring screen = std::wstring(game::kGameOverScreenClass) + L":";
        const std::wstring modal = L"/Game/UI/Widgets/InGame_Menu/Modal/WBP_ModalPopup.WBP_ModalPopup_C:";
        const std::wstring bm = game::path::BattleMgr, ctrl = game::path::CtrlWorld;
        const std::pair<std::wstring, const wchar_t*> traced[] = {
            {bm + L"OnAllHeroesKilled", nullptr}, {bm + L"ShowReserveTeamGameOver", nullptr}, {bm + L"OnReserveTeamResponse", L"ShouldSendReserve"},
            {bm + L"OnGameOverScreenClosed", nullptr}, {bm + L"RequestRestartCurrentEncounter", nullptr}, {bm + L"PrepareForEncounterRestart", nullptr},
            {bm + L"LoadRestartState", nullptr}, {bm + L"TriggerAbandonBattle", nullptr}, {bm + L"StartBattleNEW", nullptr},
            {bm + L"SpawnBackupTeam", nullptr}, {bm + L"OnBackupTeamLoaded", nullptr},
            {ctrl + L"RestartCurrentEncounter", nullptr}, {ctrl + L"ResumeExplorationOnBattleEnd", nullptr},
            {screen + L"PromptBattleRetry", nullptr}, {screen + L"OnRetryUserResponse", L"InUserResponse"},
            {screen + L"OnReserveTeamModalResponse", L"InUserResponse"}, {screen + L"DisappearAndRemove", L"FadeBackToBattle"},
            {screen + L"FadeInGameOverScreen", nullptr}, {screen + L"OnUIAction", nullptr}, {screen + L"BP_OnDeactivated", nullptr},
            {modal + L"NotifyUserResponse", L"InUserResponse"},
            {L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:ChangeMap", nullptr},
        };
        for (const auto& t : traced) {
            const std::string name = ue::to_utf8(t.first.substr(t.first.rfind(L':') + 1));
            const wchar_t* param = t.second;
            bus.pre(t.first, [name, param](HookCtx& ctx, UFunction* f) {
                auto& d = s.defeat;
                if (!d.armed && d.host_choice[0] < 0 && d.host_choice[1] < 0) return;   // seulement autour d'une défaite
                if (++d.traces > 80) return;
                int size = 0;
                const auto* v = param ? static_cast<const uint8_t*>(ue::param_address(ctx, f, param, &size)) : nullptr;
                log().write("coop_defeat_trace", JsonObj().add("fn", name).add("arg", v && size >= 1 ? static_cast<int>(*v) : -1)
                                                     .add("self", ue::short_name(ctx.Context)).add("by_mod", game::mod_call_depth() > 0));
            });
        }
    }

    // Fuite : retour à l'exploration sans StartBattleEndFlow (vu le 2026-10-20) -> fin « neutre », même chemin.
    bus.post(fn(game::path::CtrlWorld, L"ResumeExplorationOnBattleEnd"), [](HookCtx&, UFunction*) {
        auto& b = s.battle;
        if (!b.active || b.ending) return;
        b.ending = true;
        b.local_end = game::EndNeutral;
        b.left = true;
        if (s.role == Role::Host) broadcast(proto::BattleEnd{b.id, static_cast<uint8_t>(game::EndNeutral)});
        bump("battles_left");
        log().write("coop_battle_left", JsonObj().add("battle", b.id));
        b.end_flow_at = s.now;
    });
}

} // namespace e33::coop::detail
