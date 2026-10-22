#include "steam.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

namespace e33::net::steam {

namespace {

// --- Types Steamworks 1.57 (mêmes dispositions que steamnetworkingtypes.h / isteammatchmaking.h, Windows 64 bits) ---
#pragma pack(push, 1)
struct SteamNetworkingIdentity {
    int32_t type = 0;      // ESteamNetworkingIdentityType : 16 = SteamID
    int32_t size = 0;
    union {
        uint64_t steam_id64;
        uint8_t raw[128];
    };
};
#pragma pack(pop)
static_assert(sizeof(SteamNetworkingIdentity) == 136, "SteamNetworkingIdentity");

#pragma pack(push, 8)
struct SteamNetworkingMessage {
    void* data;
    int size;
    uint32_t conn;
    SteamNetworkingIdentity peer;
    int64_t conn_user_data;
    int64_t time_received;
    int64_t message_number;
    void (*free_data)(SteamNetworkingMessage*);
    void (*release)(SteamNetworkingMessage*);
    int channel;
    int flags;
    int64_t user_data;
    uint16_t lane;
    uint16_t pad;
};
struct LobbyCreated { int32_t result; uint64_t lobby; };                                          // 513
struct LobbyEnter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };       // 504
struct GameLobbyJoinRequested { uint64_t lobby; uint64_t friend_id; };                             // 333
struct FriendGameInfo { uint64_t game_id; uint32_t ip; uint16_t port; uint16_t query_port; uint64_t lobby; };
#pragma pack(pop)
static_assert(offsetof(SteamNetworkingMessage, peer) == 16, "message.peer");
static_assert(offsetof(SteamNetworkingMessage, conn_user_data) == 152, "message.conn_user_data");
static_assert(offsetof(SteamNetworkingMessage, release) == 184, "message.release");
static_assert(sizeof(SteamNetworkingMessage) == 216, "message size");
static_assert(sizeof(LobbyCreated) == 16 && sizeof(LobbyEnter) == 24 && sizeof(FriendGameInfo) == 24, "callback sizes");

constexpr int kIdentitySteamID = 16;
constexpr int kSendUnreliableNoNagle = 1;
constexpr int kSendAutoRestartBrokenSession = 32;
constexpr int kSendReliable = 8;
constexpr int kLobbyTypeFriendsOnly = 1;
constexpr int kCallbackLobbyCreated = 513;
constexpr int kCallbackLobbyEnter = 504;
constexpr int kCallbackGameLobbyJoinRequested = 333;
constexpr int kEnterSuccess = 1;
constexpr int kResultOK = 1;
constexpr int kFriendFlagImmediate = 0x04;

// Classe de base des callbacks Steam : même déclaration que steam_api.h (même ordre des virtuelles, même compilateur),
// donc même table virtuelle. Seul moyen d'être prévenu d'une invitation acceptée pendant que le jeu tourne.
class CCallbackBase {
public:
    CCallbackBase() = default;
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool io_failure, uint64_t call) = 0;
    int GetICallback() { return callback_; }
    virtual int GetCallbackSizeBytes() = 0;

protected:
    uint8_t flags_ = 0;
    int callback_ = 0;
};

std::atomic<uint64_t> g_join_request{0};

class JoinRequestCallback final : public CCallbackBase {
public:
    void Run(void* param) override { if (param) g_join_request = static_cast<GameLobbyJoinRequested*>(param)->lobby; }
    void Run(void* param, bool, uint64_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return sizeof(GameLobbyJoinRequested); }
};
JoinRequestCallback g_join_cb;

struct Api {
    bool ok = false;
    void* msgs = nullptr;
    void* user = nullptr;
    void* friends = nullptr;
    void* mm = nullptr;
    void* utils = nullptr;
    int32_t (*send)(void*, const SteamNetworkingIdentity*, const void*, uint32_t, int, int) = nullptr;
    int (*receive)(void*, int, SteamNetworkingMessage**, int) = nullptr;
    bool (*accept)(void*, const SteamNetworkingIdentity*) = nullptr;
    uint64_t (*get_steam_id)(void*) = nullptr;
    const char* (*friend_persona)(void*, uint64_t) = nullptr;
    const char* (*persona)(void*) = nullptr;
    void (*invite)(void*, uint64_t) = nullptr;
    int (*friend_count)(void*, int) = nullptr;
    uint64_t (*friend_by_index)(void*, int, int) = nullptr;
    bool (*friend_game)(void*, uint64_t, FriendGameInfo*) = nullptr;
    uint64_t (*create_lobby)(void*, int, int) = nullptr;
    uint64_t (*join_lobby)(void*, uint64_t) = nullptr;
    void (*leave_lobby)(void*, uint64_t) = nullptr;
    bool (*set_lobby_data)(void*, uint64_t, const char*, const char*) = nullptr;
    const char* (*get_lobby_data)(void*, uint64_t, const char*) = nullptr;
    int (*num_members)(void*, uint64_t) = nullptr;
    uint64_t (*member)(void*, uint64_t, int) = nullptr;
    uint64_t (*owner)(void*, uint64_t) = nullptr;
    bool (*call_done)(void*, uint64_t, bool*) = nullptr;
    bool (*call_result)(void*, uint64_t, void*, int, int, bool*) = nullptr;
    uint32_t (*app_id)(void*) = nullptr;
} api;

uint64_t g_lobby_call = 0;   // appel Steam en cours (création ou entrée de lobby)

template <class F>
bool resolve(HMODULE m, const char* name, F& out, std::string& err)
{
    out = reinterpret_cast<F>(GetProcAddress(m, name));
    if (!out) err = std::string("steam_api64.dll: ") + name + " introuvable";
    return out != nullptr;
}

SteamNetworkingIdentity identity(uint64_t id)
{
    SteamNetworkingIdentity i{};
    i.type = kIdentitySteamID;
    i.size = sizeof(uint64_t);
    i.steam_id64 = id;
    return i;
}

class SteamTransport final : public Transport {
public:
    bool send(const std::string& peer, const uint8_t* data, size_t len) override
    {
        const uint64_t id = id_of(peer);
        if (!id || !api.ok) return false;
        const SteamNetworkingIdentity to = identity(id);
        std::lock_guard lk(mu_);   // appelé aussi par le thread des battements « occupé »
        return api.send(api.msgs, &to, data, static_cast<uint32_t>(len), kSendUnreliableNoNagle | kSendAutoRestartBrokenSession, 0) == kResultOK;
    }

    bool poll(Datagram& out) override
    {
        if (!api.ok) return false;
        while (queue_.empty()) {
            SteamNetworkingMessage* msgs[32];
            const int n = api.receive(api.msgs, 0, msgs, 32);
            if (n <= 0) return false;
            for (int i = 0; i < n; ++i) {
                SteamNetworkingMessage* m = msgs[i];
                const auto* p = static_cast<const uint8_t*>(m->data);
                if (m->peer.type == kIdentitySteamID && m->size > 0 && !is_punch(p, static_cast<size_t>(m->size)))
                    queue_.push_back({peer_of(m->peer.steam_id64), std::vector<uint8_t>(p, p + m->size)});
                if (m->release) m->release(m);
            }
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    std::string local_address() const override { return peer_of(my_id()); }

private:
    std::mutex mu_;
    std::deque<Datagram> queue_;
};

} // namespace

bool load(std::string& err)
{
    if (api.ok) return true;
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    if (!m) { err = "steam_api64.dll non chargé (jeu lancé hors Steam ?)"; return false; }
    bool (*running)() = nullptr;
    void* (*msgs_v)() = nullptr;
    void* (*user_v)() = nullptr;
    void* (*friends_v)() = nullptr;
    void* (*mm_v)() = nullptr;
    void* (*utils_v)() = nullptr;
    void* (*netutils_v)() = nullptr;
    void (*init_relay)(void*) = nullptr;
    void (*register_cb)(void*, int) = nullptr;
    const bool ok =
        resolve(m, "SteamAPI_IsSteamRunning", running, err) &&
        resolve(m, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002", msgs_v, err) &&
        resolve(m, "SteamAPI_SteamUser_v021", user_v, err) &&
        resolve(m, "SteamAPI_SteamFriends_v017", friends_v, err) &&
        resolve(m, "SteamAPI_SteamMatchmaking_v009", mm_v, err) &&
        resolve(m, "SteamAPI_SteamUtils_v010", utils_v, err) &&
        resolve(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", netutils_v, err) &&
        resolve(m, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", init_relay, err) &&
        resolve(m, "SteamAPI_RegisterCallback", register_cb, err) &&
        resolve(m, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser", api.send, err) &&
        resolve(m, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", api.receive, err) &&
        resolve(m, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", api.accept, err) &&
        resolve(m, "SteamAPI_ISteamUser_GetSteamID", api.get_steam_id, err) &&
        resolve(m, "SteamAPI_ISteamFriends_GetPersonaName", api.persona, err) &&
        resolve(m, "SteamAPI_ISteamFriends_GetFriendPersonaName", api.friend_persona, err) &&
        resolve(m, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", api.invite, err) &&
        resolve(m, "SteamAPI_ISteamFriends_GetFriendCount", api.friend_count, err) &&
        resolve(m, "SteamAPI_ISteamFriends_GetFriendByIndex", api.friend_by_index, err) &&
        resolve(m, "SteamAPI_ISteamFriends_GetFriendGamePlayed", api.friend_game, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_CreateLobby", api.create_lobby, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_JoinLobby", api.join_lobby, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_LeaveLobby", api.leave_lobby, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_SetLobbyData", api.set_lobby_data, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_GetLobbyData", api.get_lobby_data, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", api.num_members, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", api.member, err) &&
        resolve(m, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", api.owner, err) &&
        resolve(m, "SteamAPI_ISteamUtils_IsAPICallCompleted", api.call_done, err) &&
        resolve(m, "SteamAPI_ISteamUtils_GetAPICallResult", api.call_result, err) &&
        resolve(m, "SteamAPI_ISteamUtils_GetAppID", api.app_id, err);
    if (!ok) return false;
    if (!running()) { err = "Steam ne tourne pas"; return false; }
    api.msgs = msgs_v();
    api.user = user_v();
    api.friends = friends_v();
    api.mm = mm_v();
    api.utils = utils_v();
    if (!api.msgs || !api.user || !api.friends || !api.mm || !api.utils) { err = "interfaces Steam indisponibles"; return false; }
    if (void* nu = netutils_v()) init_relay(nu);   // relais Steam prêts dès maintenant (première connexion plus rapide)
    register_cb(&g_join_cb, kCallbackGameLobbyJoinRequested);
    api.ok = true;
    return true;
}

bool loaded() { return api.ok; }
uint64_t my_id() { return api.ok ? api.get_steam_id(api.user) : 0; }
std::string persona_name()
{
    const char* n = api.ok ? api.persona(api.friends) : nullptr;
    return n ? n : "";
}
std::string peer_of(uint64_t id) { return "steam:" + std::to_string(id); }
uint64_t id_of(const std::string& peer)
{
    if (peer.rfind("steam:", 0) != 0) return 0;
    return std::strtoull(peer.c_str() + 6, nullptr, 10);
}

std::unique_ptr<Transport> make_transport(std::string& err)
{
    if (!load(err)) return nullptr;
    return std::make_unique<SteamTransport>();
}

bool is_punch(const uint8_t* data, size_t len) { return len == 4 && std::memcmp(data, "E33P", 4) == 0; }

void punch(Transport& t, uint64_t id)
{
    if (!api.ok || !id) return;
    const SteamNetworkingIdentity who = identity(id);
    api.accept(api.msgs, &who);   // session en attente de cet utilisateur : acceptée
    static const uint8_t p[4] = {'E', '3', '3', 'P'};
    t.send(peer_of(id), p, sizeof p);
}

void create_lobby(Lobby& l, const std::string& mod_version, int protocol)
{
    if (!api.ok) { l.state = LobbyState::Failed; l.error = "Steam indisponible"; return; }
    l = Lobby{};
    l.state = LobbyState::Creating;
    l.error = mod_version + "|" + std::to_string(protocol);   // gardé pour les données du lobby (une fois créé)
    g_lobby_call = api.create_lobby(api.mm, kLobbyTypeFriendsOnly, 3);
    if (!g_lobby_call) { l.state = LobbyState::Failed; l.error = "CreateLobby a échoué"; }
}

void join_lobby(Lobby& l, uint64_t lobby_id)
{
    if (!api.ok) { l.state = LobbyState::Failed; l.error = "Steam indisponible"; return; }
    l = Lobby{};
    l.state = LobbyState::Joining;
    l.id = lobby_id;
    g_lobby_call = api.join_lobby(api.mm, lobby_id);
    if (!g_lobby_call) { l.state = LobbyState::Failed; l.error = "JoinLobby a échoué"; }
}

void poll_lobby(Lobby& l)
{
    if (!api.ok || (l.state != LobbyState::Creating && l.state != LobbyState::Joining) || !g_lobby_call) return;
    bool failed = false;
    if (!api.call_done(api.utils, g_lobby_call, &failed)) return;
    if (l.state == LobbyState::Creating) {
        LobbyCreated r{};
        if (failed || !api.call_result(api.utils, g_lobby_call, &r, sizeof r, kCallbackLobbyCreated, &failed) || failed || r.result != kResultOK) {
            l.state = LobbyState::Failed;
            l.error = "création du lobby refusée (code " + std::to_string(r.result) + ")";
        } else {
            const std::string meta = l.error;
            l.error.clear();
            l.id = r.lobby;
            l.owner = my_id();
            l.state = LobbyState::Ready;
            const auto bar = meta.find('|');
            api.set_lobby_data(api.mm, l.id, "e33coop", "1");
            api.set_lobby_data(api.mm, l.id, "mod", meta.substr(0, bar).c_str());
            if (bar != std::string::npos) api.set_lobby_data(api.mm, l.id, "protocol", meta.substr(bar + 1).c_str());
        }
    } else {
        LobbyEnter r{};
        if (failed || !api.call_result(api.utils, g_lobby_call, &r, sizeof r, kCallbackLobbyEnter, &failed) || failed || r.response != kEnterSuccess) {
            l.state = LobbyState::Failed;
            l.error = "entrée dans le lobby refusée (code " + std::to_string(r.response) + ")";
        } else {
            l.id = r.lobby;
            l.owner = api.owner(api.mm, l.id);
            l.state = LobbyState::Ready;
        }
    }
    g_lobby_call = 0;
}

void leave_lobby(Lobby& l)
{
    if (api.ok && l.id) api.leave_lobby(api.mm, l.id);
    l = Lobby{};
}

std::vector<uint64_t> lobby_members(const Lobby& l)
{
    std::vector<uint64_t> out;
    if (!api.ok || l.state != LobbyState::Ready) return out;
    const int n = api.num_members(api.mm, l.id);
    for (int i = 0; i < n; ++i) out.push_back(api.member(api.mm, l.id, i));
    return out;
}

std::string lobby_data(uint64_t lobby_id, const char* key)
{
    const char* v = api.ok ? api.get_lobby_data(api.mm, lobby_id, key) : nullptr;
    return v ? v : "";
}

void invite_dialog(const Lobby& l)
{
    if (api.ok && l.state == LobbyState::Ready) api.invite(api.friends, l.id);
}

uint64_t command_line_lobby()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    uint64_t id = 0;
    for (int i = 0; argv && i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], L"+connect_lobby") == 0) id = _wcstoui64(argv[i + 1], nullptr, 10);
    if (argv) LocalFree(argv);
    return id;
}

uint64_t take_join_request() { return g_join_request.exchange(0); }

std::vector<FriendLobby> friend_lobbies(const std::string& mod_version, bool any_version)
{
    std::vector<FriendLobby> out;
    if (!api.ok) return out;
    const uint32_t app = api.app_id(api.utils);
    const int n = api.friend_count(api.friends, kFriendFlagImmediate);
    for (int i = 0; i < n; ++i) {
        const uint64_t f = api.friend_by_index(api.friends, i, kFriendFlagImmediate);
        FriendGameInfo g{};
        if (!api.friend_game(api.friends, f, &g) || !g.lobby) continue;
        if (static_cast<uint32_t>(g.game_id & 0xFFFFFF) != app) continue;
        // Données du lobby d'un ami pas toujours connues avant d'y entrer : un écart de version sera refusé au Hello.
        const std::string v = lobby_data(g.lobby, "mod");
        const bool compatible = v.empty() || v == mod_version;
        if (!compatible && !any_version) continue;
        const char* name = api.friend_persona(api.friends, f);
        out.push_back({g.lobby, f, name ? name : "", v, compatible});
    }
    return out;
}

uint64_t find_friend_lobby(const std::string& mod_version)
{
    const auto l = friend_lobbies(mod_version);
    return l.empty() ? 0 : l.front().lobby;
}

uint64_t friend_lobby(uint64_t friend_id)
{
    if (!api.ok || !friend_id) return 0;
    FriendGameInfo g{};
    if (!api.friend_game(api.friends, friend_id, &g) || !g.lobby) return 0;
    if (static_cast<uint32_t>(g.game_id & 0xFFFFFF) != api.app_id(api.utils)) return 0;
    return g.lobby;
}

std::string friend_name(uint64_t id)
{
    const char* n = api.ok && id ? api.friend_persona(api.friends, id) : nullptr;
    return n ? n : "";
}

void accept_user(uint64_t id)
{
    if (!api.ok || !id) return;
    const SteamNetworkingIdentity who = identity(id);
    api.accept(api.msgs, &who);
}

int send_raw(uint64_t to, const void* data, size_t len, bool reliable, int channel)
{
    if (!api.ok || !to) return 0;
    const SteamNetworkingIdentity who = identity(to);
    const int flags = (reliable ? kSendReliable : kSendUnreliableNoNagle) | kSendAutoRestartBrokenSession;
    return api.send(api.msgs, &who, data, static_cast<uint32_t>(len), flags, channel);
}

bool receive_raw(int channel, std::vector<RawMessage>& out, int max)
{
    if (!api.ok) return false;
    SteamNetworkingMessage* msgs[32];
    while (max > 0) {
        const int n = api.receive(api.msgs, channel, msgs, max < 32 ? max : 32);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            SteamNetworkingMessage* m = msgs[i];
            if (m->peer.type == kIdentitySteamID && m->size > 0) {
                const auto* p = static_cast<const uint8_t*>(m->data);
                out.push_back({m->peer.steam_id64, std::vector<uint8_t>(p, p + m->size)});
            }
            if (m->release) m->release(m);
        }
        max -= n;
    }
    return true;
}

} // namespace e33::net::steam
