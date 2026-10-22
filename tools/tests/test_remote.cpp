// Tests unitaires du test à distance (format des paquets, règles de sécurité, configuration). Exécutés en CI via ctest.
#include <cstdio>
#include <string>
#include <vector>

#include "../../native/common/minijson.hpp"
#include "../../native/common/remotecfg.hpp"
#include "../../native/remote/remote_proto.hpp"

using namespace e33;
using namespace e33::remote;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::optional<Packet> roundtrip(const Packet& p)
{
    const auto b = encode(p);
    std::string err;
    return decode(b.data(), b.size(), err);
}

static void test_packets()
{
    Packet h; h.op = Op::Hello; h.mod_version = "0.5.0"; h.protocol = 6; h.flags = kFlagRestart | kFlagUpdate; h.name = "Ami";
    auto r = roundtrip(h);
    CHECK(r && r->op == Op::Hello && r->mod_version == "0.5.0" && r->protocol == 6 && r->flags == 3 && r->name == "Ami");

    Packet c; c.op = Op::Command; c.req = 42; c.cmd = "run"; c.arg = std::string(70000, 'x');
    r = roundtrip(c);
    CHECK(r && r->req == 42 && r->cmd == "run" && r->arg.size() == 70000);

    Packet y; y.op = Op::Reply; y.req = 42; y.ok = true; y.final = false; y.json = "{\"a\":1}";
    r = roundtrip(y);
    CHECK(r && r->ok && !r->final && r->json == "{\"a\":1}");

    Packet f; f.op = Op::File; f.req = 7; f.path = "logs/20261022-210000/e33coop-client.jsonl"; f.size = 10; f.offset = 4; f.data = {1, 2, 3, 4, 5, 6};
    f.sha256 = std::string(64, 'a');
    r = roundtrip(f);
    CHECK(r && r->path == f.path && r->size == 10 && r->offset == 4 && r->data == f.data && r->sha256 == f.sha256);

    // Morceau qui dépasse la taille annoncée : refusé.
    Packet bad = f; bad.offset = 8;
    CHECK(!roundtrip(bad));

    Packet d; d.op = Op::Done; d.req = 7; d.count = 3; d.total = 123456789012ull; d.sha256 = std::string(64, 'b');
    r = roundtrip(d);
    CHECK(r && r->count == 3 && r->total == 123456789012ull);

    // Tronqué, mauvais magic, op inconnue : refusés sans planter.
    auto b = encode(c);
    std::string err;
    for (size_t n = 0; n < 20; ++n) CHECK(!decode(b.data(), n, err));
    b[0] ^= 0xFF;
    CHECK(!decode(b.data(), b.size(), err));
    auto u = encode(h);
    u[6] = 99;
    CHECK(!decode(u.data(), u.size(), err));

    // Version future : mêmes champs en tête, octets en plus ignorés (compatibilité entre versions du mod).
    auto fut = encode(h);
    fut[4] = 9;
    fut.push_back(0xAB); fut.push_back(0xCD);
    r = decode(fut.data(), fut.size(), err);
    CHECK(r && r->mod_version == "0.5.0" && r->flags == 3);
}

// Op::Lobby (version 2) : salon coop proposé par l'hôte de confiance au client du test à distance.
static void test_lobby()
{
    constexpr uint64_t host = 76561198355797267ull, other = 76561198045698677ull, lobby = 109775243312345678ull;
    Packet l; l.op = Op::Lobby; l.lobby = lobby; l.owner = host; l.mod_version = "0.6.3";
    auto r = roundtrip(l);
    CHECK(r && r->op == Op::Lobby && r->lobby == lobby && r->owner == host && r->mod_version == "0.6.3" && r->version == kVersion);
    CHECK(kVersion >= kVersionLobby);

    // Disposition fixe : en-tête (7 octets) + lobby u64 + owner u64 + chaîne (u16 + octets).
    const auto b = encode(l);
    CHECK(b.size() == 7 + 8 + 8 + 2 + 5);
    CHECK(b[6] == 6);
    std::string err;
    for (size_t n = 0; n < b.size(); ++n) CHECK(!decode(b.data(), n, err));   // tronqué : refusé

    // Version plus récente avec des champs en fin : en tête inchangés, reste ignoré.
    auto fut = b;
    fut[4] = 7;
    fut.push_back(1); fut.push_back(2); fut.push_back(3);
    r = decode(fut.data(), fut.size(), err);
    CHECK(r && r->lobby == lobby && r->owner == host && r->version == 7);

    // Hello d'un client v1 : version lue dans l'en-tête (l'hôte ne lui propose pas de salon).
    Packet h; h.op = Op::Hello; h.mod_version = "0.6.2";
    auto old = encode(h);
    old[4] = 1; old[5] = 0;
    r = decode(old.data(), old.size(), err);
    CHECK(r && r->op == Op::Hello && r->version == 1);
    // Le Hello garde sa disposition v1 (aucun champ ajouté) : un hôte v1 le lit toujours.
    CHECK(old.size() == 7 + 2 + 5 + 2 + 4 + 2);

    // Client : seulement du SteamID de confiance, qui se dit propriétaire, salon non nul.
    CHECK(lobby_offer_valid(l, host, host));
    CHECK(!lobby_offer_valid(l, other, host));            // autre compte
    CHECK(!lobby_offer_valid(l, host, 0));                // mode test à distance inactif
    CHECK(!lobby_offer_valid(l, 0, 0));
    Packet stolen = l; stolen.owner = other;
    CHECK(!lobby_offer_valid(stolen, host, host));        // salon d'un autre propriétaire
    Packet zero = l; zero.lobby = 0;
    CHECK(!lobby_offer_valid(zero, host, host));
    Packet notlobby = l; notlobby.op = Op::Command;
    CHECK(!lobby_offer_valid(notlobby, host, host));

    // Hôte : pair v2+, salon ouvert, pair pas déjà dedans.
    CHECK(should_offer_lobby(kVersionLobby, lobby, false));
    CHECK(should_offer_lobby(9, lobby, false));
    CHECK(!should_offer_lobby(1, lobby, false));          // ancien client : op inconnue chez lui
    CHECK(!should_offer_lobby(kVersionLobby, 0, false));  // pas de salon
    CHECK(!should_offer_lobby(kVersionLobby, lobby, true));
}

static void test_paths()
{
    CHECK(safe_relative("logs/20261022-210000/e33coop-client.jsonl"));
    CHECK(safe_relative("UE4SS.log"));
    CHECK(safe_relative("saves/e33coop/host.sav"));
    CHECK(!safe_relative(""));
    CHECK(!safe_relative("/etc/passwd"));
    CHECK(!safe_relative("../x"));
    CHECK(!safe_relative("a/../b"));
    CHECK(!safe_relative("a/./b"));
    CHECK(!safe_relative("a//b"));
    CHECK(!safe_relative("C:/Windows/x"));
    CHECK(!safe_relative("a\\b"));
    CHECK(!safe_relative("a/b."));
    CHECK(!safe_relative("con:x"));
    CHECK(!safe_relative("~/x"));
    CHECK(!safe_relative(std::string(300, 'a')));
    CHECK(!safe_relative("a/b/c/d/e/f/g/h/i"));

    CHECK(update_dest_allowed("dwmapi.dll"));
    CHECK(update_dest_allowed("ue4ss/UE4SS.dll"));
    CHECK(update_dest_allowed("ue4ss/Mods/E33Coop/dlls/main.dll"));
    CHECK(update_dest_allowed("ue4ss/Mods/E33Coop/enabled.txt"));
    CHECK(!update_dest_allowed("ue4ss/Mods/E33Coop/remotetest.txt"));
    CHECK(!update_dest_allowed("UE4SS/mods/e33coop/REMOTETEST.TXT"));
    CHECK(!update_dest_allowed("ue4ss/Mods/E33Coop/remote/relaunch.ps1"));
    CHECK(!update_dest_allowed("UE4SS/Mods/E33COOP/Remote/apply-update.ps1"));
    CHECK(!update_dest_allowed("ue4ss/Mods/E33Coop/logs/x.jsonl"));
    CHECK(!update_dest_allowed("ue4ss/Mods/Other/main.dll"));
    CHECK(!update_dest_allowed("ue4ss/UE4SS-settings.ini"));
    CHECK(!update_dest_allowed("SandFall-Win64-Shipping.exe"));
    CHECK(!update_dest_allowed("ue4ss/Mods/E33Coop/../../../x.dll"));
    CHECK(!update_dest_allowed("ue4ss/Mods/E33Coop"));

    CHECK(safe_id("20261022-210000-status"));
    CHECK(!safe_id("../x"));
    CHECK(!safe_id(""));
}

static void test_scenario()
{
    std::string out, err;
    std::vector<std::string> notes;
    const std::string client = R"({"name":"p6_world_client","join":true,"explore":[{"do":"wait","s":5}],
        "launch_args":["-e33role=client","-e33connect=127.0.0.1:7777","-e33join=1","-windowed"],
        "fakepeer":{"start":"before_game","args":["host"]},"expect":{"desyncs":"0"}})";
    CHECK(validate_scenario(client, out, err, &notes));
    json::Value v;
    CHECK(json::parse(out, v, err));
    const auto& la = v["launch_args"];
    std::vector<std::string> args;
    for (size_t i = 0; i < la.size(); ++i) args.push_back(la[i].as_string());
    CHECK((args == std::vector<std::string>{"-e33role=client", "-e33join=1", "-e33transport=steam"}));
    CHECK(v["fakepeer"].is_null());
    CHECK(v["join"].as_bool() && v["expect"]["desyncs"].as_string() == "0" && v["explore"][0]["s"].as_int() == 5);
    CHECK(notes.size() >= 3);

    CHECK(validate_scenario(R"({"save":"e33coop/host","encounter":"X"})", out, err));
    CHECK(validate_scenario(R"({"save":"e33coop-test/TEST_B","encounter":"X"})", out, err));
    CHECK(!validate_scenario(R"({"save":"76561198045698677/EXPEDITION_0","encounter":"X"})", out, err));
    CHECK(!validate_scenario(R"({"save":"e33coop/../76561198045698677/EXPEDITION_0"})", out, err));
    CHECK(!validate_scenario(R"({"save":"C:/x"})", out, err));
    CHECK(!validate_scenario(R"({"join":true,"launch_args":["-e33role=host"]})", out, err));
    CHECK(!validate_scenario(R"({"join":true,"launch_args":["-e33transport=udp"]})", out, err));
    CHECK(!validate_scenario(R"({"join":true,"launch_args":["-e33test=C:/x.json"]})", out, err));
    CHECK(!validate_scenario(R"({"join":true,"launch_args":["-e33run=C:/Windows"]})", out, err));
    CHECK(!validate_scenario(R"({"join":true,"launch_args":["-e33remotetest=76561198000000000"]})", out, err));
    CHECK(!validate_scenario(R"({"join":true,"explore":[{"do":"shell","cmd":"calc"}]})", out, err));
    CHECK(!validate_scenario(R"([1,2])", out, err));
    CHECK(!validate_scenario("{", out, err));
    CHECK(!validate_scenario(std::string(kMaxScenario + 1, ' '), out, err));
}

static void test_config()
{
    auto c = remotecfg::parse("# test\nhost=76561198355797267\nrestart=1\n");
    CHECK(c.host == 76561198355797267ull && c.allow_restart && !c.allow_update);
    c = remotecfg::parse("76561198355797267\r\n");
    CHECK(c.host == 76561198355797267ull && !c.allow_restart);
    c = remotecfg::parse("host=1234\nrestart=1\nupdate=1\n");
    CHECK(c.host == 0 && !c.allow_restart && !c.allow_update);
    c = remotecfg::parse("\xEF\xBB\xBFhost=76561198355797267\nupdate=true\n");
    CHECK(c.host == 76561198355797267ull && c.allow_update);
    CHECK(remotecfg::parse_steam_id("76561198045698677") == 76561198045698677ull);
    CHECK(remotecfg::parse_steam_id("7656119804569867x") == 0);
    CHECK(remotecfg::parse("").host == 0);
}

static void test_sha()
{
    CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256 h;
    h.update("a", 1);
    h.update("bc", 2);
    CHECK(h.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::vector<ManifestEntry> a = {{"ue4ss/UE4SS.dll", 3, "x"}, {"dwmapi.dll", 2, "y"}};
    std::vector<ManifestEntry> b = {a[1], a[0]};
    CHECK(manifest_hash(a) == manifest_hash(b));
    CHECK(manifest_text(a) == "y 2 dwmapi.dll\nx 3 ue4ss/UE4SS.dll\n");
}

int main()
{
    test_packets();
    test_lobby();
    test_paths();
    test_scenario();
    test_config();
    test_sha();
    std::printf(g_fail ? "%d FAILURE(S)\n" : "all remote tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
