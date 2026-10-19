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

// v2 : AiDraws ; v3 : graine de combat + Draws (D10) ;
// v4 : modèle de rejeu symétrique (D11) : cibles (camp, emplacement), compétences/objets, ComboResult,
//      défenses par héros relayées, StateSync (état complet), BattleRewards, équipe imposée.
inline constexpr uint16_t kProtocolVersion = 4;
inline constexpr uint32_t kMagic = 0x43333345; // "E33C"

enum class MsgType : uint16_t {
    Hello = 1, Welcome = 2, Reject = 3, Bye = 4, Ping = 5, Pong = 6,
    BattleStart = 20, TurnStart = 21, ActionCommand = 22,
    DefenseRequest = 24, DefenseResult = 25, StateHash = 26, BattleEnd = 27, Draws = 28,
    ComboResult = 29, StateSync = 30, BattleRewards = 32,
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
enum class ActorKind : uint8_t { Hero = 0, Enemy = 1 };
// Combattant identifié par son camp et son emplacement de combat (BattleSpotIndex), stable sur toutes les instances.
inline constexpr uint8_t kAutoSpot = 0xFF;   // cible « automatique » (faux client) : résolue par l'hôte
struct Unit { ActorKind kind = ActorKind::Enemy; uint8_t spot = 0; };

struct SpotOwner { uint8_t spot = 0; std::string character; uint8_t player_id = 0; };
struct BattleStart {
    uint32_t battle_id = 0;
    std::string encounter;
    std::vector<SpotOwner> owners;        // qui contrôle quel héros
    uint64_t rng_seed = 0;                // graine des tirages de gameplay (D10)
    std::vector<std::string> party;       // équipe imposée (noms internes) ; vide = équipe de la save
};
struct TurnStart {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    ActorKind kind = ActorKind::Hero;
    uint8_t index = 0;                    // emplacement du combattant
    std::string actor;                    // nom de classe, pour les logs et la vérification
};
// Choix d'action d'un héros. Propriétaire -> hôte, puis hôte -> autres instances (relais).
struct ActionCommand {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t actor_spot = 0;
    uint8_t action_type = 1;              // E_jRPG_ActionType : 1 attaque, 3 objet, 5 compétence
    std::string secondary;                // FName de la compétence ou de l'objet (vide pour l'attaque)
    std::vector<Unit> targets;
    std::vector<uint8_t> combo;           // résultats de QTE déjà connus (faux client) : bit0 réussi, bit1 parfait
};
// Résultat d'une fenêtre de QTE offensif, résolu chez le propriétaire du héros (seq : n° de fenêtre du héros dans le tour).
struct ComboResult {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    uint16_t seq = 0;
    bool success = false;
    bool perfect = false;
};
struct DefenseRequest {                   // hôte -> propriétaire : un coup arrive sur ce héros (utile au faux client)
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    uint16_t hit = 0;                     // n° du coup reçu par ce héros dans le tour
    uint8_t jumpable = 0;
    uint16_t timeout_ms = 3000;
};
enum class Defense : uint8_t { Fail = 0, Parry = 1, Dodge = 2, PerfectDodge = 3, Jump = 4, GradientParry = 5 };
// Défense résolue chez le propriétaire du héros. Propriétaire -> hôte, puis hôte -> autres instances.
struct DefenseResult {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    uint16_t hit = 0;
    Defense result = Defense::Fail;
};
struct StateHash { uint32_t battle_id = 0; uint32_t turn = 0; uint64_t hash = 0; };
struct BattleEnd { uint32_t battle_id = 0; uint8_t result = 0; };
struct Draw { uint32_t key = 0; uint16_t rank = 0; double value = 0; };   // flux (appelant BP + fonction), rang dans le tour
struct Draws {                            // hôte -> clients : tirages de gameplay faits pendant un tour (vérification D10)
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    std::vector<Draw> draws;
};
struct BuffState { std::string cls; int32_t stacks = 0; int32_t turns = 0; bool permanent = false; };
struct UnitState {
    Unit unit;
    std::string cls;                      // nom de classe (logs)
    double hp = 0;
    double ap = 0;
    int32_t shield = 0;
    int64_t break_bar = 0;
    bool dead = false;
    std::vector<BuffState> buffs;
};
// État complet du combat au début d'un tour (hôte -> clients). Le client ne corrige que ce qui diffère.
struct StateSync {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t reason = 0;                   // 0 = début de tour (1 réservé : resynchro en cours de tour)
    std::vector<UnitState> units;
    int32_t gradient_charges = 0;
    double gradient_progress = 0;
};
struct LootEntry { std::string item; int32_t quantity = 0; };
struct BattleRewards {
    uint32_t battle_id = 0;
    int32_t xp = 0;
    int32_t gold = 0;
    std::vector<LootEntry> loot;
};

using Message = std::variant<Hello, Welcome, Reject, Bye, Ping, Pong, BattleStart, TurnStart, ActionCommand,
                             DefenseRequest, DefenseResult, StateHash, BattleEnd, Draws, ComboResult, StateSync,
                             BattleRewards>;

MsgType type_of(const Message& m);
// Sérialise un message (type u16 + longueur u16 + charge utile).
void encode(const Message& m, wire::Writer& w);
// Lit UN message ; renvoie nullopt (et err) si invalide.
std::optional<Message> decode(wire::Reader& r, std::string& err);
// Description JSON compacte (pour les logs).
std::string to_json(const Message& m);

} // namespace e33::proto
