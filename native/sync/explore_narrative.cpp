// Barrière narrative, pause, scènes synchronisées (D20, D24), relevés périodiques.
#include "explore_internal.hpp"

namespace e33::explore::detail {

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

// Les événements d'exploration ne concernent que les joueurs entrés dans la partie (hôte : au moins un client
// entré ; client : instantané chargé, ou session sans « rejoindre » des tests de combat).
bool sharing() { return s.host ? joined_players() > 0 : (!s.want_join || s.cj == S::ClientJoin::Joined); }

} // namespace e33::explore::detail
