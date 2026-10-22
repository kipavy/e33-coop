#pragma once
// Mode « test à distance » : configuration côté joueur (fichier ue4ss/Mods/E33Coop/remotetest.txt, écrit par
// install.ps1 -RemoteTest <SteamID>). Supprimer le fichier désactive tout. Sans dépendance (testé dans tools/tests).
//
// Format (une option par ligne, # = commentaire) :
//   host=76561198355797267     SteamID64 du seul hôte de confiance
//   restart=1                  autorise le redémarrage du jeu à distance (option -AllowRestart)
//   update=1                   autorise l'installation à distance d'une nouvelle version du mod (option -AllowUpdate)
// Une ligne qui ne contient qu'un SteamID vaut host=<id>.

#include <cstdint>
#include <string>
#include <string_view>

namespace e33::remotecfg {

struct Config {
    uint64_t host = 0;            // 0 = mode désactivé
    bool allow_restart = false;
    bool allow_update = false;
};

// SteamID64 d'un compte individuel : 17 chiffres, préfixe 7656119.
inline uint64_t parse_steam_id(std::string_view s)
{
    if (s.size() != 17 || s.substr(0, 7) != "7656119") return 0;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

inline std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n' || s.front() == '\xEF' || s.front() == '\xBB' || s.front() == '\xBF')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

inline Config parse(std::string_view text)
{
    Config c;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (const uint64_t id = parse_steam_id(line)) c.host = id;
            continue;
        }
        const std::string_view k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "host") c.host = parse_steam_id(v);
        else if (k == "restart") c.allow_restart = v == "1" || v == "true";
        else if (k == "update") c.allow_update = v == "1" || v == "true";
    }
    if (!c.host) c.allow_restart = c.allow_update = false;
    return c;
}

} // namespace e33::remotecfg
