// Miroir des drapeaux persistants du pilote et réparation sur place de l'état d'exploration (D38, 2026-10-25).
// Session du 2026-10-25 : chaque écart d'empreinte (or, objets, drapeaux) se réglait par un instantané complet, soit
// 8-9 s d'écran figé puis une téléportation. Désormais :
//  - le pilote publie chaque WritePersistentFlag (FlagSet) ; les suiveurs l'écrivent au point sûr ;
//  - un écart limité à {gold, inventory2, flags} est d'abord réparé sur place : le client demande les feuilles de
//    l'hôte (WorldLeavesRequest / WorldLeaves), applique la différence avec les fonctions du jeu, puis vérifie son
//    empreinte contre celle de l'hôte ; l'instantané reste le repli (échec, délai, partie non réparable).
// -e33worldrepair=0 coupe les deux (ancien comportement).
#include "explore_internal.hpp"

#include <cstdlib>

namespace e33::explore::detail {

namespace {

constexpr double kLeavesTimeout = 15.0;     // feuilles de l'hôte attendues au plus (hôte ancien : jamais de réponse)
constexpr double kApplyMaxWait = 60.0;      // feuilles reçues, application attendue au calme au plus
constexpr double kVerifyDelay = 2.0;        // vérification après l'application (fonctions du jeu appliquées)
constexpr double kBlockAfterFail = 300.0;   // après un échec : instantané direct pendant 5 min
constexpr size_t kPageBytes = 30000;        // charge utile d'une page de WorldLeaves (fragmentée par le lien, D11)

bool repairable(const std::string& part) { return part == "gold" || part == "inventory2" || part == "flags"; }

std::string lower(std::string v)
{
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

// Tenues et coiffures : hors empreinte (D27, inventory_leaves), donc hors réparation.
bool skin_or_face(const std::string& low) { return low.rfind("item:skin", 0) == 0 || low.rfind("item:face", 0) == 0; }

// "NamedIDsStates{NID_X}" -> "NID_X" (vide pour le compte « NamedIDsStates# »).
std::string flag_name(const std::string& path)
{
    static const std::string head = "NamedIDsStates{";
    if (path.rfind(head, 0) != 0 || path.size() < head.size() + 1 || path.back() != '}') return {};
    return path.substr(head.size(), path.size() - head.size() - 1);
}

struct ApplyOut {
    int applied = 0, failed = 0;
    std::string detail;
    void note(const std::string& key, int64_t v, bool ok, const std::string& err = {})
    {
        ok ? ++applied : ++failed;
        if (detail.size() < 1500) detail += key + (ok ? "" : "!") + "=" + std::to_string(v) + (err.empty() ? "" : "(" + err + ")") + ";";
    }
};

void apply_gold(const std::vector<proto::LeafEntry>& host, ApplyOut& o)
{
    int64_t want = -1;
    for (auto& l : host) if (l.key == "gold") want = std::atoll(l.value.c_str());
    if (want < 0) { o.note("gold", 0, false, "no host value"); return; }
    std::map<std::string, int64_t> eco;
    game::economy_sample(eco);
    const int64_t d = want - (eco.count("gold") ? eco["gold"] : 0);
    if (d == 0) return;
    std::string err;
    o.note("gold", d, game::economy_apply("gold", d, err), err);
}

// Quantités par nom en minuscules (comme l'empreinte), avec les noms d'origine (casse de ce PC) et leur part.
struct Tot { int64_t n = 0; std::vector<std::pair<std::string, int64_t>> names; };

std::map<std::string, Tot> local_inventory()
{
    std::map<std::string, int64_t> eco;
    game::economy_sample(eco);
    std::map<std::string, Tot> m;
    for (auto& [k, v] : eco) {
        if (k == "gold" || v == 0) continue;
        const std::string lk = lower(k);
        if (skin_or_face(lk)) continue;
        m[lk].n += v;
        m[lk].names.emplace_back(k, v);
    }
    return m;
}

// Objets d'abord (AddItemToInventory / RemoveItemFromInventory), niveaux ensuite (ChangeLevel : l'instance d'une arme
// ajoutée existe alors). Le nom passé au jeu garde sa casse : celui d'ici s'il existe (niveaux : comparaison exacte du
// nom d'instance dans economy_apply), sinon celui de l'hôte.
void apply_inventory(const std::vector<proto::LeafEntry>& host, ApplyOut& o)
{
    std::map<std::string, Tot> theirs;
    for (auto& l : host) {
        const int64_t v = std::atoll(l.value.c_str());
        const std::string lk = lower(l.key);
        if (v == 0 || skin_or_face(lk)) continue;
        theirs[lk].n += v;
        theirs[lk].names.emplace_back(l.key, v);
    }
    for (const char* prefix : {"item:", "level:"}) {
        const std::map<std::string, Tot> mine = local_inventory();
        std::set<std::string> keys;
        for (auto& [k, t] : mine) if (k.rfind(prefix, 0) == 0) keys.insert(k);
        for (auto& [k, t] : theirs) if (k.rfind(prefix, 0) == 0) keys.insert(k);
        for (const std::string& k : keys) {
            const auto mi = mine.find(k), ti = theirs.find(k);
            const int64_t mn = mi == mine.end() ? 0 : mi->second.n, hn = ti == theirs.end() ? 0 : ti->second.n;
            const int64_t d = hn - mn;
            if (d == 0) continue;
            const std::string name = mi != mine.end() ? mi->second.names.front().first : ti->second.names.front().first;
            std::string err;
            if (std::string_view(prefix) == "level:") {
                if (hn == 0) { o.note(name, d, false, "not on host"); continue; }
                o.note(name, d, game::economy_apply(name, d, err), err);
                continue;
            }
            if (d > 0) { o.note(name, d, game::economy_apply(name, d, err), err); continue; }
            int64_t left = -d;   // retrait réparti sur les noms d'ici (même objet écrit avec deux casses)
            for (auto& [n, v] : mi->second.names) {
                const int64_t take = std::min(v, left);
                if (take <= 0) continue;
                err.clear();
                o.note(n, -take, game::economy_apply(n, -take, err), err);
                left -= take;
            }
        }
    }
}

void apply_flags(const std::vector<proto::LeafEntry>& host, ApplyOut& o)
{
    std::map<std::string, std::string> mine, theirs;
    for (auto& [p, v] : reflect::field(game::gi(), L"NamedIDsStates"))
        if (std::string n = flag_name(p); !n.empty()) mine[n] = v;
    for (auto& l : host) theirs[l.key] = l.value;
    for (auto& [n, v] : theirs) {
        if (auto it = mine.find(n); it != mine.end() && it->second == v) continue;
        std::string how;
        UObject* id = game::find_named_id(n, nullptr, how);
        const bool ok = id && game::write_persistent_flag(id, v == "1");
        o.note("flag:" + n, v == "1", ok, id ? std::string() : how);
    }
    // Drapeau présent ici seulement : WritePersistentFlag ne retire pas une clé de la map -> non réparable ici.
    for (auto& [n, v] : mine) if (!theirs.count(n)) o.note("flag:" + n, v == "1", false, "local only");
}

void repair_log(const char* result, const ApplyOut* o = nullptr, const std::string& still = {})
{
    std::string parts;
    for (auto& p : s.wr.parts) parts += (parts.empty() ? "" : ",") + p;
    JsonObj j;
    j.add("req", s.wr.req).add("parts", parts).add("result", result);
    if (o) j.add("applied", o->applied).add("failed", o->failed).add("detail", o->detail);
    if (!still.empty()) j.add("still", still);
    log().write("coop_world_repair", j);
}

// Échec : instantané complet (ancien chemin), et plus de réparation sur place pendant 5 min.
void repair_fail(const char* why, std::string differ, std::vector<std::string> parts, const ApplyOut* o = nullptr)   // copies : s.wr est remis à zéro
{
    bump("world_repairs_failed");
    repair_log(why, o, differ);
    const double blocked = s.now + kBlockAfterFail;
    s.wr = S::Repair{};
    s.wr.blocked_until = blocked;
    start_resync(differ, parts);
}

void repair_end()
{
    const double blocked = s.wr.blocked_until;
    s.wr = S::Repair{};
    s.wr.blocked_until = blocked;
}

void apply_flag(const proto::FlagSet& f)
{
    std::string how;
    UObject* id = game::find_named_id(f.name, f.guid.data(), how);
    const int before = id ? game::persistent_flag(id) : -1;
    const bool already = before == (f.value ? 1 : 0);
    const bool ok = id && (already || game::write_persistent_flag(id, f.value));
    if (id && !already) touch();
    bump(ok ? "flags_mirrored" : "flag_mirror_errors");
    log().write("coop_flag_mirrored", JsonObj().add("seq", f.seq).add("name", f.name).add("value", f.value).add("found", id != nullptr)
                                          .add("how", how).add("before", before).add("already", already).add("ok", ok));
}

} // namespace

bool world_repair_on() { static const bool v = cmdline::arg(L"e33worldrepair") != L"0"; return v; }

// --- Drapeaux du pilote -----------------------------------------------------------------------------

void on_local_flag(UObject* id, bool value)
{
    if (!world_repair_on() || !sharing() || !is_pilot() || s.replaying || game::mod_call_depth() > 0) return;
    if (game::map_change_in_progress()) return;   // chargement (drapeaux relus de la save) : pas une action du pilote
    const std::string name = game::named_id_name(id);
    if (name.empty()) return;
    if (auto it = s.flags_sent.find(name); it != s.flags_sent.end() && it->second == value) return;
    s.flags_sent[name] = value;
    proto::FlagSet f;
    f.seq = ++s.flag_seq;
    f.name = name;
    f.value = value;
    if (uint32_t g[4]{}; game::named_id_guid(id, g)) std::copy(g, g + 4, f.guid.begin());
    s.net.publish(f, true);
    bump("flags_published");
    log().write("coop_flag_set", JsonObj().add("seq", f.seq).add("name", name).add("value", value).add("in_battle", game::in_battle()));
}

void on_flag_set(const proto::FlagSet& f)
{
    if (!world_repair_on() || is_pilot()) return;
    if (s.flags_in.size() >= 512) s.flags_in.pop_front();
    s.flags_in.push_back(f);
}

// Suiveur : drapeaux reçus écrits au point sûr (ni combat, ni chargement, ni dialogue, ni cinématique).
void flags_tick()
{
    if (s.flags_in.empty() || !safe_point()) return;
    while (!s.flags_in.empty()) {
        const proto::FlagSet f = s.flags_in.front();
        s.flags_in.pop_front();
        apply_flag(f);
    }
}

// --- Réparation sur place : hôte --------------------------------------------------------------------

void send_world_leaves(uint8_t to, const proto::WorldLeavesRequest& rq)
{
    std::map<std::string, reflect::Leaves> leaves;
    const auto parts = world_parts(&leaves);
    std::map<std::string, int64_t> eco;
    game::economy_sample(eco);
    std::string done;
    int entries = 0, pages_n = 0;
    for (const std::string& part : rq.parts) {
        if (!repairable(part)) continue;
        uint64_t h = 0;
        for (auto& p : parts) if (p.name == part) h = p.value;
        std::vector<proto::LeafEntry> out;
        if (part == "gold") out.push_back({"gold", std::to_string(eco.count("gold") ? eco["gold"] : 0)});
        else if (part == "inventory2") {
            // Casse d'origine (l'empreinte est en minuscules) : le client la passe telle quelle au jeu.
            for (auto& [k, v] : eco)
                if (k != "gold" && v != 0 && !skin_or_face(lower(k))) out.push_back({k, std::to_string(v)});
        } else {
            for (auto& [p, v] : leaves[part])
                if (std::string n = flag_name(p); !n.empty()) out.push_back({n, v});
        }
        std::vector<std::vector<proto::LeafEntry>> pages(1);
        size_t bytes = 0;
        for (auto& l : out) {
            const size_t b = l.key.size() + l.value.size() + 4;
            if (bytes + b > kPageBytes && !pages.back().empty()) { pages.emplace_back(); bytes = 0; }
            pages.back().push_back(l);
            bytes += b;
        }
        for (size_t i = 0; i < pages.size(); ++i)
            s.net.send_to(to, proto::WorldLeaves{rq.seq, part, h, static_cast<uint16_t>(i), static_cast<uint16_t>(pages.size()), pages[i]});
        entries += static_cast<int>(out.size());
        pages_n += static_cast<int>(pages.size());
        done += (done.empty() ? "" : ",") + part;
        if (auto it = leaves.find(part); it != leaves.end()) log_world_leaves(part, it->second, s.world_seq);   // diagnostic
    }
    bump("world_leaves_sent");
    log().write("coop_world_leaves_sent", JsonObj().add("player", static_cast<int>(to)).add("req", rq.seq).add("parts", done)
                                              .add("entries", entries).add("pages", pages_n));
}

// --- Réparation sur place : client ------------------------------------------------------------------

bool try_world_repair(const std::string& differ, const std::vector<std::string>& parts)
{
    if (!world_repair_on() || parts.empty() || s.wr.st != S::Repair::St::None) return false;
    if (s.now < s.wr.blocked_until) { log().write("coop_world_repair_skipped", JsonObj().add("parts", differ).add("why", "failed recently")); return false; }
    for (const std::string& p : parts)
        if (!repairable(p)) { log().write("coop_world_repair_skipped", JsonObj().add("parts", differ).add("why", "not repairable: " + p)); return false; }
    auto& r = s.wr;
    r.st = S::Repair::St::Requested;
    r.req += 1;
    r.parts = parts;
    r.differ = differ;
    r.at = s.now;
    s.net.publish(proto::WorldLeavesRequest{r.req, parts}, true);
    bump("world_repairs_requested");
    log().write("coop_world_repair_request", JsonObj().add("req", r.req).add("parts", differ));
    return true;
}

void on_world_leaves(const proto::WorldLeaves& wl)
{
    auto& r = s.wr;
    if (r.st != S::Repair::St::Requested || wl.seq != r.req) { bump("world_leaves_stale"); return; }
    if (std::find(r.parts.begin(), r.parts.end(), wl.part) == r.parts.end()) return;
    if (r.pages[wl.part].insert(wl.page).second) r.leaves[wl.part].insert(r.leaves[wl.part].end(), wl.leaves.begin(), wl.leaves.end());
    r.pages_total[wl.part] = wl.pages;
    r.host_hash[wl.part] = wl.hash;
    for (auto& p : r.parts)
        if (!r.pages_total.count(p) || r.pages[p].size() < r.pages_total[p]) return;
    r.st = S::Repair::St::Received;
    r.at = s.now;
    int n = 0;
    for (auto& [p, l] : r.leaves) n += static_cast<int>(l.size());
    log().write("coop_world_repair_received", JsonObj().add("req", r.req).add("entries", n));
}

void world_repair_tick()
{
    auto& r = s.wr;
    if (r.st == S::Repair::St::None) return;
    if (s.cj != S::ClientJoin::Joined && s.want_join) { repair_end(); return; }   // instantané en cours : il répare tout
    if (r.st == S::Repair::St::Requested) {
        if (s.now - r.at > kLeavesTimeout) repair_fail("timeout", r.differ, r.parts);   // hôte ancien : type inconnu ignoré
        return;
    }
    if (r.st == S::Repair::St::Received) {
        if (s.now - r.at > kApplyMaxWait) { repair_log("never_calm"); repair_end(); return; }
        // Jamais en combat, dialogue, cinématique, chargement, menu pause ou menu de jeu ouvert (D38).
        if (!quiet(3.0) || !resync_calm() || builds::menu_open()) return;
        if (!prof::claim_heavy("world_repair")) return;
        prof::Scope p("world_repair");
        ApplyOut o;
        for (const std::string& part : r.parts) {
            const auto& host = r.leaves[part];
            if (part == "gold") apply_gold(host, o);
            else if (part == "inventory2") apply_inventory(host, o);
            else if (part == "flags") apply_flags(host, o);
        }
        // Événement : le relevé d'économie (builds.cpp) reprend sa référence au lieu d'envoyer ces écarts à l'hôte.
        touch();
        if (o.failed > 0) { repair_fail("apply_failed", r.differ, r.parts, &o); return; }
        repair_log("applied", &o);
        r.st = S::Repair::St::Applied;
        r.at = s.now;
        r.event_mark = s.last_event_at;
        return;
    }
    // Appliquée : empreinte d'ici comparée à celle de l'hôte (dans WorldLeaves) pour les parties réparées.
    if (s.now - r.at < kVerifyDelay) return;
    if (s.last_event_at > r.event_mark || s.now - r.at > 30.0) { repair_log("inconclusive"); repair_end(); return; }   // l'état a bougé depuis
    if (!safe_point() || !prof::claim_heavy("world_hash")) return;
    const auto mine = world_parts();
    std::string still;
    std::vector<std::string> still_parts;
    for (auto& p : mine)
        if (auto it = r.host_hash.find(p.name); it != r.host_hash.end() && it->second != p.value) {
            still += (still.empty() ? "" : ",") + p.name;
            still_parts.push_back(p.name);
        }
    if (!still.empty()) { repair_fail("still_differs", still, still_parts); return; }
    bump("world_repairs_in_place");
    repair_log("ok");
    s.world_mismatch_streak = 0;
    repair_end();
}

} // namespace e33::explore::detail
