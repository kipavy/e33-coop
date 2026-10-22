#include "coopsave.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FStrProperty.hpp>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/minijson.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"
#include "explore.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

using namespace RC::Unreal;
namespace fs = std::filesystem;

namespace e33::coopsave {

namespace {

constexpr const wchar_t* kScratchSlot = L"e33coop/copy-scratch";   // instantané local avant copie (dossier vidé par les tests)
constexpr const wchar_t* kSaveMgr = L"/Game/Gameplay/Save/BP_SaveManager.BP_SaveManager_C:";

struct Copy { std::wstring stem; std::string host; int64_t saved_at = 0; std::string level; };

struct S {
    bool host = false;
    std::function<bool()> become_host;           // mode neutre : F10 fait de ce joueur l'hôte (coop.cpp)
    std::function<void(const char*, int)> bump;
    double now = 0, next_poll = 0;
    // Partie chargée depuis l'hôte ou depuis une copie : aucune écriture dans les emplacements perso.
    bool coop_game = false;
    bool seen_world = false;
    // Hôte : partie hébergée depuis une copie
    bool hosting = false;
    std::wstring host_stem;
    std::string host_name;
    bool save_wanted = false;                    // le jeu a voulu sauvegarder (autosave) : copie au prochain calme
    // Rafraîchissement
    double last_copy_at = -1e9;
    uint64_t last_copy_hash = 0;
    double min_interval = 60;                    // -e33copymin : écart minimal entre deux copies
    double every = 300;                          // -e33copyevery : copie au plus tard (hôte-copie, ou état inchangé)
    // Menu principal
    bool at_title = false;
    std::optional<Copy> latest;
    bool f10_down = false;
    bool can_host = true;                        // session hôte encore ouverte (finie après un retour au menu)
    bool ever_pawn = false;
} s;

void bump(const char* c, int n = 1) { if (s.bump) s.bump(c, n); }

std::wstring sub_dir() { return cmdline::test_mode() ? L"test-copies" : L"copies"; }   // tests : jamais les vraies copies
fs::path copies_dir() { return fs::path(game::saves_dir()) / L"e33coop" / sub_dir(); }
std::wstring slot_of(const std::wstring& stem) { return L"e33coop/" + sub_dir() + L"/" + stem; }

// Nom de fichier sûr (ASCII) à partir du nom affiché de l'hôte.
std::wstring sanitize(const std::string& name)
{
    std::wstring out;
    for (unsigned char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) out += static_cast<wchar_t>(c);
        else if (c < 0x80 || (c & 0xC0) == 0xC0) { if (out.empty() || out.back() != L'_') out += L'_'; }   // un '_' par caractère
        if (out.size() >= 40) break;
    }
    while (!out.empty() && out.back() == L'_') out.pop_back();
    while (!out.empty() && out.front() == L'_') out.erase(out.begin());
    return out.empty() ? L"coop" : out;
}

std::string host_label(const std::string& name) { return name.empty() || name == ui::text("host") ? std::string("hote") : name; }

std::string date_label(int64_t t)
{
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char b[32];
    std::strftime(b, sizeof b, "%d/%m %H:%M", &tm);
    return b;
}

bool write_file(const fs::path& p, const void* data, size_t n)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

// Écrit la copie (tmp puis renommage ; l'ancienne gardée en .prev.sav) et ses métadonnées.
bool write_copy(const std::wstring& stem, const std::vector<uint8_t>& bytes, const std::string& host, const std::string& level, const char* source)
{
    std::error_code ec;
    const fs::path dir = copies_dir();
    fs::create_directories(dir, ec);
    const fs::path dst = dir / (stem + L".sav"), tmp = dir / (stem + L".tmp"), prev = dir / (stem + L".prev.sav");
    if (bytes.size() < 4 || !write_file(tmp, bytes.data(), bytes.size())) {
        bump("coop_copy_errors");
        log().write("coop_copy_error", JsonObj().add("what", "write tmp").add("size", static_cast<int64_t>(bytes.size())));
        return false;
    }
    if (fs::exists(dst, ec)) fs::rename(dst, prev, ec);
    ec.clear();
    fs::rename(tmp, dst, ec);
    if (ec) {
        bump("coop_copy_errors");
        log().write("coop_copy_error", JsonObj().add("what", "rename: " + ec.message()));
        return false;
    }
    const int64_t t = static_cast<int64_t>(std::time(nullptr));
    const std::string meta = JsonObj().add("host", host).add("saved_at", t).add("level", level).add("source", source)
                                 .add("size", static_cast<int64_t>(bytes.size())).str();
    write_file(dir / (stem + L".json"), meta.data(), meta.size());
    s.last_copy_at = s.now;
    bump("coop_copies_saved");
    log().write("coop_copy_saved", JsonObj().add("file", ue::to_utf8(dst.wstring())).add("host", host).add("level", level).add("source", source)
                                       .add("size", static_cast<int64_t>(bytes.size())));
    return true;
}

std::optional<Copy> find_latest()
{
    std::optional<Copy> best;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(copies_dir(), ec)) {
        if (e.path().extension() != L".json") continue;
        const std::wstring stem = e.path().stem().wstring();
        if (!fs::exists(copies_dir() / (stem + L".sav"), ec)) continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        json::Value v;
        std::string err;
        if (!json::parse(ss.str(), v, err) || !v.is_object()) continue;
        Copy c{stem, v["host"].as_string("?"), v["saved_at"].as_int(0), v["level"].as_string("")};
        if (!best || c.saved_at > best->saved_at) best = c;
    }
    return best;
}

// Nouvelle copie depuis la partie en cours (CreateSaveGameDataFromGameState, comme l'instantané de l'hôte).
bool refresh(const std::wstring& stem, const std::string& host, const char* source)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> bytes;
    std::string err;
    if (!game::make_snapshot(kScratchSlot, bytes, err)) {
        bump("coop_copy_errors");
        log().write("coop_copy_error", JsonObj().add("what", "snapshot: " + err));
        return false;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    log().write("coop_copy_snapshot", JsonObj().add("ms", static_cast<int64_t>(ms)));
    return write_copy(stem, bytes, host, game::current_level(), source);
}

bool in_world() { return game::explore_pawn() && !game::in_battle() && !game::map_change_in_progress(); }

void reset_coop_game(const char* why)
{
    if (!s.coop_game && !s.hosting) return;
    log().write("coop_copy_guard_off", JsonObj().add("why", why).add("was_hosting", s.hosting));
    s.coop_game = false;
    s.hosting = false;
    s.save_wanted = false;
    s.seen_world = false;
}

// Chaîne et objet d'un appel hooké : par nom, sinon premier paramètre du bon type (les paramètres précèdent les locales).
FString* str_param(HookCtx& ctx, UFunction* f, const wchar_t* name)
{
    if (auto* p = static_cast<FString*>(ue::param_address(ctx, f, name))) return p;
    void* locals = ctx.TheStack.Locals();
    if (!f || !locals) return nullptr;
    for (FProperty* p : TFieldRange<FProperty>(f, EFieldIterationFlags::IncludeDeprecated))
        if (CastField<FStrProperty>(p)) return p->ContainerPtrToValuePtr<FString>(locals);
    return nullptr;
}

UObject** obj_param(HookCtx& ctx, UFunction* f)
{
    void* locals = ctx.TheStack.Locals();
    if (!f || !locals) return nullptr;
    for (FProperty* p : TFieldRange<FProperty>(f, EFieldIterationFlags::IncludeDeprecated))
        if (CastField<FObjectPropertyBase>(p)) return p->ContainerPtrToValuePtr<UObject*>(locals);
    return nullptr;
}

std::string str_of(const FString* fs_)
{
    if (!fs_) return {};
    const wchar_t* c = **fs_;
    return c ? ue::to_utf8(c) : std::string();
}

void title_changed(bool title)
{
    if (title == s.at_title) return;
    s.at_title = title;
    if (!title) { ui::clear_status("copy"); return; }
    s.latest = find_latest();
    if (!s.latest) return;
    const std::string when = date_label(s.latest->saved_at);
    log().write("coop_copy_available", JsonObj().add("host", s.latest->host).add("saved_at", s.latest->saved_at).add("level", s.latest->level));
    if ((s.host || s.become_host) && s.can_host) ui::set_status("copy", "copy_host_hint", {s.latest->host, when});
    else if (!s.host) ui::toast("copy_client_hint", {s.latest->host, when}, 12.0);
}

// À chaque image (un appui court tomberait entre deux relevés).
void key_tick()
{
    if (!s.at_title || !(s.host || s.become_host) || !s.can_host || !s.latest || s.hosting) { s.f10_down = false; return; }
    // F10 au menu principal (fenêtre du jeu au premier plan) : héberger la copie la plus récente.
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    DWORD pid = 0;
    if (down && !s.f10_down) GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool pressed = down && !s.f10_down && pid == GetCurrentProcessId();
    s.f10_down = down;
    if (!pressed) return;
    if (!s.host && !(s.become_host && s.become_host())) return;   // mode neutre : devient l'hôte (lobby créé)
    std::string err;
    if (!host_latest_copy(err)) {
        ui::toast("copy_error", {err}, 8.0);
        log().write("coop_copy_error", JsonObj().add("what", "host: " + err));
    }
}

} // namespace

void set_host(bool host) { s.host = host; }

void init(bool host, std::function<void(const char*, int)> b, std::function<bool()> become_host)
{
    s.host = host;
    s.bump = std::move(b);
    s.become_host = std::move(become_host);
    if (const std::wstring v = cmdline::arg(L"e33copymin"); !v.empty()) s.min_interval = _wtof(v.c_str());
    if (const std::wstring v = cmdline::arg(L"e33copyevery"); !v.empty()) s.every = _wtof(v.c_str());
    for (const char* c : {"coop_copies_saved", "coop_copies_from_host", "coop_copies_local", "coop_copy_errors", "coop_copy_hosted",
                          "coop_save_requests", "coop_autosave_reenabled", "coop_personal_saves_blocked"})
        bump(c, 0);
    log().write("coop_copy_init", JsonObj().add("dir", ue::to_utf8(copies_dir().wstring())).add("min_s", s.min_interval).add("every_s", s.every));
}

void subscribe()
{
    auto& bus = HookBus::get();
    // Le jeu veut sauvegarder alors que la partie vient de l'hôte (ou d'une copie) : autosave recoupée avant que la
    // fonction ne la consulte. Hôte-copie : on refait la copie au prochain calme (équivalent de l'autosave).
    for (const wchar_t* fn : {L"RequestAutoSave", L"ForceAutoSave", L"RequestSaveInternal", L"SaveGameToFile"}) {
        bus.pre(std::wstring(kSaveMgr) + fn, [fn](HookCtx& ctx, UFunction* f) {
            if (!s.coop_game) return;
            if (UObject* sm = game::save_manager()) ue::set_bool(sm, STR("EnableAutoSave"), false);
            if (s.hosting) s.save_wanted = true;
            bump("coop_save_requests");
            log().write("coop_save_request", JsonObj().add("fn", ue::to_utf8(fn)).add("arg", str_of(str_param(ctx, f, STR("DebugReason")))));
        });
    }
    // Filet : écriture d'un fichier de save hors e33coop pendant une partie coop -> objet de save retiré (l'écriture
    // échoue sans rien toucher). Ne doit jamais arriver (autosave coupée) ; non observé en jeu à ce jour.
    bus.pre(std::wstring(kSaveMgr) + L"AsyncSaveGameToSlot", [](HookCtx& ctx, UFunction* f) {
        if (!s.coop_game) return;
        const std::string slot = str_of(str_param(ctx, f, STR("SlotName")));
        if (slot.rfind("e33coop", 0) == 0) return;
        UObject** obj = obj_param(ctx, f);
        if (obj) *obj = nullptr;
        bump("coop_personal_saves_blocked");
        ui::toast("copy_personal_save_blocked", {}, 8.0);
        log().write("coop_personal_save_blocked", JsonObj().add("slot", slot).add("blocked", obj != nullptr));
    });
    // Chargement d'une autre save (menu du jeu) ou nouvelle partie : ce n'est plus la partie coop.
    bus.pre(std::wstring(kSaveMgr) + L"LoadGameFromSaveFile", [](HookCtx& ctx, UFunction* f) {
        const std::string slot = str_of(str_param(ctx, f, STR("SlotName")));
        if (slot.rfind("e33coop", 0) != 0) reset_coop_game("load personal save");
    });
    bus.pre(std::wstring(kSaveMgr) + L"StartNewGame", [](HookCtx&, UFunction*) { reset_coop_game("new game"); });   // suivie : remarquée juste après
}

void tick(double now, bool can_host)
{
    s.now = now;
    s.can_host = can_host;
    key_tick();
    if (now < s.next_poll) return;
    s.next_poll = now + 1.0;   // FindFirstOf parcourt tous les objets : pas à chaque image
    // Pas d'appel au jeu (ProcessEvent) avant le premier pion d'exploration (gels au démarrage, voir coop.cpp).
    if (!s.ever_pawn) s.ever_pawn = game::world_pawn() != nullptr;
    const bool ctrl = s.ever_pawn && game::controller() != nullptr;
    const bool title = !ctrl && game::main_menu();
    if (ctrl) s.seen_world = true;
    if (title && s.seen_world) reset_coop_game("title");
    title_changed(title);
    if (s.coop_game) {
        // Garde : l'autosave reste coupée tant que la partie vient de l'hôte ou d'une copie.
        UObject* sm = game::save_manager();
        if (sm && ue::get_bool(sm, STR("EnableAutoSave"))) {
            ue::set_bool(sm, STR("EnableAutoSave"), false);
            bump("coop_autosave_reenabled");
            log().write("coop_autosave_reenabled", JsonObj());
        }
    }
    // Hôte d'une copie : la progression va dans la copie (après une demande de sauvegarde du jeu, ou périodiquement).
    if (s.hosting && ctrl && explore::calm(8.0)) {
        const double since = now - s.last_copy_at;
        if ((s.save_wanted && since >= s.min_interval) || since >= s.every) {
            s.save_wanted = false;
            refresh(s.host_stem, s.host_name, "host_copy");
        }
    }
}

void mark_coop_game()
{
    if (!s.coop_game) log().write("coop_copy_guard_on", JsonObj());
    s.coop_game = true;
    s.seen_world = game::controller() != nullptr;
}

void on_host_snapshot(const std::vector<uint8_t>& bytes, const std::string& host_name, const std::string& level)
{
    if (s.host) return;
    mark_coop_game();
    const std::string name = host_label(host_name);
    if (write_copy(sanitize(name), bytes, name, level, "host_snapshot")) {
        s.last_copy_hash = 0;   // empreinte inconnue : la première comparaison réussie pourra rafraîchir
        bump("coop_copies_from_host");
    }
}

void on_world_ok(uint64_t host_hash, const std::string& host_name)
{
    if (s.host || !s.coop_game) return;
    const double since = s.now - s.last_copy_at;
    if (since < s.min_interval) return;
    if (host_hash == s.last_copy_hash && since < s.every) return;   // rien de neuf chez l'hôte
    const std::string name = host_label(host_name);
    if (refresh(sanitize(name), name, "local")) {
        s.last_copy_hash = host_hash;
        bump("coop_copies_local");
    }
}

void on_snapshot_sent(const std::vector<uint8_t>& bytes)
{
    if (!s.hosting) return;
    write_copy(s.host_stem, bytes, s.host_name, game::current_level(), "host_copy_snapshot");
}

void on_quit_to_title()
{
    if (!s.hosting || !in_world() || game::in_dialogue() || game::playing_cinematic()) return;
    refresh(s.host_stem, s.host_name, "host_copy_quit");
}

bool hosting_copy() { return s.hosting; }

bool host_latest_copy(std::string& err)
{
    if (!s.host) { err = "role is not host"; return false; }
    const auto c = find_latest();
    if (!c) { err = "no coop copy in " + ue::to_utf8(copies_dir().wstring()); return false; }
    s.coop_game = true;   // avant le chargement : aucune écriture perso dès le premier instant
    s.seen_world = false;
    if (!game::load_save_slot(slot_of(c->stem), err)) { s.coop_game = false; return false; }
    s.hosting = true;
    s.host_stem = c->stem;
    s.host_name = c->host;
    s.save_wanted = false;
    s.last_copy_at = s.now;
    bump("coop_copy_hosted");
    ui::clear_status("copy");
    ui::toast("copy_hosting", {c->host, date_label(c->saved_at)}, 10.0);
    log().write("coop_copy_hosting", JsonObj().add("slot", ue::to_utf8(slot_of(c->stem))).add("host", c->host).add("saved_at", c->saved_at).add("level", c->level));
    return true;
}

bool install_copy(const std::wstring& slot, const std::string& host_name, std::string& err)
{
    std::wstring rel = slot;
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    std::ifstream f(fs::path(game::saves_dir()) / (rel + L".sav"), std::ios::binary);
    if (!f) { err = "cannot read " + ue::to_utf8(slot); return false; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!write_copy(sanitize(host_name), bytes, host_name, "", "test")) { err = "write failed"; return false; }
    return true;
}

} // namespace e33::coopsave
