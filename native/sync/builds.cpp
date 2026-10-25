#include "builds.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../common/prof.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"
#include "explore.hpp"

using RC::Unreal::UObject;

namespace e33::builds {

namespace {

constexpr double kCheckPeriod = 2.0;
constexpr const wchar_t* kOutSlot = L"e33coop/build_out";
constexpr const wchar_t* kInSlot = L"e33coop/build_in";

uint64_t fnv64(const std::vector<uint8_t>& b)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t x : b) { h ^= x; h *= 1099511628211ull; }
    return h;
}

struct Inbound {
    proto::BuildOffer offer;
    uint8_t from = 0;
    std::vector<uint8_t> data;
    uint32_t received = 0;
};

struct S {
    bool host = false;
    bool active = false;
    Net net;
    uint8_t me = 0;
    double now = 0;
    double next_check = 0;
    std::map<std::string, uint64_t> baseline;     // empreinte connue (dernier état accepté) par personnage
    bool have_baseline = false;
    std::vector<uint8_t> cache;                   // hôte : états acceptés de tous les personnages (annulation)
    uint32_t next_xfer = 1;
    std::map<uint32_t, Inbound> inbound;
} s;

void bump(const char* c, int n = 1) { if (s.net.bump) s.net.bump(c, n); }

bool can_check()
{
    return game::controller() && !game::in_battle() && !game::map_change_in_progress() && game::explore_pawn() && !menu_open();
}

std::map<std::string, uint64_t> current_hashes()
{
    std::map<std::string, uint64_t> out;
    for (const auto& n : game::character_names()) {
        uint64_t h = 0;
        if (game::character_build(n, h)) out[n] = h;
    }
    return out;
}

// Envoie un état (blob de tous les personnages, noms à appliquer) : hôte -> to (0xFF = tous), client -> hôte.
void send_offer(const std::vector<uint8_t>& blob, const std::vector<std::string>& names, bool authoritative, const std::string& reason, uint8_t to = 0xFF)
{
    proto::BuildOffer o;
    o.xfer = (static_cast<uint32_t>(s.me) << 24) | s.next_xfer++;
    o.origin = s.me;
    o.authoritative = authoritative;
    o.size = static_cast<uint32_t>(blob.size());
    o.hash = fnv64(blob);
    o.reason = reason;
    const auto hashes = current_hashes();
    for (auto& n : names) { auto it = hashes.find(n); o.names.push_back({n, it == hashes.end() ? 0 : it->second}); }
    std::vector<proto::Message> msgs{o};
    for (uint32_t off = 0; off < blob.size(); off += proto::kMaxChunk) {
        proto::BuildChunk c;
        c.xfer = o.xfer;
        c.offset = off;
        c.data.assign(blob.begin() + off, blob.begin() + off + std::min<size_t>(proto::kMaxChunk, blob.size() - off));
        msgs.push_back(c);
    }
    for (auto& m : msgs) {
        if (!s.host) s.net.publish(m);
        else if (to == 0xFF) s.net.publish(m);
        else s.net.send_to(to, m);
    }
    // Copie dans le dossier du run (données de test, jamais versionnées).
    if (!launch_args().run_dir.empty()) {
        std::ofstream f(std::filesystem::path(launch_args().run_dir) / (L"build-" + std::to_wstring(o.xfer) + L".sav"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    }
    std::string ns;
    for (auto& n : names) ns += (ns.empty() ? "" : ",") + n;
    bump(authoritative ? "build_offers_authoritative" : "build_offers_sent");
    log().write("coop_build_offer", JsonObj().add("xfer", o.xfer).add("names", ns).add("authoritative", authoritative).add("reason", reason)
                                        .add("size", static_cast<int64_t>(blob.size())).add("to", static_cast<int>(to)));
}

bool make_blob(std::vector<uint8_t>& blob)
{
    std::string err;
    if (game::make_characters_blob(kOutSlot, blob, err)) return true;
    bump("build_errors");
    log().write("coop_build_error", JsonObj().add("what", "make blob: " + err));
    return false;
}

// Applique les personnages nommés d'un blob ; met à jour la référence.
std::vector<std::string> apply(const std::vector<uint8_t>& blob, const std::vector<proto::BuildName>& names)
{
    std::vector<std::string> want, applied;
    for (auto& n : names) want.push_back(n.name);
    std::string err;
    if (!game::apply_characters_blob(kInSlot, blob, want, applied, err)) {
        bump("build_errors");
        log().write("coop_build_error", JsonObj().add("what", "apply: " + err));
        return applied;
    }
    for (auto& n : names) {
        if (std::find(applied.begin(), applied.end(), n.name) == applied.end()) continue;
        uint64_t h = 0;
        if (!game::character_build(n.name, h)) continue;
        s.baseline[n.name] = h;
        if (n.hash && h != n.hash) {
            // État appliqué mais empreinte différente de l'émetteur : à surveiller (champ recalculé localement ?).
            bump("build_hash_mismatches");
            log().write("coop_build_hash_mismatch", JsonObj().add("name", n.name).add("mine", static_cast<int64_t>(h)).add("theirs", static_cast<int64_t>(n.hash)));
        }
    }
    bump("builds_applied", static_cast<int>(applied.size()));
    return applied;
}

void refresh_cache()
{
    if (!s.host) return;
    std::vector<uint8_t> blob;
    if (make_blob(blob)) s.cache = std::move(blob);
}

void on_complete(Inbound& in)
{
    const auto& o = in.offer;
    if (fnv64(in.data) != o.hash) {
        bump("build_errors");
        log().write("coop_build_error", JsonObj().add("what", "hash").add("xfer", o.xfer));
        return;
    }
    std::string ns;
    for (auto& n : o.names) ns += (ns.empty() ? "" : ",") + n.name;
    if (!s.host) {
        // De l'hôte : appliqué tel quel (changement d'un autre joueur, ou annulation d'une modification d'ici).
        const auto applied = apply(in.data, o.names);
        log().write("coop_build_applied", JsonObj().add("xfer", o.xfer).add("names", ns).add("reason", o.reason).add("applied", static_cast<int64_t>(applied.size())));
        if (o.reason == "not yours")
            for (auto& n : o.names) ui::toast("build_not_yours", {n.name, s.net.name_of ? s.net.name_of(s.net.owner_of(n.name)) : ""});
        return;
    }
    // Hôte : proposition d'un client. Seuls ses propres personnages sont acceptés.
    std::vector<proto::BuildName> mine, foreign;
    for (auto& n : o.names) (s.net.owner_of(n.name) == in.from ? mine : foreign).push_back(n);
    if (!mine.empty()) {
        const auto applied = apply(in.data, mine);
        std::vector<std::string> names(applied.begin(), applied.end());
        log().write("coop_build_accepted", JsonObj().add("from", static_cast<int>(in.from)).add("names", ns).add("applied", static_cast<int64_t>(applied.size())));
        if (!applied.empty()) {
            // Rediffusé aux autres (mêmes octets), état de référence de l'hôte rafraîchi.
            proto::BuildOffer fwd = o;
            fwd.authoritative = true;
            fwd.names = mine;
            s.net.relay(fwd, in.from);
            for (uint32_t off = 0; off < in.data.size(); off += proto::kMaxChunk) {
                proto::BuildChunk c;
                c.xfer = o.xfer;
                c.offset = off;
                c.data.assign(in.data.begin() + off, in.data.begin() + off + std::min<size_t>(proto::kMaxChunk, in.data.size() - off));
                s.net.relay(c, in.from);
            }
            refresh_cache();
        }
    }
    if (!foreign.empty()) {
        // Personnages d'un autre joueur : refusé, l'auteur reçoit l'état de l'hôte (sa modification est annulée).
        bump("builds_rejected", static_cast<int>(foreign.size()));
        std::vector<std::string> names;
        for (auto& n : foreign) names.push_back(n.name);
        log().write("coop_build_rejected", JsonObj().add("from", static_cast<int>(in.from)).add("count", static_cast<int64_t>(names.size())));
        std::vector<uint8_t> blob;
        if (make_blob(blob)) send_offer(blob, names, true, "not yours", in.from);
    }
}

void check_builds()
{
    const auto cur = current_hashes();
    if (cur.empty()) return;
    if (!s.have_baseline) {
        s.baseline = cur;
        s.have_baseline = true;
        refresh_cache();
        log().write("coop_build_baseline", JsonObj().add("characters", static_cast<int64_t>(cur.size())));
        return;
    }
    std::vector<std::string> changed;
    for (auto& [n, h] : cur) {
        auto it = s.baseline.find(n);
        if (it == s.baseline.end() || it->second != h) changed.push_back(n);
    }
    if (changed.empty()) return;
    bump("build_changes_detected", static_cast<int>(changed.size()));
    if (!s.net.sharing || !s.net.sharing()) {   // seul : la nouvelle référence, rien à envoyer
        for (auto& n : changed) s.baseline[n] = cur.at(n);
        refresh_cache();
        return;
    }
    std::vector<std::string> mine, foreign;
    for (auto& n : changed) (s.net.owner_of(n) == s.me ? mine : foreign).push_back(n);
    if (!s.host) {
        // Client : seuls ses personnages partent à l'hôte. Un personnage d'un autre joueur changé ici sans menu
        // (effet d'un événement rejoué) prend sa nouvelle référence : l'hôte, lui, le corrige par ses propres offres.
        // Avant : tout partait, l'hôte refusait et renvoyait son état (« pas à vous » affiché à tort, essai du 2026-10-25).
        for (auto& n : foreign) s.baseline[n] = cur.at(n);
        if (!foreign.empty())
            log().write("coop_build_foreign_change", JsonObj().add("count", static_cast<int64_t>(foreign.size())));
        if (mine.empty()) return;
        std::vector<uint8_t> blob;
        if (!make_blob(blob)) return;
        for (auto& n : mine) s.baseline[n] = cur.at(n);
        send_offer(blob, mine, false, "menu");
        return;
    }
    // Hôte : ceux d'un client sont d'abord remis à leur état accepté (cache = dernier état accepté, pas l'état
    // courant : avant, le cache était refait avec le changement étranger dedans et l'« annulation » ne défaisait rien),
    // puis ses personnages sont diffusés.
    if (!foreign.empty() && !s.cache.empty()) {
        std::vector<proto::BuildName> names;
        for (auto& n : foreign) names.push_back({n, 0});
        apply(s.cache, names);
        bump("builds_reverted", static_cast<int>(foreign.size()));
        for (auto& n : foreign) ui::toast("build_not_yours", {n, s.net.name_of ? s.net.name_of(s.net.owner_of(n)) : ""});
        log().write("coop_build_reverted", JsonObj().add("count", static_cast<int64_t>(foreign.size())));
    }
    if (!mine.empty()) {
        std::vector<uint8_t> blob;
        if (make_blob(blob)) {
            for (auto& n : mine) s.baseline[n] = cur.at(n);
            send_offer(blob, mine, true, "menu");
            s.cache = blob;
        }
    }
}


// --- Ressources communes (D13, option 1) ---------------------------------------------------------
// Or, objets et niveaux d'équipement relevés au calme (aucun événement rejoué depuis 2 s, hors combat). Une variation
// entre deux relevés sans événement entre les deux vient d'ici (achat, amélioration, objet utilisé dans un menu) : elle
// part en EconomyDelta. L'hôte vérifie qu'elle est possible chez lui, l'applique avec les fonctions du jeu et la relaie.

struct Econ {
    std::map<std::string, int64_t> prev;
    double prev_at = -1;
    uint32_t seq = 0;
    double next = 0;
};
Econ e;

std::map<std::string, int64_t> econ_diff(const std::map<std::string, int64_t>& cur, const std::map<std::string, int64_t>& prev)
{
    std::map<std::string, int64_t> d;
    for (auto& [k, v] : cur) { auto it = prev.find(k); const int64_t p = it == prev.end() ? 0 : it->second; if (v != p) d[k] = v - p; }
    for (auto& [k, v] : prev) if (!cur.count(k) && v != 0) d[k] = -v;
    return d;
}

// Objets utilisés hors combat (élixir de chroma, objet donné à un personnage depuis le menu) : la quantité seule ne
// suffit pas, l'effet (soin) doit aussi avoir lieu chez les autres. Vu le 2026-10-25 : élixir de J2, soin absent chez
// l'hôte. L'utilisation part en clé « use:<quoi> » (au lieu de « item:<objet> » = -1) ; les autres appellent la même
// fonction du jeu, qui dépense l'objet elle-même.
struct Use { std::string what, item; double at; };
std::vector<Use> g_uses;                       // utilisations d'ici dont la dépense n'est pas encore partie
std::map<std::string, int64_t> g_expect;       // dépenses attendues ici après un rejeu (dépense différée par le jeu)
double g_expect_at = -1;

std::string use_item_of(const std::string& what)
{
    if (what == "heal_party") return "PartyHealConsumable";
    return what.substr(0, what.find('@'));
}

// Remplace dans d la dépense d'un objet utilisé ici par sa clé « use: ».
void convert_uses(std::map<std::string, int64_t>& d)
{
    for (auto it = g_uses.begin(); it != g_uses.end();) {
        if (s.now - it->at > 15.0) { it = g_uses.erase(it); continue; }
        auto f = d.find("item:" + it->item);
        if (f == d.end() || f->second >= 0) { ++it; continue; }
        if (++f->second == 0) d.erase(f);
        d["use:" + it->what] += 1;
        it = g_uses.erase(it);
    }
}

// Retire de d les dépenses faites ici par un rejeu (pas à renvoyer).
void drop_expected(std::map<std::string, int64_t>& d)
{
    if (g_expect_at >= 0 && s.now - g_expect_at > 15.0) g_expect.clear();
    for (auto it = g_expect.begin(); it != g_expect.end();) {
        auto f = d.find(it->first);
        if (f != d.end() && f->second < 0) {
            const int64_t take = std::max(f->second, it->second);   // deux négatifs : le plus proche de 0
            f->second -= take;
            it->second -= take;
            if (f->second == 0) d.erase(f);
        }
        if (it->second == 0) it = g_expect.erase(it); else ++it;
    }
}

bool econ_apply(const std::vector<proto::EconomyPart>& parts, std::string& err)
{
    bool ok = true;
    for (auto& p : parts) {
        std::string e1;
        if (p.key.rfind("use:", 0) == 0) {
            const std::string what = p.key.substr(4), item = "item:" + use_item_of(what);
            std::map<std::string, int64_t> before, after;
            game::economy_sample(before);
            bool one = true;
            for (int64_t i = 0; i < p.delta; ++i) one = game::replay_item_use(what, e1) && one;
            game::economy_sample(after);
            const int64_t spent = (before.count(item) ? before[item] : 0) - (after.count(item) ? after[item] : 0);
            // Dépense pas encore faite par le jeu (animation ?) : à ne pas renvoyer quand elle arrivera.
            if (spent < p.delta) { g_expect[item] -= p.delta - spent; g_expect_at = s.now; }
            log().write("coop_item_use_replayed", JsonObj().add("what", what).add("count", p.delta).add("ok", one).add("spent", spent).add("err", e1));
            if (!one) { ok = false; err += (err.empty() ? "" : "; ") + p.key + ": " + e1; }
            continue;
        }
        if (!game::economy_apply(p.key, p.delta, e1)) { ok = false; err += (err.empty() ? "" : "; ") + p.key + ": " + e1; }
    }
    return ok;
}

void econ_resample()
{
    game::economy_sample(e.prev);
    e.prev_at = s.now;
}

// Amélioration d'arme faite ici (Curateur du camp, pendant son dialogue) : le relevé périodique ne la voit pas (un
// dialogue est un « événement », la référence est reprise après lui, amélioration comprise). Vu le 2026-10-25 : armes
// améliorées des deux côtés, aucun delta envoyé, niveau 3 chez l'hôte et 4 chez le client. On mesure donc l'économie
// juste avant et juste après la fonction du jeu, et l'écart (niveau + matériaux dépensés) part aussitôt.
std::map<std::string, int64_t> g_upgrade_before;
bool g_upgrade_open = false;

void econ_send(const std::map<std::string, int64_t>& d, const char* why)
{
    proto::EconomyDelta m;
    m.seq = ++e.seq;
    m.origin = s.me;
    m.authoritative = s.host;
    std::string ks;
    for (auto& [k, v] : d) { m.parts.push_back({k, v}); ks += (ks.empty() ? "" : ",") + k + "=" + std::to_string(v); }
    s.net.publish(m);
    bump("economy_deltas_sent");
    log().write("coop_economy_delta", JsonObj().add("seq", m.seq).add("parts", ks).add("authoritative", m.authoritative).add("why", why));
}

void econ_tick()
{
    if (s.now < e.next) return;
    if (!prof::claim_heavy("economy")) return;   // pas dans la même image qu'une autre tâche lourde
    prof::Scope p("economy");
    e.next = s.now + 2.0;
    if (!game::controller() || game::in_battle() || game::map_change_in_progress() || !game::explore_pawn()) { e.prev_at = -1; return; }
    const double age = s.net.last_event_age ? s.net.last_event_age() : 1e9;
    if (age < 2.0) { e.prev_at = -1; return; }
    std::map<std::string, int64_t> cur;
    game::economy_sample(cur);
    // Premier relevé, ou un événement (coffre, combat, butin…) depuis le précédent : nouvelle référence, rien à envoyer.
    if (e.prev_at < 0 || age < s.now - e.prev_at) { e.prev = cur; e.prev_at = s.now; return; }
    auto d = econ_diff(cur, e.prev);
    e.prev = cur;
    e.prev_at = s.now;
    drop_expected(d);
    convert_uses(d);
    if (d.empty() || !s.net.sharing || !s.net.sharing()) return;
    econ_send(d, "sample");
}

void econ_receive(uint8_t from, const proto::EconomyDelta& m)
{
    std::string err;
    if (!s.host) {
        if (m.rejected) {
            // L'hôte n'a pas pu appliquer notre dépense (or ou objets épuisés chez lui) : on reprend son état.
            bump("economy_rejected");
            ui::toast("economy_rejected");
            log().write("coop_economy_rejected", JsonObj().add("seq", m.seq));
            if (s.net.request_resync) s.net.request_resync("economy rejected");
            return;
        }
        const bool ok = econ_apply(m.parts, err);
        econ_resample();
        bump(ok ? "economy_deltas_applied" : "economy_errors");
        log().write("coop_economy_applied", JsonObj().add("seq", m.seq).add("origin", static_cast<int>(m.origin)).add("ok", ok).add("err", err));
        return;
    }
    // Hôte : possible chez lui ? (dépenses : assez d'or et d'objets)
    std::map<std::string, int64_t> have;
    game::economy_sample(have);
    bool affordable = true;
    for (auto& p : m.parts)
        if (p.delta < 0 && p.key.rfind("level:", 0) != 0 && (have.count(p.key) ? have[p.key] : 0) + p.delta < 0) affordable = false;
    if (!affordable) {
        proto::EconomyDelta r = m;
        r.rejected = true;
        s.net.send_to(from, r);
        bump("economy_rejected");
        log().write("coop_economy_reject", JsonObj().add("from", static_cast<int>(from)).add("seq", m.seq));
        return;
    }
    const bool ok = econ_apply(m.parts, err);
    econ_resample();
    proto::EconomyDelta fwd = m;
    fwd.authoritative = true;
    s.net.relay(fwd, from);
    bump(ok ? "economy_deltas_applied" : "economy_errors");
    log().write("coop_economy_applied", JsonObj().add("seq", m.seq).add("origin", static_cast<int>(from)).add("ok", ok).add("err", err));
}
} // namespace

void reset() { s = S{}; }

void on_upgrade_pre()
{
    if (!s.active || game::in_battle() || game::mod_call_depth() > 0) return;
    game::economy_sample(g_upgrade_before);
    g_upgrade_open = true;
}

// Écart mesuré autour d'une fonction du jeu appelée ici : envoyé aussitôt, et référence du relevé périodique décalée
// d'autant (l'écart ne repart pas une seconde fois).
void send_measured(std::map<std::string, int64_t> d, const char* why)
{
    if (e.prev_at >= 0) for (auto& [k, v] : d) e.prev[k] += v;
    convert_uses(d);
    log().write("coop_economy_measured", JsonObj().add("why", why).add("changes", static_cast<int64_t>(d.size())).add("sharing", s.net.sharing && s.net.sharing()));
    if (d.empty() || !s.net.sharing || !s.net.sharing()) return;
    econ_send(d, why);
}

void on_upgrade_post()
{
    if (!g_upgrade_open) return;
    g_upgrade_open = false;
    std::map<std::string, int64_t> after;
    game::economy_sample(after);
    bump("economy_upgrades_sent");
    send_measured(econ_diff(after, g_upgrade_before), "upgrade");
}

std::map<std::string, int64_t> g_use_before;
bool g_use_open = false;
int g_use_depth = 0;   // appels imbriqués (HealParty peut passer par UseItemOnCharacter) : seul le premier compte

void on_item_use_pre(const std::string& what)
{
    if (g_use_depth++ > 0) return;
    if (!s.active || game::in_battle() || game::mod_call_depth() > 0) return;
    g_uses.push_back({what, use_item_of(what), s.now});
    game::economy_sample(g_use_before);
    g_use_open = true;
    log().write("coop_item_use_local", JsonObj().add("what", what));
}

void on_item_use_post()
{
    if (g_use_depth > 0) --g_use_depth;
    if (g_use_depth > 0 || !g_use_open) return;
    g_use_open = false;
    std::map<std::string, int64_t> after;
    game::economy_sample(after);
    auto d = econ_diff(after, g_use_before);
    // Dépense pas encore faite (le jeu la fait plus tard) : le relevé périodique la convertira (g_uses).
    if (d.empty()) return;
    bump("economy_item_uses_sent");
    send_measured(std::move(d), "use");
}

void init(bool host, const Net& net)
{
    s.host = host;
    s.net = net;
    s.active = true;
    for (const char* c : {"build_offers_sent", "build_offers_authoritative", "builds_applied", "builds_rejected", "builds_reverted", "build_errors",
                          "build_hash_mismatches", "build_changes_detected",
                          "economy_deltas_sent", "economy_deltas_applied", "economy_rejected", "economy_errors", "economy_upgrades_sent", "economy_item_uses_sent"})
        bump(c, 0);
}

void set_player_id(uint8_t id) { s.me = id; }

bool menu_open()
{
    ue::Call c(game::controller(), STR("IsAnyMenuVisible"));
    return c && c.invoke() && c.out_bool(STR("ReturnValue"));
}

bool handle(uint8_t from, const proto::Message& m)
{
    if (!s.active) return false;
    if (auto* o = std::get_if<proto::BuildOffer>(&m)) {
        if (!s.host && !o->authoritative) return true;   // un client n'accepte que les états de l'hôte
        Inbound in;
        in.offer = *o;
        in.from = from;
        in.data.assign(o->size, 0);
        s.inbound[o->xfer] = std::move(in);
        if (o->size == 0) { on_complete(s.inbound[o->xfer]); s.inbound.erase(o->xfer); }
        return true;
    }
    if (auto* ed = std::get_if<proto::EconomyDelta>(&m)) { econ_receive(from, *ed); return true; }
    if (auto* c = std::get_if<proto::BuildChunk>(&m)) {
        auto it = s.inbound.find(c->xfer);
        if (it == s.inbound.end() || c->offset != it->second.received || c->offset + c->data.size() > it->second.offer.size) {
            bump("build_errors");
            log().write("coop_build_error", JsonObj().add("what", "chunk").add("xfer", c->xfer).add("offset", c->offset));
            return true;
        }
        std::copy(c->data.begin(), c->data.end(), it->second.data.begin() + c->offset);
        it->second.received += static_cast<uint32_t>(c->data.size());
        if (it->second.received == it->second.offer.size) {
            on_complete(it->second);
            s.inbound.erase(it);
        }
        return true;
    }
    return false;
}

void tick(double now)
{
    if (!s.active) return;
    s.now = now;
    econ_tick();
    if (now < s.next_check) return;
    // Empreinte des builds : SaveCharacterState + réflexion de chaque personnage, coûteux. Seule tâche lourde de son
    // image, et pas pendant que le pion court (les builds ne changent que dans les menus, pion arrêté) sauf toutes
    // les 20 s au plus (2026-10-22 : à-coups en exploration quand l'hôte court).
    static double last_done = -1000;
    if (explore::local_moving() && now - last_done < 20.0) { s.next_check = now + 0.5; return; }
    if (!prof::claim_heavy("builds")) return;   // réessai à l'image suivante
    s.next_check = now + kCheckPeriod;
    if (!can_check()) return;
    last_done = now;
    prof::Scope p("build_check");
    check_builds();
}

void rebaseline()
{
    e.prev_at = -1;
    s.have_baseline = false;
    s.baseline.clear();
    s.next_check = s.now + 3.0;
}

} // namespace e33::builds
