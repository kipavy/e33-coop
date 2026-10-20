#pragma once
// Options -e33... du mod. Partie normale : ligne de commande. Test (-e33test=<scénario>) : seules -e33test, -e33run et
// les options de "launch_args" du scénario comptent. Le jeu est lancé par Steam, qui ajoute les options de lancement
// du joueur (ex. -e33role=host -e33transport=steam) : vu le 2026-10-21, un test hôte UDP est parti en Steam.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "minijson.hpp"

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

// "launch_args" du scénario de test (vide hors test).
inline const std::vector<std::wstring>& test_args()
{
    static const std::vector<std::wstring> v = [] {
        std::vector<std::wstring> out;
        const std::wstring path = find(argv_all(), L"e33test");
        if (path.empty()) return out;
        std::ifstream f(path, std::ios::binary);
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

inline bool test_mode() { return !find(argv_all(), L"e33test").empty(); }

inline std::wstring arg(const wchar_t* key)
{
    if (!test_mode() || _wcsicmp(key, L"e33test") == 0 || _wcsicmp(key, L"e33run") == 0) return find(argv_all(), key);
    return find(test_args(), key);
}

} // namespace e33::cmdline
