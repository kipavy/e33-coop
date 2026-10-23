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
//      défenses par héros relayées, StateSync (état complet), BattleRewards, équipe imposée, visée libre (FreeAimEvent).
// v5 : session et exploration (D12) : instantané de save, pilote, interactions, dialogues, transitions, barrière narrative, pause.
// v6 : robustesse (phase 5) : époque du lien (reconnexion), battement « occupé », propriétaires réattribués en combat
//      (Owners), démarrage de combat coordonné (BattleReady), joueurs (Roster), hash d'exploration (WorldHash) et
//      resynchro (ResyncRequest), minuteur de tour (BattleStart.turn_timer_s, ActionCommand.forced).
// Ajouts sans changement de version (un pair plus ancien ignore un type inconnu, le flux reste lisible) :
//      aperçu de l'action en préparation (IntentPreview), caméra de la visée libre du propriétaire (AimView),
//      état des menus de combat du propriétaire, rejoué dans les menus du jeu chez les autres (MenuState),
//      scène (cinématique) commencée par le pilote et sa cause, déclenchée aussitôt chez les suiveurs (SceneStart), et passée
//      ensemble (SceneSkip).
inline constexpr uint16_t kProtocolVersion = 6;
inline constexpr uint32_t kMagic = 0x43333345; // "E33C"

enum class MsgType : uint16_t {
    Hello = 1, Welcome = 2, Reject = 3, Bye = 4, Ping = 5, Pong = 6,
    BattleStart = 20, TurnStart = 21, ActionCommand = 22,
    DefenseRequest = 24, DefenseResult = 25, StateHash = 26, BattleEnd = 27, Draws = 28,
    ComboResult = 29, StateSync = 30, BattleRewards = 32, FreeAimEvent = 33, IntentPreview = 34, AimView = 35, MenuState = 36,
    SaveOffer = 40, SaveChunk = 41, JoinWait = 42, JoinReady = 43, PilotState = 44, PilotRequest = 45, PilotChange = 46,
    WorldInteract = 47, DialogueChoice = 48, MapChange = 49, NarrativeState = 50, BarrierRelease = 51, PauseState = 52, SceneStart = 53, SceneSkip = 54,
    Owners = 60, BattleReady = 61, Roster = 62, WorldHash = 63, ResyncRequest = 64,
    BuildOffer = 65, BuildChunk = 66, EconomyDelta = 67,
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
    bool join = false;                    // v5 : le client veut l'instantané de la partie de l'hôte (rejoindre)
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
    // v5 : combat lancé depuis l'exploration (vide = combat déclenché par le test runner)
    std::string world_enemy;              // nom de l'acteur ennemi du monde (stable entre instances)
    std::string enemy_global_id;          // EnemyGlobalID des paramètres de combat
    uint8_t engage = 0;                   // E_jRPG_EngageTypes
    uint16_t turn_timer_s = 0;            // v6 : minuteur de tour (0 = aucun, réglage par défaut)
    uint8_t duo_parry = 0;                // bonus « parade en duo » choisi par l'hôte (champ final facultatif : absent = 0)
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
    bool forced = false;                  // v6 : action imposée par l'hôte (minuteur de tour écoulé)
};
// Résultat d'une fenêtre de QTE offensif, résolu chez le propriétaire du héros (seq : n° de fenêtre du héros dans le tour).
// Les autres instances rejouent un vrai appui au même instant de la fenêtre (press, en millièmes de sa durée).
inline constexpr uint16_t kNoPress = 0xFFFF;     // pas d'appui (raté)
inline constexpr uint16_t kAutoPress = 0xFFFE;   // réussite automatique (option d'accessibilité, mode test) : résultat imposé en fin de fenêtre
struct ComboResult {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    uint16_t seq = 0;
    bool success = false;
    bool perfect = false;
    uint16_t press = kNoPress;
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
struct MechField { std::string name; int64_t value = 0; };   // "Composant.Champ" (ou Champ[i] pour un tableau)
struct UnitState {
    Unit unit;
    std::string cls;                      // nom de classe (logs)
    double hp = 0;
    double ap = 0;
    int32_t shield = 0;
    int64_t break_bar = 0;
    bool dead = false;
    std::vector<BuffState> buffs;
    std::vector<MechField> mech;          // mécanique propre (perfection, posture, taches…) : champs entiers du composant
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
// Visée libre d'un héros, chez son propriétaire (D11) : rejouée par les autres instances avec le même décalage
// que ses actions. t_ms : temps écoulé depuis le début de la visée. Point visé : absolu, ou relatif à un
// combattant (target.spot != kAutoSpot : position de l'acteur + point, utilisé par le faux client).
enum class FreeAimKind : uint8_t { Start = 0, Aim = 1, Fire = 2, Shot = 3, Cancel = 4, End = 5 };
struct FreeAimEvent {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    FreeAimKind kind = FreeAimKind::Start;
    uint16_t seq = 0;                     // Fire / Shot : n° du tir dans la visée
    uint32_t t_ms = 0;
    double x = 0, y = 0, z = 0;           // Aim / Shot : point visé
    Unit target{ActorKind::Enemy, kAutoSpot};
};
// Ce que le propriétaire d'un héros prépare pendant son tour (aperçu chez les autres) : action et compétence/objet
// choisis dans la roue, cibles visées. Propriétaire -> hôte -> autres, à chaque changement (10 fois/s au plus).
struct IntentPreview {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    uint16_t seq = 0;                     // croissant dans le tour
    uint8_t action_type = 0;              // E_jRPG_ActionType (0 = rien de choisi)
    bool aiming = false;                  // visée libre en cours
    std::string secondary;                // compétence ou objet (FName), vide sinon
    std::vector<Unit> targets;
};
// Caméra du propriétaire pendant sa visée libre (non fiable, ~20 fois/s) : les autres instances la rejouent avec le même
// décalage que la visée. t_ms : depuis le début de la visée (comme FreeAimEvent). fov : 0 = inconnu.
struct AimView {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    uint32_t t_ms = 0;
    double x = 0, y = 0, z = 0;           // position de la caméra
    double pitch = 0, yaw = 0;
    double fov = 0;
};
// Menus de combat du propriétaire pendant son tour (miroir chez les autres, affichage seulement) : page de la roue,
// colonne de la liste, bouton survolé, action / compétence choisie, ciblage et cibles. Fiable, à chaque changement
// (15 fois/s au plus). Rien ici ne lance l'action : elle part toujours par ActionCommand.
enum class MenuPage : uint8_t { None = 0, Actions = 1, Skills = 2, Items = 3, Gradient = 4, FreeAim = 5 };
struct MenuState {
    uint32_t battle_id = 0;
    uint32_t turn = 0;
    uint8_t spot = 0;
    uint16_t seq = 0;                     // croissant dans le tour
    MenuPage page = MenuPage::None;
    uint8_t column = 0;                   // liste à deux colonnes : 0 gauche, 1 droite
    int8_t hover = -1;                    // bouton survolé dans la colonne (0..5), -1 aucun
    uint8_t action_type = 0;              // E_jRPG_ActionType choisi (0 = rien)
    std::string secondary;                // compétence ou objet choisi (ciblage en cours), vide sinon
    bool targeting = false;
    uint8_t targeting_type = 0;           // E_jRPG_TargetingType
    std::vector<Unit> targets;
};

// --- Session et exploration (v5, D12) ----------------------------------------------------------
// Instantané de la partie de l'hôte (save complète) : annoncé puis envoyé par morceaux, dans l'ordre.
inline constexpr uint32_t kMaxSaveSize = 16u * 1024u * 1024u;
inline constexpr uint16_t kMaxChunk = 16000;
struct SaveOffer {
    uint32_t xfer = 0;
    uint32_t size = 0;
    uint64_t hash = 0;                    // FNV-1a 64 des octets
    std::string level;                    // niveau de l'instantané (logs, vérification)
};
struct SaveChunk {
    uint32_t xfer = 0;
    uint32_t offset = 0;
    std::vector<uint8_t> data;            // <= kMaxChunk octets
};
struct JoinWait { std::string reason; };  // hôte -> client : entrée au prochain point sûr (ex. "battle")
struct JoinReady { uint32_t xfer = 0; std::string level; };   // client -> hôte : instantané chargé, monde prêt
// Position du pilote, ~20 fois par seconde (non fiable). seq croissant : les paquets en retard sont ignorés.
struct PilotState {
    uint8_t pilot = 0;
    uint32_t seq = 0;
    std::string level;
    double x = 0, y = 0, z = 0, yaw = 0;
    double vx = 0, vy = 0, vz = 0;
    // Champs finaux facultatifs (2026-10-22, carte du monde / Esquie) : absents = inconnus (move = kMoveUnknown).
    uint8_t move = 0xFF;                  // EMovementMode du pion du pilote (0 aucun, 1 marche, 3 chute, 4 nage, 5 vol, 6 custom)
    uint8_t custom = 0;                   // CustomMovementMode
    uint8_t flags = 0;                    // kPilotMount | kPilotWorldMap | kPilotAttached
    std::string pawn;                     // classe du pion possédé (nom court)
    // Champ final facultatif (2026-10-23) : mode d'Esquie sur la carte du monde (BP_WorldMapCharacter_C.CurrentMovementMode :
    // 0 marche, 2 vol…), 255 = pas sur Esquie / inconnu.
    uint8_t wm_mode = 0xFF;
};
constexpr uint8_t kMoveUnknown = 0xFF;
constexpr uint8_t kPilotMount = 1;        // GI.IsMountInUse : sur Esquie
constexpr uint8_t kPilotWorldMap = 2;     // pion de la carte du monde (bIsWorldMapPawn)
constexpr uint8_t kPilotAttached = 4;     // pion attaché à un autre acteur (monture)
struct PilotRequest { uint8_t player = 0; };        // un joueur demande la main
struct PilotChange { uint8_t pilot = 0; };          // hôte -> tous : nouveau pilote
// Interaction du pilote avec un objet du monde (PNJ, coffre, porte…), rejouée par les autres instances.
struct WorldInteract { uint32_t seq = 0; std::string actor; bool instant = false; };
struct DialogueChoice { uint32_t seq = 0; int32_t index = 0; };
// Changement de niveau du pilote (niveau + point d'apparition).
struct MapChange { uint32_t seq = 0; std::string level; std::string spawn_tag; };
// Début / fin d'une cinématique ou d'un dialogue sur une instance (barrière : tout le monde attend tout le monde).
enum class NarrativeKind : uint8_t { Dialogue = 0, Cinematic = 1 };
struct NarrativeState { uint8_t player = 0; NarrativeKind kind = NarrativeKind::Dialogue; bool active = false; uint32_t seq = 0; };
struct BarrierRelease { uint32_t seq = 0; };
// Pilote -> autres : il commence une cinématique (TriggerCinematic). scene = Cinematic_LevelSequence_ID, sequence = asset
// joué ; cause = ce qui l'a déclenchée chez lui (zone traversée, interaction, game action) et cause_path son chemin
// complet ; action = game action exécutée (chemin) ; x/y/z = position du pilote à ce moment. Le suiveur déclenche la
// même scène tout de suite (même zone), au lieu de la jouer après la fin chez le pilote (2026-10-23).
enum class SceneCause : uint8_t { Unknown = 0, Zone = 1, Interaction = 2, Action = 3 };
struct SceneStart {
    uint8_t player = 0;
    uint32_t seq = 0;
    std::string scene;
    std::string sequence;
    SceneCause cause = SceneCause::Unknown;
    std::string cause_path;
    std::string action;
    double x = 0, y = 0, z = 0;
};
// Pilote -> autres : il passe la cinématique en cours (SkipCinematic) ; les autres passent la même (fin ensemble).
struct SceneSkip { uint8_t player = 0; uint32_t seq = 0; std::string scene; };
struct PauseState { uint8_t player = 0; bool paused = false; bool in_battle = false; };

// --- Robustesse (v6, phase 5) -----------------------------------------------------------------
// Hôte -> clients : propriétaires des héros du combat en cours, après une déconnexion (l'hôte reprend les héros du
// joueur parti) ou une réattribution.
struct Owners { uint32_t battle_id = 0; std::vector<SpotOwner> owners; };
// Client -> hôte : ce combat a commencé ici (premier tour). L'hôte attend tout le monde avant de jouer son premier tour.
struct BattleReady { uint32_t battle_id = 0; uint8_t player = 0; };
// Hôte -> clients : joueurs de la session (indicateurs « tour de X », « pause demandée par X »).
struct RosterEntry { uint8_t player = 0; std::string name; };
struct Roster { std::vector<RosterEntry> players; };
// État d'exploration résumé (or, objets, drapeaux, objets du monde utilisés…) : hôte -> clients au point sûr. seq : n° de
// la dernière interaction / du dernier choix publiés, pour comparer au même point de la partie.
struct HashPart { std::string name; uint64_t value = 0; };
struct WorldHash { uint32_t seq = 0; std::string level; std::vector<HashPart> parts; };
// Client -> hôte : mon état d'exploration diffère du sien, renvoyer un instantané au prochain point sûr.
struct ResyncRequest { std::string reason; };
// Builds des personnages (D13) : états sérialisés par le jeu (save ne contenant que les personnages), envoyés par morceaux
// comme l'instantané. names : personnages à appliquer (avec l'empreinte de leur build chez l'émetteur). authoritative : envoyé
// par l'hôte (à appliquer tel quel) ; sinon proposition d'un client, que l'hôte vérifie (propriétaire).
struct BuildName { std::string name; uint64_t hash = 0; };
struct BuildOffer { uint32_t xfer = 0; uint8_t origin = 0; bool authoritative = false; uint32_t size = 0; uint64_t hash = 0; std::vector<BuildName> names; std::string reason; };
struct BuildChunk { uint32_t xfer = 0; uint32_t offset = 0; std::vector<uint8_t> data; };
// Ressources communes (D13, option 1) : variations faites hors événement rejoué (achat, amélioration, objet utilisé
// dans un menu). Client -> hôte (vérifiées : assez d'or / d'objets), puis hôte -> autres. rejected : refus de l'hôte (l'auteur
// se resynchronise).
struct EconomyPart { std::string key; int64_t delta = 0; };
struct EconomyDelta { uint32_t seq = 0; uint8_t origin = 0; bool authoritative = false; bool rejected = false; std::vector<EconomyPart> parts; };

using Message = std::variant<Hello, Welcome, Reject, Bye, Ping, Pong, BattleStart, TurnStart, ActionCommand,
                             DefenseRequest, DefenseResult, StateHash, BattleEnd, Draws, ComboResult, StateSync,
                             BattleRewards, FreeAimEvent,
                             SaveOffer, SaveChunk, JoinWait, JoinReady, PilotState, PilotRequest, PilotChange,
                             WorldInteract, DialogueChoice, MapChange, NarrativeState, BarrierRelease, PauseState,
                             Owners, BattleReady, Roster, WorldHash, ResyncRequest, BuildOffer, BuildChunk, EconomyDelta,
                             IntentPreview, AimView, MenuState, SceneStart, SceneSkip>;

MsgType type_of(const Message& m);
// Sérialise un message (type u16 + longueur u16 + charge utile).
void encode(const Message& m, wire::Writer& w);
// Lit UN message ; renvoie nullopt (et err) si invalide.
std::optional<Message> decode(wire::Reader& r, std::string& err);
// Description JSON compacte (pour les logs).
std::string to_json(const Message& m);

} // namespace e33::proto
