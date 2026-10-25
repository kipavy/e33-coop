// Défenses : résultat publié par le propriétaire, imposé ailleurs (D11, D22, D32).
#include "coop_internal.hpp"

namespace e33::coop::detail {

// --- Hooks ---------------------------------------------------------------------------------

UObject* timeline_hero(UObject* timeline)
{
    UObject* ch = ue::get_object(timeline, STR("BattleCharacter"));
    return ch && game::is_player(ch) ? ch : nullptr;
}

// Impose sur ce héros la défense décidée par son propriétaire, juste avant le test du jeu.
bool impose_defense(UObject* hero, proto::Defense d, bool jumpable)
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
    return started;
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
        // Parade appuyée pendant une attente figée juste avant (attaque de zone) : lancée maintenant, avant le test du jeu.
        if (b.wait_parry_at && !jumpable && GetTickCount64() - b.wait_parry_at <= 300) {
            game::clear_defense_lock(hero);
            const bool started = game::start_parry(hero);
            bump(started ? "parries_from_wait" : "parries_from_wait_failed");
            log().write("coop_parry_from_wait", JsonObj().add("spot", spot).add("hit", static_cast<int>(hit)).add("started", started)
                                                   .add("ago_ms", static_cast<int64_t>(GetTickCount64() - b.wait_parry_at)), b.turn);
        }
        return;
    }
    const uint8_t owner = owner_of_spot(spot);
    if (s.role == Role::Host) {
        // turn_ms : temps de jeu du tour ici au moment du coup (le faux client --turn-lag s'en sert pour frapper au même
        // moment de son propre tour ; le vrai client l'ignore).
        const double since = std::max(0.0, s.now - b.play_since) * 1000.0;
        const uint16_t turn_ms = static_cast<uint16_t>(std::min(since, 65000.0));
        for (auto& p : s.peers)
            if (p.accepted && p.player_id == owner)
                send_to(p, proto::DefenseRequest{b.id, b.turn, static_cast<uint8_t>(spot), hit, static_cast<uint8_t>(jumpable), 3000, turn_ms});
    }
    const TurnSpotSeq key{b.turn, spot, hit};
    const ULONGLONG t0 = GetTickCount64();
    // Attente bloquante (image figée) du résultat du propriétaire, bornée à kDefenseTimeout. Sortie sans attendre la fin
    // du délai : propriétaire parti (l'hôte reprend le héros), combat déjà fini chez l'hôte (client : aucun résultat ne
    // viendra), ou propriétaire qui ne répond plus dans ce combat (hôte : tour bloqué chez lui, attaque imposée).
    const bool unresponsive = s.role == Role::Host && b.unresponsive.count(owner) > 0;
    // Propriétaire déjà à un tour suivant chez lui (hôte : un message de lui pour un tour plus loin ; client : état de
    // l'hôte du tour suivant) : ce coup n'existe pas chez lui, aucun résultat ne viendra. Vu le 2026-10-25 (tour 9) :
    // attaque ennemie à 5 coups chez l'hôte, 3 chez le client déjà dans son menu du tour 10 -> deux gels de 3 s.
    const uint32_t turn_now = b.turn;
    // Action finie chez le propriétaire avec moins de coups sur ce héros (EnemyActionDone, D36) : ce coup n'existe pas chez
    // lui. Vu le 2026-10-25 (combat 9, tour 15) : propriétaire encore dans le tour, deux gels de 3 s chez l'hôte.
    auto owner_done = [&]() -> int {   // coups du propriétaire sur ce héros si son action est finie avec moins de coups, sinon -1
        if (!s.enemy_done) return -1;
        auto it = b.owner_done.find({turn_now, spot});
        if (it == b.owner_done.end() || it->second.hits >= hit) return -1;
        const proto::Unit& a = it->second.actor;
        const proto::Unit& me = b.turn_actor;
        if (a.spot != proto::kAutoSpot && me.spot != proto::kAutoSpot && (a.kind != me.kind || a.spot != me.spot)) return -1;   // autre tour chez lui
        return it->second.hits;
    };
    auto owner_ahead = [&] {
        if (s.role == Role::Host) { auto it = b.acks.upper_bound(turn_now); for (; it != b.acks.end(); ++it) if (it->second.count(owner)) return true; return false; }
        return b.host_sync.upper_bound(turn_now) != b.host_sync.end();
    };
    // Résultat déjà reçu : pris même si le propriétaire est déjà plus loin (wait_for teste la condition d'abord).
    const bool got = !unresponsive &&
                     wait_for("defense", [&] { return b.defenses.count(key) > 0 || owner_of_spot(spot) != owner || (s.role == Role::Client && b.host_end >= 0) || owner_done() >= 0 || owner_ahead(); },
                              kDefenseTimeout) &&
                     b.defenses.count(key) > 0;
    const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
    bump_max("defense_wait_max_ms", static_cast<int>(waited_ms));
    if (waited_ms >= 250) bump("defense_waits_long");   // même seuil que coop_wait_long (site defense)
    proto::Defense res = proto::Defense::Fail;
    const char* why = "received";
    int owner_hits = -1;
    if (got) res = b.defenses[key];
    else if (unresponsive) { why = "owner_unresponsive"; bump("defense_waits_skipped"); }
    else if (owner_of_spot(spot) != owner) { why = "owner_left"; bump("defense_waits_owner_left"); }
    else if (s.role == Role::Client && b.host_end >= 0) { why = "host_battle_over"; bump("defense_waits_battle_over"); }
    else if ((owner_hits = owner_done()) >= 0) {
        // Action finie chez le propriétaire avec moins de coups : esquivé ici, comme owner_ahead (l'état de l'hôte fait foi).
        why = "owner_done";
        res = jumpable ? proto::Defense::Jump : proto::Defense::Dodge;
        bump("defense_waits_owner_done");
    }
    else if (owner_ahead()) {
        // Coup absent chez le propriétaire : esquivé ici (pas de dégâts qu'il n'a pas reçus ; l'état de l'hôte fait foi).
        why = "owner_ahead";
        res = jumpable ? proto::Defense::Jump : proto::Defense::Dodge;
        bump("defense_waits_owner_ahead");
    }
    else {
        why = "timeout";
        bump("defense_timeouts");
        // Hôte : propriétaire qui n'a même pas commencé ce tour chez lui (aucun StateHash) : il est bloqué ou très en
        // retard. Ses prochains coups ne figeront plus l'image 3 s chacun (échec imposé, l'état de l'hôte fait foi) ; il
        // redevient attendu dès son prochain StateHash.
        if (s.role == Role::Host && !(b.acks.count(b.turn) && b.acks[b.turn].count(owner)) && b.unresponsive.insert(owner).second) {
            bump("players_unresponsive");
            log().write("coop_player_unresponsive", JsonObj().add("player", static_cast<int>(owner)).add("why", "defense timeout, turn not started there"), b.turn);
        }
    }
    const bool started = impose_defense(hero, res, jumpable);
    b.imposed_check[stats_comp] = static_cast<int>(res);
    b.turn_defenses.push_back({spot, res});
    bump("defenses_imposed");
    log().write("coop_defense_imposed", JsonObj().add("spot", spot).add("hit", static_cast<int>(hit)).add("owner", static_cast<int>(owner)).add("jumpable", jumpable)
                                         .add("started", started)
                                         .add("result", static_cast<int>(res)).add("received", got).add("waited_ms", waited_ms).add("why", why)
                                         .add("owner_hits", owner_hits), b.turn);
}

// Fin de l'action du tour en cours ici (début du tour suivant, ou fin du combat) : chaque instance publie le nombre de
// coups reçus par les héros qu'elle contrôle (D36). Une fois par tour ; seulement si c'était un tour ennemi ou si un héros
// a été touché. why : point de détection (journal).
void enemy_action_done(const char* why)
{
    auto& b = s.battle;
    if (!s.enemy_done || s.role == Role::Solo || !b.active || b.turn == 0 || b.done_sent_turn == b.turn) return;
    if (!s.enemy_turn && b.hit_seq.empty()) return;
    b.done_sent_turn = b.turn;
    proto::EnemyActionDone d{b.id, b.turn, b.turn_actor, s.my_player_id, {}};
    std::string hs = "{";
    for (const auto& [spot, owner] : b.owner_by_spot) {
        if (owner != s.my_player_id || d.hits.size() >= 8) continue;
        auto it = b.hit_seq.find(spot);
        const uint16_t n = it == b.hit_seq.end() ? 0 : it->second;
        d.hits.push_back({static_cast<uint8_t>(spot), n});
        hs += (hs.size() > 1 ? "," : "") + ("\"" + std::to_string(spot) + "\":" + std::to_string(n));
    }
    if (d.hits.empty()) return;   // aucun héros d'ici dans ce combat
    publish(d);
    bump("enemy_action_done_sent");
    log().write("coop_enemy_action_done", JsonObj().add("dir", "sent").add("turn", b.turn).add("actor_kind", static_cast<int>(b.turn_actor.kind))
                                             .add("actor_spot", static_cast<int>(b.turn_actor.spot)).raw("hits", hs + "}").add("why", why), b.turn);
}

// Fin d'action reçue (propriétaire, ou relayée par l'hôte) : gardée pour les attentes de défense de ce tour (bornée).
void on_enemy_action_done(Peer& p, const proto::EnemyActionDone& d)
{
    auto& b = s.battle;
    if (d.battle_id != b.id || !s.enemy_done) return;
    if (s.role == Role::Host && d.player != p.player_id) return;
    std::string hs = "{";
    for (const auto& h : d.hits) {
        if (owner_of_spot(h.spot) != d.player) continue;   // seul le propriétaire du héros compte
        b.owner_done[{d.turn, h.spot}] = {h.hits, d.actor};
        hs += (hs.size() > 1 ? "," : "") + ("\"" + std::to_string(h.spot) + "\":" + std::to_string(h.hits));
    }
    while (b.owner_done.size() > 64) b.owner_done.erase(b.owner_done.begin());
    // Même tour ici, autre combattant : numéros de tour décalés, la fin d'action ne sera pas utilisée (owner_done).
    const bool mismatch = d.turn == b.turn && d.actor.spot != proto::kAutoSpot && b.turn_actor.spot != proto::kAutoSpot &&
                          (d.actor.kind != b.turn_actor.kind || d.actor.spot != b.turn_actor.spot);
    if (mismatch) bump("enemy_action_done_actor_mismatches");
    bump("enemy_action_done_received");
    log().write("coop_enemy_action_done", JsonObj().add("dir", "recv").add("player", static_cast<int>(d.player)).add("turn", d.turn)
                                             .add("actor_kind", static_cast<int>(d.actor.kind)).add("actor_spot", static_cast<int>(d.actor.spot))
                                             .raw("hits", hs + "}").add("actor_mismatch", mismatch), b.turn);
    if (s.role == Role::Host) publish(d, &p);
}

} // namespace e33::coop::detail
