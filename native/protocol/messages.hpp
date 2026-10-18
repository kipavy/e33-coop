// Messages du protocole coop, versionnés. Partagés entre le mod et les outils (fakepeer).
// Règle : chaque message reçu est validé (bornes, énumérations, fin de tampon) ; un message invalide
// est rejeté par decode() et ne doit jamais faire planter le jeu.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "wire.hpp"

namespace e33::proto {

inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr uint32_t kMagic = 0x43333345; // "E33C"

enum class MsgType : uint16_t {
    Hello = 1, Welcome = 2, Reject = 3, Bye = 4, Ping = 5, Pong = 6,
    BattleStart = 20, TurnStart = 21, ActionCommand = 22, ActionResult = 23,
    DefenseRequest = 24, DefenseResult = 25, StateHash = 26, BattleEnd = 27,
};

const char* type_name(MsgType t);

// --- Session -------------------------------------------------------------------------------
struct Hello {
    uint16_t protocol = kProtocolVersion;
    uint32_t game_build = 0;              // build ID Steam
    std::string mod_version;
    std::vector<std::string> gameplay_mods;   // autres mods de gameplay installés (triés)
    std::string player_name;
    uint64_t instance_id = 0;
};
struct Welcome {
    uint8_t player_id = 0;
    uint32_t session_id = 0;
};
enum class RejectReason : uint8_t { ProtocolMismatch = 1, GameBuildMismatch = 2, ModVersionMismatch = 3, ModListMismatch = 4, SessionFull = 5, Other = 6 };
struct Reject {
    RejectReason reason = RejectReason::Other;
    std::string detail;                   // ce qui diffère, en clair
};
struct Bye { std::string reason; };
struct Ping { uint64_t t_us = 0; };
struct Pong { uint64_t t_us = 0; };

// --- Combat --------------------------------------------------------------------------------
struct SpotOwner { uint8_t spot = 0; std::string character; uint8_t player_id = 0; };
struct BattleStart {
    uint32_t battle_id = 0;
    std::string encounter;
    std::vector<SpotOwner> owners;        // qui contrôle quel héros
};
enum class ActorKind : uint8_t { Hero = 0, Enemy = 1 };
struct TurnStart {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    ActorKind kind = ActorKind::Hero;
    uint8_t index = 0;                    // emplacement (héros) ou index d'ennemi
    std::string actor;                    // nom de classe, pour les logs et la vérification
};
struct ActionCommand {                    // client -> hôte : choix d'action + résultats des QTE offensifs
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t actor_spot = 0;
    uint8_t action_type = 1;              // E_jRPG_ActionType (1 = attaque de base)
    std::string skill;                    // FName de compétence (vide si attaque)
    std::vector<uint8_t> targets;         // index d'ennemis
    uint8_t combo_hits = 0;               // QTE offensifs réussis (résolus chez le client)
    uint8_t combo_perfect = 0;
};
struct HitResult { ActorKind kind = ActorKind::Enemy; uint8_t index = 0; double damage = 0; bool critical = false; double hp_after = 0; };
struct ActionResult {                     // hôte -> clients : résultats calculés par l'hôte
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    ActorKind kind = ActorKind::Hero;
    uint8_t actor_index = 0;
    uint8_t action_type = 1;
    std::string skill;
    std::vector<uint8_t> targets;
    std::vector<HitResult> hits;
};
struct DefenseRequest {                   // hôte -> client : un coup arrive sur des héros de ce client
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint16_t hit = 0;
    uint8_t attacker_index = 0;
    std::vector<uint8_t> defender_spots;
    uint16_t timeout_ms = 3000;
};
enum class Defense : uint8_t { Fail = 0, Parry = 1, Dodge = 2, PerfectDodge = 3, Jump = 4, GradientParry = 5 };
struct DefenseResult {                    // client -> hôte : résolu localement, appliqué par l'hôte
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint16_t hit = 0;
    Defense result = Defense::Fail;
};
struct StateHash { uint32_t battle_id = 0; uint32_t turn = 0; uint64_t hash = 0; };
struct BattleEnd { uint32_t battle_id = 0; uint8_t result = 0; };

using Message = std::variant<Hello, Welcome, Reject, Bye, Ping, Pong, BattleStart, TurnStart, ActionCommand,
                             ActionResult, DefenseRequest, DefenseResult, StateHash, BattleEnd>;

MsgType type_of(const Message& m);
// Sérialise un message (type u16 + longueur u16 + charge utile).
void encode(const Message& m, wire::Writer& w);
// Lit UN message ; renvoie nullopt (et err) si invalide.
std::optional<Message> decode(wire::Reader& r, std::string& err);
// Description JSON compacte (pour les logs).
std::string to_json(const Message& m);

} // namespace e33::proto
