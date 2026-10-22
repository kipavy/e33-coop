#include "update.hpp"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <Unreal/UObjectGlobals.hpp>

#include "../common/cmdline.hpp"
#include "../common/https.hpp"
#include "../common/jsonlog.hpp"
#include "../common/release.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"
#include "coop.hpp"

using RC::Unreal::UObject;
namespace fs = std::filesystem;

namespace e33::update {

namespace {

constexpr const wchar_t* kInstaller = L"Installer E33 Coop.exe";
constexpr int64_t kRejoinMaxAge = 15 * 60;

struct S {
    bool enabled = false;
    double now = 0, next_menu = 0;
    bool at_menu = false;
    // Vérification (une fois par lancement, thread à part)
    std::atomic<int> check{0};                  // 0 : pas lancée, 1 : en cours, 2 : finie
    std::mutex mu;
    std::string tag, err;
    bool processed = false;
    std::string newer;                          // version plus récente publiée (vide : aucune connue)
    // auto : téléchargement + installation à la fermeture par l'installateur
    bool staged_launched = false, staged_toast = false;
    double next_ready_poll = 0;
    // Carte « U pour mettre à jour »
    std::string card_key;
    std::vector<std::string> card_args;
    bool card_shown = false, u_down = false;
    uint64_t rejoin_id = 0;
    double quit_at = -1;
    uint64_t rejoin_read = 0;
    bool rejoin_taken = false;
} s;

fs::path installer_path() { return cmdline::mod_dir_path() / kInstaller; }
fs::path rejoin_path() { return fs::path(game::saves_dir()) / L"e33coop" / L"rejoin.txt"; }

// Installateur du mod : chemin fixe, arguments fixes (aucune donnée venant du réseau sur la ligne de commande).
bool launch_installer(const wchar_t* mode)
{
    const fs::path p = installer_path();
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        ui::toast("update_no_installer", {}, 12.0);
        log().write("update_error", JsonObj().add("what", "installer missing"));
        return false;
    }
    std::wstring cmd = L"\"" + p.wstring() + L"\" " + mode;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Hors du job du jeu si possible : l'installateur doit survivre à la fermeture du jeu.
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, p.parent_path().c_str(), &si, &pi);
    if (!ok) ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, p.parent_path().c_str(), &si, &pi);
    if (!ok) {
        log().write("update_error", JsonObj().add("what", "CreateProcess " + std::to_string(GetLastError())));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log().write("update_installer_started", JsonObj().add("mode", ue::to_utf8(mode)));
    return true;
}

// Sortie normale du moteur (filet : fin forcée 15 s plus tard). Seulement au menu principal ou en rejoignant : rien à
// sauvegarder.
void quit_game()
{
    log().write("update_quit", JsonObj());
    if (coop::active()) coop::shutdown("update");
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

void write_rejoin(uint64_t id)
{
    if (!id) return;
    std::error_code ec;
    fs::create_directories(rejoin_path().parent_path(), ec);
    std::ofstream f(rejoin_path(), std::ios::binary | std::ios::trunc);
    f << "host=" << id << "\ntime=" << static_cast<int64_t>(std::time(nullptr)) << "\n";
}

void start_update_now(uint64_t rejoin)
{
    if (!launch_installer(L"--update")) return;
    write_rejoin(rejoin);
    s.quit_at = s.now + 3.0;
}

void stage()
{
    if (s.staged_launched) return;
    s.staged_launched = launch_installer(L"--stage");
}

void show_card(const std::string& key, const std::vector<std::string>& args, uint64_t rejoin)
{
    s.card_key = key;
    s.card_args = args;
    s.rejoin_id = rejoin;
}

} // namespace

const char* current_version() { return E33COOP_VERSION; }

std::string policy()
{
    const std::string p = ue::to_utf8(cmdline::arg(L"e33updates"));
    return p == "ask" || p == "off" ? p : "auto";
}

void init()
{
    s.enabled = !cmdline::test_mode();
    // Rejoindre après une mise à jour : SteamID de l'hôte écrit avant de quitter (lu une fois, 15 min au plus).
    std::ifstream f(rejoin_path());
    if (!f) return;
    std::string line;
    uint64_t id = 0;
    int64_t t = 0;
    while (std::getline(f, line)) {
        if (line.rfind("host=", 0) == 0) id = std::strtoull(line.c_str() + 5, nullptr, 10);
        if (line.rfind("time=", 0) == 0) t = std::strtoll(line.c_str() + 5, nullptr, 10);
    }
    f.close();
    std::error_code ec;
    fs::remove(rejoin_path(), ec);
    const int64_t age = static_cast<int64_t>(std::time(nullptr)) - t;
    if (s.enabled && id && age >= 0 && age < kRejoinMaxAge) s.rejoin_read = id;
    log().write("update_rejoin", JsonObj().add("host", std::to_string(id)).add("age_s", age).add("used", s.rejoin_read != 0));
}

uint64_t take_rejoin()
{
    if (s.rejoin_taken) return 0;
    s.rejoin_taken = true;
    return s.rejoin_read;
}

void on_version_mismatch(bool we_older, const std::string& other, const std::string& other_version, bool joining, uint64_t friend_id)
{
    const std::string pol = policy();
    log().write("update_version_mismatch", JsonObj().add("we_older", we_older).add("other", other).add("other_version", other_version)
                                               .add("joining", joining).add("policy", pol));
    const std::string ov = other_version.empty() ? "?" : other_version;
    if (!s.enabled) return;   // tests : journal seulement (textes de refus inchangés)
    if (!we_older) {
        ui::toast("update_need_other", {other, ov, E33COOP_VERSION}, 14.0);
        return;
    }
    if (joining && !game::controller()) {   // au menu principal : rien à sauvegarder, relance possible
        // auto : seulement si une release plus récente est publiée, et pas juste après une relance de mise à jour (pas de
        // boucle quand l'hôte a une version de développement plus récente que la dernière release).
        if (pol == "auto" && !s.newer.empty() && !s.rejoin_read) {
            start_update_now(friend_id);
            if (s.quit_at >= 0) show_card("update_now", {ov}, friend_id);
        } else if (pol != "off") {
            show_card("update_need_ours", {ov, E33COOP_VERSION}, friend_id);
        } else {
            ui::toast("update_need_ours_off", {ov, E33COOP_VERSION}, 14.0);
        }
        return;
    }
    // Hôte en pleine partie : jamais de relance au milieu du jeu.
    if (pol == "auto") { stage(); ui::toast("update_need_ours_host_auto", {other, ov, E33COOP_VERSION}, 14.0); }
    else if (pol == "ask") { show_card("update_need_ours", {ov, E33COOP_VERSION}, 0); ui::toast("update_need_ours_host", {other, ov, E33COOP_VERSION}, 14.0); }
    else ui::toast("update_need_ours_off", {ov, E33COOP_VERSION}, 14.0);
}

void tick(double dt)
{
    if (!s.enabled) return;
    s.now += dt;
    if (s.quit_at >= 0 && s.now >= s.quit_at) { s.quit_at = -1; quit_game(); return; }
    if (s.now >= s.next_menu) {
        s.next_menu = s.now + 1.0;   // FindFirstOf : pas à chaque image
        s.at_menu = !game::controller() && game::main_menu() != nullptr;
    }
    const std::string pol = policy();
    // Vérification : une fois par lancement, au premier passage au menu principal.
    if (s.at_menu && pol != "off" && s.check.load() == 0) {
        s.check = 1;
        log().write("update_check_start", JsonObj());
        std::thread([] {
            std::string err;
            const std::string tag = https::latest_tag(err, 6000);
            std::lock_guard<std::mutex> lk(s.mu);
            s.tag = tag;
            s.err = err;
            s.check = 2;
        }).detach();
    }
    if (s.check.load() == 2 && !s.processed) {
        s.processed = true;
        std::string tag, err;
        { std::lock_guard<std::mutex> lk(s.mu); tag = s.tag; err = s.err; }
        const bool newer = !tag.empty() && release::compare_versions(tag, E33COOP_VERSION) > 0;
        if (newer) s.newer = tag.substr(1);
        log().write("update_check", JsonObj().add("latest", tag).add("current", E33COOP_VERSION).add("newer", newer).add("error", err));   // erreurs réseau : ignorées
    }
    if (!s.newer.empty()) {
        if (pol == "auto") stage();
        else if (pol == "ask" && s.card_key.empty()) show_card("update_available", {s.newer}, 0);
    }
    // auto : l'installateur a téléchargé et vérifié la mise à jour (update/ready.txt) -> message, une fois.
    if (s.staged_launched && !s.staged_toast && s.now >= s.next_ready_poll) {
        s.next_ready_poll = s.now + 3.0;
        std::ifstream f(cmdline::mod_dir_path() / L"update" / L"ready.txt");
        std::string line, ver;
        while (std::getline(f, line)) if (line.rfind("version=", 0) == 0) ver = release::trim(line.substr(8));
        if (!ver.empty()) {
            s.staged_toast = true;
            ui::toast("update_staged", {ver}, 10.0);
            log().write("update_staged", JsonObj().add("version", ver));
        }
    }
    // Carte « U pour mettre à jour » : au menu principal seulement (rien à sauvegarder).
    const bool want = !s.card_key.empty() && s.at_menu;
    if (want) ui::set_status("update", s.card_key, s.card_args);
    else if (s.card_shown) ui::clear_status("update");
    s.card_shown = want;
    const bool down = (GetAsyncKeyState('U') & 0x8000) != 0;
    if (want && down && !s.u_down && s.card_key != "update_now") {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) {
            log().write("update_key", JsonObj().add("card", s.card_key));
            const std::string ver = s.card_key == "update_available" || s.card_args.empty() ? (s.newer.empty() ? std::string("?") : s.newer) : s.card_args.front();
            start_update_now(s.rejoin_id);
            if (s.quit_at >= 0) show_card("update_now", {ver}, s.rejoin_id);
        }
    }
    s.u_down = down;
}

} // namespace e33::update
