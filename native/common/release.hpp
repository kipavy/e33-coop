#pragma once
// Logique pure partagée par le mod, l'installateur et les tests (tools/tests/test_release.cpp) : versions, tags de
// release, SHA256SUMS, bibliothèques Steam (libraryfolders.vdf, appmanifest), hôtes de téléchargement autorisés,
// fichier d'options settings.ini. Sans dépendance Windows.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace e33::release {

constexpr const char* kRepo = "kipavy/e33-coop";
constexpr const char* kZipAsset = "e33coop-player.zip";
constexpr const char* kSumsAsset = "SHA256SUMS";
constexpr uint32_t kAppId = 1903340;

using Version = std::array<int, 3>;

// "0.6.0" ou "v0.6.0" (suffixe après un '-' ou '+' ignoré : "0.6.0-dev").
inline std::optional<Version> parse_version(std::string_view s)
{
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);
    Version v{};
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return std::nullopt;
        long n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { n = n * 10 + (s[i] - '0'); if (n > 1000000) return std::nullopt; ++i; }
        v[static_cast<size_t>(part)] = static_cast<int>(n);
        if (part < 2) { if (i >= s.size() || s[i] != '.') return std::nullopt; ++i; }
    }
    if (i < s.size() && s[i] != '-' && s[i] != '+') return std::nullopt;
    return v;
}

// <0 : a plus ancienne, 0 : égales, >0 : a plus récente. Version illisible : la plus ancienne.
inline int compare_versions(std::string_view a, std::string_view b)
{
    const auto va = parse_version(a), vb = parse_version(b);
    if (!va && !vb) return 0;
    if (!va) return -1;
    if (!vb) return 1;
    for (size_t i = 0; i < 3; ++i) if ((*va)[i] != (*vb)[i]) return (*va)[i] < (*vb)[i] ? -1 : 1;
    return 0;
}

// Tag de release accepté : vX.Y.Z strict (sert à construire l'URL de téléchargement).
inline bool valid_tag(std::string_view t)
{
    if (t.size() < 6 || t.size() > 32 || t.front() != 'v') return false;
    for (char c : t.substr(1)) if (!((c >= '0' && c <= '9') || c == '.')) return false;
    const auto v = parse_version(t);
    return v.has_value();
}

// Hôtes HTTPS autorisés (API, page de release, stockage des fichiers de release vers lequel github.com redirige).
inline bool allowed_host(std::string_view host)
{
    for (const char* h : {"api.github.com", "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"})
        if (host == h) return true;
    return false;
}

inline std::string to_lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// SHA256SUMS (format sha256sum : "<64 hex>  <nom>" ou "<64 hex> *<nom>") -> nom -> empreinte en minuscules.
inline std::map<std::string, std::string> parse_sha256sums(std::string_view text)
{
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (line.size() < 66) continue;
        const std::string hash = to_lower(std::string(line.substr(0, 64)));
        bool hex = true;
        for (char c : hash) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hex = false;
        if (!hex || (line[64] != ' ' && line[64] != '\t')) continue;
        std::string_view name = line.substr(65);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
        if (!name.empty() && name.front() == '*') name.remove_prefix(1);
        if (!name.empty()) out[std::string(name)] = hash;
    }
    return out;
}

// --- Format VDF de Steam (clés/valeurs entre guillemets, échappements \\ et \") ---------------------------------------
struct VdfToken { bool brace = false; char ch = 0; std::string str; };

inline std::vector<VdfToken> vdf_tokens(std::string_view t)
{
    std::vector<VdfToken> out;
    for (size_t i = 0; i < t.size();) {
        const char c = t[i];
        if (c == '{' || c == '}') { out.push_back({true, c, {}}); ++i; continue; }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') { while (i < t.size() && t[i] != '\n') ++i; continue; }
        if (c != '"') { ++i; continue; }
        std::string s;
        ++i;
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\' && i + 1 < t.size()) { s.push_back(t[i + 1]); i += 2; continue; }
            s.push_back(t[i++]);
        }
        ++i;
        out.push_back({false, 0, std::move(s)});
    }
    return out;
}

// Chemins des bibliothèques Steam (valeurs "path" de libraryfolders.vdf, toutes profondeurs).
inline std::vector<std::string> vdf_library_paths(std::string_view text)
{
    std::vector<std::string> out;
    const auto tk = vdf_tokens(text);
    for (size_t i = 0; i + 1 < tk.size(); ++i)
        if (!tk[i].brace && !tk[i + 1].brace && to_lower(tk[i].str) == "path" && !tk[i + 1].str.empty()) {
            out.push_back(tk[i + 1].str);
            ++i;
        }
    return out;
}

// Première valeur d'une clé (ex. "installdir", "buildid") dans un appmanifest_*.acf ; vide si absente.
inline std::string vdf_value(std::string_view text, std::string_view key)
{
    const auto tk = vdf_tokens(text);
    for (size_t i = 0; i + 1 < tk.size(); ++i)
        if (!tk[i].brace && !tk[i + 1].brace && to_lower(tk[i].str) == to_lower(std::string(key))) return tk[i + 1].str;
    return {};
}

// Dossier Win64 du jeu (…\Sandfall\Binaries\Win64) : bibliothèques Steam (racine + libraryfolders.vdf), celle qui a
// appmanifest_1903340.acf (dossier "installdir", sinon « Expedition 33 »). read(chemin) : contenu, vide si absent ;
// exists(chemin) : fichier présent. Chemins Windows ('\'), UTF-8. Vide si introuvable.
template <class Read, class Exists>
inline std::string find_game_win64(std::string steam_root, Read read, Exists exists)
{
    auto norm = [](std::string p) {
        for (char& c : p) if (c == '/') c = '\\';
        while (p.size() > 3 && p.back() == '\\') p.pop_back();
        return p;
    };
    if (steam_root.empty()) return {};
    steam_root = norm(steam_root);
    std::vector<std::string> libs{steam_root};
    for (const char* vdf : {"\\steamapps\\libraryfolders.vdf", "\\config\\libraryfolders.vdf"})
        for (auto& p : vdf_library_paths(read(steam_root + vdf))) {
            const std::string n = norm(p);
            bool dup = false;
            for (auto& l : libs) if (to_lower(l) == to_lower(n)) dup = true;
            if (!dup) libs.push_back(n);
        }
    const std::string exe = "\\Sandfall\\Binaries\\Win64\\SandFall-Win64-Shipping.exe";
    // D'abord la bibliothèque qui déclare le jeu (manifeste), puis le dossier par défaut dans chacune.
    for (auto& l : libs) {
        const std::string acf = read(l + "\\steamapps\\appmanifest_1903340.acf");
        if (acf.empty()) continue;
        std::string dir = vdf_value(acf, "installdir");
        if (dir.empty()) dir = "Expedition 33";
        const std::string game = l + "\\steamapps\\common\\" + dir;
        if (exists(game + exe)) return game + "\\Sandfall\\Binaries\\Win64";
    }
    for (auto& l : libs) {
        const std::string game = l + "\\steamapps\\common\\Expedition 33";
        if (exists(game + exe)) return game + "\\Sandfall\\Binaries\\Win64";
    }
    return {};
}

// --- settings.ini du mod (ue4ss/Mods/E33Coop/settings.ini) ---------------------------------------------------------
// Une option par ligne "clé = valeur", '#' ou ';' : commentaire. Les options de lancement -e33… restent prioritaires.
struct SettingDef { const char* key; const char* arg; const char* def; const char* values; };
constexpr SettingDef kSettings[] = {
    {"lang", "e33lang", "auto", "auto|fr|en"},
    {"updates", "e33updates", "auto", "auto|ask|off"},
    {"aim_cam", "e33aimcam", "1", "1|0"},
    {"turndep", "e33turndep", "1", "1|0"},
    {"duo_parry", "e33duoparry", "1", "1|0"},
    {"turn_timer_s", "e33turntimer", "0", "0|30|60|90"},
};

inline std::string trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\xEF' || s.front() == '\xBB' || s.front() == '\xBF')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return std::string(s);
}

// Valeur normalisée : booléens en 1/0, minuscules ; vide si invalide.
inline std::string normalize_setting(std::string_view key, std::string_view raw)
{
    std::string v = to_lower(trim(raw));
    for (const auto& d : kSettings) {
        if (key != d.key) continue;
        const std::string_view vals = d.values;
        if (vals == "1|0") {
            if (v == "1" || v == "on" || v == "true" || v == "yes" || v == "oui") return "1";
            if (v == "0" || v == "off" || v == "false" || v == "no" || v == "non") return "0";
            return {};
        }
        if (key == "turn_timer_s") {
            if (v.empty() || v.size() > 4) return {};
            for (char c : v) if (c < '0' || c > '9') return {};
            return std::to_string(std::stoi(v));
        }
        size_t p = 0;
        while (p <= vals.size()) {
            size_t e = vals.find('|', p);
            if (e == std::string_view::npos) e = vals.size();
            if (vals.substr(p, e - p) == v) return v;
            p = e + 1;
        }
        return {};
    }
    return {};
}

inline std::map<std::string, std::string> parse_settings(std::string_view text)
{
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = to_lower(trim(std::string_view(line).substr(0, eq)));
        std::string v = trim(std::string_view(line).substr(eq + 1));
        if (const size_t c = v.find_first_of("#;"); c != std::string::npos) v = trim(std::string_view(v).substr(0, c));
        const std::string n = normalize_setting(k, v);
        if (!n.empty()) out[k] = n;
    }
    return out;
}

inline std::string setting_or_default(const std::map<std::string, std::string>& m, std::string_view key)
{
    if (auto it = m.find(std::string(key)); it != m.end()) return it->second;
    for (const auto& d : kSettings) if (key == d.key) return d.def;
    return {};
}

inline std::string default_settings_text()
{
    return "# E33 Coop - options (aussi modifiables en jeu : F9 ; ou avec l'installateur : Options).\r\n"
           "# Une option par ligne. Les options de lancement Steam -e33... restent prioritaires.\r\n"
           "# lang : auto | fr | en          langue des textes du mod\r\n"
           "# updates : auto | ask | off     mises a jour depuis GitHub (auto : installee a la fermeture du jeu)\r\n"
           "# aim_cam : 1 | 0                visee libre de l'autre joueur vue depuis son epaule\r\n"
           "# turndep : 1 | 0                attente du tour de l'hote sans figer le jeu\r\n"
           "# duo_parry : 1 | 0              (hote) bonus parade en duo\r\n"
           "# turn_timer_s : 0 | 30 | 60 | 90  (hote) minuteur de tour, 0 = aucun\r\n"
           "lang = auto\r\nupdates = auto\r\naim_cam = 1\r\nturndep = 1\r\nduo_parry = 1\r\nturn_timer_s = 0\r\n";
}

// Remplace (ou ajoute) "key = value" en gardant commentaires et autres lignes.
inline std::string set_setting(std::string_view text, std::string_view key, std::string_view value)
{
    std::string out;
    bool done = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        const bool has_nl = end != std::string_view::npos;
        if (!has_nl) end = text.size();
        std::string_view raw = text.substr(pos, end - pos);
        pos = end + 1;
        const std::string line = trim(raw);
        const size_t eq = line.find('=');
        if (!done && !line.empty() && line[0] != '#' && line[0] != ';' && eq != std::string::npos &&
            to_lower(trim(std::string_view(line).substr(0, eq))) == key) {
            out += std::string(key) + " = " + std::string(value) + "\r\n";
            done = true;
            continue;
        }
        out.append(raw);
        if (has_nl) out += '\n';
    }
    if (!done) {
        if (!out.empty() && out.back() != '\n') out += "\r\n";
        out += std::string(key) + " = " + std::string(value) + "\r\n";
    }
    return out;
}

// Valeur suivante d'une option (cycle dans la liste des valeurs).
inline std::string next_setting_value(std::string_view key, std::string_view current)
{
    for (const auto& d : kSettings) {
        if (key != d.key) continue;
        std::vector<std::string> vals;
        std::string_view all = d.values;
        size_t p = 0;
        while (p <= all.size()) {
            size_t e = all.find('|', p);
            if (e == std::string_view::npos) e = all.size();
            vals.emplace_back(all.substr(p, e - p));
            p = e + 1;
        }
        for (size_t i = 0; i < vals.size(); ++i) if (vals[i] == current) return vals[(i + 1) % vals.size()];
        return vals.front();
    }
    return std::string(current);
}

} // namespace e33::release
