// Test à distance (docs/REMOTE-TEST.md) : format des paquets et règles de sécurité, sans dépendance au jeu
// (testé dans tools/tests).
//
// Canal à part du coop : canal Steam 1 (le coop utilise le canal 0), messages fiables de Steam, aucun lien net::Link.
// Paquet : magic "E33R" u32 | version u16 | op u8 | contenu. Ce format NE DÉPEND PAS de proto::kProtocolVersion :
// on n'ajoute que des ops ou des champs en fin de contenu (ignorés par les anciennes versions), on ne change jamais
// un champ existant. Une mise à jour qui change le protocole coop ne coupe donc jamais status / fetch / restart / update.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace e33::remote {

inline constexpr uint32_t kMagic = 0x52333345;   // "E33R"
// 2 : op Lobby (hôte -> client : salon coop ouvert par l'hôte de confiance).
inline constexpr uint16_t kVersion = 2;
inline constexpr uint16_t kVersionLobby = 2;   // première version qui comprend Op::Lobby
inline constexpr int kSteamChannel = 1;

inline constexpr size_t kMaxArg = 128 * 1024;          // scénario, options d'une commande
inline constexpr size_t kMaxJson = 256 * 1024;
inline constexpr size_t kMaxChunk = 64 * 1024;
inline constexpr uint64_t kMaxFetchTotal = 64ull << 20;   // journaux + saves coop renvoyés par requête
inline constexpr uint64_t kMaxFetchFile = 32ull << 20;
inline constexpr uint64_t kMaxUpdateTotal = 128ull << 20; // paquet de mise à jour du mod
inline constexpr size_t kMaxScenario = 64 * 1024;

enum class Op : uint8_t {
    Hello = 1,     // client -> hôte, toutes les 5 s : version, droits
    Command = 2,   // hôte -> client : commande de la liste fermée
    Reply = 3,     // client -> hôte : réponse (final = fin de la requête)
    File = 4,      // morceau de fichier (client -> hôte : journaux ; hôte -> client : mise à jour)
    Done = 5,      // fin d'un lot de fichiers (nombre, octets, empreinte du manifeste)
    // Version 2 : hôte -> client, en réponse au Hello tant que l'hôte a un salon coop ouvert sans ce client : id du
    // salon, propriétaire (= l'hôte), version du mod de l'hôte. Le client le rejoint au menu principal si
    // GetFriendGamePlayed ne donne rien (jeu relancé par script : salon d'ami invisible, essai à deux PC du 2026-10-23).
    Lobby = 6,
};

inline constexpr uint32_t kFlagRestart = 1;
inline constexpr uint32_t kFlagUpdate = 2;
inline constexpr uint32_t kFlagTestMode = 4;

struct Packet {
    Op op = Op::Hello;
    uint16_t version = kVersion;     // décodage : version E33R de l'émetteur (en-tête ; ignorée à l'encodage)
    uint32_t req = 0;
    // Hello
    std::string mod_version;
    uint16_t protocol = 0;           // protocole coop (information seulement)
    uint32_t flags = 0;
    std::string name;
    // Command
    std::string cmd;
    std::string arg;
    // Reply
    bool ok = false;
    bool final = true;
    std::string json;
    // File / Done
    std::string path;                // relatif, séparateur '/'
    uint64_t size = 0;               // taille totale du fichier
    uint64_t offset = 0;
    std::string sha256;              // File : du fichier (premier morceau) ; Done : du manifeste
    std::vector<uint8_t> data;
    uint32_t count = 0;              // Done
    uint64_t total = 0;              // Done
    // Lobby (mod_version : celle de l'hôte)
    uint64_t lobby = 0;
    uint64_t owner = 0;
};

std::vector<uint8_t> encode(const Packet& p);
// nullopt si ce n'est pas un paquet E33R valide (op inconnue comprise : ignorée par l'appelant).
std::optional<Packet> decode(const uint8_t* data, size_t len, std::string& err);

// --- Sécurité --------------------------------------------------------------------------------------
// Chemin relatif sûr : séparateur '/', ni absolu, ni lecteur, ni '..', caractères simples, profondeur bornée.
bool safe_relative(std::string_view path);
// Mise à jour : destinations permises (relatives à Sandfall/Binaries/Win64) : dwmapi.dll, ue4ss/UE4SS.dll,
// ue4ss/Mods/E33Coop/** sauf remote/, logs/ et remotetest.txt (la confiance ne se change pas à distance).
bool update_dest_allowed(std::string_view path);
// Scénario reçu : JSON objet, "save" limité à e33coop/ ou e33coop-test/, étapes connues du runner, launch_args filtrés
// (options -e33 connues ; -e33connect / -e33port retirées ; rôle client et transport Steam seulement). sanitized : le
// scénario réécrit à exécuter ; notes : ce qui a été retiré.
bool validate_scenario(std::string_view text, std::string& sanitized, std::string& err, std::vector<std::string>* notes = nullptr);
// Client : salon proposé acceptable ? Seulement venu du SteamID de confiance (non nul), qui s'en dit propriétaire,
// salon non nul.
bool lobby_offer_valid(const Packet& p, uint64_t from, uint64_t trusted);
// Hôte : proposer son salon à ce pair ? Pair qui comprend Op::Lobby (version E33R de son Hello), salon ouvert, pair
// pas déjà dedans.
bool should_offer_lobby(uint16_t peer_version, uint64_t lobby, bool peer_in_lobby);
// Identifiant de requête (nom de dossier) : [A-Za-z0-9_-], 1 à 64 caractères.
bool safe_id(std::string_view id);

// --- Empreintes ----------------------------------------------------------------------------------
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t len);
    std::string hex();   // termine le calcul
private:
    void* h_ = nullptr;
    std::vector<uint8_t> obj_;
};
std::string sha256_hex(const void* data, size_t len);
struct ManifestEntry { std::string path; uint64_t size = 0; std::string sha256; };
// Texte du manifeste (une ligne "<sha256> <taille> <chemin>" par fichier, triées par chemin) et son empreinte.
std::string manifest_text(std::vector<ManifestEntry> entries);
std::string manifest_hash(const std::vector<ManifestEntry>& entries);

} // namespace e33::remote
