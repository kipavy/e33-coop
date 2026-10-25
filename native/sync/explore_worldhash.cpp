// Empreinte d'état d'exploration, copie coop, resynchro (D14, D15).
#include "explore_internal.hpp"

namespace e33::explore::detail {

// Inventaire résumé : quantité totale par objet et niveau de chaque équipement (comme les ressources communes, D13),
// trié par nom. Avant : réflexion brute de GI.Inventory (~11 700 feuilles : ordre des piles, données statiques
// recopiées, pointeurs d'objets) ; l'hôte en avait 11 741 et le client 11 342 juste après avoir chargé l'instantané de
// l'hôte (2026-10-22) : écart permanent, donc nouvel instantané (rechargement de 5-6 s) toutes les 1 à 2 min.
// Noms en minuscules : le jeu écrit parfois le même objet avec une casse différente selon le PC (« CritCHanceOnWeak »
// / « CritChanceOnWeak »). Tenues et coiffures (Skin*, Face*) ignorées : elles viennent des éditions / DLC du compte,
// pas de la partie (l'hôte en a, le client non : resynchros sans fin, essai à deux PC du 2026-10-23).
reflect::Leaves inventory_leaves()
{
    std::map<std::string, int64_t> eco;
    game::economy_sample(eco);
    std::map<std::string, int64_t> low;
    for (auto& [k, v] : eco) {
        if (k == "gold" || v == 0) continue;
        std::string n = k;
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n.rfind("item:skin", 0) == 0 || n.rfind("item:face", 0) == 0) continue;
        low[n] += v;
    }
    reflect::Leaves l;
    for (auto& [k, v] : low) l.emplace_back(k, std::to_string(v));
    return l;
}

// Objets utilisés sans les zones de passage ni les séquences de niveau : le pilote les déclenche en passant, le
// suiveur pas toujours (zones sans cinématique ni dialogue, non rejouées) ; écart permanent, donc une resynchro toutes
// les 10 à 20 s d'exploration (essai à deux PC du 2026-10-24). Le compte (« InteractedObjects#… ») est retiré aussi.
reflect::Leaves interacted_leaves(reflect::Leaves in)
{
    reflect::Leaves out;
    for (auto& kv : in) {
        const std::string& k = kv.first;
        if (k.find('#') != std::string::npos) continue;
        if (k.find("_BP_GameActionTriggerZone") != std::string::npos || k.find("ObjectID_LevelSequence_") != std::string::npos) continue;
        out.push_back(std::move(kv));
    }
    return out;
}

std::vector<proto::HashPart> world_parts(std::map<std::string, reflect::Leaves>* leaves)
{
    std::vector<proto::HashPart> out;
    UObject* gi = game::gi();
    if (!gi) return out;
    prof::Scope prof_scope("world_parts");
    s.last_world_compute = s.now;
    for (const auto& p : kWorldParts) {
        reflect::Leaves l = std::string_view(p.name) == "inventory2" ? inventory_leaves() : reflect::field(gi, p.field);
        if (std::string_view(p.name) == "interacted2") l = interacted_leaves(std::move(l));
        out.push_back({p.name, reflect::hash(l)});
        if (leaves) (*leaves)[p.name] = std::move(l);
    }
    return out;
}

// Inventaire résumé en une ligne de journal ("objet=quantité,…"), pour comparer hôte et client quand il diffère.
std::string leaves_line(const reflect::Leaves& l)
{
    std::string out;
    for (auto& [k, v] : l) { if (!out.empty()) out += ","; out += k + "=" + v; }
    return out;
}

void touch() { s.last_event_at = s.now; }

std::vector<std::string> split_csv(const std::string& csv)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= csv.size()) {
        const size_t b = std::min(csv.find(',', a), csv.size());
        if (b > a) out.push_back(csv.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

// Diagnostic des écarts d'exploration : toutes les feuilles d'une partie (au plus 3000), une ligne par partie.
void log_world_leaves(const std::string& part, const reflect::Leaves& l, uint32_t seq)
{
    std::string arr = "[";
    for (size_t i = 0; i < l.size() && i < 3000; ++i) { if (arr.size() > 1) arr += ","; arr += JsonObj().add("p", l[i].first).add("v", l[i].second).str(); }
    log().write("coop_world_leaves", JsonObj().add("part", part).add("seq", seq).add("count", static_cast<int64_t>(l.size())).raw("leaves", arr + "]"));
}

bool quiet(double secs)
{
    return safe_point() && s.interactions.empty() && s.choices.empty() && !s.choice_pending && s.now - s.last_event_at >= secs;
}

// Client : copie coop après une empreinte égale à celle de l'hôte, pion arrêté (au plus 60 s après la vérification, et
// seulement sans événement depuis : sinon l'état n'est plus celui vérifié).
void copy_tick()
{
    if (s.host || s.copy_ok_at < 0) return;
    if (s.last_event_at > s.copy_ok_at || s.now - s.copy_ok_at > 60.0) { s.copy_ok_at = -1; return; }
    if (local_moving() || !quiet(3.0) || !prof::claim_heavy("coop_copy")) return;
    s.copy_ok_at = -1;
    prof::Scope p("coop_copy");
    coopsave::on_world_ok(s.copy_ok_hash, s.net.name_of ? s.net.name_of(0) : std::string());
}

void world_hash_tick()
{
    if (game::in_battle() || game::map_change_in_progress() || s.local_narr) touch();
    copy_tick();
    if (s.now < s.next_world_check) return;
    // Lecture de l'état : seule tâche lourde de son image, et pas pendant que le pion court (sauf toutes les 30 s au
    // plus) : l'hôte la faisait toutes les 5 s en courant (courir compte comme « calme »). false : réessai bientôt.
    auto may_read = [] {
        if (local_moving() && s.now - s.last_world_compute < 30.0) { s.next_world_check = s.now + 0.5; return false; }
        if (!prof::claim_heavy("world_hash")) { s.next_world_check = s.now; return false; }   // image suivante
        return true;
    };
    s.next_world_check = s.now + (s.host ? 5.0 : 1.0);   // hôte : lecture de tout l'état, pas à chaque seconde
    if (s.host) {
        if (!sharing() || !quiet(8.0)) return;
        if (!may_read()) return;
        std::map<std::string, reflect::Leaves> leaves;
        auto parts = world_parts(&leaves);
        if (parts.empty()) return;
        uint64_t all = 0;
        for (auto& p : parts) all = all * 31 + p.value;
        if (all == s.last_world_sent && s.now - s.last_world_sent_at < 60.0) return;
        uint64_t prev_inventory = 0;
        for (auto& p : s.last_world_parts) if (p.name == "inventory2") prev_inventory = p.value;
        s.last_world_sent = all;
        s.last_world_sent_at = s.now;
        proto::WorldHash wh{++s.world_seq, game::current_level(), parts};
        s.net.publish(wh, true);
        bump("world_hashes_sent");
        std::string sizes = "{";
        for (auto& [k, l] : leaves) { if (sizes.size() > 1) sizes += ","; sizes += "\"" + k + "\":" + std::to_string(l.size()); }
        JsonObj o;
        o.raw("msg", proto::to_json(wh)).raw("leaves", sizes + "}");
        // Inventaire résumé quand il change (à comparer à celui d'un client en écart).
        for (auto& p : parts) if (p.name == "inventory2" && p.value != prev_inventory) o.add("inventory", leaves_line(leaves["inventory2"]));
        s.last_world_parts = parts;
        log().write("coop_world_hash", o);
        return;
    }
    // Client : comparer la dernière empreinte de l'hôte une fois calme ici aussi.
    if (!s.pending_world) return;
    if (s.cj != S::ClientJoin::Joined && s.want_join) { s.pending_world.reset(); return; }
    if (s.now - s.pending_world_at > 45.0) {
        bump("world_hash_skipped");
        log().write("coop_world_hash_skipped", JsonObj().add("seq", s.pending_world->seq).add("why", "never quiet"));
        s.pending_world.reset();
        return;
    }
    if (!quiet(3.0)) return;
    if (s.now - s.pending_world_at < 20.0) { if (!may_read()) return; }
    else (void)prof::claim_heavy("world_hash");   // empreinte reçue depuis 20 s : comparée même en courant
    const proto::WorldHash host = *s.pending_world;
    s.pending_world.reset();
    if (host.level != game::current_level()) { log().write("coop_world_hash_skipped", JsonObj().add("seq", host.seq).add("why", "level")); return; }
    std::map<std::string, reflect::Leaves> leaves;
    const auto mine = world_parts(&leaves);
    std::string differ;
    for (auto& hp : host.parts)
        for (auto& mp : mine)
            if (mp.name == hp.name && mp.value != hp.value) { if (!differ.empty()) differ += ","; differ += mp.name; }
    bump("world_hashes_compared");
    // Première comparaison après un instantané demandé (ResyncRequest) : une partie qui diffère encore juste après
    // avoir rechargé l'état exact de l'hôte, deux fois de suite, ne sera pas réparée par un autre instantané (écart
    // d'inventaire permanent après une nouvelle partie, 2026-10-22 : 5 rechargements en 10 min, chacun en plein jeu).
    // Elle reste journalisée, mais ne demande plus d'instantané.
    std::string ignored;
    if (s.resync_at >= 0 && s.cj_joined_at > s.resync_at) {
        const auto now_differ = split_csv(differ);
        for (const std::string& part : s.resync_parts) {
            if (std::find(now_differ.begin(), now_differ.end(), part) == now_differ.end()) { s.resync_failures.erase(part); continue; }
            const int n = ++s.resync_failures[part];
            log().write("coop_world_resync_failed", JsonObj().add("part", part).add("seq", host.seq).add("times", n));
            if (n >= 2 && s.world_ignored.insert(part).second) {
                bump("world_parts_unfixable");
                log().write("coop_world_part_unfixable", JsonObj().add("part", part).add("seq", host.seq));
            }
        }
        s.resync_at = -1;
        s.resync_parts.clear();
    }
    {
        std::string kept;
        for (const std::string& part : split_csv(differ)) {
            std::string& dst = s.world_ignored.count(part) ? ignored : kept;
            if (!dst.empty()) dst += ",";
            dst += part;
        }
        differ = kept;
    }
    if (differ.empty() && !ignored.empty()) {
        s.world_mismatch_streak = 0;
        bump("world_hash_diffs_ignored");
        log().write("coop_world_hash_ignored", JsonObj().add("seq", host.seq).add("parts", ignored));
        // Le reste est égal : la copie coop (D15) continue d'avancer (sinon un écart permanent la figerait).
    }
    if (differ.empty()) {
        s.world_mismatch_streak = 0;
        log().write("coop_world_hash_ok", JsonObj().add("seq", host.seq));
        uint64_t all = 0;
        for (auto& p : host.parts) all = all * 31 + p.value;
        // État vérifié : copie coop (D15). Écrire une save fige l'image : reportée si le pion court (copy_tick).
        // Pion fantôme (pilote sur Esquie) : position en l'air, sans monture ici : pas de copie coop.
        if (mount::ghosted()) log().write("coop_copy_skipped", JsonObj().add("why", "follower_ghost"));
        else { s.copy_ok_hash = all; s.copy_ok_at = s.now; }
        return;
    }
    ++s.world_mismatch_streak;
    bump("world_hash_diffs");
    std::string sizes = "{";
    for (auto& [k, l] : leaves) { if (sizes.size() > 1) sizes += ","; sizes += "\"" + k + "\":" + std::to_string(l.size()); }
    // Détail local des parties en écart (l'hôte journalise sa taille de chaque partie à l'envoi).
    std::string detail = "{";
    const auto parts = split_csv(differ);
    for (auto& [k, l] : leaves) {
        if (std::find(parts.begin(), parts.end(), k) == parts.end()) continue;
        if (detail.size() > 1) detail += ",";
        std::string first = "[";
        for (size_t i = 0; i < l.size() && i < 12; ++i) { if (first.size() > 1) first += ","; first += JsonObj().add("p", l[i].first).add("v", l[i].second).str(); }
        detail += "\"" + k + "\":" + first + "]";
    }
    JsonObj diff_log;
    diff_log.add("seq", host.seq).add("parts", differ).add("ignored", ignored).add("streak", s.world_mismatch_streak).raw("leaves", sizes + "}").raw("mine", detail + "}");
    if (differ.find("inventory") != std::string::npos) diff_log.add("inventory", leaves_line(leaves["inventory2"]));   // à comparer au coop_world_hash de l'hôte
    log().write("coop_world_hash_diff", diff_log);
    if (s.world_mismatch_streak >= 2) {
        s.world_mismatch_streak = 0;
        bump("world_resyncs");
        // Diagnostic : toutes les feuilles locales des parties en écart (l'hôte journalise les siennes à la réception de
        // la demande, coop_world_leaves) : comparer les deux journaux hors ligne.
        for (const std::string& part : parts)
            if (auto it = leaves.find(part); it != leaves.end()) log_world_leaves(part, it->second, host.seq);
        s.resync_parts = std::set<std::string>(parts.begin(), parts.end());
        // Rechargement (5-6 s d'écran figé) jamais en plein mouvement : attendu jusqu'à l'arrêt (world_resync_tick).
        // Ni en dialogue, cinématique ou combat : l'instantané arriverait à leur fin (chargement bloqué, 2026-10-23).
        if (s.now - s.last_motion_at < 1.5 || !resync_calm()) {
            if (s.resync_deferred.empty()) {
                s.resync_deferred_since = s.now;
                bump("resyncs_deferred_moving");
                log().write("coop_world_resync_deferred", JsonObj().add("parts", differ));
            }
            s.resync_deferred = differ;
            return;
        }
        send_world_resync(differ);
    }
}

void send_world_resync(std::string differ)   // copie : l'appelant peut passer s.resync_deferred
{
    s.resync_deferred.clear();
    if (s.now - s.last_motion_at < 0.5) bump("resyncs_while_moving");
    ui::set_status("session", "resyncing");
    s.resync_at = s.now;
    s.net.publish(proto::ResyncRequest{"world state differs: " + differ}, true);
    log().write("coop_world_resync", JsonObj().add("parts", differ).add("deferred_s", s.resync_deferred_since >= 0 ? s.now - s.resync_deferred_since : 0.0));
    s.resync_deferred_since = -1;
}

// Client : resynchro attendue -> envoyée après 1,5 s sans mouvement (pilote et pion), ou au bout de 120 s.
void world_resync_tick()
{
    if (s.host || s.resync_deferred.empty()) return;
    if (s.cj != S::ClientJoin::Joined && s.want_join) { s.resync_deferred.clear(); s.resync_deferred_since = -1; return; }
    if ((s.now - s.last_motion_at >= 1.5 && resync_calm()) || s.now - s.resync_deferred_since > 120.0) send_world_resync(s.resync_deferred);
}

// Test de resynchro (-e33perturbworld=N) : une fois entré, le client fausse son or de N (l'écart doit être détecté,
// puis réparé par un nouvel instantané).
void perturb_world_tick()
{
    if (s.perturb_gold == 0 || s.cj != S::ClientJoin::Joined || s.now - s.cj_joined_at < 2.0) return;
    if (auto* gold = ue::value_ptr<int32_t>(game::gi(), STR("Gold"))) {
        *gold += s.perturb_gold;
        bump("world_perturbations");
        log().write("coop_world_perturb", JsonObj().add("gold_delta", s.perturb_gold).add("gold", *gold));
    }
    s.perturb_gold = 0;
}

} // namespace e33::explore::detail
