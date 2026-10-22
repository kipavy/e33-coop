// « Installer E33 Coop.exe » : installation, mise à jour, options et désinstallation du mod coop, en un seul fichier.
//  - sans argument : écran Installer / Mettre à jour / Options / Désinstaller (jeu trouvé tout seul par Steam) ;
//  - --update : lancé par le mod (touche U) : attend la fermeture du jeu, met à jour, relance le jeu par Steam ;
//  - --stage : lancé par le mod (option updates=auto), sans fenêtre : télécharge et vérifie la mise à jour, puis
//    l'installe à la fermeture du jeu.
// Paquet : dernière release GitHub de kipavy/e33-coop (e33coop-player.zip), téléchargé en HTTPS depuis github.com
// seulement, SHA-256 vérifié contre le SHA256SUMS de la même release ; ou un zip posé à côté de l'exe avec son
// SHA256SUMS (hors ligne). Seul le dossier Sandfall\Binaries\Win64 du jeu est modifié (dwmapi.dll + ue4ss\), l'ancien
// contenu est mis de côté dans Win64\e33coop-backup-<date> et remis en place si l'installation échoue.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../native/common/https.hpp"
#include "../../native/common/release.hpp"

namespace fs = std::filesystem;
using namespace e33;

namespace {

constexpr const wchar_t* kTitle = L"E33 Coop";
constexpr const wchar_t* kExeName = L"Installer E33 Coop.exe";
constexpr const wchar_t* kGameExe = L"SandFall-Win64-Shipping.exe";
constexpr size_t kMaxZip = 300u << 20;

bool g_fr = true;
const wchar_t* T(const wchar_t* fr, const wchar_t* en) { return g_fr ? fr : en; }

// --- Journal (%TEMP%\E33CoopInstaller.log) ---------------------------------------------------------------------------
std::mutex g_log_mu;
fs::path log_path()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return fs::path(tmp) / L"E33CoopInstaller.log";
}
std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
void logline(const std::string& m)
{
    std::lock_guard lk(g_log_mu);
    std::ofstream f(log_path(), std::ios::app | std::ios::binary);
    char ts[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    f << ts << " " << m << "\r\n";
}

// --- Petits utilitaires fichiers -------------------------------------------------------------------------------------
std::string read_file(const fs::path& p, size_t max = 1u << 20)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s;
    s.resize(max);
    f.read(s.data(), static_cast<std::streamsize>(max));
    s.resize(static_cast<size_t>(f.gcount()));
    return s;
}
bool write_file(const fs::path& p, const std::string& data)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::string sha256_hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char out[32]{};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        size_t off = 0;
        bool ok = true;
        while (off < data.size() && ok) {
            const ULONG n = static_cast<ULONG>(std::min<size_t>(data.size() - off, 1u << 24));
            ok = BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data() + off)), n, 0) == 0;
            off += n;
        }
        if (ok && BCryptFinishHash(h, out, sizeof out, 0) == 0) {
            static const char* d = "0123456789abcdef";
            for (unsigned char c : out) { hex.push_back(d[c >> 4]); hex.push_back(d[c & 15]); }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

fs::path self_path()
{
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

bool game_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, kGameExe) == 0) found = true;
    CloseHandle(snap);
    return found;
}

// --- Jeu -------------------------------------------------------------------------------------------------------------
std::wstring reg_steam_path()
{
    wchar_t buf[1024]{};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    return {};
}

bool is_win64(const fs::path& p)
{
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p / kGameExe, ec);
}

fs::path find_game()
{
    const std::string g = release::find_game_win64(utf8(reg_steam_path()),
        [](const std::string& p) { return read_file(wide(p), 4u << 20); },
        [](const std::string& p) { std::error_code ec; return fs::is_regular_file(wide(p), ec); });
    return g.empty() ? fs::path() : fs::path(wide(g));
}

fs::path mod_dir(const fs::path& win64) { return win64 / L"ue4ss" / L"Mods" / L"E33Coop"; }

std::string installed_version(const fs::path& win64)
{
    std::error_code ec;
    if (!fs::exists(mod_dir(win64) / L"dlls" / L"main.dll", ec)) return {};
    const std::string v = release::trim(read_file(mod_dir(win64) / L"version.txt", 64));
    return release::parse_version(v) ? v : std::string("?");
}

// --- Paquet : téléchargement, vérification, extraction -----------------------------------------------------------------
struct Progress {
    std::mutex mu;
    std::wstring text;
    int pct = -1;                 // -1 : barre animée
    std::atomic<bool> cancel{false};
    void set(const std::wstring& t, int p = -1) { std::lock_guard lk(mu); text = t; pct = p; }
};

struct Package {
    fs::path dir;                 // dossier extrait (contient game\)
    std::string version;
};

fs::path temp_root()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return fs::path(tmp) / L"E33CoopInstaller";
}

// Dernière release : tag vX.Y.Z (vide + err hors ligne).
std::string latest_tag(std::string& err) { return https::latest_tag(err, 10000); }

// Zip et SHA256SUMS d'une release (vérifiés). zip_out : contenu du zip.
bool download_release(const std::string& tag, std::string& zip_out, std::string& sums_out, Progress* pr, std::string& err)
{
    https::Options o;
    o.max_bytes = 64u << 10;
    o.timeout_ms = 15000;
    if (pr) { pr->set(T(L"Téléchargement de la liste des empreintes (SHA256SUMS)…", L"Downloading the checksum list (SHA256SUMS)…")); o.cancel = &pr->cancel; }
    if (!https::get(https::asset_url(tag, release::kSumsAsset), sums_out, err, o)) { err = "SHA256SUMS : " + err; return false; }
    const auto sums = release::parse_sha256sums(sums_out);
    const auto it = sums.find(release::kZipAsset);
    if (it == sums.end()) { err = "SHA256SUMS sans e33coop-player.zip"; return false; }
    https::Options z;
    z.max_bytes = kMaxZip;
    z.timeout_ms = 30000;
    if (pr) {
        z.cancel = &pr->cancel;
        z.progress = [pr, tag](uint64_t done, uint64_t total) {
            const std::wstring t = std::wstring(T(L"Téléchargement de la version ", L"Downloading version ")) + wide(tag.substr(1)) + L"… " +
                                   std::to_wstring(done >> 20) + L" / " + std::to_wstring(total >> 20) + L" Mo";
            pr->set(t, total ? static_cast<int>(done * 100 / total) : -1);
        };
    }
    if (!https::get(https::asset_url(tag, release::kZipAsset), zip_out, err, z)) { err = "e33coop-player.zip : " + err; return false; }
    if (pr) pr->set(T(L"Vérification de l'empreinte SHA-256…", L"Verifying the SHA-256 checksum…"));
    const std::string got = sha256_hex(zip_out);
    if (got != it->second) {
        logline("sha256 mismatch: expected " + it->second + " got " + got);
        err = "empreinte SHA-256 différente : fichier refusé";
        return false;
    }
    logline("downloaded " + tag + " sha256 " + got);
    return true;
}

// Zip local (hors ligne) vérifié contre un SHA256SUMS posé à côté.
bool verified_local_zip(const fs::path& zip, const fs::path& sums_file, std::string& zip_out, std::string& err)
{
    std::error_code ec;
    if (!fs::is_regular_file(zip, ec) || !fs::is_regular_file(sums_file, ec)) { err = "zip ou SHA256SUMS absent"; return false; }
    const auto sums = release::parse_sha256sums(read_file(sums_file, 64u << 10));
    const auto it = sums.find(release::kZipAsset);
    if (it == sums.end()) { err = "SHA256SUMS sans e33coop-player.zip"; return false; }
    zip_out = read_file(zip, kMaxZip);
    if (sha256_hex(zip_out) != it->second) { err = "empreinte SHA-256 différente : zip refusé"; return false; }
    return true;
}

bool run_wait(std::wstring cmd, DWORD& code)
{
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Extraction par tar.exe de Windows (10 1803+ ; refuse les chemins absolus et « .. »), dans un dossier neuf.
bool extract_zip(const std::string& zip, Package& pkg, std::string& err)
{
    std::error_code ec;
    const fs::path root = temp_root() / (L"pkg-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::remove_all(root, ec);
    fs::create_directories(root / L"x", ec);
    const fs::path zp = root / L"e33coop-player.zip";
    if (!write_file(zp, zip)) { err = "écriture du zip impossible dans " + utf8(root.wstring()); return false; }
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    DWORD code = 1;
    const std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zp.wstring() + L"\" -C \"" + (root / L"x").wstring() + L"\"";
    if (!run_wait(cmd, code) || code != 0) { err = "extraction du zip impossible (tar.exe, code " + std::to_string(code) + ")"; return false; }
    pkg.dir = root / L"x";
    for (const wchar_t* need : {L"game\\dwmapi.dll", L"game\\ue4ss\\UE4SS.dll", L"game\\ue4ss\\Mods\\mods.txt", L"game\\ue4ss\\Mods\\E33Coop\\dlls\\main.dll"})
        if (!fs::is_regular_file(pkg.dir / need, ec)) { err = "paquet incomplet : " + utf8(need) + " manque"; return false; }
    pkg.version = release::trim(read_file(pkg.dir / L"game\\ue4ss\\Mods\\E33Coop\\version.txt", 64));
    return true;
}

// --- Installation ----------------------------------------------------------------------------------------------------
std::wstring now_stamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t b[32];
    wcsftime(b, 32, L"%Y%m%d-%H%M%S", &tm);
    return b;
}

void wait_game_closed(Progress* pr)
{
    bool told = false;
    while (game_running()) {
        if (pr && pr->cancel) return;
        if (pr && !told) { pr->set(T(L"Le jeu est ouvert : fermez-le, la suite se fera toute seule.", L"The game is running: close it, the rest will happen on its own.")); told = true; }
        Sleep(1000);
    }
    if (told) Sleep(3000);   // le temps que le jeu libère ses fichiers
}

// Fichiers du joueur gardés d'une installation à l'autre (dossier du mod).
struct Kept { std::string settings, remotetest; };

bool copy_tree(const fs::path& from, const fs::path& to, std::string& err)
{
    std::error_code ec;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "copie de " + utf8(from.filename().wstring()) + " : " + ec.message(); return false; }
    return true;
}

void prune_backups(const fs::path& win64)
{
    std::vector<fs::path> b;
    std::error_code ec;
    for (auto it = fs::directory_iterator(win64, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec) && it->path().filename().wstring().rfind(L"e33coop-backup-", 0) == 0) b.push_back(it->path());
    std::sort(b.begin(), b.end());
    while (b.size() > 3) { fs::remove_all(b.front(), ec); logline("old backup removed: " + utf8(b.front().wstring())); b.erase(b.begin()); }
}

// Comme tools/package/install.ps1 : ancien dwmapi.dll / ue4ss mis de côté, paquet copié, options et test à distance gardés.
bool install_package(const fs::path& win64, const Package& pkg, Progress* pr, std::string& err)
{
    std::error_code ec;
    if (!is_win64(win64)) { err = "dossier du jeu invalide : " + utf8(win64.wstring()); return false; }
    wait_game_closed(pr);
    if (pr && pr->cancel) { err = "annulé"; return false; }
    if (pr) pr->set(T(L"Installation…", L"Installing…"));
    Kept kept;
    kept.settings = read_file(mod_dir(win64) / L"settings.ini", 64u << 10);
    kept.remotetest = read_file(mod_dir(win64) / L"remotetest.txt", 4096);
    // 1. Mise de côté
    std::vector<std::wstring> moved;
    const fs::path bak = win64 / (L"e33coop-backup-" + now_stamp());
    auto restore = [&] {
        fs::remove(win64 / L"dwmapi.dll", ec);
        fs::remove_all(win64 / L"ue4ss", ec);
        for (auto& n : moved) { std::error_code e2; fs::rename(bak / n, win64 / n, e2); }
        std::error_code e3;
        fs::remove(bak, e3);   // vide si tout est revenu
    };
    for (const wchar_t* n : {L"dwmapi.dll", L"ue4ss"}) {
        if (!fs::exists(win64 / n, ec)) continue;
        fs::create_directories(bak, ec);
        if (!MoveFileExW((win64 / n).c_str(), (bak / n).c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD e = GetLastError();
            for (auto& m : moved) { std::error_code e2; fs::rename(bak / m, win64 / m, e2); }
            fs::remove(bak, ec);
            err = utf8(std::wstring(T(L"Fichier en cours d'utilisation dans ", L"A file is in use in ")) + (win64 / n).wstring() +
                       T(L". Fermez le jeu (et son rapport de plantage), l'éditeur ou la fenêtre de l'Explorateur ouverte sur les journaux, puis réessayez.",
                         L". Close the game (and its crash reporter), any editor or Explorer window showing the logs, then try again.")) +
                  " (code " + std::to_string(e) + ")";
            return false;
        }
        moved.push_back(n);
    }
    // 2. Copie du paquet
    if (!copy_tree(pkg.dir / L"game", win64, err)) { restore(); return false; }
    // 3. Options du joueur et test à distance (seulement dans le dossier du mod)
    const fs::path md = mod_dir(win64);
    if (!write_file(md / L"settings.ini", kept.settings.empty() ? release::default_settings_text() : kept.settings)) { err = "settings.ini"; restore(); return false; }
    if (!kept.remotetest.empty()) {
        write_file(md / L"remotetest.txt", kept.remotetest);
        auto flag = [&](const char* k) {
            const std::string t = release::to_lower(kept.remotetest);
            for (const char* v : {"=1", "= 1", "=true", "= true"}) if (t.find(std::string(k) + v) != std::string::npos) return true;
            return false;
        };
        const bool restart = flag("restart"), update = flag("update");
        logline("remote test kept (restart " + std::to_string(restart) + ", update " + std::to_string(update) + ")");
    }
    // Assistants du test à distance toujours posés : inertes sans remotetest.txt (le mod vérifie les droits du fichier
    // avant de les lancer). Un remotetest.txt créé à la main après l'installation suffit donc.
    fs::create_directories(md / L"remote", ec);
    for (const wchar_t* n : {L"relaunch.ps1", L"apply-update.ps1", L"rollback.ps1"})
        fs::copy_file(pkg.dir / L"remote-test" / n, md / L"remote" / n, fs::copy_options::overwrite_existing, ec);
    prune_backups(win64);
    logline("installed " + pkg.version + " in " + utf8(win64.wstring()) + (moved.empty() ? "" : " (backup " + utf8(bak.wstring()) + ")"));
    return true;
}

bool uninstall(const fs::path& win64, Progress* pr, std::string& err)
{
    wait_game_closed(pr);
    if (pr && pr->cancel) { err = "annulé"; return false; }
    std::error_code ec;
    for (const wchar_t* n : {L"dwmapi.dll", L"ue4ss"}) {
        fs::remove_all(win64 / n, ec);
        if (ec) { err = utf8(std::wstring(T(L"Suppression impossible : ", L"Cannot delete: ")) + (win64 / n).wstring()) + " (" + ec.message() + ")"; return false; }
    }
    logline("uninstalled from " + utf8(win64.wstring()));
    return true;
}

// Paquet à installer : dernière release (téléchargée, vérifiée) ; hors ligne seulement, zip posé à côté de l'exe avec
// son SHA256SUMS (vérifié aussi).
bool obtain_package(const std::string& tag, Package& pkg, Progress* pr, std::string& err)
{
    std::string zip, sums;
    bool downloaded = false;
    if (!tag.empty()) downloaded = download_release(tag, zip, sums, pr, err);
    if (!downloaded) {
        if (pr && pr->cancel) return false;
        const fs::path here = self_path().parent_path();
        std::string lerr;
        if (!verified_local_zip(here / L"e33coop-player.zip", here / L"SHA256SUMS", zip, lerr)) {
            if (tag.empty()) err = "dernière version inconnue (hors ligne ?) et pas de zip vérifiable à côté de l'installateur (" + lerr + ")";
            return false;
        }
        logline("using the local zip next to the installer (" + (tag.empty() ? std::string("offline") : err) + ")");
        err.clear();
    }
    if (pr) pr->set(T(L"Décompression…", L"Unpacking…"));
    if (!extract_zip(zip, pkg, err)) return false;
    if (downloaded && release::compare_versions(pkg.version, tag) != 0) {
        err = "version du paquet (" + pkg.version + ") différente de la release " + tag;
        return false;
    }
    return true;
}

// --- Interface (TaskDialog) ------------------------------------------------------------------------------------------
struct Job {
    Progress pr;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string err;
    std::wstring shown_text;      // déjà affichés (fenêtre de progression)
    int shown_pct = -2;
};

HRESULT CALLBACK progress_cb(HWND h, UINT msg, WPARAM wp, LPARAM, LONG_PTR ref)
{
    auto* job = reinterpret_cast<Job*>(ref);
    if (msg == TDN_CREATED) {
        SendMessageW(h, TDM_SET_MARQUEE_PROGRESS_BAR, TRUE, 0);
        SendMessageW(h, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
    } else if (msg == TDN_TIMER) {
        std::wstring t;
        int pct;
        { std::lock_guard lk(job->pr.mu); t = job->pr.text; pct = job->pr.pct; }
        std::wstring& last = job->shown_text;
        int& last_pct = job->shown_pct;
        if (t != last) { SendMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(t.c_str())); last = t; }
        if (pct != last_pct) {
            if (pct < 0) { SendMessageW(h, TDM_SET_MARQUEE_PROGRESS_BAR, TRUE, 0); SendMessageW(h, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30); }
            else { SendMessageW(h, TDM_SET_MARQUEE_PROGRESS_BAR, FALSE, 0); SendMessageW(h, TDM_SET_PROGRESS_BAR_POS, pct, 0); }
            last_pct = pct;
        }
        if (job->done) SendMessageW(h, TDM_CLICK_BUTTON, IDCANCEL, 0);
    } else if (msg == TDN_BUTTON_CLICKED) {
        if (!job->done) { job->pr.cancel = true; job->pr.set(T(L"Annulation…", L"Cancelling…")); return S_FALSE; }
    }
    (void)wp;
    return S_OK;
}

// Travail en arrière-plan avec une fenêtre de progression (hidden : sans fenêtre).
bool run_job(const std::wstring& heading, std::function<bool(Progress&, std::string&)> work, std::string& err, bool hidden = false)
{
    Job job;
    job.pr.set(T(L"Préparation…", L"Preparing…"));
    std::thread th([&] { job.ok = work(job.pr, job.err); job.done = true; });
    if (hidden) th.join();
    else {
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof c;
        c.hInstance = GetModuleHandleW(nullptr);
        c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = kTitle;
        c.pszMainIcon = MAKEINTRESOURCEW(1);
        c.pszMainInstruction = heading.c_str();
        c.pszContent = L" ";
        c.pfCallback = progress_cb;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
        TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
        th.join();
    }
    err = job.err;
    if (!job.ok) logline("job failed: " + err);
    return job.ok;
}

void message(const std::wstring& heading, const std::wstring& text, bool error = false)
{
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hInstance = GetModuleHandleW(nullptr);
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = error ? TD_ERROR_ICON : MAKEINTRESOURCEW(1);
    c.pszMainInstruction = heading.c_str();
    c.pszContent = text.c_str();
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
}

bool confirm(const std::wstring& heading, const std::wstring& text)
{
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hInstance = GetModuleHandleW(nullptr);
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = TD_WARNING_ICON;
    c.pszMainInstruction = heading.c_str();
    c.pszContent = text.c_str();
    c.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    c.nDefaultButton = IDNO;
    int b = 0;
    TaskDialogIndirect(&c, &b, nullptr, nullptr);
    return b == IDYES;
}

fs::path pick_game_folder()
{
    fs::path out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(T(L"Dossier du jeu « Expedition 33 » (steamapps\\common\\Expedition 33)", L"Game folder \"Expedition 33\" (steamapps\\common\\Expedition 33)"));
    if (SUCCEEDED(d->Show(nullptr))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    if (out.empty()) return out;
    // Accepte le dossier du jeu, Sandfall, ou directement Win64.
    for (const fs::path& c : {out / L"Sandfall" / L"Binaries" / L"Win64", out / L"Binaries" / L"Win64", out})
        if (is_win64(c)) return c;
    message(T(L"Ce n'est pas le dossier du jeu", L"This is not the game folder"),
            T(L"Choisissez le dossier « Expedition 33 » dans steamapps\\common (il contient le dossier Sandfall).",
              L"Pick the \"Expedition 33\" folder in steamapps\\common (it contains the Sandfall folder)."), true);
    return {};
}

void relaunch_game()
{
    logline("relaunch steam://rungameid/1903340");
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/1903340", nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Écran des options (settings.ini du mod) ------------------------------------------------------------------------
std::wstring option_label(const char* key)
{
    const std::string_view k = key;
    if (k == "lang") return T(L"Langue du mod", L"Mod language");
    if (k == "updates") return T(L"Mises à jour", L"Updates");
    if (k == "aim_cam") return T(L"Visée de l'autre joueur vue depuis son épaule", L"Other player's aim seen from their shoulder");
    if (k == "turndep") return T(L"Attente du tour de l'hôte sans figer le jeu", L"Wait for the host's turn without freezing");
    if (k == "duo_parry") return T(L"Parade en duo (si vous êtes l'hôte)", L"Duo parry (when you host)");
    return T(L"Minuteur de tour (si vous êtes l'hôte)", L"Turn timer (when you host)");
}
std::wstring option_value(const char* key, const std::string& v)
{
    const std::string_view k = key;
    if (k == "lang") return v == "fr" ? L"Français" : v == "en" ? L"English" : T(L"auto (langue de Windows)", L"auto (Windows language)");
    if (k == "updates") return v == "ask" ? T(L"demander (touche U au menu)", L"ask (U key at the menu)")
                             : v == "off" ? T(L"désactivées", L"off") : T(L"automatiques (installées à la fermeture du jeu)", L"automatic (installed when the game closes)");
    if (k == "turn_timer_s") return v == "0" ? T(L"aucun", L"none") : wide(v) + L" s";
    return v == "1" ? T(L"oui", L"on") : T(L"non", L"off");
}

void options_screen(const fs::path& win64)
{
    const fs::path file = mod_dir(win64) / L"settings.ini";
    for (;;) {
        std::string text = read_file(file, 64u << 10);
        if (text.empty()) text = release::default_settings_text();
        const auto m = release::parse_settings(text);
        std::vector<std::wstring> labels;
        std::vector<TASKDIALOG_BUTTON> buttons;
        for (const auto& d : release::kSettings) labels.push_back(option_label(d.key) + L"\n" + option_value(d.key, release::setting_or_default(m, d.key)));
        for (size_t i = 0; i < labels.size(); ++i) buttons.push_back({static_cast<int>(100 + i), labels[i].c_str()});
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof c;
        c.hInstance = GetModuleHandleW(nullptr);
        c.pszWindowTitle = kTitle;
        c.pszMainIcon = MAKEINTRESOURCEW(1);
        c.pszMainInstruction = T(L"Options du mod coop", L"Coop mod options");
        c.pszContent = T(L"Cliquez sur une option pour changer sa valeur (enregistrée tout de suite dans settings.ini). Aussi en jeu : F9, puis Maj+F9.",
                         L"Click an option to change its value (saved at once in settings.ini). Also in game: F9, then Shift+F9.");
        c.dwFlags = TDF_USE_COMMAND_LINKS;
        c.pButtons = buttons.data();
        c.cButtons = static_cast<UINT>(buttons.size());
        c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        int b = 0;
        if (FAILED(TaskDialogIndirect(&c, &b, nullptr, nullptr)) || b < 100) return;
        const auto& d = release::kSettings[static_cast<size_t>(b - 100)];
        const std::string next = release::next_setting_value(d.key, release::setting_or_default(m, d.key));
        if (!write_file(file, release::set_setting(text, d.key, next))) { message(T(L"Enregistrement impossible", L"Cannot save"), file.wstring(), true); return; }
        logline(std::string("option ") + d.key + " = " + next);
    }
}

// --- Modes -----------------------------------------------------------------------------------------------------------
int mode_stage(const fs::path& win64)
{
    // Sans fenêtre : télécharge et vérifie, signale « prêt » au mod, puis installe à la fermeture du jeu.
    const fs::path upd = mod_dir(win64) / L"update";
    std::string err;
    const std::string tag = latest_tag(err);
    const std::string cur = installed_version(win64);
    if (tag.empty() || release::compare_versions(tag, cur) <= 0) { logline("stage: nothing to do (" + tag + " / " + cur + ") " + err); return 0; }
    std::string zip, sums;
    if (!download_release(tag, zip, sums, nullptr, err)) { write_file(upd / L"error.txt", err); logline("stage: " + err); return 1; }
    std::error_code ec;
    fs::create_directories(upd, ec);
    write_file(upd / L"e33coop-player.zip", zip);
    write_file(upd / L"SHA256SUMS", sums);
    write_file(upd / L"ready.txt", "version=" + tag.substr(1) + "\r\n");
    logline("stage: " + tag + " ready, waiting for the game to close");
    while (game_running()) Sleep(2000);
    Sleep(3000);
    // Zip relu depuis le dossier du mod et revérifié (il a pu changer depuis).
    std::string z2;
    if (!verified_local_zip(upd / L"e33coop-player.zip", upd / L"SHA256SUMS", z2, err)) { logline("stage: " + err); return 1; }
    Package pkg;
    if (!extract_zip(z2, pkg, err) || !install_package(win64, pkg, nullptr, err)) { logline("stage install failed: " + err); return 1; }
    fs::remove_all(pkg.dir.parent_path(), ec);
    return 0;
}

int mode_update(const fs::path& win64)
{
    std::string err, tag;
    Package pkg;
    bool updated = false;
    const bool ok = run_job(T(L"Mise à jour du mod coop", L"Updating the coop mod"), [&](Progress& pr, std::string& e) {
        pr.set(T(L"En attente de la fermeture du jeu…", L"Waiting for the game to close…"));
        wait_game_closed(&pr);
        if (pr.cancel) { e = "annulé"; return false; }
        pr.set(T(L"Recherche de la dernière version…", L"Looking for the latest version…"));
        tag = latest_tag(e);
        if (tag.empty()) return false;
        if (release::compare_versions(tag, installed_version(win64)) <= 0) return true;   // déjà à jour
        if (!obtain_package(tag, pkg, &pr, e) || !install_package(win64, pkg, &pr, e)) return false;
        updated = true;
        pr.set(std::wstring(T(L"Version ", L"Version ")) + wide(tag.substr(1)) + T(L" installée. Relance du jeu…", L" installed. Restarting the game…"), 100);
        Sleep(1500);
        return true;
    }, err);
    if (!ok) message(T(L"Mise à jour impossible", L"Update failed"), wide(err) + T(L"\n\nLe jeu garde la version installée.", L"\n\nThe game keeps the installed version."), true);
    std::error_code ec;
    if (!pkg.dir.empty()) fs::remove_all(pkg.dir.parent_path(), ec);
    (void)updated;
    relaunch_game();
    return ok ? 0 : 1;
}

int mode_interactive(fs::path win64)
{
    if (win64.empty()) win64 = find_game();
    std::string tag, terr;
    run_job(T(L"Installateur E33 Coop", L"E33 Coop installer"), [&](Progress& pr, std::string&) {
        pr.set(T(L"Recherche de la dernière version sur GitHub…", L"Looking for the latest version on GitHub…"));
        tag = latest_tag(terr);
        return true;
    }, terr);
    for (;;) {
        const std::string inst = win64.empty() ? std::string() : installed_version(win64);
        const std::string latest = tag.empty() ? std::string() : tag.substr(1);
        std::wstring content = std::wstring(T(L"Jeu : ", L"Game: ")) + (win64.empty() ? std::wstring(T(L"introuvable", L"not found")) : win64.wstring()) + L"\n";
        content += std::wstring(T(L"Installé : ", L"Installed: ")) + (inst.empty() ? std::wstring(T(L"non", L"no")) : wide(inst)) + L"\n";
        content += std::wstring(T(L"Dernière version : ", L"Latest version: ")) + (latest.empty() ? std::wstring(T(L"inconnue (hors ligne ?)", L"unknown (offline?)")) : wide(latest));
        if (game_running()) content += T(L"\n\nLe jeu est ouvert : fermez-le avant d'installer.", L"\n\nThe game is running: close it before installing.");
        std::wstring b_install = inst.empty() ? std::wstring(T(L"Installer", L"Install"))
                               : (!latest.empty() && release::compare_versions(latest, inst) > 0) ? std::wstring(T(L"Mettre à jour vers ", L"Update to ")) + wide(latest)
                               : std::wstring(T(L"Réinstaller", L"Reinstall"));
        if (!latest.empty() && inst.empty()) b_install += L" " + wide(latest);
        b_install += T(L"\nTélécharge depuis GitHub, vérifie l'empreinte SHA-256 et installe UE4SS + le mod dans le dossier du jeu",
                       L"\nDownloads from GitHub, verifies the SHA-256 checksum and installs UE4SS + the mod in the game folder");
        std::vector<TASKDIALOG_BUTTON> buttons;
        if (!win64.empty()) buttons.push_back({10, b_install.c_str()});
        if (!inst.empty()) buttons.push_back({11, T(L"Options\nLangue, mises à jour, réglages de jeu", L"Options\nLanguage, updates, gameplay settings")});
        if (!inst.empty()) buttons.push_back({12, T(L"Désinstaller\nRetire UE4SS et le mod du dossier du jeu (les sauvegardes ne sont pas touchées)",
                                                    L"Uninstall\nRemoves UE4SS and the mod from the game folder (saves are not touched)")});
        buttons.push_back({13, T(L"Choisir le dossier du jeu…", L"Choose the game folder…")});
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof c;
        c.hInstance = GetModuleHandleW(nullptr);
        c.pszWindowTitle = kTitle;
        c.pszMainIcon = MAKEINTRESOURCEW(1);
        c.pszMainInstruction = T(L"Mod coop en ligne pour Clair Obscur: Expedition 33", L"Online coop mod for Clair Obscur: Expedition 33");
        c.pszContent = content.c_str();
        c.dwFlags = TDF_USE_COMMAND_LINKS;
        c.pButtons = buttons.data();
        c.cButtons = static_cast<UINT>(buttons.size());
        c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        c.pszFooter = T(L"Mod non officiel · github.com/kipavy/e33-coop", L"Unofficial mod · github.com/kipavy/e33-coop");
        int b = 0;
        if (FAILED(TaskDialogIndirect(&c, &b, nullptr, nullptr))) return 1;
        if (b == 10) {
            std::string err;
            Package pkg;
            const bool ok = run_job(inst.empty() ? T(L"Installation du mod coop", L"Installing the coop mod") : T(L"Mise à jour du mod coop", L"Updating the coop mod"),
                                    [&](Progress& pr, std::string& e) { return obtain_package(tag, pkg, &pr, e) && install_package(win64, pkg, &pr, e); }, err);
            std::error_code ec;
            if (!pkg.dir.empty()) fs::remove_all(pkg.dir.parent_path(), ec);
            if (ok) message(T(L"Mod coop installé", L"Coop mod installed"),
                            std::wstring(T(L"Version ", L"Version ")) + wide(pkg.version) +
                            T(L" installée.\n\nLancez le jeu normalement par Steam, sans option de lancement.\n• Pour inviter : chargez votre partie, puis F7.\n"
                              L"• Pour rejoindre : au menu principal, « Rejoindre <ami> » ou F7, ou acceptez l'invitation Steam.\n• Options en jeu : F9.\n\n"
                              L"Si vous aviez mis des options de lancement -e33… (anciennes versions), retirez-les dans Steam.",
                              L" installed.\n\nStart the game normally from Steam, without launch options.\n• To invite: load your game, then F7.\n"
                              L"• To join: at the main menu, \"Join <friend>\" or F7, or accept the Steam invite.\n• In-game options: F9.\n\n"
                              L"If you had set -e33… launch options (older versions), remove them in Steam."));
            else if (err != "annulé") message(T(L"Installation impossible", L"Install failed"), wide(err) + T(L"\n\nRien n'a été changé dans le dossier du jeu.", L"\n\nNothing was changed in the game folder."), true);
        } else if (b == 11) {
            options_screen(win64);
        } else if (b == 12) {
            if (!confirm(T(L"Désinstaller le mod coop ?", L"Uninstall the coop mod?"),
                         T(L"UE4SS et le mod sont retirés du dossier du jeu. Vos sauvegardes et les copies coop ne sont pas touchées.",
                           L"UE4SS and the mod are removed from the game folder. Your saves and coop copies are not touched."))) continue;
            std::string err;
            const bool ok = run_job(T(L"Désinstallation", L"Uninstalling"), [&](Progress& pr, std::string& e) { return uninstall(win64, &pr, e); }, err);
            if (ok) message(T(L"Mod coop désinstallé", L"Coop mod uninstalled"), T(L"Le jeu est revenu à son état d'origine.", L"The game is back to its original state."));
            else if (err != "annulé") message(T(L"Désinstallation impossible", L"Uninstall failed"), wide(err), true);
        } else if (b == 13) {
            if (const fs::path p = pick_game_folder(); !p.empty()) win64 = p;
        } else {
            return 0;
        }
    }
}

// Lancé depuis le dossier du mod (par le jeu) : l'exe se copie dans %TEMP% et s'y relance, sinon il bloquerait la mise
// de côté du dossier ue4ss qui le contient. Arguments recopiés tels quels + --game <Win64> (chemin local).
bool relocate_if_needed(const std::vector<std::wstring>& args)
{
    const fs::path self = self_path();
    const fs::path md = self.parent_path();
    if (_wcsicmp(md.filename().c_str(), L"E33Coop") != 0 || _wcsicmp(md.parent_path().parent_path().filename().c_str(), L"ue4ss") != 0) return false;
    const fs::path win64 = md.parent_path().parent_path().parent_path();
    if (!is_win64(win64)) return false;
    std::error_code ec;
    const fs::path dir = temp_root() / (L"run-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir, ec);
    const fs::path copy = dir / kExeName;
    if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)) return false;
    std::wstring cmd = L"\"" + copy.wstring() + L"\"";
    for (size_t i = 1; i < args.size(); ++i) if (args[i] == L"--update" || args[i] == L"--stage") cmd += L" " + args[i];
    cmd += L" --game \"" + win64.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, dir.c_str(), &si, &pi) &&
        !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logline("relocated to " + utf8(copy.wstring()));
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    g_fr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 0; argv && i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    bool update = false, stage = false;
    fs::path game;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--update") update = true;
        else if (args[i] == L"--stage") stage = true;
        else if (args[i] == L"--game" && i + 1 < args.size()) game = args[++i];
    }
    // Paramètre du joueur : fichier settings.ini du mod (lang) pour la langue de l'installateur si présent.
    logline("start " + std::string(E33COOP_VERSION) + (update ? " --update" : "") + (stage ? " --stage" : "") + (game.empty() ? "" : " --game " + utf8(game.wstring())));
    if (game.empty() && relocate_if_needed(args)) return 0;   // exe du dossier du mod : copie dans %TEMP%
    if (!game.empty() && !is_win64(game)) game.clear();
    if (!game.empty()) {
        const std::string l = release::setting_or_default(release::parse_settings(read_file(mod_dir(game) / L"settings.ini", 64u << 10)), "lang");
        if (l == "fr") g_fr = true;
        else if (l == "en") g_fr = false;
    }
    // Une seule instance à la fois.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\E33CoopInstaller");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!stage) message(kTitle, T(L"L'installateur E33 Coop est déjà ouvert (ou une mise à jour attend la fermeture du jeu).",
                                      L"The E33 Coop installer is already running (or an update is waiting for the game to close)."));
        return 0;
    }
    int rc = 0;
    if (stage) rc = game.empty() ? 1 : mode_stage(game);
    else if (update) {
        if (game.empty()) game = find_game();
        rc = game.empty() ? 1 : mode_update(game);
        if (game.empty()) message(T(L"Jeu introuvable", L"Game not found"), T(L"Lancez l'installateur à la main.", L"Run the installer by hand."), true);
    } else rc = mode_interactive(game);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    CoUninitialize();
    return rc;
}
