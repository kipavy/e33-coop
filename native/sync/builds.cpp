#include "builds.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"

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
        else if (!applied.empty() && o.origin != s.me)
            for (auto& n : applied) ui::toast("build_applied", {n, s.net.name_of ? s.net.name_of(o.origin) : ""});
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
            for (auto& n : applied) ui::toast("build_applied", {n, s.net.name_of ? s.net.name_of(in.from) : ""});
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

void check()
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
    if (!s.host) {
        // Client : tout changement part à l'hôte, qui tranche (annulation éventuelle par son état).
        std::vector<uint8_t> blob;
        if (!make_blob(blob)) return;
        for (auto& n : changed) s.baseline[n] = cur.at(n);
        send_offer(blob, changed, false, "menu");
        return;
    }
    // Hôte : ses personnages sont diffusés ; ceux d'un client sont remis à leur état accepté.
    std::vector<std::string> mine, foreign;
    for (auto& n : changed) (s.net.owner_of(n) == s.me ? mine : foreign).push_back(n);
    if (!mine.empty()) {
        std::vector<uint8_t> blob;
        if (make_blob(blob)) {
            for (auto& n : mine) s.baseline[n] = cur.at(n);
            send_offer(blob, mine, true, "menu");
            s.cache = blob;
        }
    }
    if (!foreign.empty() && !s.cache.empty()) {
        std::vector<proto::BuildName> names;
        for (auto& n : foreign) names.push_back({n, 0});
        apply(s.cache, names);
        bump("builds_reverted", static_cast<int>(foreign.size()));
        for (auto& n : foreign) ui::toast("build_not_yours", {n, s.net.name_of ? s.net.name_of(s.net.owner_of(n)) : ""});
        log().write("coop_build_reverted", JsonObj().add("count", static_cast<int64_t>(foreign.size())));
    }
}

} // namespace

void init(bool host, const Net& net)
{
    s.host = host;
    s.net = net;
    s.active = true;
    for (const char* c : {"build_offers_sent", "build_offers_authoritative", "builds_applied", "builds_rejected", "builds_reverted", "build_errors",
                          "build_hash_mismatches", "build_changes_detected"})
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
    if (now < s.next_check) return;
    s.next_check = now + kCheckPeriod;
    if (!can_check()) return;
    check();
}

void rebaseline()
{
    s.have_baseline = false;
    s.baseline.clear();
    s.next_check = s.now + 3.0;
}

} // namespace e33::builds
