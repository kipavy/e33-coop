#pragma once
// Options -e33... du mod. Partie normale : ligne de commande. Test (-e33test=<scénario>) : seules -e33test, -e33run et
// les options de "launch_args" du scénario comptent. Le jeu est lancé par Steam, qui ajoute les options de lancement
// du joueur (ex. -e33role=host -e33transport=steam) : vu le 2026-10-21, un test hôte UDP est parti en Steam.
// Test à distance (docs/REMOTE-TEST.md) : un scénario reçu de l'hôte de confiance tient lieu de -e33test au lancement
// suivant (remote_run).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "minijson.hpp"
#include "release.hpp"
#include "remotecfg.hpp"

namespace e33::cmdline {

inline const std::vector<std::wstring>& argv_all()
{
    static const std::vector<std::wstring> v = [] {
        std::vector<std::wstring> out;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 0; argv && i < argc; ++i) out.emplace_back(argv[i]);
        if (argv) LocalFree(argv);
        return out;
    }();
    return v;
}

// Dernière valeur de -<key>=... dans une liste d'arguments.
inline std::wstring find(const std::vector<std::wstring>& args, const wchar_t* key)
{
    const std::wstring k = std::wstring(L"-") + key + L"=";
    std::wstring out;
    for (auto& a : args)
        if (a.size() > k.size() && _wcsnicmp(a.c_str(), k.c_str(), k.size()) == 0) out = a.substr(k.size());
    if (out.size() >= 2 && out.front() == L'"' && out.back() == L'"') out = out.substr(1, out.size() - 2);
    return out;
}

inline std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Dossier du mod (…/ue4ss/Mods/E33Coop) : celui du DLL qui contient ce code (…/E33Coop/dlls/main.dll).
inline std::filesystem::path mod_dir_path()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&mod_dir_path), &self);
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(buf).parent_path().parent_path();
}

// Mode test à distance : remotetest.txt du mod, ou -e33remotetest=<SteamID> (lu sur la vraie ligne de commande, jamais
// dans un scénario). Les droits « restart » et « update » ne viennent que du fichier.
inline const remotecfg::Config& remote_config()
{
    static const remotecfg::Config c = [] {
        remotecfg::Config out;
        std::ifstream f(mod_dir_path() / L"remotetest.txt", std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            out = remotecfg::parse(ss.str());
        }
        if (!out.host) {
            const std::wstring id = find(argv_all(), L"e33remotetest");
            std::string a;
            for (wchar_t ch : id) a.push_back(ch < 128 ? static_cast<char>(ch) : '?');
            out.host = remotecfg::parse_steam_id(a);
        }
        return out;
    }();
    return c;
}

// Scénario envoyé par l'hôte de confiance (commande « run »), déposé par le mod dans remote/pending/remote-<date>.json :
// pris une seule fois au lancement suivant (déplacé dans logs/remote-<date>/scenario.json, qui devient le dossier du run).
struct RemoteRun { std::wstring test, run_dir; };
inline const RemoteRun& remote_run()
{
    static const RemoteRun r = [] {
        RemoteRun out;
        if (!find(argv_all(), L"e33test").empty() || !remote_config().host) return out;
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<fs::path> pending;
        for (auto it = fs::directory_iterator(mod_dir_path() / L"remote" / L"pending", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const fs::path& p = it->path();
            const std::wstring stem = p.stem().wstring();
            // Nom choisi par le mod lui-même : remote-AAAAMMJJ-HHMMSS
            const bool ok_name = p.extension() == L".json" && stem.size() == 22 && stem.rfind(L"remote-", 0) == 0 &&
                                 std::all_of(stem.begin() + 7, stem.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'-'; });
            std::error_code e2;
            if (ok_name && it->is_regular_file(e2)) pending.push_back(p);
        }
        if (pending.empty()) return out;
        std::sort(pending.begin(), pending.end());
        const fs::path run = mod_dir_path() / L"logs" / pending.back().stem();
        fs::create_directories(run, ec);
        const fs::path dst = run / L"scenario.json";
        fs::rename(pending.back(), dst, ec);   // consommé même si le jeu plante ensuite
        for (auto& p : pending) fs::remove(p, ec);   // plus anciens : abandonnés
        if (!fs::exists(dst, ec)) return out;
        out.test = dst.wstring();
        out.run_dir = run.wstring();
        return out;
    }();
    return r;
}

// Chemin du scénario de test : -e33test, sinon scénario reçu à distance.
inline std::wstring test_path()
{
    std::wstring p = find(argv_all(), L"e33test");
    return p.empty() ? remote_run().test : p;
}

// "launch_args" du scénario de test (vide hors test).
inline const std::vector<std::wstring>& test_args()
{
    static const std::vector<std::wstring> v = [] {
        std::vector<std::wstring> out;
        const std::wstring path = test_path();
        if (path.empty()) return out;
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        json::Value root;
        std::string err;
        if (!json::parse(ss.str(), root, err)) return out;
        const json::Value& la = root["launch_args"];
        for (size_t i = 0; i < la.size(); ++i) out.push_back(widen(la[i].as_string("")));
        return out;
    }();
    return v;
}

inline bool test_mode() { return !test_path().empty(); }

// --- Options du joueur : ue4ss/Mods/E33Coop/settings.ini (format : native/common/release.hpp) ---------------------
// Ordre : test -> options du scénario seulement ; sinon option de lancement -e33…, puis settings.ini, puis défaut.
inline std::filesystem::path settings_path() { return mod_dir_path() / L"settings.ini"; }

inline std::map<std::string, std::string>& settings_cache()
{
    static std::map<std::string, std::string> m;
    return m;
}
inline bool& settings_loaded()
{
    static bool b = false;
    return b;
}
inline std::string read_settings_text()
{
    std::ifstream f(settings_path(), std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline void reload_settings()
{
    settings_cache() = release::parse_settings(read_settings_text());
    settings_loaded() = true;
}
// Valeur effective d'une option du fichier (défaut si absente). Game thread.
inline std::string setting(const char* key)
{
    if (!settings_loaded()) reload_settings();
    return release::setting_or_default(settings_cache(), key);
}
// Écrit une option dans settings.ini (fichier créé avec ses commentaires s'il manque), puis le relit.
inline bool save_setting(const char* key, const std::string& value)
{
    std::string text = read_settings_text();
    if (text.empty()) text = release::default_settings_text();
    text = release::set_setting(text, key, value);
    const std::filesystem::path p = settings_path(), tmp = p.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    reload_settings();
    return !ec;
}

inline std::wstring arg(const wchar_t* key)
{
    if (_wcsicmp(key, L"e33test") == 0) return test_path();
    if (_wcsicmp(key, L"e33run") == 0) {
        const std::wstring r = find(argv_all(), key);
        return r.empty() && find(argv_all(), L"e33test").empty() ? remote_run().run_dir : r;
    }
    if (test_mode()) return find(test_args(), key);
    std::wstring v = find(argv_all(), key);
    if (!v.empty()) return v;
    for (const auto& d : release::kSettings) {
        if (_wcsicmp(key, widen(d.arg).c_str()) != 0) continue;
        const std::string s = setting(d.key);
        return s == "auto" && std::string_view(d.key) == "lang" ? std::wstring() : widen(s);
    }
    return v;
}

// Option venue de la ligne de commande (elle l'emporte sur settings.ini) : l'écran d'options le signale.
inline bool from_command_line(const char* settings_key)
{
    if (test_mode()) return true;
    for (const auto& d : release::kSettings)
        if (std::string_view(d.key) == settings_key) return !find(argv_all(), widen(d.arg).c_str()).empty();
    return false;
}

} // namespace e33::cmdline
