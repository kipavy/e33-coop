// Fin de combat : récompenses, clôture, défaite (D33), fin imposée chez le client.
#include "coop_internal.hpp"

namespace e33::coop::detail {

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

// Client : butin de l'hôte écrit dans les récompenses d'ici (même nombre d'entrées : objet et quantité de chacune).
// Un nombre d'entrées différent n'est pas réparable ici (tableau du jeu) : compté (rewards_loot_unrepaired).
bool write_loot(void* st_ptr, RC::Unreal::UStruct* st, const std::vector<proto::LootEntry>& loot)
{
    using namespace RC::Unreal;
    auto* ap = CastField<FArrayProperty>(ue::struct_member(st, STR("RolledLootEntries")));
    if (!ap) return false;
    auto* inner = CastField<FStructProperty>(ap->GetInner());
    auto* arr = static_cast<FScriptArray*>(ap->ContainerPtrToValuePtr<void>(st_ptr));
    if (!inner || !arr || arr->Num() != static_cast<int32>(loot.size())) return false;
    UStruct* es = inner->GetStruct();
    FProperty* id = ue::struct_member(es, STR("ItemID"));
    FProperty* qty = ue::struct_member(es, STR("Quantity"));
    if (!id || !qty) return false;
    auto* data = static_cast<uint8_t*>(arr->GetData());
    for (size_t i = 0; i < loot.size(); ++i) {
        void* e = data + i * inner->GetSize();
        *static_cast<FName*>(id->ContainerPtrToValuePtr<void>(e)) = FName(ue::from_utf8(loot[i].item).c_str(), FNAME_Add);
        *static_cast<int32_t*>(qty->ContainerPtrToValuePtr<void>(e)) = loot[i].quantity;
    }
    return true;
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
        if (!b.left) compare_draws(b.turn);
    }
    b.finalized = true;
    bump("battles_finalized");
    b.active = false;
    s.setup.open = false;   // combat fini avant son tour 1 : jamais de fenêtre de mise en place en exploration (D30)
    set_local_input(true, "battle end");
    ui::clear_status("turn");
    if (b.hold) { b.hold = false; explore::hold_pause("battle_start", false); }
    log().write("coop_battle_finalized", JsonObj().add("battle", b.id).add("why", why).add("desyncs", counter("desyncs")));
}

// --- Défaite (D33) -------------------------------------------------------------------------------
// Écran de défaite du jeu (WBP_jRPG_GameOverScreen) : d'abord « Envoyer l'équipe de réserve ? » si elle existe, puis
// « Réessayer » / « Abandonner ». Seul l'hôte répond (DefeatChoice). Client : chaque fenêtre de choix est refermée dès
// qu'elle s'ouvre (statut « l'hôte choisit »), puis la réponse de l'hôte est donnée ici :
//   même réponse donnée à l'écran d'ici (le jeu continue avec la réserve, relance le combat ou revient à l'exploration ;
//   le mod suit un combat qui reprend comme un combat engagé ici avant le BattleStart de l'hôte).
// Vu en test (2026-10-24, combat lancé par une action de jeu, non « rejouable ») : réserve refusée -> le jeu répond seul
// « abandonner » (OnRetryEncounterResponse(false), sans fenêtre) -> retour à l'exploration sur place, sans chargement.
// Filet : encore en combat 20 s après l'abandon (écran noir chez TOTOFFF le 2026-10-24) -> instantané de l'hôte demandé.
// -e33defeatabandon=resync : pas d'abandon ici, instantané demandé tout de suite.
// Hôte muet 120 s après l'ouverture d'une fenêtre : le joueur d'ici choisit lui-même.

void defeat_reset(uint32_t battle)
{
    s.defeat = State::Defeat{};
    s.defeat.battle = battle;
    s.defeat.at = s.now;
}

// Défaite vue ici (StartBattleEndFlow, résultat 2) : écran de défaite attendu.
void defeat_arm()
{
    auto& d = s.defeat;
    // Choix de l'hôte déjà reçu pour ce combat (il a pu choisir avant notre fin de combat) : gardé.
    if (d.battle != s.battle.id || d.done_kind >= 0) defeat_reset(s.battle.id);
    d.armed = true;
    d.at = s.now;
    d.next_poll = 0;
    log().write("coop_defeat", JsonObj().add("battle", d.battle).add("host_retry", d.host_choice[0]).add("host_reserve", d.host_choice[1])
                                   .add("sync", s.defeat_sync), s.battle.turn);
}

void defeat_on_host_choice(const proto::DefeatChoice& dc)
{
    auto& d = s.defeat;
    if (d.battle != dc.battle_id || d.done_kind >= 0) defeat_reset(dc.battle_id);
    const int k = dc.prompt == 1 ? 1 : 0;
    d.host_choice[k] = dc.yes ? 1 : 0;
    d.host_choice_at[k] = s.now;
    d.next_poll = 0;
    bump("defeat_choices_received");
    log().write("coop_defeat_host_choice", JsonObj().add("battle", dc.battle_id).add("prompt", k).add("yes", static_cast<int>(dc.yes))
                                               .add("armed", d.armed).add("prompt_seen", d.prompt_kind).add("sync", s.defeat_sync));
}

// Fenêtre de choix refermée ici (le joueur ne peut pas choisir) : désactivée (ses touches ne répondent plus) et réduite.
void defeat_hide(UObject* modal)
{
    auto& d = s.defeat;
    {
        game::ModCall mod;
        ue::Call off(modal, STR("DeactivateWidget"));
        if (off) off.invoke();
        ue::Call vis(modal, STR("SetVisibility"));
        if (vis) { vis.set_int(STR("InVisibility"), 1); vis.invoke(); }   // ESlateVisibility::Collapsed
    }
    ++d.hidden;
    bump("defeat_prompts_hidden");
    ui::set_status("defeat", "defeat_host_choosing", {player_name(0)});
    bool still = false;
    game::defeat_modal(game::defeat_screen(), &still);
    log().write("coop_defeat_prompt_hidden", JsonObj().add("battle", d.battle).add("prompt", d.prompt_kind).add("n", d.hidden).add("still_visible", still));
}

// Réponse de l'hôte à la fenêtre k donnée ici. modal : fenêtre encore ouverte (réponse par elle, comme un clic).
void defeat_apply(int k, UObject* screen, UObject* modal)
{
    auto& d = s.defeat;
    d.applied[k] = true;
    d.applied_at = s.now;
    d.prompt_kind = -1;   // prompt_modal gardé : la même fenêtre (parfois encore « active ») n'est pas une nouvelle fenêtre
    bump("defeat_choices_applied");
    const bool yes = d.host_choice[k] == 1;
    const std::string host = player_name(0);
    // Réserve refusée : la fenêtre « Réessayer / Abandonner » (ou l'abandon automatique du jeu) suit, l'attente continue.
    if (k == 0 || yes) { d.done_kind = k; bump("defeat_follows_done"); }
    // Le jeu d'ici a déjà répondu (abandon automatique après la réserve refusée) : rien à redonner.
    if (d.responded[k]) {
        if (k == 0) ui::set_status("defeat", yes ? "defeat_host_retry" : "defeat_host_abandon", {host});
        log().write("coop_defeat_applied", JsonObj().add("battle", d.battle).add("prompt", k).add("yes", yes).add("how", "already").add("ok", true));
        return;
    }
    if (k == 1 || yes || !s.defeat_abandon_resync) {
        const int resp = yes ? game::RespConfirm : game::RespCancel;
        bool ok = false;
        const char* how = "none";
        d.applying = true;
        {
            game::ModCall mod;
            if (modal) {
                ue::Call c(modal, STR("NotifyUserResponse"));
                if (c) { c.set_int(STR("InUserResponse"), resp); ok = c.invoke(); how = "modal"; }
            }
            if (!ok && screen) {
                ue::Call c(screen, k == 1 ? STR("OnReserveTeamModalResponse") : STR("OnRetryUserResponse"));
                if (c) { c.set_int(STR("InUserResponse"), resp); ok = c.invoke(); how = "screen"; }
            }
        }
        d.applying = false;
        if (!ok) bump("defeat_apply_errors");
        if (k == 1 && !yes) ui::set_status("defeat", "defeat_host_choosing", {host});
        else ui::set_status("defeat", k == 1 ? "defeat_host_reserve" : yes ? "defeat_host_retry" : "defeat_host_abandon", {host});
        log().write("coop_defeat_applied", JsonObj().add("battle", d.battle).add("prompt", k).add("yes", yes).add("how", how).add("ok", ok)
                                               .add("response", resp).add("wait_s", d.prompt_at >= 0 ? s.now - d.prompt_at : -1.0));
        return;
    }
    // Abandon : la partie de l'hôte, rechargée chez lui, arrive par un instantané (point sûr de l'hôte, D27).
    publish(proto::ResyncRequest{"host abandoned the battle"});
    bump("defeat_resyncs_requested");
    ui::set_status("defeat", "defeat_host_abandon", {host});
    log().write("coop_defeat_applied", JsonObj().add("battle", d.battle).add("prompt", 0).add("yes", false).add("how", "resync").add("ok", true)
                                           .add("wait_s", d.prompt_at >= 0 ? s.now - d.prompt_at : -1.0));
}

void defeat_tick()
{
    auto& d = s.defeat;
    // Classe de l'écran de défaite chargée dès le premier combat : ses hooks sont posés avant la première défaite.
    static bool screen_class_loaded = false;
    if (!screen_class_loaded && s.battle.active) { screen_class_loaded = true; game::load_asset(game::kGameOverScreenClass); }
    if (s.role != Role::Client || !s.defeat_sync) return;
    if (s.now < d.next_poll) return;
    // Statut retiré une fois le choix suivi (nouveau combat ou combat repris, ou monde rechargé après l'abandon).
    if (d.done_kind >= 0) {
        d.next_poll = s.now + 0.5;
        const bool abandon = d.done_kind == 0 && d.host_choice[0] == 0;
        // Filet : toujours en combat 20 s après l'abandon -> partie de l'hôte par un instantané (point sûr de l'hôte, D27).
        if (abandon && !d.rescue_sent && s.now - d.applied_at > 20.0 && game::in_battle()) {
            d.rescue_sent = true;
            d.applied_at = s.now;   // nouvelle attente (chargement de l'instantané)
            publish(proto::ResyncRequest{"still in battle after the host gave up"});
            bump("defeat_resyncs_requested");
            log().write("coop_defeat_rescue", JsonObj().add("battle", d.battle));
            return;
        }
        const bool done = s.now - d.applied_at > 120.0 ||
                          (!abandon && s.battle.active && s.battle.id != d.battle) ||
                          (abandon && s.now - d.applied_at > 5.0 && game::controller() && !game::in_battle());
        if (done) {
            ui::clear_status("defeat");
            log().write("coop_defeat_done", JsonObj().add("battle", d.battle).add("prompt", d.done_kind).add("after_s", s.now - d.applied_at));
            defeat_reset(0);
        }
        return;
    }
    if (!d.armed && d.host_choice[0] < 0 && d.host_choice[1] < 0) return;
    d.next_poll = s.now + 0.2;   // FindAllOf : pas à chaque image
    UObject* screen = game::defeat_screen();
    bool visible = false;
    UObject* modal = screen ? game::defeat_modal(screen, &visible) : nullptr;
    if (modal && modal != d.prompt_modal) {
        // Nouvelle fenêtre de choix : « Réessayer / Abandonner » si PromptBattleRetry est passé, sinon l'équipe de réserve.
        d.prompt_modal = modal;
        d.prompt_kind = d.retry_prompted ? 0 : 1;
        d.prompt_at = s.now;
        bump("defeat_prompts_seen");
        log().write("coop_defeat_prompt", JsonObj().add("battle", d.battle).add("prompt", d.prompt_kind).add("host_choice", d.host_choice[d.prompt_kind])
                                              .add("visible", visible).add("after_s", s.now - d.at));
    }
    const int k = d.prompt_kind;
    // Réponse de l'hôte connue pour la fenêtre ouverte ici : donnée tout de suite.
    if (k >= 0 && !d.local_allowed && d.host_choice[k] >= 0 && !d.applied[k]) { defeat_apply(k, screen, modal); return; }
    // Le jeu d'ici a déjà répondu à une fenêtre dont l'hôte a donné la réponse (abandon automatique) : suivi noté.
    for (int j = 1; j >= 0; --j)
        if (d.responded[j] && d.host_choice[j] >= 0 && !d.applied[j]) { defeat_apply(j, screen, nullptr); return; }
    // Sans fenêtre vue ici 15 s après la réponse de l'hôte (écran pas encore là, autre déroulé) : réponse donnée à
    // l'écran, réserve d'abord.
    for (int j = 1; j >= 0 && k < 0 && screen && !d.local_allowed; --j)
        if (d.host_choice[j] >= 0 && !d.applied[j] && s.now - d.host_choice_at[j] > 15.0 && (j == 1 || d.host_choice[1] != 1)) {
            log().write("coop_defeat_no_prompt_apply", JsonObj().add("prompt", j));
            defeat_apply(j, screen, nullptr);
            return;
        }
    // Abandon de l'hôte sans écran de défaite ici (combat local pas perdu) : instantané de l'hôte demandé.
    if (!screen && !d.armed && !d.applied[0] && d.host_choice[0] == 0 && s.now - d.host_choice_at[0] > 30.0) {
        d.applied[0] = true;
        d.applied_at = s.now;
        d.done_kind = 0;
        d.rescue_sent = true;
        publish(proto::ResyncRequest{"host gave up a battle not lost here"});
        bump("defeat_resyncs_requested");
        log().write("coop_defeat_rescue", JsonObj().add("battle", d.battle).add("why", "no defeat here"));
        return;
    }
    if (modal && visible && !d.local_allowed) defeat_hide(modal);
    // Hôte muet : le joueur d'ici choisit (« Réessayer / Abandonner » rouverte ; réserve : refusée ici, la suivante s'ouvre).
    if (!d.local_allowed && k >= 0 && d.host_choice[k] < 0 && s.now - d.prompt_at > kDefeatHostSilentS && screen) {
        d.local_allowed = true;
        bump("defeat_local_fallbacks");
        bool ok = false;
        {
            game::ModCall mod;
            if (k == 0) { ue::Call c(screen, STR("PromptBattleRetry")); ok = c && c.invoke(); }
            else { ue::Call c(screen, STR("OnReserveTeamModalResponse")); if (c) { c.set_int(STR("InUserResponse"), game::RespCancel); ok = c.invoke(); } }
        }
        ui::clear_status("defeat");
        ui::toast("defeat_host_silent", {player_name(0)}, 10.0);
        log().write("coop_defeat_local_fallback", JsonObj().add("battle", d.battle).add("prompt", k).add("ok", ok));
    }
    // Diagnostic : écran de défaite sans fenêtre de choix reconnue 20 s après la défaite (autre parent ?).
    if (screen && d.prompt_at < 0 && !d.diag_logged && s.now - d.at > 20.0) {
        d.diag_logged = true;
        std::vector<UObject*> all;
        RC::Unreal::UObjectGlobals::FindAllOf(STR("WBP_ModalPopup_C"), all);
        std::string list = "[";
        for (UObject* m : all) {
            if (list.size() > 1) list += ",";
            UObject* parent = ue::get_object(m, STR("ParentActivatableWidget"));
            list += JsonObj().add("active", ue::get_bool(m, STR("bIsActive"))).add("parent", parent ? ue::short_name(parent) : std::string()).str();
        }
        log().write("coop_defeat_no_prompt", JsonObj().add("screen", ue::short_name(screen)).raw("modals", list + "]"));
    }
}

} // namespace e33::coop::detail

namespace e33::coop {

// Client : l'hôte a fini le combat (BattleEnd) mais le jeu d'ici ne l'a pas fini (ennemi encore vivant ici après un écart :
// essai à deux PC du 2026-10-22, Lancelier tué chez l'hôte seulement). L'hôte décide : fin imposée ici, par étapes, chacune
// tentée si la précédente n'a pas lancé la fin du jeu (StartBattleEndFlow -> s.battle.ending) :
//   1. BM.ForceBattleEnd(résultat de l'hôte), dès qu'un tour recommence ici ou 5 s après le BattleEnd (le jeu d'ici finit
//      normalement de lui-même dans ce délai : il n'a que le décalage de rejeu de retard) ;
//   2. BM.StartBattleEndFlow(résultat), 3 s plus tard ;
//   3. victoire : ennemis restants mis à mort (Stats.SetDead), 4 s plus tard ; le jeu conclut à la fin du tour ;
//   4. 10 s plus tard : combat clos pour le mod (entrées rendues) et nouvel instantané demandé à l'hôte (ResyncRequest :
//      il l'envoie hors combat, le client le recharge et sort du combat).
// Les récompenses restent celles de l'hôte (RollBattleRewards) ; la clôture passe par les chemins habituels.

void client_force_end_tick()
{
    auto& b = s.battle;
    if (s.role != Role::Client || !b.active || b.finalized || b.host_end < 0 || b.ending || b.end_flow_at >= 0) return;
    if (!game::in_battle()) return;   // déjà sorti du combat : clôture par la règle « host_end » du tick
    if (game::pause_menu_visible()) return;
    const double since = s.now - b.host_end_at;
    auto step_log = [&](const char* what, bool ok, const std::string& how) {
        bump("battle_end_forced_steps");
        log().write("coop_force_end", JsonObj().add("battle", b.id).add("step", b.force_step).add("what", what).add("ok", ok).add("how", how)
                                          .add("host_end", b.host_end).add("since_host_end_s", since).add("turn_restarted", b.force_now), b.turn);
    };
    std::string how;
    switch (b.force_step) {
    case 0: {
        if (!b.force_now && since < kForceEndGrace) return;
        b.force_step = 1;
        b.force_at = s.now;
        bump("battles_end_forced");
        const bool ok = game::force_battle_end(b.host_end, false, how);
        step_log("ForceBattleEnd", ok, how);
        return;
    }
    case 1: {
        if (s.now - b.force_at < 3.0) return;
        b.force_step = 2;
        b.force_at = s.now;
        const bool ok = game::force_battle_end(b.host_end, true, how);
        step_log("StartBattleEndFlow", ok, how);
        return;
    }
    case 2: {
        if (s.now - b.force_at < 4.0) return;
        b.force_step = 3;
        b.force_at = s.now;
        int killed = 0, failed = 0;
        if (b.host_end == game::EndVictory)
            for (UObject* e : game::enemies())
                if (!game::is_dead_fast(e)) (game::kill_unit(e) ? killed : failed)++;
        step_log("kill_enemies", failed == 0, std::to_string(killed) + " killed, " + std::to_string(failed) + " failed");
        return;
    }
    case 3: {
        if (s.now - b.force_at < 10.0) return;
        b.force_step = 4;
        step_log("resync", true, "finalize + ResyncRequest");
        bump("battle_end_resyncs");
        finalize_battle("host_end_forced");
        publish(proto::ResyncRequest{"battle still running after host BattleEnd"});
        return;
    }
    default:
        return;
    }
}

} // namespace e33::coop
