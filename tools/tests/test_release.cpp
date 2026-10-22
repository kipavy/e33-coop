// Tests unitaires de l'installation et des mises à jour (native/common/release.hpp) : versions, tags, SHA256SUMS,
// bibliothèques Steam et recherche du jeu, fichier settings.ini. Exécutés en CI via ctest.
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "../../native/common/release.hpp"

using namespace e33::release;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void test_versions()
{
    CHECK(parse_version("0.6.0").has_value());
    CHECK(parse_version("v1.2.3") == (Version{1, 2, 3}));
    CHECK(parse_version("1.2.3-dev") == (Version{1, 2, 3}));
    CHECK(!parse_version("1.2"));
    CHECK(!parse_version("1.2.x"));
    CHECK(!parse_version("1.2.3.4"));
    CHECK(!parse_version(""));
    CHECK(compare_versions("0.6.0", "0.5.9") > 0);
    CHECK(compare_versions("v0.6.0", "0.6.0") == 0);
    CHECK(compare_versions("0.10.0", "0.9.9") > 0);   // numérique, pas lexical
    CHECK(compare_versions("1.0.0", "0.99.99") > 0);
    CHECK(compare_versions("0.5.0", "0.5.1") < 0);
    CHECK(compare_versions("garbage", "0.0.1") < 0);   // illisible : plus ancienne
    CHECK(valid_tag("v0.6.0"));
    CHECK(valid_tag("v10.20.30"));
    CHECK(!valid_tag("0.6.0"));
    CHECK(!valid_tag("v0.6.0-rc1"));
    CHECK(!valid_tag("v0.6.0/../../x"));
    CHECK(!valid_tag("v0.6"));
}

static void test_hosts()
{
    CHECK(allowed_host("api.github.com"));
    CHECK(allowed_host("github.com"));
    CHECK(allowed_host("objects.githubusercontent.com"));
    CHECK(allowed_host("release-assets.githubusercontent.com"));
    CHECK(!allowed_host("evil.com"));
    CHECK(!allowed_host("github.com.evil.com"));
    CHECK(!allowed_host("raw.githubusercontent.com"));
}

static void test_sums()
{
    const std::string h1(64, 'a'), h2 = "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef";
    const auto m = parse_sha256sums(h1 + "  e33coop-player.zip\r\n" + h2 + " *Installer.E33.Coop.exe\n\nnot a line\n" + std::string(63, 'b') + "  short.zip\n");
    CHECK(m.size() == 2);
    CHECK(m.count("e33coop-player.zip") && m.at("e33coop-player.zip") == h1);
    CHECK(m.count("Installer.E33.Coop.exe") && m.at("Installer.E33.Coop.exe") == to_lower(h2));
    CHECK(!m.count("short.zip"));
    CHECK(parse_sha256sums(std::string(64, 'g') + "  x.zip\n").empty());   // pas hexadécimal
}

static const char* kVdf = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps"
		{
			"228980"		"0"
		}
	}
	"1"
	{
		"path"		"E:\\SteamLibrary"
		"apps"
		{
			"1903340"		"62000000000"
		}
	}
}
)";

static void test_vdf()
{
    const auto libs = vdf_library_paths(kVdf);
    CHECK(libs.size() == 2);
    CHECK(libs.size() == 2 && libs[0] == "C:\\Program Files (x86)\\Steam" && libs[1] == "E:\\SteamLibrary");
    const std::string acf = "\"AppState\"\n{\n\t\"appid\"\t\t\"1903340\"\n\t\"name\"\t\t\"Clair Obscur: Expedition 33\"\n\t\"installdir\"\t\t\"Expedition 33\"\n\t\"buildid\"\t\t\"23765773\"\n}\n";
    CHECK(vdf_value(acf, "installdir") == "Expedition 33");
    CHECK(vdf_value(acf, "buildid") == "23765773");
    CHECK(vdf_value(acf, "missing").empty());
}

static void test_find_game()
{
    // Jeu sur une autre bibliothèque (E:), Steam en minuscules avec des '/', comme dans le registre.
    std::map<std::string, std::string> files = {
        {"c:\\program files (x86)\\steam\\steamapps\\libraryfolders.vdf", kVdf},
        {"e:\\steamlibrary\\steamapps\\appmanifest_1903340.acf", "\"AppState\" { \"installdir\" \"Expedition 33\" }"},
    };
    std::set<std::string> exists = {"e:\\steamlibrary\\steamapps\\common\\expedition 33\\sandfall\\binaries\\win64\\sandfall-win64-shipping.exe"};
    auto read = [&](const std::string& p) { auto it = files.find(to_lower(p)); return it == files.end() ? std::string() : it->second; };
    auto ex = [&](const std::string& p) { return exists.count(to_lower(p)) > 0; };
    const std::string g = find_game_win64("c:/program files (x86)/steam", read, ex);
    CHECK(to_lower(g) == "e:\\steamlibrary\\steamapps\\common\\expedition 33\\sandfall\\binaries\\win64");
    // Manifeste absent : dossier par défaut trouvé quand même.
    files.erase("e:\\steamlibrary\\steamapps\\appmanifest_1903340.acf");
    CHECK(!find_game_win64("c:/program files (x86)/steam", read, ex).empty());
    // Jeu absent.
    exists.clear();
    CHECK(find_game_win64("c:/program files (x86)/steam", read, ex).empty());
    CHECK(find_game_win64("", read, ex).empty());
}

static void test_settings()
{
    const std::string def = default_settings_text();
    auto m = parse_settings(def);
    CHECK(m.size() == std::size(kSettings));
    CHECK(setting_or_default(m, "updates") == "auto");
    CHECK(setting_or_default({}, "duo_parry") == "1");
    m = parse_settings("\xEF\xBB\xBF# x\nlang = FR   # commentaire\nupdates=Off\naim_cam = non\nturn_timer_s = 45\nduo_parry = maybe\n[section]\nunknown = 1\n");
    CHECK(m["lang"] == "fr");
    CHECK(m["updates"] == "off");
    CHECK(m["aim_cam"] == "0");
    CHECK(m["turn_timer_s"] == "45");
    CHECK(!m.count("duo_parry"));   // invalide : défaut
    CHECK(!m.count("unknown"));
    std::string t = set_setting(def, "updates", "ask");
    CHECK(parse_settings(t)["updates"] == "ask");
    CHECK(t.find("# updates") != std::string::npos);   // commentaires gardés
    t = set_setting("# vide\n", "lang", "en");
    CHECK(parse_settings(t)["lang"] == "en");
    CHECK(next_setting_value("updates", "auto") == "ask");
    CHECK(next_setting_value("updates", "off") == "auto");
    CHECK(next_setting_value("turn_timer_s", "45") == "0");
    CHECK(next_setting_value("aim_cam", "1") == "0");
    CHECK(normalize_setting("turn_timer_s", "99999").empty());
}

int main()
{
    test_versions();
    test_hosts();
    test_sums();
    test_vdf();
    test_find_game();
    test_settings();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("release: all tests passed\n");
    return 0;
}
