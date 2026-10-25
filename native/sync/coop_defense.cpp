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
    auto owner_ahead = [&] {
        if (s.role == Role::Host) { auto it = b.acks.upper_bound(turn_now); for (; it != b.acks.end(); ++it) if (it->second.count(owner)) return true; return false; }
        return b.host_sync.upper_bound(turn_now) != b.host_sync.end();
    };
    // Résultat déjà reçu : pris même si le propriétaire est déjà plus loin (wait_for teste la condition d'abord).
    const bool got = !unresponsive &&
                     wait_for("defense", [&] { return b.defenses.count(key) > 0 || owner_of_spot(spot) != owner || (s.role == Role::Client && b.host_end >= 0) || owner_ahead(); },
                              kDefenseTimeout) &&
                     b.defenses.count(key) > 0;
    const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
    bump_max("defense_wait_max_ms", static_cast<int>(waited_ms));
    if (waited_ms >= 250) bump("defense_waits_long");   // même seuil que coop_wait_long (site defense)
    proto::Defense res = proto::Defense::Fail;
    const char* why = "received";
    if (got) res = b.defenses[key];
    else if (unresponsive) { why = "owner_unresponsive"; bump("defense_waits_skipped"); }
    else if (owner_of_spot(spot) != owner) { why = "owner_left"; bump("defense_waits_owner_left"); }
    else if (s.role == Role::Client && b.host_end >= 0) { why = "host_battle_over"; bump("defense_waits_battle_over"); }
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
                                         .add("result", static_cast<int>(res)).add("received", got).add("waited_ms", waited_ms).add("why", why), b.turn);
}

} // namespace e33::coop::detail
