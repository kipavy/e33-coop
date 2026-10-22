#include "remote.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <Unreal/UObjectGlobals.hpp>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/minijson.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../net/steam.hpp"
#include "../protocol/messages.hpp"
#include "../sync/coop.hpp"
#include "../sync/explore.hpp"
#include "../ui/ui.hpp"
#include "remote_proto.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

using RC::Unreal::UObject;
namespace fs = std::filesystem;

namespace e33::remote {

namespace {

constexpr size_t kChunk = 32 * 1024;
constexpr int kChunksPerTick = 8;
constexpr double kRequestTimeout = 180.0;     // hôte : rien reçu pour une requête
constexpr double kStall = 120.0;              // envoi bloqué (pair parti)
constexpr int kResultLimitExceeded = 25;

// --- Envoi de fichiers par morceaux (journaux : client -> hôte ; mise à jour : hôte -> client) --------------------
struct Item { std::string rel; fs::path abs; uint64_t size = 0; std::string sha256; };
struct OutXfer {
    uint64_t to = 0;
    uint32_t req = 0;
    std::vector<Item> items;
    size_t idx = 0;
    std::vector<uint8_t> buf;     // fichier courant (lu en entier : <= 32 Mo)
    bool loaded = false;
    uint64_t off = 0;
    uint64_t sent = 0;
    double last_progress = 0;
    bool done_sent = false;
    std::vector<std::string> notes;
    bool host_side = false;       // hôte (mise à jour) : pas de réponse finale à envoyer
};

// --- Réception d'une mise à jour (client) -----------------------------------------------------------------------------
struct RxFile { uint64_t size = 0, got = 0; std::string sha_expected; std::unique_ptr<Sha256> sha; std::ofstream f; };
struct InUpdate {
    bool active = false;
    uint32_t req = 0;
    uint64_t announced = 0, total = 0;
    std::map<std::string, RxFile> files;
    std::string error;
};

// --- Requêtes de l'hôte ------------------------------------------------------------------------------------------------
struct HostReq {
    std::string id, cmd;
    fs::path dir;
    uint64_t peer = 0;
    double last_activity = 0;
    uint64_t bytes = 0;
    int files = 0;
    std::map<std::string, std::unique_ptr<std::ofstream>> open;
    bool finished = false;
    double last_state_write = 0;
};

struct State {
    double now = 0;
    bool steam_ok = false;
    double next_steam_try = 5.0;   // laisser le jeu finir de démarrer
    // client
    double next_hello = 0, next_accept = 0, next_menu_check = 0, next_auto_join = 0;
    bool at_menu = false;
    bool indicator = false;
    std::string host_name;
    double fps = 0;
    uint64_t last_lobby_logged = 0;
    InUpdate upd;
    double quit_at = -1;
    int dropped_untrusted = 0;
    bool snapshot_loaded = false;   // save coop chargée à distance (autosave coupée) : quitter reste sans risque
    // envois en cours
    std::deque<OutXfer> out;
    // hôte
    bool host_active = false;
    double next_inbox = 0;
    uint32_t next_req = 0;
    std::map<uint32_t, HostReq> reqs;
    std::set<uint64_t> peers;
} s;

const remotecfg::Config& cfg() { return cmdline::remote_config(); }
fs::path mod_dir() { return cmdline::mod_dir_path(); }
fs::path remote_dir() { return mod_dir() / L"remote"; }
fs::path saves_dir()
{
    const wchar_t* la = _wgetenv(L"LOCALAPPDATA");
    return la ? fs::path(la) / L"Sandfall" / L"Saved" / L"SaveGames" : fs::path();
}

std::string stamp()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof b, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string read_text(const fs::path& p, size_t max = 1u << 20)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string text(max, '\0');
    f.read(text.data(), static_cast<std::streamsize>(max));
    text.resize(static_cast<size_t>(f.gcount()));
    return text;
}

bool write_text(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

fs::path rel_path(const fs::path& base, const std::string& rel) { return base / fs::path(cmdline::widen(rel)).make_preferred(); }

int send(uint64_t to, const Packet& p)
{
    const auto bytes = encode(p);
    return net::steam::send_raw(to, bytes.data(), bytes.size(), true, kSteamChannel);
}

void reply(uint32_t req, bool ok, const std::string& json, bool final = true)
{
    Packet p;
    p.op = Op::Reply;
    p.req = req;
    p.ok = ok;
    p.final = final;
    p.json = json;
    send(cfg().host, p);
    log().write("remote_reply", JsonObj().add("req", req).add("ok", ok).add("final", final).raw("data", json.empty() ? "{}" : json));
}
void reply_error(uint32_t req, const std::string& why) { reply(req, false, JsonObj().add("error", why).str()); }

// --- Quitter / relancer --------------------------------------------------------------------------------------------

// Quitter sans risque pour les sauvegardes de l'ami : menu principal, test, ou partie coop (autosave coupée).
bool quit_allowed(std::string& why)
{
    if (cmdline::test_mode() || s.at_menu || s.snapshot_loaded || !game::controller()) return true;
    if (coop::active() && coop::role() == coop::Role::Client && explore::joined()) return true;   // partie de l'hôte chargée
    why = "partie solo en cours chez l'ami : refusé (ses sauvegardes passent avant)";
    return false;
}

// Assistant PowerShell installé par install.ps1 dans remote/ (aucun argument venant du réseau : seulement notre PID).
bool start_helper(const wchar_t* script, std::string& err)
{
    const fs::path p = remote_dir() / script;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { err = "assistant absent : remote/" + ue::to_utf8(script) + " (réinstaller avec l'option voulue)"; return false; }
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                       L"-WindowStyle Hidden -File \"" + p.wstring() + L"\" -GamePid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Hors du job du jeu si possible (sinon il serait tué avec le jeu).
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
    if (!ok) ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { err = "CreateProcess : erreur " + std::to_string(GetLastError()); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log().write("remote_helper_started", JsonObj().add("script", ue::to_utf8(script)));
    return true;
}

void quit_game()
{
    log().write("remote_quit", JsonObj());
    if (coop::active()) coop::shutdown("remote quit");
    // Sortie normale du moteur ; filet : fin forcée 15 s plus tard (aucune save écrite : voir quit_allowed).
    UObject* pc = game::controller();
    if (!pc && game::gi()) {
        ue::Call g(ue::find_object(STR("/Script/Engine.Default__GameplayStatics")), STR("GetPlayerController"));
        if (g && g.set_object(STR("WorldContextObject"), game::gi()).set_int(STR("PlayerIndex"), 0).invoke()) pc = g.out_object(STR("ReturnValue"));
    }
    ue::Call c(ue::find_object(STR("/Script/Engine.Default__KismetSystemLibrary")), STR("QuitGame"));
    if (c && pc) c.set_object(STR("WorldContextObject"), pc).set_object(STR("SpecificPlayer"), pc).set_int(STR("QuitPreference"), 0)
                     .set_bool(STR("bIgnorePlatformRestrictions"), true).invoke();
    std::thread([] { Sleep(15000); TerminateProcess(GetCurrentProcess(), 0); }).detach();
}

// Réponse envoyée, puis sortie 2 s plus tard (le temps que Steam la transmette).
void schedule_quit() { s.quit_at = s.now + 2.0; }

// --- Client : état --------------------------------------------------------------------------------------------------

std::string last_run_json()
{
    fs::path best;
    std::error_code ec;
    for (auto it = fs::directory_iterator(mod_dir() / L"logs", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (n.rfind(L"remote-", 0) == 0 && (best.empty() || n > best.filename().wstring())) best = it->path();
    }
    if (best.empty()) return "null";
    JsonObj o;
    o.add("id", ue::to_utf8(best.filename().wstring()));
    o.add("running", cmdline::remote_run().run_dir == best.wstring());
    json::Value r;
    std::string err;
    const std::string txt = read_text(best / L"result.json");
    if (!txt.empty() && json::parse(txt, r, err)) o.add("status", r["status"].as_string("?")).add("reason", r["reason"].as_string(""));
    else o.add("status", "none");
    return o.str();
}

std::string status_json()
{
    const auto role = coop::role();
    std::error_code ec;
    const bool staged = fs::exists(remote_dir() / L"staging" / L"manifest.txt", ec);
    JsonObj o;
    o.add("mod_version", coop::mod_version()).add("coop_protocol", static_cast<int>(proto::kProtocolVersion)).add("remote_version", static_cast<int>(kVersion))
        .add("steam_name", net::steam::persona_name())
        .add("test_mode", cmdline::test_mode()).add("test", ue::to_utf8(fs::path(cmdline::test_path()).filename().wstring()))
        .add("role", role == coop::Role::Host ? "host" : role == coop::Role::Client ? "client" : "solo")
        .add("session_ok", coop::active() && coop::session_ok()).add("session_ended", coop::session_ended())
        .add("connected", coop::connected_players()).add("joined", coop::active() && explore::joined())
        .add("main_menu", s.at_menu).add("in_battle", game::in_battle()).add("level", game::current_level())
        .add("fps", std::round(s.fps * 10.0) / 10.0)
        .add("allow_restart", cfg().allow_restart).add("allow_update", cfg().allow_update)
        .add("staged_update", staged)
        .raw("last_run", last_run_json());
    return o.str();
}

// --- Client : journaux et saves coop (liste fermée) -----------------------------------------------------------------

void add_file(std::vector<Item>& items, const fs::path& abs, const std::string& rel, uint64_t& total, std::vector<std::string>& notes)
{
    std::error_code ec;
    const uint64_t size = fs::file_size(abs, ec);
    if (ec || !safe_relative(rel)) return;
    if (size > kMaxFetchFile) { notes.push_back("trop gros, ignoré : " + rel); return; }
    if (total + size > kMaxFetchTotal) { notes.push_back("limite totale atteinte, ignoré : " + rel); return; }
    total += size;
    items.push_back({rel, abs, size, {}});
}

std::vector<Item> fetch_items(int count, std::vector<std::string>& notes)
{
    std::vector<Item> items;
    uint64_t total = 0;
    std::error_code ec;
    // 1. Derniers dossiers de journaux du mod (ue4ss/Mods/E33Coop/logs/<date-heure> ou remote-<date-heure>).
    std::vector<std::pair<fs::file_time_type, fs::path>> dirs;
    for (auto it = fs::directory_iterator(mod_dir() / L"logs", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) dirs.push_back({fs::last_write_time(it->path(), e2), it->path()});
    }
    std::sort(dirs.begin(), dirs.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (int i = 0; i < count && i < static_cast<int>(dirs.size()); ++i) {
        const fs::path& d = dirs[static_cast<size_t>(i)].second;
        const std::string dn = ue::to_utf8(d.filename().wstring());
        for (auto it = fs::recursive_directory_iterator(d, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || it.depth() > 2) continue;
            std::string rel = ue::to_utf8(fs::relative(it->path(), d, e2).generic_wstring());
            add_file(items, it->path(), "logs/" + dn + "/" + rel, total, notes);
        }
    }
    // 2. Journal d'UE4SS.
    add_file(items, mod_dir().parent_path().parent_path() / L"UE4SS.log", "UE4SS.log", total, notes);
    // Journal des assistants (relance, mise à jour, retour arrière).
    add_file(items, remote_dir() / L"helper.log", "remote/helper.log", total, notes);
    // 3. Saves coop seulement (SaveGames\e33coop\*.sav), jamais les saves perso (SaveGames\<SteamID>\…).
    if (const fs::path sd = saves_dir(); !sd.empty()) {
        for (auto it = fs::directory_iterator(sd / L"e33coop", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2) && it->path().extension() == L".sav")
                add_file(items, it->path(), "saves/e33coop/" + ue::to_utf8(it->path().filename().wstring()), total, notes);
        }
    }
    return items;
}

// --- Envoi des lots ------------------------------------------------------------------------------------------------

void finish_out(OutXfer& x, bool ok, const std::string& why)
{
    if (x.host_side) {
        auto it = s.reqs.find(x.req);
        if (it != s.reqs.end() && !ok) {
            it->second.finished = true;
            write_text(it->second.dir / L"state.json", JsonObj().add("state", "error").add("cmd", it->second.cmd).add("error", why).str());
        }
        log().write("remote_upload_end", JsonObj().add("req", x.req).add("ok", ok).add("why", why).add("bytes", x.sent));
        return;
    }
    std::string notes = "[";
    for (auto& n : x.notes) { if (notes.size() > 1) notes += ","; notes += "\"" + json_escape(n) + "\""; }
    reply(x.req, ok, JsonObj().add("files", static_cast<int64_t>(x.items.size())).add("bytes", x.sent).add("error", why).raw("notes", notes + "]").str());
}

void pump_out()
{
    if (s.out.empty()) return;
    OutXfer& x = s.out.front();
    if (x.last_progress == 0) x.last_progress = s.now;
    for (int n = 0; n < kChunksPerTick; ++n) {
        if (x.idx >= x.items.size()) {
            if (!x.done_sent) {
                Packet d;
                d.op = Op::Done;
                d.req = x.req;
                d.count = static_cast<uint32_t>(x.items.size());
                d.total = x.sent;
                std::vector<ManifestEntry> m;
                for (auto& it : x.items) m.push_back({it.rel, it.size, it.sha256});
                d.sha256 = manifest_hash(m);
                const int r = send(x.to, d);
                if (r == kResultLimitExceeded) break;
                x.done_sent = true;
            }
            finish_out(x, true, "");
            s.out.pop_front();
            return;
        }
        Item& it = x.items[x.idx];
        if (!x.loaded) {
            std::ifstream f(it.abs, std::ios::binary);
            x.buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            it.size = x.buf.size();   // le fichier a pu grossir (journal en cours)
            it.sha256 = sha256_hex(x.buf.data(), x.buf.size());
            x.off = 0;
            x.loaded = true;
        }
        Packet p;
        p.op = Op::File;
        p.req = x.req;
        p.path = it.rel;
        p.size = it.size;
        p.offset = x.off;
        if (x.off == 0) p.sha256 = it.sha256;
        const size_t k = static_cast<size_t>(std::min<uint64_t>(kChunk, it.size - x.off));
        p.data.assign(x.buf.begin() + static_cast<std::ptrdiff_t>(x.off), x.buf.begin() + static_cast<std::ptrdiff_t>(x.off + k));
        const int r = send(x.to, p);
        if (r != 1) {
            if (s.now - x.last_progress > kStall) { finish_out(x, false, "envoi bloqué (code Steam " + std::to_string(r) + ")"); s.out.pop_front(); }
            return;   // tampon plein : on réessaie au prochain tick
        }
        x.last_progress = s.now;
        x.off += k;
        x.sent += k;
        if (x.off >= it.size) { ++x.idx; x.loaded = false; x.buf.clear(); x.buf.shrink_to_fit(); }
    }
}

// --- Client : mise à jour reçue ---------------------------------------------------------------------------------------

fs::path staging() { return remote_dir() / L"staging"; }

void update_fail(const std::string& why)
{
    log().write("remote_update_failed", JsonObj().add("why", why));
    const uint32_t req = s.upd.req;
    s.upd = InUpdate{};
    std::error_code ec;
    fs::remove_all(staging(), ec);
    reply_error(req, why);
}

void update_file(const Packet& p)
{
    auto& u = s.upd;
    if (!u.active || p.req != u.req) return;
    if (!update_dest_allowed(p.path)) { update_fail("destination refusée : " + p.path); return; }
    auto& f = u.files[p.path];
    if (p.offset == 0 && !f.sha) {
        if (p.sha256.size() != 64) { update_fail("empreinte manquante : " + p.path); return; }
        f.size = p.size;
        f.sha_expected = p.sha256;
        f.sha = std::make_unique<Sha256>();
        const fs::path dst = rel_path(staging(), p.path);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        f.f.open(dst, std::ios::binary | std::ios::trunc);
        if (!f.f) { update_fail("écriture impossible dans staging : " + p.path); return; }
    }
    if (!f.sha || p.offset != f.got || p.size != f.size) { update_fail("morceau inattendu : " + p.path); return; }
    u.total += p.data.size();
    if (u.total > kMaxUpdateTotal || u.total > u.announced) { update_fail("taille dépassée"); return; }
    f.f.write(reinterpret_cast<const char*>(p.data.data()), static_cast<std::streamsize>(p.data.size()));
    f.sha->update(p.data.data(), p.data.size());
    f.got += p.data.size();
}

void update_done(const Packet& p)
{
    auto& u = s.upd;
    if (!u.active || p.req != u.req) return;
    std::vector<ManifestEntry> m;
    for (auto& [path, f] : u.files) {
        f.f.close();
        if (f.got != f.size) { update_fail("fichier incomplet : " + path); return; }
        const std::string h = f.sha->hex();
        if (h != f.sha_expected) { update_fail("empreinte fausse : " + path); return; }
        m.push_back({path, f.size, h});
    }
    if (m.empty() || p.count != m.size() || p.total != u.total) { update_fail("lot incomplet"); return; }
    if (manifest_hash(m) != p.sha256) { update_fail("empreinte du manifeste fausse"); return; }
    // Manifeste écrit en dernier : apply-update.ps1 ne fait rien sans lui (et revérifie chaque empreinte).
    if (!write_text(staging() / L"manifest.txt", manifest_text(m))) { update_fail("manifeste non écrit"); return; }
    const uint32_t req = u.req;
    s.upd = InUpdate{};
    log().write("remote_update_staged", JsonObj().add("files", static_cast<int64_t>(m.size())).add("sha256", p.sha256));
    std::string why, err;
    if (!quit_allowed(why)) { reply(req, true, JsonObj().add("staged", true).add("applied", false).add("error", why).str()); return; }
    if (!start_helper(L"apply-update.ps1", err)) { reply(req, false, JsonObj().add("staged", true).add("applied", false).add("error", err).str()); return; }
    reply(req, true, JsonObj().add("staged", true).add("applied", true).add("sha256", p.sha256).str());
    schedule_quit();
}

// --- Client : commandes (liste fermée) ------------------------------------------------------------------------------

void client_command(const Packet& p)
{
    const std::string& c = p.cmd;
    log().write("remote_command", JsonObj().add("req", p.req).add("cmd", c).add("arg_bytes", static_cast<int64_t>(p.arg.size())));
    ui::toast("Commande de test à distance : " + c, {}, 4.0);
    json::Value a;
    std::string err, why;
    if (!p.arg.empty() && c != "run") json::parse(p.arg, a, err);
    if (c == "status") { reply(p.req, true, status_json()); return; }
    if (c == "fetch-logs") {
        OutXfer x;
        x.to = cfg().host;
        x.req = p.req;
        x.items = fetch_items(static_cast<int>(std::clamp<int64_t>(a["count"].as_int(2), 1, 5)), x.notes);
        uint64_t total = 0;
        for (auto& i : x.items) total += i.size;
        reply(p.req, true, JsonObj().add("files", static_cast<int64_t>(x.items.size())).add("bytes", total).str(), false);
        s.out.push_back(std::move(x));
        return;
    }
    if (c == "run") {
        std::string sanitized;
        std::vector<std::string> notes;
        if (!validate_scenario(p.arg, sanitized, err, &notes)) { reply_error(p.req, "scénario refusé : " + err); return; }
        const std::string id = "remote-" + stamp();
        if (!write_text(remote_dir() / L"pending" / cmdline::widen(id + ".json"), sanitized)) { reply_error(p.req, "écriture du scénario impossible"); return; }
        std::string ns = "[";
        for (auto& n : notes) { if (ns.size() > 1) ns += ","; ns += "\"" + json_escape(n) + "\""; }
        JsonObj o;
        o.add("run_id", id).raw("notes", ns + "]");
        // Le scénario part au prochain lancement du jeu (comme -e33test) : relance tout de suite si l'ami l'a permis.
        if (!cfg().allow_restart) { reply(p.req, true, o.add("restarting", false).add("error", "relance à distance non autorisée : l'ami doit relancer le jeu").str()); return; }
        if (!quit_allowed(why)) { reply(p.req, true, o.add("restarting", false).add("error", why).str()); return; }
        if (!start_helper(L"relaunch.ps1", err)) { reply(p.req, true, o.add("restarting", false).add("error", err).str()); return; }
        reply(p.req, true, o.add("restarting", true).str());
        schedule_quit();
        return;
    }
    if (c == "menu") {
        if (s.at_menu) { reply(p.req, true, JsonObj().add("already", true).str()); return; }
        if (!quit_allowed(why)) { reply_error(p.req, why); return; }
        const bool ok = game::controller() && game::back_to_title();
        if (ok) reply(p.req, true, "{}");
        else reply_error(p.req, "retour au menu impossible ici");
        return;
    }
    if (c == "load-snapshot") {
        if (!s.at_menu) { reply_error(p.req, "seulement depuis le menu principal"); return; }
        if (coop::active() && coop::session_ok() && coop::connected_players() > 0) { reply_error(p.req, "session coop en cours"); return; }
        std::error_code ec;
        if (!fs::exists(saves_dir() / L"e33coop" / L"host.sav", ec)) { reply_error(p.req, "pas d'instantané coop (SaveGames\\e33coop\\host.sav)"); return; }
        if (!game::load_save_slot(L"e33coop/host", err)) { reply_error(p.req, err); return; }   // autosave coupée
        s.snapshot_loaded = true;
        reply(p.req, true, "{}");
        return;
    }
    if (c == "quit") {
        if (!quit_allowed(why)) { reply_error(p.req, why); return; }
        reply(p.req, true, JsonObj().add("quitting", true).str());
        schedule_quit();
        return;
    }
    if (c == "restart") {
        if (!cfg().allow_restart) { reply_error(p.req, "relance à distance non autorisée (install.ps1 -AllowRestart)"); return; }
        if (!quit_allowed(why)) { reply_error(p.req, why); return; }
        if (!start_helper(L"relaunch.ps1", err)) { reply_error(p.req, err); return; }
        reply(p.req, true, JsonObj().add("restarting", true).str());
        schedule_quit();
        return;
    }
    if (c == "update") {
        if (!cfg().allow_update) { reply_error(p.req, "mise à jour à distance non autorisée (install.ps1 -AllowUpdate)"); return; }
        const int64_t bytes = a["bytes"].as_int(0);
        if (bytes <= 0 || static_cast<uint64_t>(bytes) > kMaxUpdateTotal) { reply_error(p.req, "taille de mise à jour refusée"); return; }
        std::error_code ec;
        fs::remove_all(staging(), ec);
        fs::create_directories(staging(), ec);
        s.upd = InUpdate{};
        s.upd.active = true;
        s.upd.req = p.req;
        s.upd.announced = static_cast<uint64_t>(bytes);
        reply(p.req, true, JsonObj().add("ready", true).str(), false);
        return;
    }
    reply_error(p.req, "commande inconnue : " + c);
}

void client_tick()
{
    const uint64_t host = cfg().host;
    if (s.now >= s.next_accept) { s.next_accept = s.now + 1.0; net::steam::accept_user(host); }
    if (!s.indicator) {
        s.host_name = net::steam::friend_name(host);
        if (s.host_name.empty()) s.host_name = std::to_string(host);
        ui::set_status("remote", "Mode test à distance actif — piloté par " + s.host_name);
        s.indicator = true;
        log().write("remote_client_ready", JsonObj().add("host", std::to_string(host)).add("host_name", s.host_name)
                                               .add("restart", cfg().allow_restart).add("update", cfg().allow_update));
    }
    if (s.now >= s.next_hello) {
        s.next_hello = s.now + 5.0;
        Packet h;
        h.op = Op::Hello;
        h.mod_version = coop::mod_version();
        h.protocol = proto::kProtocolVersion;
        h.flags = (cfg().allow_restart ? kFlagRestart : 0) | (cfg().allow_update ? kFlagUpdate : 0) | (cmdline::test_mode() ? kFlagTestMode : 0);
        h.name = net::steam::persona_name();
        send(host, h);
    }
    std::vector<net::steam::RawMessage> in;
    net::steam::receive_raw(kSteamChannel, in);
    for (auto& m : in) {
        if (m.from != host) {   // seul l'hôte de confiance est écouté
            if (++s.dropped_untrusted <= 5) log().write("remote_untrusted", JsonObj().add("from", std::to_string(m.from)));
            continue;
        }
        std::string err;
        auto p = decode(m.data.data(), m.data.size(), err);
        if (!p) { log().write("remote_invalid", JsonObj().add("why", err)); continue; }
        if (p->op == Op::Command) client_command(*p);
        else if (p->op == Op::File) update_file(*p);
        else if (p->op == Op::Done) update_done(*p);
    }
}

// --- Hôte : boîte de commandes ---------------------------------------------------------------------------------------

void host_state(HostReq& r, const char* state, const std::string& extra_key = "", const std::string& extra_raw = "")
{
    JsonObj o;
    o.add("state", state).add("cmd", r.cmd).add("peer", std::to_string(r.peer)).add("files", r.files).add("bytes", r.bytes);
    if (!extra_key.empty()) o.raw(extra_key, extra_raw);
    write_text(r.dir / L"state.json", o.str());
    r.last_state_write = s.now;
}

// Mise à jour : fichiers préparés par remote.ps1 dans remote/updates/<id>/ (disposition du dossier Win64).
bool update_items(const std::string& update_id, std::vector<Item>& items, std::string& err)
{
    if (!safe_id(update_id)) { err = "update_id invalide"; return false; }
    const fs::path root = remote_dir() / L"updates" / cmdline::widen(update_id);
    std::error_code ec;
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string rel = ue::to_utf8(fs::relative(it->path(), root, e2).generic_wstring());
        if (!update_dest_allowed(rel)) { err = "fichier hors liste : " + rel; return false; }
        const uint64_t size = fs::file_size(it->path(), e2);
        total += size;
        items.push_back({rel, it->path(), size, {}});
    }
    if (ec) { err = "dossier de mise à jour illisible"; return false; }
    if (items.empty()) { err = "mise à jour vide"; return false; }
    if (total > kMaxUpdateTotal) { err = "mise à jour trop grosse"; return false; }
    return true;
}

void host_command_file(const fs::path& file)
{
    const std::string text = read_text(file, kMaxArg + 4096);
    std::error_code ec;
    json::Value v;
    std::string err;
    const bool parsed = json::parse(text, v, err) && v.is_object();
    std::string id = parsed ? v["id"].as_string("") : "";
    if (!safe_id(id)) id = "cmd-" + stamp() + "-" + std::to_string(s.next_req);
    HostReq r;
    r.id = id;
    r.dir = remote_dir() / L"outbox" / cmdline::widen(id);
    fs::create_directories(r.dir, ec);
    fs::rename(file, r.dir / L"command.json", ec);
    if (ec) fs::remove(file, ec);
    r.cmd = parsed ? v["cmd"].as_string("") : "";
    r.peer = parsed ? remotecfg::parse_steam_id(v["peer"].as_string("")) : 0;
    r.last_activity = s.now;
    static const char* cmds[] = {"status", "fetch-logs", "run", "menu", "load-snapshot", "quit", "restart", "update"};
    const bool known = std::any_of(std::begin(cmds), std::end(cmds), [&](const char* c) { return r.cmd == c; });
    auto fail = [&](const std::string& why) {
        write_text(r.dir / L"state.json", JsonObj().add("state", "error").add("cmd", r.cmd).add("error", why).str());
        log().write("remote_host_command_error", JsonObj().add("id", id).add("why", why));
    };
    if (!parsed) { fail("commande illisible : " + err); return; }
    if (!known) { fail("commande inconnue : " + r.cmd); return; }
    if (!r.peer) { fail("peer : SteamID64 attendu"); return; }
    if (!s.steam_ok) { fail("Steam indisponible dans le jeu"); return; }
    Packet p;
    p.op = Op::Command;
    p.req = ++s.next_req;
    p.cmd = r.cmd;
    std::vector<Item> items;
    if (r.cmd == "run") {
        p.arg = v["scenario_text"].as_string("");
        if (p.arg.empty() || p.arg.size() > kMaxScenario) { fail("scenario_text manquant ou trop gros"); return; }
    } else if (r.cmd == "update") {
        if (!update_items(v["update_id"].as_string(""), items, err)) { fail(err); return; }
        uint64_t total = 0;
        for (auto& i : items) total += i.size;
        p.arg = JsonObj().add("files", static_cast<int64_t>(items.size())).add("bytes", total).str();
    } else if (r.cmd == "fetch-logs") {
        p.arg = JsonObj().add("count", v["count"].as_int(2)).str();
    }
    s.peers.insert(r.peer);
    net::steam::accept_user(r.peer);
    const int code = send(r.peer, p);
    if (code != 1) { fail("envoi Steam refusé (code " + std::to_string(code) + ")"); return; }
    log().write("remote_host_command", JsonObj().add("id", id).add("req", p.req).add("cmd", r.cmd).add("peer", std::to_string(r.peer)));
    const uint32_t req = p.req;
    host_state(r, "sent");
    if (!items.empty()) {
        OutXfer x;
        x.to = r.peer;
        x.req = req;
        x.items = std::move(items);
        x.host_side = true;
        s.out.push_back(std::move(x));
    }
    s.reqs.emplace(req, std::move(r));
}

void host_packet(uint64_t from, const Packet& p)
{
    if (p.op == Op::Hello) {
        if (!s.peers.count(from)) return;   // seulement un pair à qui l'on a parlé
        write_text(remote_dir() / L"peers" / (std::to_wstring(from) + L".json"),
                   JsonObj().add("steam_id", std::to_string(from)).add("name", p.name).add("mod_version", p.mod_version)
                       .add("coop_protocol", static_cast<int>(p.protocol)).add("allow_restart", (p.flags & kFlagRestart) != 0)
                       .add("allow_update", (p.flags & kFlagUpdate) != 0).add("test_mode", (p.flags & kFlagTestMode) != 0).add("seen", stamp()).str());
        return;
    }
    auto it = s.reqs.find(p.req);
    if (it == s.reqs.end() || it->second.peer != from || it->second.finished) return;
    HostReq& r = it->second;
    r.last_activity = s.now;
    if (p.op == Op::Reply) {
        json::Value tmp;
        std::string err;
        const std::string body = json::parse(p.json, tmp, err) ? p.json : JsonObj().add("raw", p.json).str();
        if (!p.final) {
            write_text(r.dir / L"progress.json", body);
            host_state(r, "progress", "progress", body);
            return;
        }
        for (auto& [k, f] : r.open) if (f) f->close();
        r.open.clear();
        write_text(r.dir / L"reply.json", body);
        host_state(r, p.ok ? "done" : "error", "reply", body);
        r.finished = true;
        // Mise à jour refusée par le client : on arrête d'envoyer.
        for (auto x = s.out.begin(); x != s.out.end(); ++x)
            if (x->host_side && x->req == p.req) { s.out.erase(x); break; }
        log().write("remote_host_reply", JsonObj().add("id", r.id).add("ok", p.ok));
        return;
    }
    if (p.op == Op::File) {
        if (!safe_relative(p.path)) { log().write("remote_host_bad_path", JsonObj().add("path", p.path)); return; }
        if (r.bytes + p.data.size() > kMaxFetchTotal + kMaxFetchFile) return;
        auto& f = r.open[p.path];
        if (!f) {
            if (p.offset != 0) return;
            const fs::path dst = rel_path(r.dir / L"files", p.path);
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            f = std::make_unique<std::ofstream>(dst, std::ios::binary | std::ios::trunc);
            ++r.files;
        }
        f->seekp(static_cast<std::streamoff>(p.offset));
        f->write(reinterpret_cast<const char*>(p.data.data()), static_cast<std::streamsize>(p.data.size()));
        r.bytes += p.data.size();
        if (p.offset + p.data.size() >= p.size) { f->close(); f.reset(); r.open[p.path] = nullptr; }
        if (s.now - r.last_state_write > 1.0) host_state(r, "receiving");
        return;
    }
    if (p.op == Op::Done) {
        write_text(r.dir / L"files-done.json", JsonObj().add("count", p.count).add("total", p.total).add("manifest_sha256", p.sha256).str());
    }
}

void host_tick()
{
    if (s.now >= s.next_inbox) {
        s.next_inbox = s.now + 0.5;
        std::error_code ec;
        std::vector<fs::path> files;
        for (auto it = fs::directory_iterator(remote_dir() / L"inbox", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == L".json") files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (auto& f : files) host_command_file(f);
        for (auto& [req, r] : s.reqs) {
            if (r.finished) continue;
            bool sending = false;
            for (auto& x : s.out) if (x.host_side && x.req == req) sending = true;
            if (!sending && s.now - r.last_activity > kRequestTimeout) {
                r.finished = true;
                write_text(r.dir / L"state.json", JsonObj().add("state", "error").add("cmd", r.cmd).add("error", "pas de réponse du joueur (jeu fermé, Steam, ou mode test à distance désactivé chez lui)").str());
            }
        }
        for (uint64_t p : s.peers) net::steam::accept_user(p);
    }
    std::vector<net::steam::RawMessage> in;
    net::steam::receive_raw(kSteamChannel, in);
    for (auto& m : in) {
        std::string err;
        auto p = decode(m.data.data(), m.data.size(), err);
        if (p) host_packet(m.from, *p);
    }
}

} // namespace

bool client_enabled() { return cfg().host != 0; }
uint64_t trusted_host() { return cfg().host; }

uint64_t auto_join_lobby()
{
    if (!client_enabled() || !net::steam::loaded() || s.now < s.next_auto_join) return 0;
    s.next_auto_join = s.now + 5.0;
    if (!s.at_menu) return 0;
    const uint64_t lobby = net::steam::friend_lobby(cfg().host);
    if (lobby && lobby != s.last_lobby_logged) {
        s.last_lobby_logged = lobby;
        log().write("remote_auto_join", JsonObj().add("lobby", std::to_string(lobby)));
    }
    return lobby;
}

void on_test_exit()
{
    if (cmdline::remote_run().test.empty() || !cfg().allow_restart) return;
    std::string err;
    if (!start_helper(L"relaunch.ps1", err)) log().write("remote_relaunch_failed", JsonObj().add("why", err));
}

void tick(double dt)
{
    s.now += dt;
    if (dt > 0) s.fps = s.fps <= 0 ? 1.0 / dt : 0.95 * s.fps + 0.05 / dt;
    const bool client = client_enabled();
    if (s.now >= s.next_menu_check) {
        s.next_menu_check = s.now + 1.0;
        std::error_code ec;
        s.host_active = fs::is_directory(remote_dir() / L"inbox", ec);
        if (client) {
            const bool menu = game::main_menu() != nullptr;   // FindFirstOf parcourt tous les objets : une fois par seconde
            if (menu && !s.at_menu) s.snapshot_loaded = false;   // retour au menu : la partie suivante peut être la sienne
            s.at_menu = menu;
        }
    }
    if (!client && !s.host_active) return;
    if (!s.steam_ok) {
        if (s.now < s.next_steam_try) return;
        s.next_steam_try = s.now + 10.0;
        std::string err;
        s.steam_ok = net::steam::load(err);
        log().write("remote_steam", JsonObj().add("ok", s.steam_ok).add("error", err).add("client", client).add("host", s.host_active));
        if (!s.steam_ok) return;
    }
    if (client) client_tick();
    if (s.host_active) host_tick();
    pump_out();
    if (!coop::ready()) ui::tick(s.now);   // le coop affiche déjà les textes quand il tourne (mode neutre compris)
    if (s.quit_at >= 0 && s.now >= s.quit_at) {
        s.quit_at = -1;
        quit_game();
    }
}

} // namespace e33::remote
