// Synchro des tours : début de tour hôte / client, vérification d'état, entrées, alignement des tours ennemis (D28), parade en duo (D16).
#include "coop_internal.hpp"

namespace e33::coop::detail {

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
    (void)index;
    return character_owner(name);   // même répartition qu'hors combat (builds) : le k-ième de l'équipe au k-ième joueur
}

void begin_turn_common(UObject* actor)
{
    ++s.battle.turn;
    s.battle.turn_actor = actor_unit(actor);
    // Fins d'action (D36) et états hachés des tours passés : bornés (quelques tours suffisent).
    {
        auto& od = s.battle.owner_done;
        for (auto it = od.begin(); it != od.end();) it = it->first.first + 4 < s.battle.turn ? od.erase(it) : std::next(it);
        auto& hs = s.battle.hashed_state;
        for (auto it = hs.begin(); it != hs.end();) it = it->first + 8 < s.battle.turn ? hs.erase(it) : std::next(it);
    }
    if (s.battle.turn == 1) setup_close();   // mise en place finie : tirages des tours à partir d'ici (D30)
    s.battle.draw_rank.clear();
    s.battle.hit_seq.clear();
    s.battle.combo_seq.clear();
    s.battle.action_wait_logged = false;
    refresh_enemy_spots();
    s.battle.turn_started_at = s.now;
    s.battle.timer_elapsed = 0;
    s.fa_own = FreeAimOwner{};
    s.fa_rep = FreeAimReplay{};
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
        // Combat lancé depuis l'exploration (pas par le test runner) : rencontre et ennemi du monde réels (D12).
        {
            std::string enc, gid;
            int engage = 0;
            if (game::battle_params(enc, gid, engage) && gid != "E33CoopTest") {
                bs.encounter = enc;
                bs.enemy_global_id = gid;
                bs.engage = static_cast<uint8_t>(engage);
                bs.world_enemy = explore::take_world_enemy();
                bs.party = game::party_names();   // équipe réelle : celle du client peut être restée l'ancienne
                // Héros réellement en combat, par emplacement : un combat contre un marchand se joue avec les seuls héros
                // choisis (ex. Maelle seule, 2026-10-24) ; avec l'équipe d'exploration, le client avait toute l'équipe et des
                // emplacements décalés (il voyait Killian jouer Lune).
                {
                    auto heroes = game::players();
                    std::sort(heroes.begin(), heroes.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
                    std::vector<std::string> in_battle;
                    for (UObject* h : heroes) { const std::string n = game::hardcoded_name(h); if (!n.empty() && n != "None") in_battle.push_back(n); }
                    const bool prefix = in_battle.size() <= bs.party.size() && std::equal(in_battle.begin(), in_battle.end(), bs.party.begin());
                    // Équipe complète en combat : ordre d'exploration gardé. Le jeu ne place pas les héros dans l'ordre de
                    // l'équipe (Frey, Lune, Maelle -> emplacements Lune, Maelle, Frey) : envoyer l'ordre des emplacements
                    // décalait tout chez le client (0.6.25, Maelle à TOTOFFF et ordre des tours différent).
                    if (!in_battle.empty() && !prefix && in_battle.size() < bs.party.size()) {   // préfixe (réserve en plus) : équipe gardée telle quelle
                        log().write("coop_battle_party_subset", JsonObj().add("party", static_cast<int64_t>(bs.party.size())).add("in_battle", static_cast<int64_t>(in_battle.size())));
                        bs.party = in_battle;
                    }
                }
                bump("world_battles");
            }
        }
        // Graine choisie au lancement du combat (mise en place, D30) ; repli : tirée ici comme avant.
        if (setup_window() && s.setup.seeded) {
            b.rng_seed = s.setup.seed;
        } else {
            b.rng_seed = s.fixed_seed ? *s.fixed_seed : splitmix64((static_cast<uint64_t>(GetTickCount64()) << 20) ^ s.session_id ^ b.id);
            bump("setup_windows_missing");
            log().write("coop_setup_missing", JsonObj().add("open", s.setup.open).add("age_s", s.setup.open ? s.now - s.setup.opened_at : -1.0));
        }
        bs.rng_seed = b.rng_seed;
        b.turn_timer_s = s.turn_timer_s;
        bs.turn_timer_s = s.turn_timer_s;
        b.duo_parry = s.duo_parry;
        bs.duo_parry = s.duo_parry ? 1 : 0;
        auto ps = game::players();
        std::sort(ps.begin(), ps.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
        for (size_t i = 0; i < ps.size(); ++i) {
            const int spot = game::spot(ps[i]);
            const uint8_t owner = default_owner(static_cast<int>(i), spot, game::hardcoded_name(ps[i]));
            b.owner_by_spot[spot] = owner;
            bs.owners.push_back({static_cast<uint8_t>(spot), game::char_class(ps[i]), owner});
        }
        // Démarrage coordonné : personne ne commence un combat avant les autres. Les clients de la partie lancent le même
        // combat dès qu'ils peuvent (BattleStart) ; ce premier tour attend, jeu en pause, qu'ils y soient tous (BattleReady).
        for (auto& p : s.peers) if (in_game(p)) b.hold = true;
        b.hold_since = s.now;
        broadcast(bs);
        // Tirages de la mise en place (tour 0), pour vérification chez les clients (comparés à leur tour 1).
        // Par paquets de 32 (le protocole refuse les listes de plus de 64 ; une mise en place en fait ~100).
        if (setup_window() && !s.setup.draws.empty()) {
            proto::Draws d{b.id, 0, {}};
            for (auto& [k, v] : s.setup.draws) {
                d.draws.push_back({static_cast<uint32_t>(k >> 16), static_cast<uint16_t>(k & 0xFFFF), v});
                if (d.draws.size() == 32) { broadcast(d); d.draws.clear(); }
            }
            if (!d.draws.empty()) broadcast(d);
        }
        log().write("coop_battle_inventory", JsonObj().raw("inventory", game::battle_inventory_json()));
        // Diagnostic : composants de chaque héros (mécaniques propres : perfection, taches, masques… à couvrir par le hash).
        for (UObject* h : game::players()) {
            std::string comps = "[";
            for (UObject* c : ue::get_object_array(h, STR("BlueprintCreatedComponents"))) {
                if (comps.size() > 1) comps += ",";
                comps += "\"" + game::char_class(c) + "\"";
            }
            log().write("coop_hero_components", JsonObj().add("hero", game::hardcoded_name(h)).raw("components", comps + "]"));
        }
    }
    begin_turn_common(actor);
    b.ots_seq = b.sct_seq;   // alignement des tours ennemis : OnTurnStart de ce tour passé
    b.play_since = s.now;
    b.cur_enemy = !game::is_player(actor);
    const proto::Unit au = actor_unit(actor);
    turn_indicator(au);
    broadcast(proto::TurnStart{b.id, b.turn, au.kind, au.spot, game::char_class(actor)});
    const proto::StateSync st = sync::snapshot(b.id, b.turn);
    const uint64_t h = sync::hash(st, au);
    b.local_hash[b.turn] = h;
    broadcast(proto::StateHash{b.id, b.turn, h});
    broadcast(st);
    b.hashed_state[b.turn] = st;   // journal desync : l'état exact haché
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

// Client : test de la fin imposée (-e33perturbend=N) : au début du tour N, après son contrôle, les ennemis vivants
// d'ici reçoivent 100 fois leurs PV. Le coup fatal de l'hôte ne les tue pas ici : l'hôte gagne, le combat continue ici
// (comme le Lancelier de l'essai à deux PC du 2026-10-22) et doit être clos par client_force_end_tick.
void perturb_end()
{
    int n = 0;
    for (UObject* e : game::enemies()) {
        if (game::is_dead_fast(e)) continue;
        UObject* st = game::stats(e);
        ue::set_double(st, STR("CurrentHP"), ue::get_double(st, STR("CurrentHP")) * 100.0);
        ++n;
    }
    bump("end_perturbations");
    log().write("coop_perturb_end", JsonObj().add("enemies", n), s.battle.turn);
}

// --- Essai -e33turndep=1 : attente du début de tour de l'hôte sans figer le jeu ------------------------------------
// Le jeu attend, au début de chaque tour, que les « dépendances » de BM.TurnStartNotifyDependencies soient résolues,
// en continuant d'animer. Le mod s'y inscrit au moment où le jeu commence à écouter (StartListenToDependencies) :
// plus tôt, le compteur est remis à zéro ; plus tard, l'écoute a déjà conclu et la résolution relance la suite du tour
// une deuxième fois (l'ennemi décidait deux fois de son action, vu le 2026-10-22).
UObject* turn_deps() { return ue::get_object(game::battle_manager(), STR("TurnStartNotifyDependencies")); }

void dep_solve(const char* why)
{
    auto& b = s.battle;
    if (!b.dep_active) return;
    b.dep_active = false;
    ue::Call solve(turn_deps(), STR("SolveDependency"));
    if (solve) solve.set_object(STR("Dependency"), game::save_manager()).invoke();
    log().write("coop_turn_dep_solved", JsonObj().add("turn", b.dep_turn).add("waited_s", s.now - b.dep_since).add("why", why), b.dep_turn);
}

void client_turn_start(UObject* actor)
{
    auto& b = s.battle;
    // Tour qui commence : turn + 1. Une dépendance posée pour un autre tour (le tour a avancé sans elle : action lancée
    // directement par le testeur) est retirée, sinon elle bloquerait les tours suivants.
    if (b.dep_active && b.dep_turn != b.turn + 1) { bump("turn_deps_superseded"); dep_solve("superseded"); }
    if (b.dep_finish_pending) { b.dep_finish_pending = false; b.dep_start = {}; }
    b.ots_seq = b.sct_seq;
    begin_turn_common(actor);
    if (!b.ready_sent) {   // démarrage coordonné : l'hôte attend que chacun ait commencé ce combat
        b.ready_sent = true;
        for (auto& p : s.peers) send_to(p, proto::BattleReady{b.id, s.my_player_id});
        // Délai depuis le BattleStart de l'hôte (essai à deux PC du 2026-10-22 : jamais envoyé, l'hôte restait figé).
        const int delay_ms = b.start_rx_at >= 0 ? static_cast<int>((s.now - b.start_rx_at) * 1000.0) : -1;
        if (delay_ms >= 0) bump_max("battle_ready_delay_ms", delay_ms);
        log().write("coop_battle_ready_sent", JsonObj().add("battle", b.id).add("delay_ms", delay_ms), b.turn);
    }
    // Combat déjà fini chez l'hôte (BattleEnd reçu) et pourtant un tour recommence ici : la simulation locale a divergé
    // (ennemi encore vivant ici). Aucun état de l'hôte ne viendra pour ce tour : pas d'attente, fin imposée (tick).
    // Avant : attente bloquante de 15 s du tour de l'hôte, puis session perdue (essai à deux PC du 2026-10-22).
    if (b.host_end >= 0) {
        bump("turns_after_host_end");
        b.force_now = true;
        log().write("coop_turn_after_host_end", JsonObj().add("battle", b.id).add("host_end", b.host_end).add("actor", game::char_class(actor)), b.turn);
        return;
    }
    // État de l'hôte pas encore là : vérification différée (le tick l'appelle à l'arrivée), pendant que la dépendance
    // retient le tour. Si le jeu écoutait déjà sans nous pour ce tour, trop tard : attente bloquante comme avant.
    if (s.turn_dep && !b.strict && b.turn > 1 && !b.host_sync.count(b.turn) && b.host_end < 0) {
        const bool registered = b.dep_active && b.dep_turn == b.turn;
        const bool listened_without_us = !registered && b.listen_seq == b.sct_seq;
        if (!listened_without_us) {
            b.dep_turn = b.turn;
            b.dep_actor = actor;
            b.dep_start = sync::snapshot(b.id, b.turn);
            b.dep_finish_pending = true;
            if (!registered) b.dep_since = s.now;
            bump("turn_checks_deferred");
            log().write("coop_turn_deferred", JsonObj().add("turn", b.turn).add("registered", registered), b.turn);
            return;
        }
        bump("turn_deps_too_late");
    }
    client_turn_finish(actor);
}

// Tour vérifié en différé : ce qui a changé ici depuis le début du tour (effets de début de tour appliqués pendant
// l'attente) est reporté sur l'état de l'hôte, pour ne corriger que l'écart réel et ne pas annuler ces effets.
proto::StateSync shift_host_state(const proto::StateSync& host, const proto::StateSync& start, const proto::StateSync& now)
{
    proto::StateSync out = host;
    auto find = [](const proto::StateSync& st, const proto::Unit& u) -> const proto::UnitState* {
        for (auto& x : st.units) if (x.unit.kind == u.kind && x.unit.spot == u.spot) return &x;
        return nullptr;
    };
    for (auto& h : out.units) {
        const proto::UnitState* a = find(start, h.unit);
        const proto::UnitState* c = find(now, h.unit);
        if (!a || !c) continue;
        h.hp += c->hp - a->hp;
        h.ap += c->ap - a->ap;
        h.shield += c->shield - a->shield;
        h.break_bar += c->break_bar - a->break_bar;
        if (c->dead != a->dead) h.dead = c->dead;
        // Bonus : ce qui a changé ici pendant l'attente (tours décomptés, cumuls, ajouts, retraits) est reporté sur ceux de
        // l'hôte. (Avant : bonus laissés tels quels dès qu'ils avaient bougé -> écart jamais corrigé, Verso mort ici
        // seulement au tour 21 de p3_party_client, 2026-10-21.) Bonus de même classe appariés dans l'ordre.
        {
            auto nth = [](const std::vector<proto::BuffState>& v, const std::string& cls, int k) -> const proto::BuffState* {
                for (auto& x : v) if (x.cls == cls && k-- == 0) return &x;
                return nullptr;
            };
            std::vector<proto::BuffState> merged;
            std::map<std::string, int> seen_h;
            for (const auto& hb : h.buffs) {
                const int k = seen_h[hb.cls]++;
                const proto::BuffState* sa = nth(a->buffs, hb.cls, k);
                const proto::BuffState* sc = nth(c->buffs, hb.cls, k);
                if (sa && !sc) continue;   // retiré ici pendant l'attente
                proto::BuffState m = hb;
                if (sa && sc) { m.stacks += sc->stacks - sa->stacks; m.turns += sc->turns - sa->turns; }
                merged.push_back(m);
            }
            std::map<std::string, int> count_a, count_c, count_h;
            for (auto& x : a->buffs) ++count_a[x.cls];
            for (auto& x : h.buffs) ++count_h[x.cls];
            for (auto& x : c->buffs) {   // ajoutés ici pendant l'attente
                const int k = count_c[x.cls]++;
                if (k >= count_a[x.cls] && k >= count_h[x.cls]) merged.push_back(x);
            }
            h.buffs = std::move(merged);
        }
        // Mécanique propre : écarts d'ici reportés champ par champ.
        for (auto& f : h.mech) {
            const proto::MechField* fa = nullptr;
            const proto::MechField* fc = nullptr;
            for (auto& x : a->mech) if (x.name == f.name) { fa = &x; break; }
            for (auto& x : c->mech) if (x.name == f.name) { fc = &x; break; }
            if (fa && fc) f.value += fc->value - fa->value;
        }
        // Zones de visée libre (D37) : zone changée ici pendant l'attente (tir d'ici) -> état d'ici.
        if (a->parts.size() == c->parts.size() && h.parts.size() == c->parts.size())
            for (size_t i = 0; i < h.parts.size(); ++i)
                if (a->parts[i].present != c->parts[i].present || a->parts[i].coll != c->parts[i].coll) h.parts[i] = c->parts[i];
    }
    out.gradient_charges += now.gradient_charges - start.gradient_charges;
    out.gradient_progress += now.gradient_progress - start.gradient_progress;
    return out;
}

void client_turn_finish(UObject* actor)
{
    auto& b = s.battle;
    const bool deferred = !b.dep_start.units.empty() && b.dep_start.turn == b.turn;
    // L'hôte rythme les tours : attendre son début de tour (TurnStart, StateHash, StateSync, dans cet ordre).
    // Fin de combat de l'hôte arrivée pendant l'attente : aucun état ne viendra pour ce tour.
    if (!wait_for("turn_state", [&] { return b.host_sync.count(b.turn) > 0 || b.host_end >= 0; }, 15.0)) {
        if (s.ended) return;   // l'hôte est parti pendant l'attente : ce n'est pas un tour manqué
        // Jamais une fin de session : le lien est surveillé à part (silence de 15 s, D14). L'hôte reste maître du rythme ;
        // l'écart éventuel est corrigé par son état au tour suivant, ou par la fin imposée si son BattleEnd arrive.
        bump("turn_wait_timeouts");
        ++b.turn_wait_timeouts;
        const double heard = s.peers.empty() || !s.peers[0].link ? -1 : s.peers[0].link->last_heard();
        log().write("coop_turn_wait_timeout", JsonObj().add("battle", b.id).add("count", b.turn_wait_timeouts)
                                                  .add("host_silent_s", heard >= 0 ? s.now - heard : -1.0), b.turn);
        return;
    }
    if (!b.host_sync.count(b.turn)) {
        bump("turns_after_host_end");
        b.force_now = true;
        log().write("coop_turn_after_host_end", JsonObj().add("battle", b.id).add("host_end", b.host_end).add("actor", game::char_class(actor)), b.turn);
        return;
    }
    b.turn_wait_timeouts = 0;
    const proto::Unit au = actor_unit(actor);
    turn_indicator(au);
    if (auto it = b.host_turns.find(b.turn); it != b.host_turns.end() && (it->second.kind != au.kind || it->second.index != au.spot)) {
        bump("actor_mismatches");
        s.battle.strict = true;   // essai -e33turndep : on revient à l'attente bloquante pour la suite du combat
        log().write("coop_actor_mismatch", JsonObj().add("local", game::char_class(actor)).add("host", it->second.actor), b.turn);
    }
    // Les tirages de l'hôte pour le tour précédent sont arrivés avant son TurnStart (lien ordonné).
    if (b.turn > 1) compare_draws(b.turn - 1);
    else if (b.turn == 1) compare_setup_draws();
    const proto::StateSync& host = b.host_sync[b.turn];
    // Comparaison au même instant que l'hôte (début du tour) : photo prise alors si la vérification est différée.
    const proto::StateSync mine = deferred ? b.dep_start : sync::snapshot(b.id, b.turn);
    if (b.turn == 1) check_start_mech(mine, host);
    // Hash annoncé (StateHash) : sans les zones de visée libre, comparable avec tout pair. Contrôle et réparation : avec
    // elles quand les deux états les portent (D37).
    const bool wp = mine.parts_known && host.parts_known;
    uint64_t h = sync::hash(mine, au);
    const uint64_t host_hash = sync::hash(host, au);
    proto::StateSync hashed = mine;   // état dont h est le hash (journal desync)
    if (sync::hash(mine, au, wp) != sync::hash(host, au, wp)) {
        // La simulation locale a divergé : log détaillé, puis resynchronisation depuis l'état de l'hôte.
        bump("raw_hash_diffs");
        if (mech_differs(mine, host)) bump("raw_mech_diffs");   // écart sur une mécanique propre (roue, taches…, D30)
        if (sync::parts_differ(mine, host)) bump("raw_parts_diffs");   // zones de visée libre (D37)
        const proto::StateSync now = deferred ? sync::snapshot(b.id, b.turn) : mine;
        const proto::StateSync target = deferred ? shift_host_state(host, mine, now) : host;
        // Les buffs remis par la réparation passent par ActivateBuff, qui tire au hasard : sans garde, ces tirages
        // prenaient les rangs du tour et décalaient ceux de la suite (essai du 2026-10-25, combat 1, tour 3).
        s.repairing = true;
        const sync::RepairStats r = sync::repair(now, target);
        s.repairing = false;
        bump("repairs");
        bump("repair_failures", r.failed);
        bump("repaired_parts", r.parts);   // zones de visée libre remises (D37)
        const proto::StateSync after = sync::snapshot(b.id, b.turn);
        // Différé : réussi si l'état actuel égale l'état de l'hôte décalé des effets d'ici ; on annonce alors le hash de
        // l'hôte (même début de tour des deux côtés).
        const bool fixed = sync::hash(after, au, wp) == sync::hash(target, au, wp);
        h = deferred ? (fixed ? host_hash : sync::hash(after, au)) : sync::hash(after, au);
        hashed = deferred && fixed ? host : after;
        log().write("coop_resync", JsonObj().raw("diff", sync::diff(mine, host)).raw("repaired", sync::repair_json(r)).add("deferred", deferred)
                                       .add("fixed", fixed).raw("remaining", fixed ? "[]" : sync::diff(after, target)), b.turn);
    }
    b.dep_start = {};
    b.local_hash[b.turn] = h;
    b.hashed_state[b.turn] = std::move(hashed);
    for (auto& p : s.peers) send_to(p, proto::StateHash{b.id, b.turn, h});
    check_hash(b.turn);
    b.host_sync.erase(b.turn);
    if (s.perturb_turn && *s.perturb_turn == b.turn) perturb_state();
    if (s.perturb_end_turn && *s.perturb_end_turn == b.turn) perturb_end();
}

// Contextes Enhanced Input coupés pendant le tour du héros d'un autre joueur. SetBattleInputEnabled(false) ne
// suffit pas : la roue reste active (RT / gradient ouvrait le menu de Verso, vu le 2026-10-20). Le jeu les remet
// lui-même au début de chaque tour d'un héros d'ici (tour, roue, ciblage) ou de chaque compétence / visée (QTE,
// visée libre). Pas IMC_Battle_PauseMenu (permanent) ni IMC_BattleDefense / InputBlocker (liés une fois, puis gardés).
constexpr const wchar_t* kBlockedContexts[] = {
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_PlayerTurn.IMC_Battle_PlayerTurn",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_MainWheel.IMC_Battle_MainWheel",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_Targeting.IMC_Battle_Targeting",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_QTE.IMC_Battle_QTE",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_FreeAim.IMC_Battle_FreeAim",
};
// Ceux que le jeu remet au début du tour d'un héros d'ici (rendus par le mod s'ils manquent encore).

UObject* blocked_context(size_t i)
{
    static UObject* cache[std::size(kBlockedContexts)] = {};
    if (!cache[i]) cache[i] = ue::find_object(kBlockedContexts[i]);
    return cache[i];
}

std::optional<int> context_priority(UObject* imc)
{
    ue::Call has(s.input_sub, STR("HasMappingContext"));
    if (!has) return std::nullopt;
    has.set_object(STR("MappingContext"), imc).invoke();
    if (!has.out_bool(STR("ReturnValue"))) return std::nullopt;
    return static_cast<int>(has.out_int(STR("OutFoundPriority")));
}

// À chaque image pendant le blocage : retire ceux que le jeu a (re)mis.
void enforce_input_block()
{
    if (!s.input_sub) return;
    for (size_t i = 0; i < std::size(kBlockedContexts); ++i) {
        UObject* imc = blocked_context(i);
        if (!imc) continue;
        const auto prio = context_priority(imc);
        if (!prio) continue;
        ue::Call rm(s.input_sub, STR("RemoveMappingContext"));
        if (!rm) return;
        rm.set_object(STR("MappingContext"), imc).invoke();
        if (!s.removed_contexts.count(imc)) s.removed_contexts[imc] = *prio;
        bump("input_contexts_removed");
        log().write("coop_input_context", JsonObj().add("removed", ue::to_utf8(imc->GetName())).add("priority", *prio), s.battle.turn);
    }
}

// Fin du blocage au début du tour d'un héros d'ici : le jeu a pu remettre tour/roue/ciblage juste avant (retirés
// alors par le blocage) ; on rend ceux qui manquent, avec leur priorité.
void restore_turn_contexts()
{
    if (!s.input_sub) return;
    for (size_t i = 0; i < kTurnContexts; ++i) {
        UObject* imc = blocked_context(i);
        auto it = imc ? s.removed_contexts.find(imc) : s.removed_contexts.end();
        if (it == s.removed_contexts.end() || context_priority(imc)) continue;
        ue::Call add(s.input_sub, STR("AddMappingContext"));
        if (!add) return;
        add.set_object(STR("MappingContext"), imc).set_int(STR("Priority"), it->second).invoke();
        bump("input_contexts_restored");
        log().write("coop_input_context", JsonObj().add("restored", ue::to_utf8(imc->GetName())).add("priority", it->second), s.battle.turn);
    }
}

// Entrées de combat de cette instance : aucune pendant le tour du héros d'un autre joueur (roue, gradient, ciblage,
// QTE, visée). local_turn : la fin du blocage ouvre le tour d'un héros d'ici.
void set_local_input(bool enabled, const char* why, bool local_turn)
{
    if (enabled == s.input_enabled) return;
    s.input_enabled = enabled;
    ue::Call c(game::battle_manager(), STR("SetBattleInputEnabled"));
    if (c) c.set_bool(STR("Enabled"), enabled).invoke();
    if (!enabled) {
        s.input_sub = game::input_subsystem();
        s.removed_contexts.clear();
        enforce_input_block();
    } else {
        if (local_turn) restore_turn_contexts();
        s.removed_contexts.clear();
    }
    log().write("coop_input", JsonObj().add("enabled", enabled).add("why", why).add("subsystem", s.input_sub != nullptr), s.battle.turn);
}

// --- Démarrage synchronisé des tours ennemis (hôte, D28) -----------------------------------------------------
// L'hôte est en avance sur ses clients : chaque client attend son début de tour, et rejoue les actions des héros de
// l'hôte avec un décalage (replay_lag_s, action_delay_s). Au tour ennemi qui suit, l'hôte frappe donc les héros d'un
// client avant lui, et fige son image (defense_point) jusqu'à ce que le résultat de ce client arrive : avance + aller-
// retour, ~1 s par coup en vrai (essai à deux PC du 2026-10-24, 3 s au pire). Démarrage synchronisé (D28, par défaut,
// -e33turnalign=0 pour le couper) : l'hôte retient le tour ennemi (dépendance de début de tour : le jeu anime, rien
// n'est figé) jusqu'à ce que chaque client qui a un héros vivant l'ait commencé chez lui (son StateHash du tour, ou un
// autre signe de ce tour), 4 s de jeu au plus (pauses non comptées). Le client est alors en avance d'un aller simple :
// l'hôte n'attend presque plus ses résultats, et le client attend ceux des héros de l'hôte un aller-retour au plus.
// Aucune action automatique, même résultat partout (D11). Pas attendus : joueur bloqué (unresponsive), parti, pas dans
// la partie (chargement, menu principal), sans héros vivant ; ni pendant un contre, ni en fin de combat.

// Clients de la partie qui ont un héros vivant dans ce combat et n'ont pas encore commencé ce tour (StateHash).
std::vector<uint8_t> align_missing(uint32_t turn)
{
    auto& b = s.battle;
    std::vector<uint8_t> out;
    for (auto& p : s.peers) {
        if (!in_game(p) || b.unresponsive.count(p.player_id) || (b.acks.count(turn) && b.acks[turn].count(p.player_id))) continue;
        bool hero = false;
        for (auto& [spot, owner] : b.owner_by_spot) {
            if (owner != p.player_id) continue;
            UObject* h = game::find_unit(true, spot);
            if (h && !game::is_dead_fast(h)) hero = true;
        }
        if (hero) out.push_back(p.player_id);
    }
    return out;
}

// Hôte : retient le tour ennemi `turn` (déjà commencé ici : son état est parti) tant qu'un client attendu ne l'a pas
// commencé chez lui. Inscription d'une dépendance de début de tour, résolue par align_tick.
void align_begin(uint32_t turn, const char* how)
{
    auto& b = s.battle;
    if (s.role != Role::Host || !s.turn_align || !b.active || b.ending || b.hold || b.align_active || turn <= 1) return;
    if (b.align_turn == turn) return;   // une seule retenue par tour (délai écoulé : on ne recommence pas)
    if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
    const std::vector<uint8_t> missing = align_missing(turn);
    if (missing.empty()) return;
    UObject* deps = turn_deps();
    ue::Call reg(deps, STR("Register Dependency"));
    if (!reg) return;
    reg.set_object(STR("Dependency"), game::save_manager()).invoke();
    b.align_active = true;
    b.align_turn = turn;
    b.align_since = s.now;
    b.align_paused_s = 0;
    bump("turn_aligns");
    std::string w = "[";
    for (uint8_t p : missing) w += (w.size() > 1 ? "," : "") + std::to_string(p);
    log().write("coop_turn_align", JsonObj().add("turn", turn).add("how", how).add("listening", ue::get_bool(deps, STR("IsListening")))
                                       .raw("waiting_for", w + "]"), turn);
}

void align_tick()
{
    auto& b = s.battle;
    static double last = 0;
    const double dt = s.now - last;
    last = s.now;
    if (!b.align_active) return;
    // Jeu en pause (pause d'un joueur, menu pause) : ce temps ne compte pas dans le délai (rien n'avance de toute façon).
    if (explore::remote_paused() || game::pause_menu_visible()) b.align_paused_s += std::max(0.0, std::min(dt, 1.0));
    const bool done = !b.active || b.ending || align_missing(b.align_turn).empty();
    const bool expired = s.now - b.align_since - b.align_paused_s > kAlignTimeout;
    if (!done && !expired) return;
    b.align_active = false;
    // Diagnostic : le jeu écoutait-il nos dépendances (sinon la retenue n'a rien retenu) ?
    const bool listening = ue::get_bool(turn_deps(), STR("IsListening"));
    const bool triggered = ue::get_bool(turn_deps(), STR("HasTriggered"));
    if (!listening) bump("turn_align_not_listening");
    ue::Call solve(turn_deps(), STR("SolveDependency"));
    if (solve) solve.set_object(STR("Dependency"), game::save_manager()).invoke();
    if (listening) b.play_since = s.now;   // tour vraiment retenu : il se joue à partir d'ici (DefenseRequest.turn_ms)
    const int ms = static_cast<int>((s.now - b.align_since) * 1000.0);
    bump_max("turn_align_max_ms", ms);
    if (!done) bump("turn_align_timeouts");
    std::string missing = "[";
    if (!done) for (uint8_t p : align_missing(b.align_turn)) missing += (missing.size() > 1 ? "," : "") + std::to_string(p);
    log().write("coop_turn_aligned", JsonObj().add("turn", b.align_turn).add("waited_ms", ms).add("timeout", !done)
                                         .add("paused_ms", static_cast<int>(b.align_paused_s * 1000.0)).raw("missing", missing + "]")
                                         .add("listening", listening).add("triggered", triggered), b.align_turn);
}

// Parade en duo : pendant le tour qui finit, des héros d'au moins deux joueurs ont été touchés (attaque de groupe ou
// coups enchaînés sur plusieurs héros) et tous les coups ont été parés -> +kDuoParryAP à chacun de ces héros.
// Calculé sur chaque instance avec les mêmes données (résultat du propriétaire de chaque défense, propriétaires du
// combat), au même point du jeu (début du tour suivant, avant la photo d'état) : aucun message, aucun écart de hash.
void duo_parry_check()
{
    auto& b = s.battle;
    std::vector<std::pair<int, proto::Defense>> hits;
    hits.swap(b.turn_defenses);
    if (!b.duo_parry || hits.empty()) return;
    std::map<int, bool> parried;   // emplacement -> tous ses coups parés
    std::set<uint8_t> owners;
    for (const auto& [spot, d] : hits) {
        const bool ok = d == proto::Defense::Parry || d == proto::Defense::GradientParry;
        auto [it, fresh] = parried.emplace(spot, ok);
        if (!fresh) it->second = it->second && ok;
        owners.insert(owner_of_spot(spot));
    }
    if (owners.size() < 2) return;
    for (const auto& [spot, ok] : parried) {
        if (ok) continue;
        bump("duo_parries_missed");
        log().write("coop_duo_parry_missed", JsonObj().add("spot", spot).add("hits", static_cast<int64_t>(hits.size())), b.turn);
        return;
    }
    std::string names, detail = "[";
    int gained = 0;
    for (const auto& [spot, ok] : parried) {
        UObject* hero = game::find_unit(true, spot);
        if (!hero || game::is_dead_fast(hero)) continue;
        UObject* st = game::stats(hero);
        const double before = ue::get_double(st, STR("CurrentAP"));
        // GainAP du jeu : plafonné aux PA max (SetAP), effet visuel de gain de PA ; SetAP à défaut.
        ue::Call gain(st, STR("GainAP"));
        if (gain) gain.set_int(STR("Amount"), kDuoParryAP).set_int(STR("ChangeReason"), kDuoParryReason).invoke();
        else {
            ue::Call set(st, STR("SetAP"));
            if (set) set.set_int(STR("NewAP"), std::llround(before) + kDuoParryAP).set_int(STR("ChangeReason"), kDuoParryReason).invoke();
        }
        const double after = ue::get_double(st, STR("CurrentAP"));
        gained += static_cast<int>(std::llround(after - before));
        const std::string name = game::hardcoded_name(hero);
        names += (names.empty() ? "" : ", ") + name;
        if (detail.size() > 1) detail += ",";
        detail += JsonObj().add("hero", name).add("spot", spot).add("ap_before", before).add("ap_after", after).add("via_gain", static_cast<bool>(gain)).str();
    }
    if (names.empty()) return;
    bump("duo_parries");
    bump("duo_parry_ap", gained);
    ui::toast("duo_parry", {names, std::to_string(kDuoParryAP)}, 4.0);
    log().write("coop_duo_parry", JsonObj().add("players", static_cast<int64_t>(owners.size())).add("hits", static_cast<int64_t>(hits.size()))
                                    .raw("heroes", detail + "]"), b.turn);
}

// Début de tour d'un personnage (OnTurnStart, ou rattrapage quand le combat local a démarré avant le BattleStart).
void on_turn_start(UObject* actor)
{
    // Client, combat pas encore suivi par le mod (engagé ici avant le BattleStart) : photo de l'état à cet instant, celui
    // de la photo de l'hôte. Le rattrapage peut arriver des secondes plus tard (monde prêt depuis 5 s) : entre-temps le
    // jeu a déjà appliqué les effets du début de tour (PA du tour : +1 PA à Gustave dans le duel du prologue, 2026-10-23),
    // et la comparaison d'une photo tardive « corrigeait » ce gain, puis l'écart revenait au tour suivant.
    if (s.role == Role::Client && !s.battle.active) {
        s.early_turn = sync::snapshot(0, 1);
        s.early_turn_actor = actor;
        s.early_turn_at = s.now;
    }
    // Fin de l'action du tour qui s'achève (D36) : coups reçus ici par nos héros, publiés avant l'attente éventuelle du
    // début de tour de l'hôte (client). Repli : en général déjà publiée au StartCharacterTurn (coop_hooks).
    enemy_action_done("turn_start");
    s.enemy_turn = actor && !game::is_player(actor);
    if (s.role != Role::Solo && s.battle.active) duo_parry_check();
    // Commandes de défense (parade, esquive, saut) : branchées par le jeu quand il lance lui-même le combat, pas
    // quand le mod le lance (client : TriggerBattle). Essai à deux PC du 2026-10-24 : aucun appui défensif du client
    // n'atteignait le jeu (TryStart* jamais appelés), alors qu'en solo sa parade marche.
    if (s.role != Role::Solo && s.battle.active) {
        UObject* ctrl = game::controller();
        if (ctrl && !ue::get_bool(ctrl, STR("IsDefenseInputBound"))) {
            ue::Call c(ctrl, STR("TryBindDefendInput"));
            const bool called = c && c.invoke();
            const bool bound = ue::get_bool(ctrl, STR("IsDefenseInputBound"));
            bump(bound ? "defense_input_bound" : "defense_input_bind_failed");
            log().write("coop_defense_input_bind", JsonObj().add("called", called).add("bound", bound), s.battle.turn);
        }
        // Diagnostic (2026-10-24 : parade du client toujours sans effet) : contextes d'entrée de combat actifs au début
        // de chaque tour ennemi, pour comparer avec l'hôte et le solo.
        if (actor && !game::is_player(actor) && s.imc_logs < 40) {
            ++s.imc_logs;
            UObject* sub = game::input_subsystem();
            std::string act;
            for (const wchar_t* n : {L"IMC_BattleDefense", L"IMC_Battle_InputBlocker", L"IMC_Battle_PlayerTurn", L"IMC_Battle_MainWheel",
                                     L"IMC_Battle_Targeting", L"IMC_Battle_QTE", L"IMC_Battle_FreeAim", L"IMC_Battle_PauseMenu"}) {
                UObject* imc = ue::find_object((std::wstring(L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/") + n + L"." + n).c_str());
                ue::Call has(sub, STR("HasMappingContext"));
                if (!imc || !has) continue;
                has.set_object(STR("MappingContext"), imc).invoke();
                if (!has.out_bool(STR("ReturnValue"))) continue;
                if (!act.empty()) act += ",";
                act += ue::to_utf8(n) + ":" + std::to_string(has.out_int(STR("OutFoundPriority")));
            }
            log().write("coop_enemy_turn_inputs", JsonObj().add("contexts", act).add("bound", ctrl ? ue::get_bool(ctrl, STR("IsDefenseInputBound")) : false)
                                                    .add("input_enabled", s.input_enabled), s.battle.turn);
        }
    }
    if (s.role == Role::Host) host_turn_start(actor);
    else if (s.role == Role::Client && s.battle.active) client_turn_start(actor);
    // Tour du héros d'un autre joueur : pas de choix d'action ici (son action arrive par le réseau).
    if (s.role != Role::Solo && s.battle.active && s.lock_input) {
        const bool hero = game::is_player(actor);
        const bool mine = hero && controls(game::spot(actor));
        // Combat fini chez l'hôte : plus d'action ici, la fin est imposée (client_force_end_tick).
        const bool over = s.role == Role::Client && s.battle.host_end >= 0;
        set_local_input((!hero || mine) && !over, over ? "host battle over" : "turn start", mine && !over);
    }
}

// Client : équipe de l'hôte imposée avant un combat (le joueur de l'hôte a pu la changer depuis l'instantané).
void apply_host_party(const proto::BattleStart& bs)
{
    if (bs.party.empty() || game::party_names() == bs.party) return;
    std::string err, mine;
    for (auto& n : game::party_names()) mine += (mine.empty() ? "" : ",") + n;
    std::string host;
    for (auto& n : bs.party) host += (host.empty() ? "" : ",") + n;
    const bool ok = game::set_party_names(bs.party, err);
    const bool same = ok && game::party_names() == bs.party;
    bump(same ? "parties_applied" : "party_apply_failures");
    log().write("coop_party_applied", JsonObj().add("local", mine).add("host", host).add("ok", same).add("err", err));
}

} // namespace e33::coop::detail
