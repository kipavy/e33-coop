#include "coop.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/prof.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../net/link.hpp"
#include "../net/recorder.hpp"
#include "../net/steam.hpp"
#include "../net/transport.hpp"
#include "../protocol/messages.hpp"
#include "../ui/owners_ui.hpp"
#include "../ui/joinbutton.hpp"
#include "../ui/ui.hpp"
#include "builds.hpp"
#include "coopsave.hpp"
#include "explore.hpp"
#include "intent.hpp"
#include "presence.hpp"
#include "update.hpp"
#include "state.hpp"
#include "../remote/remote.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

using RC::Unreal::UObject;
using RC::Unreal::UFunction;

namespace e33::coop {

namespace {

constexpr const char* kModVersion = E33COOP_VERSION;   // fichier VERSION (CMake)
constexpr double kDefenseTimeout = 3.0;
constexpr double kComboTimeout = 3.0;
// Parade en duo : PA gagnés par héros qui a paré, et raison passée à GainAP (EAPChangeReason, libellés non dumpés :
// valeur 0 choisie sans certitude, journalisée avec les PA avant/après).
constexpr int kDuoParryAP = 1;
constexpr int kDuoParryReason = 0;

constexpr double kLinkTimeout = 15.0;      // silence total -> pair perdu
constexpr double kBusyMax = 300.0;         // pair seulement « occupé » (chargement interminable) -> perdu
constexpr double kHandshakeTimeout = 30.0; // hôte : connexion sans Hello valide
constexpr double kClientHandshakeTimeout = 90.0;   // client : pas de réponse de l'hôte (il peut être en plein chargement)
constexpr double kReadyTimeout = 600.0;    // hôte : attente des autres au début d'un combat (cinématique de boss : ~5 min)
// Hôte, démarrage coordonné : un joueur qui n'entre pas dans le combat alors qu'il ne fait rien d'autre (ni chargement,
// ni cinématique, ni dialogue, ni menu, ni reconnexion) n'y entrera sans doute jamais (barrière restée tenue chez lui,
// lancement impossible…) : le combat commence sans lui au bout de ce délai d'inactivité (-e33readyidle=N). Les attentes
// justifiées (cinématique de boss, chargement) gardent kReadyTimeout.
constexpr double kReadyIdleTimeout = 60.0;
// Hôte : tour du héros d'un joueur qui n'a même pas commencé ce tour chez lui (aucun StateHash de ce tour) depuis ce
// temps de jeu (-e33stuckturn=N) : attaque automatique, comme le minuteur de tour, pour que la partie ne reste pas figée.
constexpr double kStuckTurnTimeout = 60.0;
constexpr uint8_t kMaxClients = 2;         // 3 joueurs au plus

struct Peer {
    std::unique_ptr<net::Link> link;
    uint8_t player_id = 0xFF;              // hôte : attribué à l'acceptation du Hello (plus petit libre)
    bool accepted = false;
    bool wants_join = false;
    std::string name;
    double created_at = 0;
    std::string drop;                      // non vide : à retirer à la fin de la pompe réseau (raison)
    double drop_at = -1;                   // retrait différé (refus : laisser partir le Reject)
};

using TurnSpotSeq = std::tuple<uint32_t, int, uint16_t>;   // (tour, emplacement, n°)

using SteadyClock = std::chrono::steady_clock;

struct Window {                                            // fenêtre de QTE offensif en cours (par timeline)
    UObject* hero = nullptr;
    int spot = 0;
    uint16_t seq = 0;
    SteadyClock::time_point start;
    double duration = 0;                                   // secondes (temps réel)
    bool local = false;                                    // héros contrôlé ici : résultat à publier
    bool published = false;
    proto::ComboResult plan;                               // héros d'un autre joueur : résultat de son propriétaire
    bool pressed = false;
};

struct Press { UObject* timeline = nullptr; SteadyClock::time_point at; };

struct FreeAimOwner {                                      // visée libre d'un héros contrôlé ici
    bool active = false;
    int spot = -1;
    SteadyClock::time_point t0;
    uint16_t fires = 0, shots = 0;
    double next_aim_s = 0;
    bool in_raycast = false;
    bool have_point = false, raycast_point = false;
    bool seen_aiming = false;                              // IsAiming vu vrai : sa retombée = fin de visée
    std::array<double, 3> point{}, shot_point{};
    UObject* shot_hit = nullptr;                           // combattant touché par le tir en cours (OnFreeAimProjectileHit)
    UObject* shot_part = nullptr;                          // zone touchée (TargetBodyPartBP : point faible…), même tir
};

struct FreeAimReplay {                                     // visée libre rejouée (héros d'un autre joueur)
    std::pair<uint32_t, int> key{0, -1};
    bool started = false, done = false;
    SteadyClock::time_point t0;
    size_t next = 0;
    uint16_t raycasts = 0;
    bool have_override = false;
    std::array<double, 3> point{};
    // Tir rejoué en cours : combattant touché chez le propriétaire (kAutoSpot : aucun ou inconnu) et touché ici
    bool in_raycast = false;
    proto::Unit want{proto::ActorKind::Enemy, proto::kAutoSpot};
    int hit_spot = -1;
    uint16_t misses = 0;
};

struct LocalHit {                                          // coup reçu par un héros contrôlé ici (résultat à publier)
    int spot = 0;
    uint16_t hit = 0;
    bool jumpable = false;
    bool perfect_dodge = false;
};

struct Battle {
    uint32_t id = 0;
    uint32_t turn = 0;
    bool active = false;
    bool ending = false;                                   // fin de combat commencée (tirages des récompenses encore remplacés)
    // Attente « propre » du TurnStart de l'hôte (-e33turndep=1, essai) : dépendance de début de tour du jeu au lieu de
    // bloquer le thread de jeu (mini-freezes ~1 s à la fin des compétences du client, 2026-10-21).
    bool dep_active = false;
    uint32_t dep_turn = 0;
    double dep_since = 0;
    UObject* dep_actor = nullptr;
    proto::StateSync dep_start;                            // état local photographié au début du tour (comparé à l'hôte plus tard)
    bool dep_finish_pending = false;                       // vérification du tour en attente de l'état de l'hôte
    double dep_recheck_at = -1;                            // vérification de secours après résolution (si le jeu écoute encore)
    uint32_t sct_seq = 0, ots_seq = 0, listen_seq = 0;     // StartCharacterTurn vus ; dernier suivi d'un OnTurnStart ; d'une écoute
    // Hôte, démarrage synchronisé des tours ennemis (D28) : tour retenu (sans figer le jeu) jusqu'à ce que les clients l'aient commencé.
    bool next_enemy = false, cur_enemy = false;            // acteur du prochain tour (StartCharacterTurn), du tour en cours
    bool align_active = false;
    uint32_t align_turn = 0;
    double align_since = 0;
    double align_paused_s = 0;                             // temps de pause pendant l'alignement (non compté dans les 4 s)
    double play_since = 0;                                 // hôte : début de jeu du tour (OnTurnStart, ou fin de l'alignement)
    bool strict = false;                                   // écart d'acteur vu dans ce combat : on revient à l'attente bloquante
    bool finalized = false;
    bool left = false;                                     // fuite : le dernier tour n'est pas comparable (l'hôte a pu en commencer un de plus)
    std::map<int, uint8_t> owner_by_spot;                  // emplacement de héros -> player_id
    // Actions des héros
    std::map<std::pair<uint32_t, int>, proto::ActionCommand> commands;   // (tour, emplacement) -> commande reçue
    std::map<std::pair<uint32_t, int>, double> command_time;             // réception (s.now)
    std::set<std::pair<uint32_t, int>> executed;
    bool action_wait_logged = false;
    // QTE offensifs
    std::map<TurnSpotSeq, proto::ComboResult> combos;
    std::map<int, uint16_t> combo_seq;                     // emplacement -> n° de fenêtre dans le tour
    std::map<UObject*, Window> windows;                    // timeline -> fenêtre en cours
    std::vector<Press> presses;                            // appuis à rejouer (héros d'un autre joueur)
    // Visée libre
    std::map<std::pair<uint32_t, int>, std::vector<proto::FreeAimEvent>> fa_events;   // (tour, emplacement) -> événements reçus
    std::map<std::pair<uint32_t, int>, double> fa_arrival;                           // réception du Start (s.now)
    // Défenses
    std::map<TurnSpotSeq, proto::Defense> defenses;
    std::map<int, uint16_t> hit_seq;                       // emplacement -> n° de coup reçu dans le tour
    std::map<UObject*, LocalHit> local_hits;               // composant de stats -> coup en cours (héros d'ici)
    ULONGLONG wait_parry_at = 0;                           // appui de parade vu pendant une attente figée (GetTickCount64), D32
    std::map<UObject*, int> imposed_check;                 // composant de stats -> défense imposée (vérification du verdict)
    // Parade en duo : défense retenue ici pour chaque coup reçu par un héros pendant le tour (résultat du propriétaire)
    bool duo_parry = false;                                // réglage de l'hôte pour ce combat (BattleStart)
    std::vector<std::pair<int, proto::Defense>> turn_defenses;   // (emplacement, défense)
    // État
    std::map<uint32_t, uint64_t> local_hash;
    std::map<uint32_t, uint64_t> remote_hash;
    std::map<uint32_t, proto::StateSync> host_sync;        // client : état de l'hôte au début de chaque tour
    std::map<uint32_t, proto::TurnStart> host_turns;       // client
    double turn_started_at = 0;
    // Tirages (D10)
    uint64_t rng_seed = 0;
    std::map<uint32_t, uint16_t> draw_rank;                // flux -> rang du prochain tirage dans le tour
    std::map<uint32_t, std::string> stream_names;
    std::map<uint32_t, std::map<uint64_t, double>> local_draws;
    std::map<uint32_t, std::map<uint64_t, double>> host_draws;
    int traced_draws = 0;
    std::map<UObject*, int> enemy_spots;                   // ennemi -> emplacement (sans appel au jeu dans les hooks de tirage)
    // Fin
    int local_end = -1;
    int host_end = -1;
    double host_end_at = -1;                               // client : réception du BattleEnd de l'hôte
    double end_flow_at = -1;                              // fin sans victoire : clôture différée (tick), jamais bloquante dans le flux de fin
    std::optional<proto::BattleRewards> host_rewards;
    // Client : fin de l'hôte sans fin locale -> fin imposée par étapes (tick) ; force_now : un tour a recommencé ici après
    // le BattleEnd de l'hôte (le combat local continue sans lui).
    int force_step = 0;
    double force_at = -1;
    bool force_now = false;
    int turn_wait_timeouts = 0;                            // client : attentes du début de tour de l'hôte expirées (combat)
    // Démarrage coordonné (hôte) : clients qui ont commencé ce combat ; jeu en pause tant qu'il en manque.
    std::set<uint8_t> ready;
    bool hold = false;
    double hold_since = 0;
    double hold_logged_at = 0;                             // hôte : dernier journal « on attend encore » (diagnostic)
    double hold_idle_s = 0;                                // hôte : temps continu où tous les joueurs attendus sont inactifs
    bool ready_sent = false;                               // client
    double start_rx_at = -1;                               // client : réception du BattleStart (délai jusqu'au BattleReady)
    // Hôte : joueurs qui ont commencé chaque tour chez eux (leur StateHash de ce tour), et joueurs qui ne répondent plus
    // dans ce combat (tour bloqué : leurs défenses et QTE ne sont plus attendus, jusqu'à leur prochain StateHash).
    std::map<uint32_t, std::set<uint8_t>> acks;
    std::set<uint8_t> unresponsive;
    // Minuteur de tour (option, 0 = aucun)
    uint16_t turn_timer_s = 0;
    std::set<std::pair<uint32_t, int>> forced;             // actions imposées par l'hôte (minuteur écoulé)
    double timer_elapsed = 0;                              // temps de jeu du tour (hors pauses et attente des joueurs)
};

// Tirages de la mise en place d'un combat (D30) : avant le tour 1 (BeginPlay des héros, équipement, effets de début de
// combat). Ex. : position de départ de la roue de Monoco (BP_UniqueMechanic_TransformMasks_Component, ReceiveBeginPlay).
// Fenêtre ouverte au lancement du combat (StartBattleNEW, ou avis précoce de l'hôte), fermée au tour 1 ou à la fin.
struct SetupDraws {
    bool open = false;
    bool seeded = false;                                   // graine de l'hôte connue ici (sinon tirages laissés au jeu)
    bool from_notice = false;                              // hôte : ouverte par l'avis précoce (StartBattleNEW pas encore vu)
    bool bm_started = false;                               // StartBattleNEW vu pour cette fenêtre
    uint64_t seed = 0;
    std::string source;                                    // d'où vient la graine (journal)
    double opened_at = 0;
    int traced = 0;
    int natural_logs = 0;
    std::map<uint32_t, uint16_t> rank;                     // flux -> rang du prochain tirage
    std::map<uint64_t, double> draws;                      // (flux << 16 | rang) -> valeur
    std::map<uint32_t, std::string> names;
};

struct State {
    Role role = Role::Solo;
    std::unique_ptr<net::Transport> transport;
    std::vector<Peer> peers;                               // hôte : clients ; client : [hôte]
    net::Recorder recorder;
    uint8_t my_player_id = 0;                              // hôte = 0
    uint32_t session_id = 0;
    uint32_t game_build = 0;
    std::vector<std::string> gameplay_mods;
    std::string connect_to;
    bool accepted = false;
    bool rejected = false;
    double now = 0;
    double tick_now = 0;                                   // horloge à la dernière image (voir advance_clock)
    Battle battle;
    uint32_t next_battle_id = 1;
    std::map<std::string, uint8_t> owner_overrides;        // -e33owners=0:1,Lune:1  (emplacement ou nom : joueur)
    DWORD game_thread = 0;
    std::optional<proto::BattleStart> pending_start;       // client : combat de l'hôte, lancé une fois le monde prêt
    std::optional<proto::BattleStart> last_start;          // client : dernier BattleStart (le StartBattle local peut suivre son lancement)
    std::optional<proto::BattleStart> early_start;         // client : avis précoce de l'hôte (battle_id 0)
    // Client : photo de l'état au début d'un tour joué ici avant que le mod suive le combat (combat engagé ici avant le
    // BattleStart : pion suiveur, dialogue). Le rattrapage compare cette photo, prise au même instant que celle de l'hôte.
    std::optional<proto::StateSync> early_turn;
    UObject* early_turn_actor = nullptr;
    double early_turn_at = -1;
    double early_start_at = -1;
    double world_ready_s = 0;
    double next_trigger_try = 0;
    double action_delay_s = 1.5;                           // délai minimal (depuis le début du tour) avant d'exécuter l'action d'un héros d'un autre joueur
    double replay_lag_s = 1.0;                             // décalage minimal derrière son propriétaire (-e33replaylag) : ses QTE sont connus à temps
    std::optional<uint64_t> fixed_seed;                    // hôte : -e33seed=N
    std::vector<std::string> draw_filter;                  // diagnostic : -e33draws=none | sous-chaînes
    bool setup_draws = true;                               // -e33setupdraws=0 : ancien comportement (mise en place laissée au jeu)
    SetupDraws setup;
    std::optional<uint32_t> perturb_turn;                  // client : -e33perturb=N (test de resynchro)
    std::optional<uint32_t> perturb_end_turn;              // client : -e33perturbend=N (test de la fin imposée : ennemis increvables ici)
    bool imposing = false;                                 // une défense imposée est en cours (pas une entrée parasite)
    bool lock_input = true;                                // -e33lockinput=0 : laisser les entrées pendant les tours des autres
    bool turn_dep = false;                                 // -e33turndep=1 : attente du TurnStart par dépendance de début de tour (essai)
    double ready_idle_s = kReadyIdleTimeout;               // hôte : -e33readyidle=N
    double stuck_turn_s = kStuckTurnTimeout;               // hôte : -e33stuckturn=N
    bool turn_align = true;                                // hôte : démarrage synchronisé des tours ennemis (-e33turnalign=0 : coupé, D28)
    double barrier_battle_since = -1;                      // diagnostic : barrière tenue pendant un combat engagé (depuis)
    bool want_join = false;                                // client : -e33join=1 (rejoindre la partie de l'hôte, D12)
    // Défaite (D33) : seul l'hôte choisit sur l'écran de défaite (« Réessayer » / « Abandonner »), les clients suivent.
    bool defeat_sync = true;                               // -e33defeatsync=0 : ancien comportement (chacun choisit chez lui)
    bool defeat_abandon_resync = false;                    // -e33defeatabandon=resync : abandon de l'hôte suivi par un instantané (pas d'abandon ici)
    struct Defeat {
        bool armed = false;                                // défaite vue ici (StartBattleEndFlow, résultat 2)
        uint32_t battle = 0;                               // n° (mod) du combat perdu
        double at = -1;
        bool retry_prompted = false;                       // PromptBattleRetry vu : la fenêtre ouverte est « Réessayer / Abandonner »
        // Par fenêtre (0 « Réessayer / Abandonner », 1 « équipe de réserve ») : choix de l'hôte (-1 inconnu, 0 non, 1 oui)
        int host_choice[2] = {-1, -1};
        double host_choice_at[2] = {-1, -1};
        bool applied[2] = {false, false};                  // client : choix de l'hôte appliqué ici
        bool responded[2] = {false, false};                // réponse arrivée au gestionnaire de combat ici (quelle qu'en soit la source)
        bool rescue_sent = false;                          // client : instantané demandé (encore en combat 20 s après l'abandon)
        int prompt_kind = -1;                              // client : dernière fenêtre de choix vue ici (en attente du choix)
        double prompt_at = -1;
        UObject* prompt_modal = nullptr;
        double applied_at = -1;
        int done_kind = -1;                                // client : choix suivi qui finit l'attente (statut retiré ensuite)
        bool local_allowed = false;                        // client : repli, le joueur d'ici choisit (hôte muet)
        bool applying = false;                             // réponse donnée par le mod (pas un clic du joueur)
        double next_poll = 0;
        int hidden = 0;                                    // fenêtres de choix refermées ici
        bool diag_logged = false;
        int traces = 0;                                    // journal coop_defeat_trace (plafonné)
    } defeat;
    bool input_enabled = true;
    int defense_try_logs = 0;                              // diagnostic : appuis de défense du propriétaire journalisés
    int shoot_input_logs = 0;                              // diagnostic : tirs en visée libre journalisés
    int fa_part_logs = 0;   // journal coop_free_aim_part (plafonné)
    bool menu_wait_logged = false;   // combat de l'hôte en attente : menu du jeu ouvert ici
    double fa_diag_at = -1;                                // diagnostic : contextes 0,5 s après le début d'une visée libre d'ici
    bool enemy_turn = false;                               // tour en cours : un ennemi (tous les rôles)
    int imc_logs = 0;                                      // diagnostic : contextes d'entrée au début des tours ennemis
    int defense_path_logs = 0;                             // diagnostic : appuis de défense (contrôleur, gestionnaire)
    UObject* input_sub = nullptr;                          // EnhancedInputLocalPlayerSubsystem (pris au blocage)
    std::map<UObject*, int> removed_contexts;              // contextes d'entrée retirés pendant le blocage -> priorité
    std::map<std::string, int> counters;
    FreeAimOwner fa_own;                                   // visée d'un héros d'ici
    FreeAimReplay fa_rep;                                  // visée rejouée d'un héros d'un autre joueur
    std::optional<std::array<double, 3>> local_aim;        // test runner : point visé par le « joueur » (set_local_aim)
    // Session (phase 5)
    std::string my_name;
    std::map<uint8_t, std::string> roster;                 // joueurs connus (nom affiché)
    bool ended = false;                                    // client : session terminée (hôte parti, refus…)
    std::string end_key;                                   // texte affiché (clé de traduction)
    double ended_at = -1;
    bool title_requested = false;
    double hello_at = 0;
    bool takeover = false;                                 // hôte : héros repris, entrées locales à rendre
    uint16_t turn_timer_s = 0;                             // hôte : -e33turntimer=N (0 = aucun, par défaut)
    bool duo_parry = true;                                 // hôte : -e33duoparry=0 coupe le bonus « parade en duo »
    std::set<std::string> version_rejected;                // adresses d'un autre protocole déjà refusées
    std::map<std::pair<std::string, uint32_t>, double> tombstones;   // hôte : (adresse, époque) d'un pair retiré -> fin d'oubli
    // Steam (phase 6)
    bool steam = false;                                    // -e33transport=steam
    uint64_t lobby_wanted = 0;                             // client : -e33lobby=<id>
    net::steam::Lobby lobby;
    bool lobby_announced = false, lobby_error_shown = false, steam_error_shown = false;
    bool f7_down = false;
    double next_punch = 0;
    bool name_from_arg = false;
    bool steam_lobby = true;                               // -e33steamlobby=0 : pas de lobby (auto-test sans toucher au compte)
    bool steam_selftest = false;                           // -e33steamselftest=1 : messages Steam envoyés à soi-même
    uint32_t selftest_seq = 0;
    // Sans -e33role (partie normale) : « prêt au coop », rôle décidé en jeu (D18)
    bool neutral = false;                                  // mode par défaut : Steam, aucun rôle tant que rien n'est décidé
    bool runtime_session = false;                          // session ouverte en jeu : retour au mode neutre à la fin
    bool hooks_subscribed = false;
    std::map<std::string, uint8_t> owner_overrides_arg;    // -e33owners (rétabli au retour au mode neutre)
    bool invite_on_ready = false;                          // hôte ouvert par F7 : fenêtre d'invitation dès le lobby prêt
    uint64_t pending_lobby = 0;                            // invitation acceptée en pleine partie : rejointe au menu principal
    bool cmdline_lobby_used = false;
    uint64_t rejoin_host = 0;                              // après une mise à jour : partie de cet ami rejointe au menu
    double rejoin_until = 0, next_rejoin_try = 0;
    double session_msg_until = -1;                         // message de fin de session gardé après le retour au mode neutre
    bool at_menu = false, in_world = false, ready_hint_shown = false;
    double next_place_check = 0;
    bool options_dirty = false;                            // options changées en jeu : appliquées hors combat
    bool keys_shown = false;                               // rappel des touches affiché (menu pause)
};

State s;

void bump(const char* name, int n = 1) { s.counters[name] += n; }

// --- Utilitaires ------------------------------------------------------------------------

std::wstring arg(const wchar_t* key) { return cmdline::arg(key); }   // en test : options du scénario seulement

std::filesystem::path game_root()
{
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    // …/Expedition 33/Sandfall/Binaries/Win64/SandFall-Win64-Shipping.exe -> …/Expedition 33
    return std::filesystem::path(buf).parent_path().parent_path().parent_path().parent_path();
}

uint32_t read_build_id()
{
    // steamapps/common/<jeu> -> steamapps/appmanifest_1903340.acf
    const auto acf = game_root().parent_path().parent_path() / L"appmanifest_1903340.acf";
    std::ifstream f(acf);
    std::string line;
    while (std::getline(f, line)) {
        const auto p = line.find("\"buildid\"");
        if (p == std::string::npos) continue;
        const auto q1 = line.find('"', p + 9);
        const auto q2 = line.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) return static_cast<uint32_t>(std::strtoul(line.substr(q1 + 1, q2 - q1 - 1).c_str(), nullptr, 10));
    }
    return 0;
}

std::vector<std::string> read_gameplay_mods()
{
    static const char* builtin[] = {"CheatManagerEnablerMod", "ConsoleCommandsMod", "ConsoleEnablerMod", "SplitScreenMod", "LineTraceMod",
                                    "BPML_GenericFunctions", "BPModLoaderMod", "Keybinds", "E33Coop", "E33Explore"};
    std::vector<std::string> mods;
    const auto mods_txt = game_root() / L"Sandfall/Binaries/Win64/ue4ss/Mods/mods.txt";
    std::ifstream f(mods_txt);
    std::string line;
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || line.find(';') == 0) continue;
        std::string name = line.substr(0, colon);
        name.erase(name.find_last_not_of(" \t") + 1);
        if (line.find('1', colon) == std::string::npos) continue;
        if (std::find_if(std::begin(builtin), std::end(builtin), [&](const char* b) { return name == b; }) != std::end(builtin)) continue;
        mods.push_back("ue4ss:" + name);
    }
    std::error_code ec;
    for (const auto* dir : {L"Sandfall/Content/Paks/~mods", L"Sandfall/Content/Paks/LogicMods"}) {
        for (auto& e : std::filesystem::directory_iterator(game_root() / dir, ec))
            if (e.path().extension() == L".pak") mods.push_back("pak:" + e.path().filename().string());
    }
    std::sort(mods.begin(), mods.end());
    return mods;
}

void send_to(Peer& p, const proto::Message& m, bool reliable = true) { if (p.link) p.link->send(m, reliable); }
// Pair qui participe à la partie : accepté, et entré (instantané chargé) s'il a demandé à rejoindre. Un joueur qui
// charge encore l'instantané (ou attend la fin d'un combat pour entrer) ne reçoit pas les messages de jeu.
bool in_game(const Peer& p) { return p.accepted && p.drop.empty() && (s.role == Role::Client || explore::peer_in_game(p.player_id)); }
void broadcast(const proto::Message& m) { for (auto& p : s.peers) if (in_game(p)) send_to(p, m); }
// Hôte : à tous les clients sauf l'émetteur ; client : à l'hôte.
void publish(const proto::Message& m, const Peer* origin = nullptr, bool reliable = true)
{
    for (auto& p : s.peers) if (in_game(p) && &p != origin) send_to(p, m, reliable);
}

std::string player_name(uint8_t id)
{
    if (id == s.my_player_id && s.role != Role::Solo) return ui::text("you");
    auto it = s.roster.find(id);
    if (it != s.roster.end() && !it->second.empty()) return it->second;
    return id == 0 ? ui::text("host") : "Joueur " + std::to_string(id + 1);
}

// Joueur qui contrôle un personnage hors combat (builds, D13) et par défaut en combat : -e33owners (nom), sinon le
// k-ième personnage de l'équipe va au k-ième joueur (hôte, puis clients par numéro), l'hôte garde le reste.
uint8_t character_owner(const std::string& name)
{
    if (auto it = s.owner_overrides.find(name); it != s.owner_overrides.end()) return it->second;
    if (!s.owner_overrides.empty()) return 0;
    const auto party = game::party_names();
    const auto pos = std::find(party.begin(), party.end(), name);
    if (pos == party.end()) return 0;
    std::vector<uint8_t> players{0};
    for (auto& [id, n] : s.roster) if (id != 0) players.push_back(id);
    std::sort(players.begin() + 1, players.end());
    const auto idx = static_cast<size_t>(pos - party.begin());
    return idx < players.size() ? players[idx] : 0;
}

// Répartition lisible de l'équipe (« Maelle : Killian · Verso : TOTOFFF »), montrée à chacun après un changement.
std::string owners_summary()
{
    std::string out;
    for (auto& c : game::party_names()) out += (out.empty() ? "" : " · ") + c + " : " + player_name(character_owner(c));
    return out;
}

// Bannière « Équipe : … » seulement quand la répartition a changé (avant : à chaque Owners reçu, donc à chaque arrivée
// ou rechargement du client, jugé parasite par Killian le 2026-10-23).
void toast_owners_if_changed()
{
    static std::string last;
    const std::string sum = owners_summary();
    if (sum.empty() || sum == last) return;   // vide : partie pas encore chargée (arrivée du client)
    last = sum;
    ui::toast("owners_summary", {sum}, 8.0);
}

// Hôte : répartition actuelle des personnages de l'équipe, envoyée aux clients (ils l'appliquent telle quelle).
proto::Owners character_owner_map()
{
    proto::Owners ow;
    ow.battle_id = 0;
    for (auto& [name, player] : s.owner_overrides) ow.owners.push_back({0, name, player});
    return ow;
}

void cycle_owner(size_t index)
{
    const auto party = game::party_names();
    if (party.empty()) return;
    std::map<std::string, uint8_t> current;
    for (auto& c : party) current[c] = character_owner(c);
    const std::string ch = party[index % party.size()];
    std::vector<uint8_t> players{0};
    for (auto& [id, n] : s.roster) if (id != 0) players.push_back(id);
    std::sort(players.begin() + 1, players.end());
    const auto pos = std::find(players.begin(), players.end(), current[ch]);
    const uint8_t next = players[pos == players.end() ? 0 : static_cast<size_t>(pos - players.begin() + 1) % players.size()];
    current[ch] = next;
    s.owner_overrides.clear();
    for (auto& [c, p] : current) s.owner_overrides[c] = p;
    broadcast(character_owner_map());
    bump("owner_reassignments");
    ui::toast("owner_changed", {ch, player_name(next)});
    toast_owners_if_changed();
    log().write("coop_owner_reassigned", JsonObj().add("character", ch).add("player", static_cast<int>(next)));
}

// --- Battement « occupé » --------------------------------------------------------------------------
// Pendant un chargement de niveau, le thread de jeu ne tourne plus pendant 30 à 60 s : un thread à part envoie alors
// à chaque pair un paquet « occupé » (net::Link::busy_packet), pour qu'il ne nous croie pas partis.
struct Heartbeat {
    std::mutex mu;
    std::vector<std::pair<std::string, uint32_t>> targets;   // adresse, époque de notre lien vers elle
    std::atomic<int64_t> last_tick_ms{0};
    std::atomic<bool> started{false};
    std::atomic<int64_t> stall_of{0};   // diagnostic : last_tick_ms du gel du thread de jeu déjà journalisé (0 : aucun)
} hb;

int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count(); }

void heartbeat_targets()
{
    std::lock_guard lk(hb.mu);
    hb.targets.clear();
    for (auto& p : s.peers) if (p.link && p.drop.empty()) hb.targets.emplace_back(p.link->peer(), p.link->epoch());
}

void heartbeat_start()
{
    if (hb.started.exchange(true)) return;
    std::thread([] {
        for (;;) {
            Sleep(250);
            const int64_t last = hb.last_tick_ms.load();
            if (last == 0 || steady_ms() - last < 750) continue;   // thread de jeu vivant : rien à faire
            // Diagnostic (essai à deux PC du 2026-10-22 : journal du client coupé net) : un gel du thread de jeu est journalisé
            // d'ici ; un journal qui s'arrête sans « game_thread_stall » ni « mod_unload » = processus tué ou planté.
            if (const int64_t age = steady_ms() - last; age >= 5000 && hb.stall_of.load() != last) {
                hb.stall_of = last;
                log().write("game_thread_stall", JsonObj().add("ms", age));
            }
            std::vector<std::pair<std::string, uint32_t>> t;
            { std::lock_guard lk(hb.mu); t = hb.targets; }
            for (auto& [peer, epoch] : t) {
                const auto pkt = net::Link::busy_packet(epoch);
                s.transport->send(peer, pkt.data(), pkt.size());
            }
        }
    }).detach();
}

uint8_t owner_of_spot(int spot)
{
    auto it = s.battle.owner_by_spot.find(spot);
    return it == s.battle.owner_by_spot.end() ? 0 : it->second;
}
bool controls(int spot) { return owner_of_spot(spot) == s.my_player_id; }

proto::Unit actor_unit(UObject* actor) { return sync::unit_of(actor); }

// Visée libre : zone touchée (BP_FreeAimTargetPart_C, ex. point faible bleu) et sa position. Vu en vrai le 2026-10-24 :
// un tir rejoué au même point relatif au combattant manquait la petite boule (animation décalée d'un PC à l'autre).
int free_aim_part_index(UObject* actor, UObject* part)
{
    if (!actor || !part) return -1;
    const auto parts = ue::get_object_array(actor, STR("FreeAimTargetParts"));
    for (size_t i = 0; i < parts.size() && i < 0xFF; ++i) if (parts[i] == part) return static_cast<int>(i);
    return -1;
}

UObject* free_aim_part_at(UObject* actor, int index)
{
    const auto parts = ue::get_object_array(actor, STR("FreeAimTargetParts"));
    return index >= 0 && index < static_cast<int>(parts.size()) ? parts[static_cast<size_t>(index)] : nullptr;
}

bool free_aim_part_location(UObject* part, double out[3])
{
    ue::Call c(part, STR("K2_GetComponentLocation"));
    if (!c || !c.invoke() || c.param_size(STR("ReturnValue")) != 24) return false;
    const auto* v = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    return true;
}

void refresh_enemy_spots()
{
    s.battle.enemy_spots.clear();
    for (UObject* e : game::enemies()) s.battle.enemy_spots[e] = game::spot(e);
}
// Sans appel au jeu (hooks très fréquents) : emplacement d'un ennemi connu, -1 sinon.
int cached_enemy_spot(UObject* o)
{
    auto it = s.battle.enemy_spots.find(o);
    return it == s.battle.enemy_spots.end() ? -1 : it->second;
}

std::string hex64(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

void check_hash(uint32_t turn)
{
    auto l = s.battle.local_hash.find(turn);
    auto r = s.battle.remote_hash.find(turn);
    if (l == s.battle.local_hash.end() || r == s.battle.remote_hash.end()) return;
    bump("hashes_compared");
    if (l->second != r->second) {
        bump("desyncs");
        log().write("desync", JsonObj().add("battle", s.battle.id).add("turn", turn).add("local", hex64(l->second)).add("remote", hex64(r->second))
                                  .raw("state", proto::to_json(sync::snapshot(s.battle.id, turn))), turn);
    } else {
        log().write("hash_ok", JsonObj().add("battle", s.battle.id).add("hash", hex64(l->second)), turn);
    }
    s.battle.local_hash.erase(l);
    s.battle.remote_hash.erase(r);
}

// --- Réseau ------------------------------------------------------------------------------

void handle(Peer& p, proto::Message& m);
void on_player_left(uint8_t player, const std::string& name, const std::string& why);
void end_session(const std::string& key, const std::string& detail);
void defeat_on_host_choice(const proto::DefeatChoice& dc);
void turn_indicator(const proto::Unit& u);
void set_local_input(bool enabled, const char* why, bool local_turn = false);

void wire_link(net::Link& l)
{
    const bool host = s.role == Role::Host;
    l.on_invalid = [](const std::string& why) { bump("invalid_messages"); log().write("net_invalid", JsonObj().add("why", why)); };
    l.on_sent = [host](const proto::Message& m, bool) { s.recorder.write(host ? "h2c" : "c2h", m); log().write("net_send", JsonObj().raw("msg", proto::to_json(m))); };
    l.on_delivered = [host](const proto::Message& m) { s.recorder.write(host ? "c2h" : "h2c", m); log().write("net_recv", JsonObj().raw("msg", proto::to_json(m))); };
}

// Retire les pairs marqués (départ, silence, redémarrage, refus) : l'hôte reprend leurs héros.
void sweep_peers()
{
    bool changed = false;
    for (size_t i = 0; i < s.peers.size();) {
        Peer& p = s.peers[i];
        if (p.drop.empty() || (p.drop_at >= 0 && s.now < p.drop_at)) { ++i; continue; }
        if (p.link) p.link->update(s.now);   // derniers messages (Reject, Bye)
        const bool was_player = p.accepted;
        const uint8_t id = p.player_id;
        const std::string name = p.name, why = p.drop;
        if (p.link && p.link->remote_epoch()) s.tombstones[{p.link->peer(), p.link->remote_epoch()}] = s.now + 60.0;
        log().write("session_peer_removed", JsonObj().add("peer", p.link ? p.link->peer() : "").add("player_id", static_cast<int>(id)).add("why", why));
        s.peers.erase(s.peers.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        if (was_player && s.role == Role::Host) on_player_left(id, name, why);
    }
    if (changed) heartbeat_targets();
}

void pump_network()
{
    if (!s.transport || s.ended) return;
    net::Datagram d;
    while (s.transport->poll(d)) {
        Peer* peer = nullptr;
        for (auto& p : s.peers) if (p.link && p.link->peer() == d.peer) peer = &p;
        if (peer && !peer->drop.empty()) {   // en cours de retrait (refus) : il finit d'acquitter, rien de plus
            peer->link->on_datagram(d.data.data(), d.data.size(), s.now);
            continue;
        }
        if (s.role == Role::Host) {
            // Même adresse, nouvelle époque : le client a redémarré (nouveau processus, Steam : même compte). L'ancien
            // joueur est parti ; le nouveau se présente comme n'importe quel arrivant (Hello, puis instantané).
            const uint32_t ep = net::Link::packet_epoch(d.data.data(), d.data.size());
            if (peer && ep && peer->link->remote_epoch() && ep != peer->link->remote_epoch()) {
                peer->drop = "restarted";
                bump("peer_restarts");
                peer = nullptr;
                sweep_peers();
            }
            if (!peer && ep) {   // derniers paquets d'un pair qui vient d'être retiré (même processus) : ignorés
                auto ts = s.tombstones.find({d.peer, ep});
                if (ts != s.tombstones.end() && s.now < ts->second) continue;
            }
            if (!peer) {
                const uint16_t pv = net::Link::packet_protocol(d.data.data(), d.data.size());
                if (pv == 0 && d.data.size() == 8 && std::memcmp(d.data.data(), "E33S", 4) == 0) { bump("steam_selftest_received"); continue; }
                if (pv == 0) { log().write("net_ignored", JsonObj().add("peer", d.peer).add("why", "not an E33Coop packet")); continue; }
                if (pv != proto::kProtocolVersion) {
                    // Autre version du mod : on répond avec notre version (son jeu affiche l'écart), sans créer de pair.
                    const auto pkt = net::Link::version_packet(1);
                    s.transport->send(d.peer, pkt.data(), pkt.size());
                    if (s.version_rejected.insert(d.peer).second) {
                        bump("sessions_rejected");
                        const std::string who = s.steam ? net::steam::friend_name(net::steam::id_of(d.peer)) : std::string();
                        update::on_version_mismatch(pv > proto::kProtocolVersion, who.empty() ? ui::text("a_player") : who, "", false, 0);
                        log().write("session_reject", JsonObj().add("peer", d.peer).add("reason", static_cast<int>(proto::RejectReason::ProtocolMismatch))
                                                         .add("detail", "protocole hôte " + std::to_string(proto::kProtocolVersion) + " / client " + std::to_string(pv)));
                    }
                    continue;
                }
                Peer np;
                np.link = std::make_unique<net::Link>(*s.transport, d.peer);
                np.created_at = s.now;
                wire_link(*np.link);
                s.peers.push_back(std::move(np));
                peer = &s.peers.back();
                heartbeat_targets();
                log().write("net_peer", JsonObj().add("peer", d.peer));
            }
        }
        if (!peer) { log().write("net_ignored", JsonObj().add("peer", d.peer).add("why", "unknown peer")); continue; }
        peer->link->on_datagram(d.data.data(), d.data.size(), s.now);
    }
    for (size_t i = 0; i < s.peers.size(); ++i) {
        proto::Message m;
        while (s.peers[i].link && s.peers[i].drop.empty() && s.peers[i].link->pop(m)) handle(s.peers[i], m);
        if (s.ended) return;
        Peer& p = s.peers[i];
        if (!p.link) continue;
        p.link->update(s.now);
        // Silence : le pair est parti (jeu fermé, réseau coupé). Un pair qui n'a jamais répondu est un intrus ou une
        // adresse erronée : retiré au bout du même délai.
        if (p.drop.empty() && (p.link->timed_out(s.now, kLinkTimeout, kBusyMax) || (p.link->last_heard() < 0 && s.now - p.created_at > (s.role == Role::Host ? kHandshakeTimeout : kClientHandshakeTimeout)))) {
            if (s.role == Role::Host) {
                p.drop = "timeout";
                bump("peer_timeouts");
            } else {
                end_session(p.link->last_heard() < 0 ? "host_unreachable" : "host_lost", "");
                return;
            }
        }
        if (s.role == Role::Host && !p.accepted && p.drop.empty() && s.now - p.created_at > kHandshakeTimeout) p.drop = "no hello";
    }
    sweep_peers();
}

// Horloge du mod (s.now) : avance de dt à chaque image. Une attente bloquante (wait_for) l'a déjà avancée du temps
// attendu, et le dt de l'image suivante contient ce même temps : il ne compte qu'une fois. Avant, il comptait deux fois :
// essai à deux PC du 2026-10-22, attente de tour de 15 s chez le client -> horloge avancée de 30 s -> lien jugé muet
// 15 s (le dernier paquet de l'hôte datait de la fin de l'attente) -> « Connexion à l'hôte perdue », hôte bien vivant.
void advance_clock(double dt)
{
    s.now = std::max(s.now, s.tick_now + dt);
    s.tick_now = s.now;
}

// Valeur maximale d'un compteur (durées les plus longues : attentes, images).
void bump_max(const char* name, int v) { int& c = s.counters[name]; c = std::max(c, v); }

// Parade appuyée pendant une attente figée (D32, essai à deux PC du 2026-10-24, 18:27) : sur une attaque de zone, le
// jeu règle le coup du héros de l'autre joueur (attente de son résultat, image figée ~60-150 ms) puis, dans la même
// image, celui du héros d'ici. Un appui pendant l'attente n'est lu par le jeu qu'à l'image suivante : la parade du héros
// d'ici était jugée ratée. On lit donc la touche de parade nous-mêmes pendant l'attente (E au clavier, RB à la manette),
// et le coup du héros d'ici qui suit lance sa parade (même chemin que l'imposition d'un résultat).
// -e33waitparry=0 : ancien comportement.
bool wait_parry_enabled()
{
    static const bool v = arg(L"e33waitparry") != L"0";
    return v;
}

bool parry_key_down()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return false;
    if (GetAsyncKeyState('E') & 0x8000) return true;
    struct Pad { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    using GetState = DWORD(WINAPI*)(DWORD, Pad*);
    static GetState get = [] {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        return m ? reinterpret_cast<GetState>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    if (!get) return false;
    for (DWORD i = 0; i < 4; ++i) {
        Pad p{};
        if (get(i, &p) == 0 && (p.buttons & 0x0200)) return true;   // XINPUT_GAMEPAD_RIGHT_SHOULDER (RB)
    }
    return false;
}

// Pompe le réseau en bloquant le game thread jusqu'à cond() ou expiration (utilisé dans les hooks). L'image est figée
// pendant ce temps : chaque attente est mesurée (wait_max_ms), et journalisée avec son origine au-delà de 250 ms.
template <class Cond>
bool wait_for(const char* site, Cond cond, double timeout_s)
{
    const ULONGLONG start = GetTickCount64();
    const double base = s.now;
    bool ok = true;
    // Attente d'un résultat de défense : appui de parade d'ici relevé (front montant, voir parry_key_down).
    const bool watch_parry = wait_parry_enabled() && std::strcmp(site, "defense") == 0;
    bool parry_was_down = watch_parry && parry_key_down();
    ULONGLONG next_poll = 0;
    while (!cond()) {
        if (watch_parry && GetTickCount64() >= next_poll) {
            next_poll = GetTickCount64() + 4;
            const bool down = parry_key_down();
            if (down && !parry_was_down) { s.battle.wait_parry_at = GetTickCount64(); bump("parry_presses_during_wait"); }
            parry_was_down = down;
        }
        const double elapsed = (GetTickCount64() - start) / 1000.0;
        if (elapsed > timeout_s || s.ended) { ok = false; break; }
        s.now = base + elapsed;
        hb.last_tick_ms = steady_ms();
        pump_network();
        Sleep(0);   // Sleep(1) dure ~15 ms sous Windows : on cède juste la main
    }
    const int ms = static_cast<int>(GetTickCount64() - start);
    bump_max("wait_max_ms", ms);
    if (ms >= 250) {
        bump("waits_over_250ms");
        log().write("coop_wait_long", JsonObj().add("site", site).add("ms", ms).add("ok", ok), s.battle.turn);
    }
    return ok;
}

// Messages de jeu communs (hôte et client) : stockés pour les points de synchro ; l'hôte relaie aux autres.
bool handle_battle_common(Peer& p, proto::Message& m)
{
    auto& b = s.battle;
    // Hôte : preuve positive que ce joueur est dans ce tour chez lui (il agit, ouvre ses menus, vise, pare : en plus de
    // son StateHash du tour). Un joueur qui réfléchit, fouille ses menus ou lit une description n'est jamais « bloqué ».
    if (s.role == Role::Host) {
        auto sign = [&](uint32_t bid, uint32_t turn) {
            if (bid != b.id) return;
            b.acks[turn].insert(p.player_id);
            if (turn >= b.turn) b.unresponsive.erase(p.player_id);
        };
        if (auto* x = std::get_if<proto::ActionCommand>(&m)) sign(x->battle_id, x->turn);
        else if (auto* x1 = std::get_if<proto::ComboResult>(&m)) sign(x1->battle_id, x1->turn);
        else if (auto* x2 = std::get_if<proto::FreeAimEvent>(&m)) sign(x2->battle_id, x2->turn);
        else if (auto* x3 = std::get_if<proto::IntentPreview>(&m)) sign(x3->battle_id, x3->turn);
        else if (auto* x4 = std::get_if<proto::MenuState>(&m)) sign(x4->battle_id, x4->turn);
        else if (auto* x5 = std::get_if<proto::AimView>(&m)) sign(x5->battle_id, x5->turn);
        else if (auto* x6 = std::get_if<proto::DefenseResult>(&m)) sign(x6->battle_id, x6->turn);
    }
    if (auto* ac = std::get_if<proto::ActionCommand>(&m)) {
        if (ac->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(ac->actor_spot) != p.player_id) {
            log().write("net_rejected_msg", JsonObj().add("why", "action for a hero this player does not own").raw("msg", proto::to_json(m)));
            return true;
        }
        if (b.forced.count({ac->turn, ac->actor_spot})) {
            // Minuteur écoulé : l'hôte a déjà imposé l'action de ce tour (la commande du joueur s'est croisée avec la sienne).
            bump("turn_timer_races");
            log().write("coop_forced_action_conflict", JsonObj().raw("msg", proto::to_json(m)), b.turn);
            return true;
        }
        if (ac->forced) b.forced.insert({ac->turn, ac->actor_spot});
        b.commands[{ac->turn, ac->actor_spot}] = *ac;
        b.command_time[{ac->turn, ac->actor_spot}] = s.now;
        // Résultats de QTE déjà connus (faux client) : rangés comme des ComboResult, avec les instants d'appui
        // canoniques du test runner (parfait à 88 % de la fenêtre, réussi à 30 %).
        for (size_t i = 0; i < ac->combo.size(); ++i) {
            const bool ok = (ac->combo[i] & 1) != 0, perfect = (ac->combo[i] & 2) != 0;
            b.combos[{ac->turn, ac->actor_spot, static_cast<uint16_t>(i + 1)}] = {ac->battle_id, ac->turn, ac->actor_spot, static_cast<uint16_t>(i + 1),
                                                                               ok || perfect, perfect, static_cast<uint16_t>(perfect ? 880 : ok ? 300 : proto::kNoPress)};
        }
        if (s.role == Role::Host) publish(*ac, &p);
        return true;
    }
    if (auto* cr = std::get_if<proto::ComboResult>(&m)) {
        if (cr->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(cr->spot) != p.player_id) return true;
        b.combos[{cr->turn, cr->spot, cr->seq}] = *cr;
        if (s.role == Role::Host) publish(*cr, &p);
        return true;
    }
    if (auto* fa = std::get_if<proto::FreeAimEvent>(&m)) {
        if (fa->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(fa->spot) != p.player_id) return true;
        const std::pair<uint32_t, int> key{fa->turn, fa->spot};
        if (fa->kind == proto::FreeAimKind::Start) {
            b.fa_events[key].clear();
            b.fa_arrival[key] = s.now;
            // Nouvelle visée du même héros dans le même tour (il est sorti puis revenu) : le rejeu repart de zéro.
            if (s.fa_rep.key == key && s.fa_rep.started) {
                if (!s.fa_rep.done) { ue::Call c(game::battle_manager(), STR("CancelFreeAimFromUI")); if (c) c.invoke(); }
                s.fa_rep = FreeAimReplay{key};
            }
        }
        b.fa_events[key].push_back(*fa);
        if (s.role == Role::Host) publish(*fa, &p, fa->kind != proto::FreeAimKind::Aim);
        return true;
    }
    // Aperçu de l'action en préparation et caméra de visée libre d'un autre joueur (affichage seulement).
    if (auto* ip = std::get_if<proto::IntentPreview>(&m)) {
        if (ip->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(ip->spot) != p.player_id) return true;
        intent::receive(*ip);
        if (s.role == Role::Host) publish(*ip, &p);
        return true;
    }
    if (auto* ms = std::get_if<proto::MenuState>(&m)) {
        if (ms->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(ms->spot) != p.player_id) return true;
        intent::receive(*ms);
        if (s.role == Role::Host) publish(*ms, &p);
        return true;
    }
    if (auto* av = std::get_if<proto::AimView>(&m)) {
        if (av->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(av->spot) != p.player_id) return true;
        intent::receive(*av);
        if (s.role == Role::Host) publish(*av, &p, false);
        return true;
    }
    if (auto* dr = std::get_if<proto::DefenseResult>(&m)) {
        if (dr->battle_id != b.id) return true;
        if (s.role == Role::Host && owner_of_spot(dr->spot) != p.player_id) return true;
        b.defenses[{dr->turn, dr->spot, dr->hit}] = dr->result;
        if (s.role == Role::Host) publish(*dr, &p);
        return true;
    }
    if (auto* sh = std::get_if<proto::StateHash>(&m)) {
        if (sh->battle_id == b.id) {
            // Hôte : ce joueur a commencé ce tour chez lui (il répond de nouveau s'il était bloqué).
            if (s.role == Role::Host) {
                b.acks[sh->turn].insert(p.player_id);
                if (sh->turn >= b.turn && b.unresponsive.erase(p.player_id))
                    log().write("coop_player_responsive", JsonObj().add("player", static_cast<int>(p.player_id)), b.turn);
            }
            b.remote_hash[sh->turn] = sh->hash;
            check_hash(sh->turn);
        }
        return true;
    }
    if (auto* ping = std::get_if<proto::Ping>(&m)) { send_to(p, proto::Pong{ping->t_us}, false); return true; }
    return false;
}

uint8_t free_player_id()
{
    for (uint8_t id = 1; id <= kMaxClients; ++id) {
        bool used = false;
        for (auto& p : s.peers) if (p.accepted && p.drop.empty() && p.player_id == id) used = true;
        if (!used) return id;
    }
    return 0xFF;
}

void send_roster()
{
    proto::Roster r;
    r.players.push_back({0, s.my_name});
    for (auto& p : s.peers) if (p.accepted && p.drop.empty()) r.players.push_back({p.player_id, p.name});
    s.roster.clear();
    for (auto& e : r.players) s.roster[e.player] = e.name;
    for (auto& p : s.peers) if (p.accepted && p.drop.empty()) send_to(p, r);
}

void reject(Peer& p, proto::RejectReason reason, const std::string& detail)
{
    send_to(p, proto::Reject{reason, detail});
    p.drop = "rejected";
    p.drop_at = s.now + 2.0;   // le temps que le Reject parte (renvoyé tant qu'il n'est pas acquitté)
    bump("sessions_rejected");
    log().write("session_reject", JsonObj().add("peer", p.link->peer()).add("reason", static_cast<int>(reason)).add("detail", detail));
}

void handle(Peer& p, proto::Message& m)
{
    if (s.role == Role::Host) {
        if (auto* h = std::get_if<proto::Hello>(&m)) {
            if (p.accepted) return;   // doublon
            proto::RejectReason reason = proto::RejectReason::Other;
            std::string diff;
            if (h->protocol != proto::kProtocolVersion) { reason = proto::RejectReason::ProtocolMismatch; diff = "protocole hôte " + std::to_string(proto::kProtocolVersion) + " / client " + std::to_string(h->protocol); }
            else if (h->game_build != s.game_build) { reason = proto::RejectReason::GameBuildMismatch; diff = "build du jeu hôte " + std::to_string(s.game_build) + " / client " + std::to_string(h->game_build); }
            else if (h->mod_version != kModVersion) { reason = proto::RejectReason::ModVersionMismatch; diff = std::string("version du mod hôte ") + kModVersion + " / client " + h->mod_version; }
            else if (h->gameplay_mods != s.gameplay_mods) {
                reason = proto::RejectReason::ModListMismatch;
                diff = "mods de gameplay différents :";
                for (auto& x : s.gameplay_mods) if (std::find(h->gameplay_mods.begin(), h->gameplay_mods.end(), x) == h->gameplay_mods.end()) diff += " hôte seul=" + x;
                for (auto& x : h->gameplay_mods) if (std::find(s.gameplay_mods.begin(), s.gameplay_mods.end(), x) == s.gameplay_mods.end()) diff += " client seul=" + x;
            }
            if (diff.empty() && free_player_id() == 0xFF) { reason = proto::RejectReason::SessionFull; diff = "partie complète (3 joueurs)"; }
            if (!diff.empty()) {
                reject(p, reason, diff);
                // Écart de version du mod : le côté le plus ancien est invité à mettre à jour (native/sync/update).
                if (reason == proto::RejectReason::ModVersionMismatch)
                    update::on_version_mismatch(release::compare_versions(kModVersion, h->mod_version) < 0,
                                                h->player_name.empty() ? ui::text("a_player") : h->player_name, h->mod_version, false, 0);
                return;
            }
            p.accepted = true;
            p.player_id = free_player_id();
            p.wants_join = h->join;
            p.name = h->player_name.empty() ? "Joueur " + std::to_string(p.player_id + 1) : h->player_name;
            send_to(p, proto::Welcome{p.player_id, s.session_id});
            explore::on_peer_accepted(p.player_id, h->join);
            send_roster();
            bump("sessions_accepted");
            if (!s.owner_overrides.empty()) send_to(p, character_owner_map());
            ui::toast("player_joined", {p.name});
            log().write("session_join", JsonObj().add("player", p.name).add("player_id", static_cast<int>(p.player_id)).add("join", h->join));
            return;
        }
        if (!p.accepted) return;
        if (std::get_if<proto::Bye>(&m)) {
            if (p.drop.empty()) p.drop = "bye";
            bump("peer_byes");
            return;
        }
        if (auto* br = std::get_if<proto::BattleReady>(&m)) {
            if (br->battle_id == s.battle.id) s.battle.ready.insert(p.player_id);
            log().write("coop_battle_ready", JsonObj().add("player", static_cast<int>(p.player_id)).add("battle", br->battle_id));
            return;
        }
        if (builds::handle(p.player_id, m)) return;
        if (explore::handle(p.player_id, m)) return;
        if (handle_battle_common(p, m)) return;
        return;
    }
    // Client
    if (auto* w = std::get_if<proto::Welcome>(&m)) {
        s.accepted = true; s.my_player_id = w->player_id; s.session_id = w->session_id;
        explore::set_player_id(w->player_id);
        builds::set_player_id(w->player_id);
        log().write("session_welcome", JsonObj().add("player_id", static_cast<int>(w->player_id)));
        return;
    }
    if (auto* r = std::get_if<proto::Reject>(&m)) {
        s.rejected = true;
        log().write("session_rejected", JsonObj().add("reason", static_cast<int>(r->reason)).add("detail", r->detail));
        // Version de l'hôte : données du lobby Steam (lues avant de le quitter), sinon le détail (« … hôte X / client Y »).
        std::string hv = s.steam && s.lobby.id ? net::steam::lobby_data(s.lobby.id, "mod") : std::string();
        if (hv.empty()) {
            const auto a = r->detail.find("hôte "), b = r->detail.find(" /");
            if (a != std::string::npos && b != std::string::npos && b > a) hv = r->detail.substr(a + 6, b - a - 6);
        }
        const uint64_t owner = s.lobby.owner;
        end_session("rejected", r->reason == proto::RejectReason::SessionFull ? ui::text("reject_full") : r->detail);
        if (r->reason == proto::RejectReason::ModVersionMismatch)
            update::on_version_mismatch(release::compare_versions(kModVersion, hv) < 0, player_name(0), hv, true, owner);
        return;
    }
    if (auto* bye = std::get_if<proto::Bye>(&m)) {
        log().write("session_host_bye", JsonObj().add("reason", bye->reason));
        end_session("host_left", bye->reason);
        return;
    }
    if (auto* ro = std::get_if<proto::Roster>(&m)) {
        s.roster.clear();
        for (auto& e : ro->players) s.roster[e.player] = e.name;
        return;
    }
    if (auto* ow = std::get_if<proto::Owners>(&m)) {
        if (ow->battle_id == 0) {   // répartition des personnages hors combat (réattribution par l'hôte, F6)
            s.owner_overrides.clear();
            for (auto& o : ow->owners) s.owner_overrides[o.character] = o.player_id;
            bump("owner_maps_received");
            toast_owners_if_changed();
            log().write("coop_character_owners", JsonObj().raw("msg", proto::to_json(m)));
            return;
        }
        if (ow->battle_id == s.battle.id) {
            for (auto& o : ow->owners) s.battle.owner_by_spot[o.spot] = o.player_id;
            bump("owner_updates");
            log().write("coop_owners", JsonObj().raw("msg", proto::to_json(m)));
        }
        return;
    }
    if (auto* bs = std::get_if<proto::BattleStart>(&m)) {
        // Le combat ne démarre qu'une fois le monde local prêt (voir tick) : l'hôte peut être en avance.
        if (bs->battle_id == 0) {   // avis précoce (StartBattle de l'hôte) : ennemi, équipe, engagement ; le vrai suit
            s.early_start = *bs;
            s.early_start_at = s.now;
            log().write("coop_battle_notice_received", JsonObj().add("enemy", bs->world_enemy).add("engage", static_cast<int>(bs->engage)));
            return;
        }
        s.pending_start = *bs;
        s.last_start = *bs;
        s.battle = Battle{};   // remis à zéro dès maintenant : les messages suivants de l'hôte s'y accumulent
        s.battle.id = bs->battle_id;
        s.battle.start_rx_at = s.now;
        for (auto& o : bs->owners) s.battle.owner_by_spot[o.spot] = o.player_id;
        s.battle.rng_seed = bs->rng_seed;
        // Mise en place déjà commencée ici avec la graine de l'avis précoce : ce doit être la même (D30).
        if (s.setup.open && s.setup.seeded && s.setup.seed != bs->rng_seed) {
            bump("setup_seed_mismatches");
            log().write("coop_setup_seed_mismatch", JsonObj().add("setup", std::to_string(s.setup.seed)).add("host", std::to_string(bs->rng_seed)).add("source", s.setup.source));
        }
        s.battle.turn_timer_s = bs->turn_timer_s;
        s.battle.duo_parry = bs->duo_parry != 0;   // réglage de l'hôte, jamais le nôtre : même calcul partout
        log().write("coop_battle_start_received", JsonObj().add("battle", bs->battle_id).add("encounter", bs->encounter));
        return;
    }
    if (builds::handle(0, m)) return;
    if (explore::handle(0, m)) return;
    if (handle_battle_common(p, m)) return;
    if (auto* ts = std::get_if<proto::TurnStart>(&m)) {
        if (ts->battle_id == s.battle.id) s.battle.host_turns[ts->turn] = *ts;
    } else if (auto* ss = std::get_if<proto::StateSync>(&m)) {
        if (ss->battle_id == s.battle.id) s.battle.host_sync[ss->turn] = *ss;
    } else if (auto* dw = std::get_if<proto::Draws>(&m)) {
        if (dw->battle_id == s.battle.id)
            for (auto& d : dw->draws) s.battle.host_draws[dw->turn][(static_cast<uint64_t>(d.key) << 16) | d.rank] = d.value;
    } else if (auto* be = std::get_if<proto::BattleEnd>(&m)) {
        if (be->battle_id == s.battle.id) { s.battle.host_end = be->result; s.battle.host_end_at = s.now; }
        log().write("coop_battle_end_received", JsonObj().add("battle", be->battle_id).add("result", static_cast<int>(be->result)));
        // Combat fini chez l'hôte avant d'avoir pu être lancé ici (monde pas prêt, lancement impossible) : il ne l'est plus,
        // sinon il partirait plus tard, hors de propos (l'hôte est passé à autre chose).
        if (s.pending_start && s.pending_start->battle_id == be->battle_id) {
            s.pending_start.reset();
            bump("battle_starts_dropped");
            log().write("coop_battle_start_dropped", JsonObj().add("battle", be->battle_id).add("why", "host battle ended before it started here"));
            if (!s.battle.active) s.battle.finalized = true;   // jamais commencé ici : rien à clore dans le jeu
        }
    } else if (auto* rw = std::get_if<proto::BattleRewards>(&m)) {
        if (rw->battle_id == s.battle.id) s.battle.host_rewards = *rw;
    } else if (auto* dc = std::get_if<proto::DefeatChoice>(&m)) {
        defeat_on_host_choice(*dc);
    }
}

// --- Tirages de gameplay (D10) ------------------------------------------------------------

bool on_game_thread() { return s.game_thread != 0 && GetCurrentThreadId() == s.game_thread; }

// Appelant Blueprint d'une fonction native hookée. Le cadre fourni par UE4SS est celui de la fonction
// elle-même (objet = CDO de KismetMathLibrary, vérifié en jeu le 2026-10-19) : on remonte PreviousFrame
// jusqu'au premier objet qui n'est pas ce CDO.
UObject* script_caller(HookCtx& ctx, UFunction** node_out = nullptr)
{
    RC::Unreal::FFrame* f = &ctx.TheStack;
    for (int depth = 0; f && depth < 4; ++depth) {
        UObject* o = f->Object();
        if (o && o != ctx.Context) {
            if (node_out) *node_out = f->Node();
            return o;
        }
        RC::Unreal::FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    return nullptr;
}

// Fenêtre de mise en place ouverte (D30). Sécurité : jamais plus de 120 s (un combat annulé ne la laisse pas ouverte
// en exploration, où les tirages ne doivent jamais être touchés).
bool setup_window()
{
    return s.setup.open && s.now - s.setup.opened_at < 120.0;
}

// Diagnostic : chaîne des cadres de script d'un tirage (5 premiers appels par fonction, puis 600 par combat, et 400
// pendant la mise en place).
void trace_random(HookCtx& ctx, const std::string& fname)
{
    static std::map<std::string, int> seen;
    const bool early = seen[fname]++ < 5;
    const bool in_battle = s.battle.active && s.battle.traced_draws < 600;
    const bool in_setup = !in_battle && setup_window() && s.setup.traced < 400;
    if (!early && !in_battle && !in_setup) return;
    if (in_battle) ++s.battle.traced_draws;
    if (in_setup) ++s.setup.traced;
    std::string frames = "[";
    RC::Unreal::FFrame* f = &ctx.TheStack;
    for (int depth = 0; f && depth < 4; ++depth) {
        if (frames.size() > 1) frames += ",";
        frames += JsonObj().add("o", ue::full_name(f->Object())).add("n", ue::short_name(f->Node())).add("enemy", cached_enemy_spot(f->Object())).str();
        RC::Unreal::FFrame* prev = f->PreviousFrame();
        if (prev == f) break;
        f = prev;
    }
    log().write("rng_trace", JsonObj().add("fn", fname).add("battle", s.battle.active).add("setup", setup_window() && !(s.battle.active && s.battle.turn > 0)).raw("frames", frames + "]")
                                 .add("has_result", ctx.RESULT_DECL != nullptr), s.battle.turn);
}

enum class RandomKind { IntInRange, Int, Float, FloatInRange, Bool, BoolWeight };
struct RandomFn { const wchar_t* name; RandomKind kind; };

uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Uniforme dans [0, 1), identique sur toutes les instances pour (graine, tour, flux, rang).
double derived_uniform_seed(uint64_t seed, uint32_t turn, uint32_t key, uint16_t rank)
{
    const uint64_t h = splitmix64(seed ^ splitmix64((static_cast<uint64_t>(turn) << 32) | key) ^ splitmix64(0x5EED0000ull + rank));
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

double derived_uniform(uint32_t turn, uint32_t key, uint16_t rank) { return derived_uniform_seed(s.battle.rng_seed, turn, key, rank); }

uint32_t fnv32(const std::string& str)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : str) { h ^= c; h *= 16777619u; }
    return h;
}

// Prochain tirage d'un flux : rang dans le tour, valeur uniforme.
double next_uniform(const std::string& stream, uint32_t& key, uint16_t& rank)
{
    key = fnv32(stream);
    rank = s.battle.draw_rank[key]++;
    s.battle.stream_names.emplace(key, stream);
    return derived_uniform(s.battle.turn, key, rank);
}

void record_draw(const std::string& stream, uint32_t key, uint16_t rank, double v, double a, double b)
{
    bump("gameplay_draws");
    s.battle.local_draws[s.battle.turn][(static_cast<uint64_t>(key) << 16) | rank] = v;
    log().write("coop_draw", JsonObj().add("stream", stream).add("rank", static_cast<int>(rank)).add("value", v).add("a", a).add("b", b), s.battle.turn);
    if (s.role == Role::Host) broadcast(proto::Draws{s.battle.id, s.battle.turn, {{key, rank, v}}});
}

// Nom du flux si le tirage vient du gameplay (premier appelant BP), vide sinon.
// Gameplay (traces du 2026-10-19, GAME-INTERNALS §15) : IA d'un ennemi, critiques, Luminas (BP_LU_*), activation de
// buffs, effets de buffs, scripts de compétence, récompenses (RollBattleRewards + système de butin), mécaniques propres
// des héros (composants *Mechanic* / StainHolder : roue de Monoco, taches de Lune…, D30).
// setup : mise en place du combat (avant le tour 1) : le cache des ennemis est celui du combat précédent chez l'hôte,
// il n'est pas consulté (même nom de flux partout).
std::string gameplay_stream(HookCtx& ctx, const std::string& fname, bool setup = false)
{
    UFunction* node = nullptr;
    UObject* caller = script_caller(ctx, &node);
    if (!caller) return {};
    const int es = setup ? -1 : cached_enemy_spot(caller);
    const std::string cls = game::char_class(caller);
    const std::string fn = ue::short_name(node);
    // Mise en place : les ennemis ne sont pas encore dans le cache (rempli au tour 1). Vu en vrai le 2026-10-24 : le
    // Portier tire un RandomBool à son OnBattleStart qui décide de ses attaques (zone chez l'hôte, un seul héros chez le
    // client). Flux par classe, sans emplacement (peut-être pas encore attribué) : l'ordre des appels départage.
    const bool setup_enemy = setup && ue::is_a(caller, STR("BP_jRPG_Enemy_Battle_Base_C"));
    if (setup_enemy) bump("setup_enemy_draws");
    const bool gameplay = es >= 0 || setup_enemy || cls == "BP_BattleDamages_C" || cls.rfind("BP_LU_", 0) == 0 || cls == "AC_jRPG_CharacterBattleStats_C" ||
                          cls.rfind("BP_BattleBuff", 0) == 0 || cls.rfind("BP_LootSystemComponent", 0) == 0 ||
                          cls.find("Mechanic") != std::string::npos || cls.find("StainHolder") != std::string::npos ||
                          (cls == "AC_jRPG_BattleManager_C" && fn == "RollBattleRewards") || ue::is_a(caller, STR("BP_Battle_SkillScript_C"));
    if (!gameplay) return {};
    std::string name = cls + ":" + fn + ":" + fname;
    if (es >= 0) name += "#" + std::to_string(es);   // un flux par ennemi (même classe possible)
    return name;
}

// Tirage de la mise en place (D30) : graine de la fenêtre, « tour 0 », rang propre à la fenêtre.
double next_setup_uniform(const std::string& stream, uint32_t& key, uint16_t& rank)
{
    key = fnv32(stream);
    rank = s.setup.rank[key]++;
    s.setup.names.emplace(key, stream);
    return derived_uniform_seed(s.setup.seed, 0, key, rank);
}

void record_setup_draw(const std::string& stream, uint32_t key, uint16_t rank, double v, double a, double b)
{
    bump("setup_draws_replaced");
    s.setup.draws[(static_cast<uint64_t>(key) << 16) | rank] = v;
    log().write("coop_draw", JsonObj().add("stream", stream).add("rank", static_cast<int>(rank)).add("value", v).add("a", a).add("b", b).add("setup", true), 0);
}

// Remplace la valeur de retour par la valeur dérivée ; l'hôte la publie pour vérification (mise en place : publiée
// avec le BattleStart, voir host_turn_start).
void replace_draw(HookCtx& ctx, UFunction* f, RandomKind kind, const std::string& stream, bool setup = false)
{
    uint32_t key = 0;
    uint16_t rank = 0;
    const double u = setup ? next_setup_uniform(stream, key, rank) : next_uniform(stream, key, rank);
    auto num = [&](const wchar_t* name, double& out) {
        int size = 0;
        void* p = ue::param_address(ctx, f, name, &size);
        if (!p) return false;
        if (kind == RandomKind::IntInRange || kind == RandomKind::Int) out = size == 4 ? *static_cast<int32_t*>(p) : static_cast<double>(*static_cast<int64_t*>(p));
        else out = size == 4 ? *static_cast<float*>(p) : *static_cast<double*>(p);
        return true;
    };
    double v = 0, a = 0, b = 0;
    bool ok = true;
    switch (kind) {
    case RandomKind::IntInRange: {
        ok = num(STR("Min"), a) && num(STR("Max"), b);
        const int64_t range = static_cast<int64_t>(b) - static_cast<int64_t>(a) + 1;   // FMath::RandRange : bornes incluses
        v = a + (range > 0 ? std::min<int64_t>(static_cast<int64_t>(u * range), range - 1) : 0);
        if (ok) *static_cast<int32_t*>(ctx.RESULT_DECL) = static_cast<int32_t>(v);
        break;
    }
    case RandomKind::Int:
        ok = num(STR("Max"), b);
        v = b > 0 ? std::min(std::floor(u * b), b - 1) : 0;
        if (ok) *static_cast<int32_t*>(ctx.RESULT_DECL) = static_cast<int32_t>(v);
        break;
    case RandomKind::Float:
        v = u;
        *static_cast<double*>(ctx.RESULT_DECL) = v;
        break;
    case RandomKind::FloatInRange:
        ok = num(STR("Min"), a) && num(STR("Max"), b);
        v = a + (b - a) * u;
        if (ok) *static_cast<double*>(ctx.RESULT_DECL) = v;
        break;
    case RandomKind::Bool:
        v = u < 0.5 ? 1 : 0;
        *static_cast<uint8_t*>(ctx.RESULT_DECL) = static_cast<uint8_t>(v);
        break;
    case RandomKind::BoolWeight:
        ok = num(STR("Weight"), a);
        v = (a > 0 && u <= a) ? 1 : 0;
        if (ok) *static_cast<uint8_t*>(ctx.RESULT_DECL) = static_cast<uint8_t>(v);
        break;
    }
    if (!ok) {
        bump("draw_errors");
        log().write("coop_draw_param_missing", JsonObj().add("stream", stream), s.battle.turn);
        return;
    }
    if (setup) record_setup_draw(stream, key, rank, v, a, b);
    else record_draw(stream, key, rank, v, a, b);
}

bool draw_allowed(const std::string& stream)
{
    if (s.draw_filter.empty()) return true;
    for (auto& f : s.draw_filter) {
        if (f == "none") return false;
        if (stream.find(f) != std::string::npos) return true;
    }
    return false;
}

double natural_value(HookCtx& ctx, RandomKind kind)
{
    switch (kind) {
    case RandomKind::IntInRange: case RandomKind::Int: return *static_cast<int32_t*>(ctx.RESULT_DECL);
    case RandomKind::Float: case RandomKind::FloatInRange: return *static_cast<double*>(ctx.RESULT_DECL);
    default: return *static_cast<uint8_t*>(ctx.RESULT_DECL) ? 1.0 : 0.0;
    }
}

// Client : compare les tirages de gameplay d'un tour terminé avec ceux de l'hôte (même flux, même rang, même valeur).
void compare_draws(uint32_t turn)
{
    auto& mine = s.battle.local_draws[turn];
    auto& host = s.battle.host_draws[turn];
    auto name = [](uint64_t k) {
        auto it = s.battle.stream_names.find(static_cast<uint32_t>(k >> 16));
        return it != s.battle.stream_names.end() ? it->second : std::to_string(k >> 16);
    };
    // Nombre de tirages par flux de chaque côté : un tirage au-delà du dernier rang de l'autre côté vient d'un
    // coup en plus (le nombre de coups d'une compétence varie d'un run à l'autre, même en solo : GAME-INTERNALS §15),
    // pas d'un tirage faux. Compté à part (draw_count_diffs) ; l'état complet de l'hôte corrige ses effets.
    std::map<uint32_t, uint16_t> mine_n, host_n;
    for (auto& [k, v] : mine) { auto& n = mine_n[static_cast<uint32_t>(k >> 16)]; n = std::max<uint16_t>(n, static_cast<uint16_t>((k & 0xFFFF) + 1)); }
    for (auto& [k, v] : host) { auto& n = host_n[static_cast<uint32_t>(k >> 16)]; n = std::max<uint16_t>(n, static_cast<uint16_t>((k & 0xFFFF) + 1)); }
    auto tail = [](const std::map<uint32_t, uint16_t>& other, uint64_t k) {
        auto it = other.find(static_cast<uint32_t>(k >> 16));
        return (k & 0xFFFF) >= (it == other.end() ? 0u : it->second);
    };
    for (auto& [k, v] : mine) {
        auto it = host.find(k);
        if (it == host.end() && tail(host_n, k)) {
            bump("draw_count_diffs");
            log().write("coop_draw_count_diff", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("extra", "local"), turn);
        } else if (it == host.end() || std::fabs(it->second - v) > 1e-9) {
            bump("draw_mismatches");
            log().write("coop_draw_mismatch", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("local", v)
                                                  .add("host_has", it != host.end()).add("host", it != host.end() ? it->second : 0.0), turn);
        }
    }
    for (auto& [k, v] : host) {
        if (mine.count(k)) continue;
        if (tail(mine_n, k)) {
            bump("draw_count_diffs");
            log().write("coop_draw_count_diff", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("extra", "host"), turn);
            continue;
        }
        bump("draw_mismatches");
        log().write("coop_draw_mismatch", JsonObj().add("stream", name(k)).add("rank", static_cast<int>(k & 0xFFFF)).add("local_has", false).add("host", v), turn);
    }
    s.battle.local_draws.erase(turn);
    s.battle.host_draws.erase(turn);
}

// --- Mise en place du combat (D30) ---------------------------------------------------------

void setup_reset(uint64_t seed, bool seeded, const std::string& source)
{
    s.setup = SetupDraws{};
    s.setup.open = true;
    s.setup.opened_at = s.now;
    s.setup.seed = seed;
    s.setup.seeded = seeded;
    s.setup.source = source;
    log().write("coop_setup_open", JsonObj().add("seeded", seeded).add("source", source).add("seed", std::to_string(seed)).add("enabled", s.setup_draws));
}

// Hôte : graine du prochain combat choisie dès son lancement (avis précoce d'un combat de la carte, ou StartBattleNEW,
// commun à tous les lancements). host_turn_start la reprend pour le BattleStart : mise en place et tours, même graine.
void host_setup_open(bool notice)
{
    const auto& u = s.setup;
    const bool recent = u.open && s.now - u.opened_at < 30.0;
    // Avis précoce : StartBattleNEW a pu passer juste avant (même combat). StartBattleNEW : fenêtre gardée seulement si
    // elle vient de l'avis précoce de ce combat.
    const bool keep = recent && (notice ? true : (u.from_notice && !u.bm_started));
    if (!keep) {
        const uint64_t seed = s.fixed_seed ? *s.fixed_seed
                                           : splitmix64((static_cast<uint64_t>(GetTickCount64()) << 20) ^ s.session_id ^ (static_cast<uint64_t>(s.next_battle_id) << 40));
        setup_reset(seed, true, notice ? "host notice" : "host StartBattleNEW");
        s.setup.from_notice = notice;
    }
    if (!notice) s.setup.bm_started = true;
}

// Client : graine du combat de l'hôte, si elle est déjà connue au lancement du combat ici (combat lancé par le mod après
// le BattleStart, ou engagé par le pion suiveur après l'avis précoce). Sinon tirages laissés au jeu (réparés au tour 1).
void client_setup_open()
{
    const proto::BattleStart* p = nullptr;
    std::string src = "none";
    if (s.pending_start) { p = &*s.pending_start; src = "pending BattleStart"; }
    else if (s.last_start && s.last_start->battle_id == s.battle.id && s.battle.turn == 0 && !s.battle.finalized) { p = &*s.last_start; src = "BattleStart"; }
    else if (s.early_start && s.now - s.early_start_at < 30.0 && s.early_start->rng_seed != 0) { p = &*s.early_start; src = "notice"; }
    setup_reset(p ? p->rng_seed : 0, p && p->rng_seed != 0, src);
    s.setup.bm_started = true;
}

// Fin de la mise en place (tour 1, ou fin de combat) : ses tirages deviennent ceux du « tour 0 » du combat.
void setup_close()
{
    if (!s.setup.open) return;
    s.setup.open = false;
    for (auto& [k, v] : s.setup.draws) s.battle.local_draws[0][k] = v;
    for (auto& [k, n] : s.setup.names) s.battle.stream_names.emplace(k, n);
    log().write("coop_setup_closed", JsonObj().add("draws", static_cast<int>(s.setup.draws.size())).add("seeded", s.setup.seeded)
                                         .add("source", s.setup.source), s.battle.turn);
}

// Client, tour 1 : tirages de la mise en place comparés à ceux de l'hôte (publiés avec son BattleStart). Un hôte plus
// ancien (ou un enregistrement d'avant D30) n'en publie pas : rien à comparer.
void compare_setup_draws()
{
    auto& b = s.battle;
    if (!b.host_draws.count(0) || !s.setup_draws || !s.setup.seeded) {
        log().write("coop_setup_draws_unverified", JsonObj().add("local", static_cast<int>(b.local_draws[0].size())).add("host_published", b.host_draws.count(0) > 0)
                                                       .add("seeded", s.setup.seeded).add("enabled", s.setup_draws), b.turn);
        b.local_draws.erase(0);
        b.host_draws.erase(0);
        return;
    }
    compare_draws(0);
}

// Client, tour 1 : mécaniques propres des héros (roue de Monoco, taches de Lune…) comparées à l'hôte avant toute
// réparation. Chaque champ différent compte (start_mech_diffs) ; position de départ de la roue journalisée des deux côtés.
void check_start_mech(const proto::StateSync& mine, const proto::StateSync& host)
{
    for (const auto& h : host.units) {
        const proto::UnitState* m = nullptr;
        for (const auto& x : mine.units) if (x.unit.kind == h.unit.kind && x.unit.spot == h.unit.spot) { m = &x; break; }
        if (!m) continue;
        for (const auto& hf : h.mech) {
            const proto::MechField* mf = nullptr;
            for (const auto& x : m->mech) if (x.name == hf.name) { mf = &x; break; }
            const bool differs = !mf || mf->value != hf.value;
            if (differs) bump("start_mech_diffs");
            if (differs || hf.name.find("Starting_Position") != std::string::npos || hf.name.find("CurrentMeter") != std::string::npos)
                log().write("coop_start_mech", JsonObj().add("unit", h.cls).add("field", hf.name).add("host", hf.value)
                                                   .add("local", mf ? mf->value : -1).add("same", !differs), 1);
        }
    }
}

// Mécanique propre (champs des composants *Mechanic* / StainHolder) différente pour au moins un héros.
bool mech_differs(const proto::StateSync& mine, const proto::StateSync& host)
{
    for (const auto& h : host.units) {
        for (const auto& m : mine.units) {
            if (m.unit.kind != h.unit.kind || m.unit.spot != h.unit.spot) continue;
            if (m.mech.size() != h.mech.size()) return true;
            for (size_t i = 0; i < h.mech.size(); ++i)
                if (m.mech[i].name != h.mech[i].name || m.mech[i].value != h.mech[i].value) return true;
        }
    }
    return false;
}

// Tirage hors des tours d'un combat. Dans la fenêtre de mise en place, avec la graine : valeur dérivée (D30). Sinon,
// tirage de gameplay laissé au jeu, compté (setup_draws_natural) : il peut différer d'une machine à l'autre.
// Hors combat et hors fenêtre (exploration) : jamais touché ni compté.
void offturn_draw(HookCtx& ctx, UFunction* f, RandomKind kind, const std::string& fname)
{
    const bool window = setup_window();
    if (!window && !s.battle.active) return;
    const std::string stream = gameplay_stream(ctx, fname, true);
    if (stream.empty()) return;
    if (window && s.setup.seeded && s.setup_draws && draw_allowed(stream)) { replace_draw(ctx, f, kind, stream, true); return; }
    bump("setup_draws_natural");
    if (++s.setup.natural_logs <= 40)
        log().write("coop_draw_offturn", JsonObj().add("stream", stream).add("value", natural_value(ctx, kind)).add("window", window)
                                             .add("seeded", s.setup.seeded).add("enabled", s.setup_draws), s.battle.turn);
}

// --- Synchro des tours ---------------------------------------------------------------------

std::vector<std::wstring> party_wide(const std::vector<std::string>& v)
{
    std::vector<std::wstring> out;
    for (auto& x : v) out.push_back(ue::from_utf8(x));
    return out;
}

// Propriétaire d'un héros : -e33owners (emplacement ou nom interne), sinon règle du brief :
// le k-ième héros (ordre des emplacements) va au k-ième joueur connecté, l'hôte prend le reste.
uint8_t default_owner(int index, int spot, const std::string& name)
{
    if (auto it = s.owner_overrides.find(std::to_string(spot)); it != s.owner_overrides.end()) return it->second;
    if (auto it = s.owner_overrides.find(name); it != s.owner_overrides.end()) return it->second;
    if (!s.owner_overrides.empty()) return 0;
    (void)index;
    return character_owner(name);   // même répartition qu'hors combat (builds) : le k-ième de l'équipe au k-ième joueur
}

void begin_turn_common()
{
    ++s.battle.turn;
    if (s.battle.turn == 1) setup_close();   // mise en place finie : tirages des tours à partir d'ici (D30)
    s.battle.draw_rank.clear();
    s.battle.hit_seq.clear();
    s.battle.combo_seq.clear();
    s.battle.action_wait_logged = false;
    refresh_enemy_spots();
    s.battle.turn_started_at = s.now;
    s.battle.timer_elapsed = 0;
    s.fa_own = FreeAimOwner{};
    s.fa_rep = FreeAimReplay{};
}

void align_begin(uint32_t turn, const char* how);

void host_turn_start(UObject* actor)
{
    auto& b = s.battle;
    if (!b.active) {
        b = Battle{};
        b.id = s.next_battle_id++;
        b.active = true;
        proto::BattleStart bs;
        bs.battle_id = b.id;
        bs.encounter = ue::to_utf8(game::last_encounter());
        for (auto& p : game::last_party()) bs.party.push_back(ue::to_utf8(p));
        // Combat lancé depuis l'exploration (pas par le test runner) : rencontre et ennemi du monde réels (D12).
        {
            std::string enc, gid;
            int engage = 0;
            if (game::battle_params(enc, gid, engage) && gid != "E33CoopTest") {
                bs.encounter = enc;
                bs.enemy_global_id = gid;
                bs.engage = static_cast<uint8_t>(engage);
                bs.world_enemy = explore::take_world_enemy();
                bs.party = game::party_names();   // équipe réelle : celle du client peut être restée l'ancienne
                bump("world_battles");
            }
        }
        // Graine choisie au lancement du combat (mise en place, D30) ; repli : tirée ici comme avant.
        if (setup_window() && s.setup.seeded) {
            b.rng_seed = s.setup.seed;
        } else {
            b.rng_seed = s.fixed_seed ? *s.fixed_seed : splitmix64((static_cast<uint64_t>(GetTickCount64()) << 20) ^ s.session_id ^ b.id);
            bump("setup_windows_missing");
            log().write("coop_setup_missing", JsonObj().add("open", s.setup.open).add("age_s", s.setup.open ? s.now - s.setup.opened_at : -1.0));
        }
        bs.rng_seed = b.rng_seed;
        b.turn_timer_s = s.turn_timer_s;
        bs.turn_timer_s = s.turn_timer_s;
        b.duo_parry = s.duo_parry;
        bs.duo_parry = s.duo_parry ? 1 : 0;
        auto ps = game::players();
        std::sort(ps.begin(), ps.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
        for (size_t i = 0; i < ps.size(); ++i) {
            const int spot = game::spot(ps[i]);
            const uint8_t owner = default_owner(static_cast<int>(i), spot, game::hardcoded_name(ps[i]));
            b.owner_by_spot[spot] = owner;
            bs.owners.push_back({static_cast<uint8_t>(spot), game::char_class(ps[i]), owner});
        }
        // Démarrage coordonné : personne ne commence un combat avant les autres. Les clients de la partie lancent le même
        // combat dès qu'ils peuvent (BattleStart) ; ce premier tour attend, jeu en pause, qu'ils y soient tous (BattleReady).
        for (auto& p : s.peers) if (in_game(p)) b.hold = true;
        b.hold_since = s.now;
        broadcast(bs);
        // Tirages de la mise en place (tour 0), pour vérification chez les clients (comparés à leur tour 1).
        // Par paquets de 32 (le protocole refuse les listes de plus de 64 ; une mise en place en fait ~100).
        if (setup_window() && !s.setup.draws.empty()) {
            proto::Draws d{b.id, 0, {}};
            for (auto& [k, v] : s.setup.draws) {
                d.draws.push_back({static_cast<uint32_t>(k >> 16), static_cast<uint16_t>(k & 0xFFFF), v});
                if (d.draws.size() == 32) { broadcast(d); d.draws.clear(); }
            }
            if (!d.draws.empty()) broadcast(d);
        }
        log().write("coop_battle_inventory", JsonObj().raw("inventory", game::battle_inventory_json()));
        // Diagnostic : composants de chaque héros (mécaniques propres : perfection, taches, masques… à couvrir par le hash).
        for (UObject* h : game::players()) {
            std::string comps = "[";
            for (UObject* c : ue::get_object_array(h, STR("BlueprintCreatedComponents"))) {
                if (comps.size() > 1) comps += ",";
                comps += "\"" + game::char_class(c) + "\"";
            }
            log().write("coop_hero_components", JsonObj().add("hero", game::hardcoded_name(h)).raw("components", comps + "]"));
        }
    }
    begin_turn_common();
    b.ots_seq = b.sct_seq;   // alignement des tours ennemis : OnTurnStart de ce tour passé
    b.play_since = s.now;
    b.cur_enemy = !game::is_player(actor);
    const proto::Unit au = actor_unit(actor);
    turn_indicator(au);
    broadcast(proto::TurnStart{b.id, b.turn, au.kind, au.spot, game::char_class(actor)});
    const proto::StateSync st = sync::snapshot(b.id, b.turn);
    const uint64_t h = sync::hash(st, au);
    b.local_hash[b.turn] = h;
    broadcast(proto::StateHash{b.id, b.turn, h});
    broadcast(st);
    check_hash(b.turn);
}

// Client : test de resynchro (-e33perturb=N) : fausse volontairement l'état local au début du tour N,
// après le contrôle de ce tour ; l'écart doit être détecté et corrigé au tour suivant.
void perturb_state()
{
    for (UObject* e : game::enemies()) {
        if (game::is_dead_fast(e)) continue;
        UObject* st = game::stats(e);
        ue::set_double(st, STR("CurrentHP"), ue::get_double(st, STR("CurrentHP")) - 1.0);
        break;
    }
    for (UObject* h : game::players()) {
        if (game::is_dead_fast(h)) continue;
        UObject* st = game::stats(h);
        ue::set_double(st, STR("CurrentAP"), ue::get_double(st, STR("CurrentAP")) + 1.0);
        break;
    }
    bump("perturbations");
    log().write("coop_perturb", JsonObj().add("what", "enemy hp -1, hero ap +1"), s.battle.turn);
}

// Client : test de la fin imposée (-e33perturbend=N) : au début du tour N, après son contrôle, les ennemis vivants
// d'ici reçoivent 100 fois leurs PV. Le coup fatal de l'hôte ne les tue pas ici : l'hôte gagne, le combat continue ici
// (comme le Lancelier de l'essai à deux PC du 2026-10-22) et doit être clos par client_force_end_tick.
void perturb_end()
{
    int n = 0;
    for (UObject* e : game::enemies()) {
        if (game::is_dead_fast(e)) continue;
        UObject* st = game::stats(e);
        ue::set_double(st, STR("CurrentHP"), ue::get_double(st, STR("CurrentHP")) * 100.0);
        ++n;
    }
    bump("end_perturbations");
    log().write("coop_perturb_end", JsonObj().add("enemies", n), s.battle.turn);
}

void client_turn_finish(UObject* actor);

// --- Essai -e33turndep=1 : attente du début de tour de l'hôte sans figer le jeu ------------------------------------
// Le jeu attend, au début de chaque tour, que les « dépendances » de BM.TurnStartNotifyDependencies soient résolues,
// en continuant d'animer. Le mod s'y inscrit au moment où le jeu commence à écouter (StartListenToDependencies) :
// plus tôt, le compteur est remis à zéro ; plus tard, l'écoute a déjà conclu et la résolution relance la suite du tour
// une deuxième fois (l'ennemi décidait deux fois de son action, vu le 2026-10-22).
UObject* turn_deps() { return ue::get_object(game::battle_manager(), STR("TurnStartNotifyDependencies")); }

void dep_solve(const char* why)
{
    auto& b = s.battle;
    if (!b.dep_active) return;
    b.dep_active = false;
    ue::Call solve(turn_deps(), STR("SolveDependency"));
    if (solve) solve.set_object(STR("Dependency"), game::save_manager()).invoke();
    log().write("coop_turn_dep_solved", JsonObj().add("turn", b.dep_turn).add("waited_s", s.now - b.dep_since).add("why", why), b.dep_turn);
}

void client_turn_start(UObject* actor)
{
    auto& b = s.battle;
    // Tour qui commence : turn + 1. Une dépendance posée pour un autre tour (le tour a avancé sans elle : action lancée
    // directement par le testeur) est retirée, sinon elle bloquerait les tours suivants.
    if (b.dep_active && b.dep_turn != b.turn + 1) { bump("turn_deps_superseded"); dep_solve("superseded"); }
    if (b.dep_finish_pending) { b.dep_finish_pending = false; b.dep_start = {}; }
    b.ots_seq = b.sct_seq;
    begin_turn_common();
    if (!b.ready_sent) {   // démarrage coordonné : l'hôte attend que chacun ait commencé ce combat
        b.ready_sent = true;
        for (auto& p : s.peers) send_to(p, proto::BattleReady{b.id, s.my_player_id});
        // Délai depuis le BattleStart de l'hôte (essai à deux PC du 2026-10-22 : jamais envoyé, l'hôte restait figé).
        const int delay_ms = b.start_rx_at >= 0 ? static_cast<int>((s.now - b.start_rx_at) * 1000.0) : -1;
        if (delay_ms >= 0) bump_max("battle_ready_delay_ms", delay_ms);
        log().write("coop_battle_ready_sent", JsonObj().add("battle", b.id).add("delay_ms", delay_ms), b.turn);
    }
    // Combat déjà fini chez l'hôte (BattleEnd reçu) et pourtant un tour recommence ici : la simulation locale a divergé
    // (ennemi encore vivant ici). Aucun état de l'hôte ne viendra pour ce tour : pas d'attente, fin imposée (tick).
    // Avant : attente bloquante de 15 s du tour de l'hôte, puis session perdue (essai à deux PC du 2026-10-22).
    if (b.host_end >= 0) {
        bump("turns_after_host_end");
        b.force_now = true;
        log().write("coop_turn_after_host_end", JsonObj().add("battle", b.id).add("host_end", b.host_end).add("actor", game::char_class(actor)), b.turn);
        return;
    }
    // État de l'hôte pas encore là : vérification différée (le tick l'appelle à l'arrivée), pendant que la dépendance
    // retient le tour. Si le jeu écoutait déjà sans nous pour ce tour, trop tard : attente bloquante comme avant.
    if (s.turn_dep && !b.strict && b.turn > 1 && !b.host_sync.count(b.turn) && b.host_end < 0) {
        const bool registered = b.dep_active && b.dep_turn == b.turn;
        const bool listened_without_us = !registered && b.listen_seq == b.sct_seq;
        if (!listened_without_us) {
            b.dep_turn = b.turn;
            b.dep_actor = actor;
            b.dep_start = sync::snapshot(b.id, b.turn);
            b.dep_finish_pending = true;
            if (!registered) b.dep_since = s.now;
            bump("turn_checks_deferred");
            log().write("coop_turn_deferred", JsonObj().add("turn", b.turn).add("registered", registered), b.turn);
            return;
        }
        bump("turn_deps_too_late");
    }
    client_turn_finish(actor);
}

// Tour vérifié en différé : ce qui a changé ici depuis le début du tour (effets de début de tour appliqués pendant
// l'attente) est reporté sur l'état de l'hôte, pour ne corriger que l'écart réel et ne pas annuler ces effets.
proto::StateSync shift_host_state(const proto::StateSync& host, const proto::StateSync& start, const proto::StateSync& now)
{
    proto::StateSync out = host;
    auto find = [](const proto::StateSync& st, const proto::Unit& u) -> const proto::UnitState* {
        for (auto& x : st.units) if (x.unit.kind == u.kind && x.unit.spot == u.spot) return &x;
        return nullptr;
    };
    for (auto& h : out.units) {
        const proto::UnitState* a = find(start, h.unit);
        const proto::UnitState* c = find(now, h.unit);
        if (!a || !c) continue;
        h.hp += c->hp - a->hp;
        h.ap += c->ap - a->ap;
        h.shield += c->shield - a->shield;
        h.break_bar += c->break_bar - a->break_bar;
        if (c->dead != a->dead) h.dead = c->dead;
        // Bonus : ce qui a changé ici pendant l'attente (tours décomptés, cumuls, ajouts, retraits) est reporté sur ceux de
        // l'hôte. (Avant : bonus laissés tels quels dès qu'ils avaient bougé -> écart jamais corrigé, Verso mort ici
        // seulement au tour 21 de p3_party_client, 2026-10-21.) Bonus de même classe appariés dans l'ordre.
        {
            auto nth = [](const std::vector<proto::BuffState>& v, const std::string& cls, int k) -> const proto::BuffState* {
                for (auto& x : v) if (x.cls == cls && k-- == 0) return &x;
                return nullptr;
            };
            std::vector<proto::BuffState> merged;
            std::map<std::string, int> seen_h;
            for (const auto& hb : h.buffs) {
                const int k = seen_h[hb.cls]++;
                const proto::BuffState* sa = nth(a->buffs, hb.cls, k);
                const proto::BuffState* sc = nth(c->buffs, hb.cls, k);
                if (sa && !sc) continue;   // retiré ici pendant l'attente
                proto::BuffState m = hb;
                if (sa && sc) { m.stacks += sc->stacks - sa->stacks; m.turns += sc->turns - sa->turns; }
                merged.push_back(m);
            }
            std::map<std::string, int> count_a, count_c, count_h;
            for (auto& x : a->buffs) ++count_a[x.cls];
            for (auto& x : h.buffs) ++count_h[x.cls];
            for (auto& x : c->buffs) {   // ajoutés ici pendant l'attente
                const int k = count_c[x.cls]++;
                if (k >= count_a[x.cls] && k >= count_h[x.cls]) merged.push_back(x);
            }
            h.buffs = std::move(merged);
        }
        // Mécanique propre : écarts d'ici reportés champ par champ.
        for (auto& f : h.mech) {
            const proto::MechField* fa = nullptr;
            const proto::MechField* fc = nullptr;
            for (auto& x : a->mech) if (x.name == f.name) { fa = &x; break; }
            for (auto& x : c->mech) if (x.name == f.name) { fc = &x; break; }
            if (fa && fc) f.value += fc->value - fa->value;
        }
    }
    out.gradient_charges += now.gradient_charges - start.gradient_charges;
    out.gradient_progress += now.gradient_progress - start.gradient_progress;
    return out;
}

void client_turn_finish(UObject* actor)
{
    auto& b = s.battle;
    const bool deferred = !b.dep_start.units.empty() && b.dep_start.turn == b.turn;
    // L'hôte rythme les tours : attendre son début de tour (TurnStart, StateHash, StateSync, dans cet ordre).
    // Fin de combat de l'hôte arrivée pendant l'attente : aucun état ne viendra pour ce tour.
    if (!wait_for("turn_state", [&] { return b.host_sync.count(b.turn) > 0 || b.host_end >= 0; }, 15.0)) {
        if (s.ended) return;   // l'hôte est parti pendant l'attente : ce n'est pas un tour manqué
        // Jamais une fin de session : le lien est surveillé à part (silence de 15 s, D14). L'hôte reste maître du rythme ;
        // l'écart éventuel est corrigé par son état au tour suivant, ou par la fin imposée si son BattleEnd arrive.
        bump("turn_wait_timeouts");
        ++b.turn_wait_timeouts;
        const double heard = s.peers.empty() || !s.peers[0].link ? -1 : s.peers[0].link->last_heard();
        log().write("coop_turn_wait_timeout", JsonObj().add("battle", b.id).add("count", b.turn_wait_timeouts)
                                                  .add("host_silent_s", heard >= 0 ? s.now - heard : -1.0), b.turn);
        return;
    }
    if (!b.host_sync.count(b.turn)) {
        bump("turns_after_host_end");
        b.force_now = true;
        log().write("coop_turn_after_host_end", JsonObj().add("battle", b.id).add("host_end", b.host_end).add("actor", game::char_class(actor)), b.turn);
        return;
    }
    b.turn_wait_timeouts = 0;
    const proto::Unit au = actor_unit(actor);
    turn_indicator(au);
    if (auto it = b.host_turns.find(b.turn); it != b.host_turns.end() && (it->second.kind != au.kind || it->second.index != au.spot)) {
        bump("actor_mismatches");
        s.battle.strict = true;   // essai -e33turndep : on revient à l'attente bloquante pour la suite du combat
        log().write("coop_actor_mismatch", JsonObj().add("local", game::char_class(actor)).add("host", it->second.actor), b.turn);
    }
    // Les tirages de l'hôte pour le tour précédent sont arrivés avant son TurnStart (lien ordonné).
    if (b.turn > 1) compare_draws(b.turn - 1);
    else if (b.turn == 1) compare_setup_draws();
    const proto::StateSync& host = b.host_sync[b.turn];
    // Comparaison au même instant que l'hôte (début du tour) : photo prise alors si la vérification est différée.
    const proto::StateSync mine = deferred ? b.dep_start : sync::snapshot(b.id, b.turn);
    if (b.turn == 1) check_start_mech(mine, host);
    uint64_t h = sync::hash(mine, au);
    const uint64_t host_hash = sync::hash(host, au);
    if (h != host_hash) {
        // La simulation locale a divergé : log détaillé, puis resynchronisation depuis l'état de l'hôte.
        bump("raw_hash_diffs");
        if (mech_differs(mine, host)) bump("raw_mech_diffs");   // écart sur une mécanique propre (roue, taches…, D30)
        const proto::StateSync now = deferred ? sync::snapshot(b.id, b.turn) : mine;
        const proto::StateSync target = deferred ? shift_host_state(host, mine, now) : host;
        const sync::RepairStats r = sync::repair(now, target);
        bump("repairs");
        bump("repair_failures", r.failed);
        const proto::StateSync after = sync::snapshot(b.id, b.turn);
        // Différé : réussi si l'état actuel égale l'état de l'hôte décalé des effets d'ici ; on annonce alors le hash de
        // l'hôte (même début de tour des deux côtés).
        const bool fixed = sync::hash(after, au) == sync::hash(target, au);
        h = deferred ? (fixed ? host_hash : sync::hash(after, au)) : sync::hash(after, au);
        log().write("coop_resync", JsonObj().raw("diff", sync::diff(mine, host)).raw("repaired", sync::repair_json(r)).add("deferred", deferred)
                                       .add("fixed", fixed).raw("remaining", fixed ? "[]" : sync::diff(after, target)), b.turn);
    }
    b.dep_start = {};
    b.local_hash[b.turn] = h;
    for (auto& p : s.peers) send_to(p, proto::StateHash{b.id, b.turn, h});
    check_hash(b.turn);
    b.host_sync.erase(b.turn);
    if (s.perturb_turn && *s.perturb_turn == b.turn) perturb_state();
    if (s.perturb_end_turn && *s.perturb_end_turn == b.turn) perturb_end();
}

// Contextes Enhanced Input coupés pendant le tour du héros d'un autre joueur. SetBattleInputEnabled(false) ne
// suffit pas : la roue reste active (RT / gradient ouvrait le menu de Verso, vu le 2026-10-20). Le jeu les remet
// lui-même au début de chaque tour d'un héros d'ici (tour, roue, ciblage) ou de chaque compétence / visée (QTE,
// visée libre). Pas IMC_Battle_PauseMenu (permanent) ni IMC_BattleDefense / InputBlocker (liés une fois, puis gardés).
constexpr const wchar_t* kBlockedContexts[] = {
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_PlayerTurn.IMC_Battle_PlayerTurn",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_MainWheel.IMC_Battle_MainWheel",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_Targeting.IMC_Battle_Targeting",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_QTE.IMC_Battle_QTE",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_FreeAim.IMC_Battle_FreeAim",
};
// Ceux que le jeu remet au début du tour d'un héros d'ici (rendus par le mod s'ils manquent encore).
constexpr size_t kTurnContexts = 3;

UObject* blocked_context(size_t i)
{
    static UObject* cache[std::size(kBlockedContexts)] = {};
    if (!cache[i]) cache[i] = ue::find_object(kBlockedContexts[i]);
    return cache[i];
}

std::optional<int> context_priority(UObject* imc)
{
    ue::Call has(s.input_sub, STR("HasMappingContext"));
    if (!has) return std::nullopt;
    has.set_object(STR("MappingContext"), imc).invoke();
    if (!has.out_bool(STR("ReturnValue"))) return std::nullopt;
    return static_cast<int>(has.out_int(STR("OutFoundPriority")));
}

// À chaque image pendant le blocage : retire ceux que le jeu a (re)mis.
void enforce_input_block()
{
    if (!s.input_sub) return;
    for (size_t i = 0; i < std::size(kBlockedContexts); ++i) {
        UObject* imc = blocked_context(i);
        if (!imc) continue;
        const auto prio = context_priority(imc);
        if (!prio) continue;
        ue::Call rm(s.input_sub, STR("RemoveMappingContext"));
        if (!rm) return;
        rm.set_object(STR("MappingContext"), imc).invoke();
        if (!s.removed_contexts.count(imc)) s.removed_contexts[imc] = *prio;
        bump("input_contexts_removed");
        log().write("coop_input_context", JsonObj().add("removed", ue::to_utf8(imc->GetName())).add("priority", *prio), s.battle.turn);
    }
}

// Fin du blocage au début du tour d'un héros d'ici : le jeu a pu remettre tour/roue/ciblage juste avant (retirés
// alors par le blocage) ; on rend ceux qui manquent, avec leur priorité.
void restore_turn_contexts()
{
    if (!s.input_sub) return;
    for (size_t i = 0; i < kTurnContexts; ++i) {
        UObject* imc = blocked_context(i);
        auto it = imc ? s.removed_contexts.find(imc) : s.removed_contexts.end();
        if (it == s.removed_contexts.end() || context_priority(imc)) continue;
        ue::Call add(s.input_sub, STR("AddMappingContext"));
        if (!add) return;
        add.set_object(STR("MappingContext"), imc).set_int(STR("Priority"), it->second).invoke();
        bump("input_contexts_restored");
        log().write("coop_input_context", JsonObj().add("restored", ue::to_utf8(imc->GetName())).add("priority", it->second), s.battle.turn);
    }
}

// Entrées de combat de cette instance : aucune pendant le tour du héros d'un autre joueur (roue, gradient, ciblage,
// QTE, visée). local_turn : la fin du blocage ouvre le tour d'un héros d'ici.
void set_local_input(bool enabled, const char* why, bool local_turn)
{
    if (enabled == s.input_enabled) return;
    s.input_enabled = enabled;
    ue::Call c(game::battle_manager(), STR("SetBattleInputEnabled"));
    if (c) c.set_bool(STR("Enabled"), enabled).invoke();
    if (!enabled) {
        s.input_sub = game::input_subsystem();
        s.removed_contexts.clear();
        enforce_input_block();
    } else {
        if (local_turn) restore_turn_contexts();
        s.removed_contexts.clear();
    }
    log().write("coop_input", JsonObj().add("enabled", enabled).add("why", why).add("subsystem", s.input_sub != nullptr), s.battle.turn);
}

// --- Indicateurs, départs, fin de session (phase 5) --------------------------------------------

// « Tour de X » / « À vous de jouer » au début du tour d'un héros ; effacé pendant les tours ennemis.
void turn_indicator(const proto::Unit& u)
{
    if (u.kind != proto::ActorKind::Hero) { ui::clear_status("turn"); return; }
    const uint8_t owner = owner_of_spot(u.spot);
    if (owner == s.my_player_id) ui::set_status("turn", "your_turn");
    else ui::set_status("turn", "turn_of", {player_name(owner)});
}

// Hôte : un joueur est parti (Bye, silence, redémarrage). L'hôte reprend ses héros, combat en cours compris ; le joueur
// pourra revenir par le même chemin que « rejoindre » (instantané au prochain point sûr).
void on_player_left(uint8_t player, const std::string& name, const std::string& why)
{
    bump("players_left");
    explore::on_peer_left(player);
    auto& b = s.battle;
    proto::Owners ow;
    ow.battle_id = b.id;
    int taken = 0;
    for (auto& [spot, owner] : b.owner_by_spot) {
        if (owner != player) continue;
        owner = 0;
        ++taken;
    }
    if (b.active && taken > 0) {
        for (auto& [spot, owner] : b.owner_by_spot) {
            UObject* h = game::find_unit(true, spot);
            ow.owners.push_back({static_cast<uint8_t>(spot), h ? game::char_class(h) : std::string(), owner});
        }
        broadcast(ow);
        s.takeover = true;
        bump("heroes_taken_over", taken);
    }
    b.ready.erase(player);
    b.unresponsive.erase(player);
    send_roster();
    ui::toast(why == "bye" ? "player_left" : "player_lost", {name});
    log().write("session_leave", JsonObj().add("player_id", static_cast<int>(player)).add("name", name).add("why", why).add("heroes_taken", taken)
                                       .add("battle_active", b.active));
}

// Client : la session est finie (hôte parti ou injoignable, refus). Message clair, entrées rendues, puis retour au menu
// principal : la partie chargée est celle de l'hôte (emplacement coop), elle n'a plus de sens sans lui.
void end_session(const std::string& key, const std::string& detail)
{
    if (s.ended) return;
    s.ended = true;
    s.end_key = key;
    if (s.steam) net::steam::leave_lobby(s.lobby);
    s.ended_at = s.now;
    bump("session_ended");
    set_local_input(true, "session end");
    explore::on_session_end();
    ui::clear_status("turn");
    ui::clear_status("wait");
    ui::clear_status("peer");
    if (key == "rejected") ui::set_status("session", "rejected", {detail});
    else ui::set_status("session", key);
    log().write("session_end", JsonObj().add("key", key).add("detail", detail).add("text", ui::statuses().count("session") ? ui::statuses().at("session") : ""));
}

// --- Fin de combat -------------------------------------------------------------------------

// Récompenses : lecture / écriture de FS_BattleRewards (out-param de RollBattleRewards).
proto::BattleRewards read_rewards(void* st_ptr, RC::Unreal::UStruct* st)
{
    using namespace RC::Unreal;
    proto::BattleRewards r;
    r.battle_id = s.battle.id;
    if (FProperty* p = ue::struct_member(st, STR("ExperienceGained"))) r.xp = *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(st_ptr));
    if (FProperty* p = ue::struct_member(st, STR("GoldAcquired"))) r.gold = *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(st_ptr));
    if (auto* ap = CastField<FArrayProperty>(ue::struct_member(st, STR("RolledLootEntries")))) {
        auto* inner = CastField<FStructProperty>(ap->GetInner());
        auto* arr = static_cast<FScriptArray*>(ap->ContainerPtrToValuePtr<void>(st_ptr));
        if (inner && arr) {
            UStruct* es = inner->GetStruct();
            FProperty* id = ue::struct_member(es, STR("ItemID"));
            FProperty* qty = ue::struct_member(es, STR("Quantity"));
            auto* data = static_cast<uint8_t*>(arr->GetData());
            for (int32 i = 0; i < arr->Num() && id && qty; ++i) {
                void* e = data + static_cast<size_t>(i) * inner->GetSize();
                r.loot.push_back({ue::to_utf8(static_cast<FName*>(id->ContainerPtrToValuePtr<void>(e))->ToString()), *static_cast<int32_t*>(qty->ContainerPtrToValuePtr<void>(e))});
            }
        }
    }
    return r;
}

// Client : butin de l'hôte écrit dans les récompenses d'ici (même nombre d'entrées : objet et quantité de chacune).
// Un nombre d'entrées différent n'est pas réparable ici (tableau du jeu) : compté (rewards_loot_unrepaired).
bool write_loot(void* st_ptr, RC::Unreal::UStruct* st, const std::vector<proto::LootEntry>& loot)
{
    using namespace RC::Unreal;
    auto* ap = CastField<FArrayProperty>(ue::struct_member(st, STR("RolledLootEntries")));
    if (!ap) return false;
    auto* inner = CastField<FStructProperty>(ap->GetInner());
    auto* arr = static_cast<FScriptArray*>(ap->ContainerPtrToValuePtr<void>(st_ptr));
    if (!inner || !arr || arr->Num() != static_cast<int32>(loot.size())) return false;
    UStruct* es = inner->GetStruct();
    FProperty* id = ue::struct_member(es, STR("ItemID"));
    FProperty* qty = ue::struct_member(es, STR("Quantity"));
    if (!id || !qty) return false;
    auto* data = static_cast<uint8_t*>(arr->GetData());
    for (size_t i = 0; i < loot.size(); ++i) {
        void* e = data + i * inner->GetSize();
        *static_cast<FName*>(id->ContainerPtrToValuePtr<void>(e)) = FName(ue::from_utf8(loot[i].item).c_str(), FNAME_Add);
        *static_cast<int32_t*>(qty->ContainerPtrToValuePtr<void>(e)) = loot[i].quantity;
    }
    return true;
}

void finalize_battle(const char* why)
{
    auto& b = s.battle;
    if (b.finalized) return;
    if (s.role == Role::Client) {
        // Jamais d'attente ici : on peut être au milieu d'un coup (mort -> fin de combat) dont l'hôte attend le résultat.
        // La clôture d'une fin sans victoire est donc faite dans le tick, une fois le BattleEnd de l'hôte reçu.
        if (b.host_end != b.local_end) {
            bump("end_mismatches");
            log().write("coop_end_mismatch", JsonObj().add("local", b.local_end).add("host", b.host_end));
        }
        if (!b.left) compare_draws(b.turn);
    }
    b.finalized = true;
    bump("battles_finalized");
    b.active = false;
    s.setup.open = false;   // combat fini avant son tour 1 : jamais de fenêtre de mise en place en exploration (D30)
    set_local_input(true, "battle end");
    ui::clear_status("turn");
    if (b.hold) { b.hold = false; explore::hold_pause("battle_start", false); }
    log().write("coop_battle_finalized", JsonObj().add("battle", b.id).add("why", why).add("desyncs", counter("desyncs")));
}

// --- Hooks ---------------------------------------------------------------------------------

UObject* timeline_hero(UObject* timeline)
{
    UObject* ch = ue::get_object(timeline, STR("BattleCharacter"));
    return ch && game::is_player(ch) ? ch : nullptr;
}

// Impose sur ce héros la défense décidée par son propriétaire, juste avant le test du jeu.
void impose_defense(UObject* hero, proto::Defense d, bool jumpable)
{
    s.imposing = true;
    bool started = true;
    switch (d) {
    case proto::Defense::Fail:
        if (game::in_immunity(hero)) game::cancel_defense(hero);
        break;
    case proto::Defense::Parry:
        game::clear_defense_lock(hero);
        started = game::start_parry(hero);
        break;
    case proto::Defense::GradientParry:
        game::clear_defense_lock(hero);
        started = game::start_gradient_parry(hero);
        break;
    case proto::Defense::Dodge:
    case proto::Defense::PerfectDodge: {
        game::clear_defense_lock(hero);
        started = game::start_dodge(hero);
        if (d == proto::Defense::Dodge) {
            // Esquive simple : début reculé au-delà de la fenêtre de parade (sinon le jeu la juge parfaite).
            const double start = ue::get_double(hero, STR("DefensiveMoveImmunityStartTime"));
            ue::set_double(hero, STR("DefensiveMoveImmunityStartTime"), start - game::parry_immune_duration(hero) - 0.05);
        }
        break;
    }
    case proto::Defense::Jump:
        if (!jumpable) break;
        game::clear_defense_lock(hero);
        started = game::start_jump(hero);
        break;
    }
    s.imposing = false;
    if (!started) bump("defense_start_failed");
}

proto::Defense local_result(const LocalHit& lh, bool parried, bool dodged, bool gradient)
{
    if (lh.jumpable) return dodged ? proto::Defense::Jump : proto::Defense::Fail;
    if (parried) return gradient ? proto::Defense::GradientParry : proto::Defense::Parry;
    if (dodged) return lh.perfect_dodge ? proto::Defense::PerfectDodge : proto::Defense::Dodge;
    return proto::Defense::Fail;
}

// Point de défense d'un héros (avant le test du jeu) : héros d'ici -> le résultat sera publié après le test ;
// héros d'un autre joueur -> attendre son résultat (relayé par l'hôte) et l'imposer.
void defense_point(UObject* stats_comp, UObject* hero, bool jumpable)
{
    auto& b = s.battle;
    const int spot = game::spot(hero);
    const uint16_t hit = ++b.hit_seq[spot];
    if (controls(spot)) {
        b.local_hits[stats_comp] = {spot, hit, jumpable, false};
        // Parade appuyée pendant une attente figée juste avant (attaque de zone) : lancée maintenant, avant le test du jeu.
        if (b.wait_parry_at && !jumpable && GetTickCount64() - b.wait_parry_at <= 300) {
            game::clear_defense_lock(hero);
            const bool started = game::start_parry(hero);
            bump(started ? "parries_from_wait" : "parries_from_wait_failed");
            log().write("coop_parry_from_wait", JsonObj().add("spot", spot).add("hit", static_cast<int>(hit)).add("started", started)
                                                   .add("ago_ms", static_cast<int64_t>(GetTickCount64() - b.wait_parry_at)), b.turn);
        }
        return;
    }
    const uint8_t owner = owner_of_spot(spot);
    if (s.role == Role::Host) {
        // turn_ms : temps de jeu du tour ici au moment du coup (le faux client --turn-lag s'en sert pour frapper au même
        // moment de son propre tour ; le vrai client l'ignore).
        const double since = std::max(0.0, s.now - b.play_since) * 1000.0;
        const uint16_t turn_ms = static_cast<uint16_t>(std::min(since, 65000.0));
        for (auto& p : s.peers)
            if (p.accepted && p.player_id == owner)
                send_to(p, proto::DefenseRequest{b.id, b.turn, static_cast<uint8_t>(spot), hit, static_cast<uint8_t>(jumpable), 3000, turn_ms});
    }
    const TurnSpotSeq key{b.turn, spot, hit};
    const ULONGLONG t0 = GetTickCount64();
    // Attente bloquante (image figée) du résultat du propriétaire, bornée à kDefenseTimeout. Sortie sans attendre la fin
    // du délai : propriétaire parti (l'hôte reprend le héros), combat déjà fini chez l'hôte (client : aucun résultat ne
    // viendra), ou propriétaire qui ne répond plus dans ce combat (hôte : tour bloqué chez lui, attaque imposée).
    const bool unresponsive = s.role == Role::Host && b.unresponsive.count(owner) > 0;
    const bool got = !unresponsive &&
                     wait_for("defense", [&] { return b.defenses.count(key) > 0 || owner_of_spot(spot) != owner || (s.role == Role::Client && b.host_end >= 0); },
                              kDefenseTimeout) &&
                     b.defenses.count(key) > 0;
    const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
    bump_max("defense_wait_max_ms", static_cast<int>(waited_ms));
    if (waited_ms >= 250) bump("defense_waits_long");   // même seuil que coop_wait_long (site defense)
    proto::Defense res = proto::Defense::Fail;
    const char* why = "received";
    if (got) res = b.defenses[key];
    else if (unresponsive) { why = "owner_unresponsive"; bump("defense_waits_skipped"); }
    else if (owner_of_spot(spot) != owner) { why = "owner_left"; bump("defense_waits_owner_left"); }
    else if (s.role == Role::Client && b.host_end >= 0) { why = "host_battle_over"; bump("defense_waits_battle_over"); }
    else {
        why = "timeout";
        bump("defense_timeouts");
        // Hôte : propriétaire qui n'a même pas commencé ce tour chez lui (aucun StateHash) : il est bloqué ou très en
        // retard. Ses prochains coups ne figeront plus l'image 3 s chacun (échec imposé, l'état de l'hôte fait foi) ; il
        // redevient attendu dès son prochain StateHash.
        if (s.role == Role::Host && !(b.acks.count(b.turn) && b.acks[b.turn].count(owner)) && b.unresponsive.insert(owner).second) {
            bump("players_unresponsive");
            log().write("coop_player_unresponsive", JsonObj().add("player", static_cast<int>(owner)).add("why", "defense timeout, turn not started there"), b.turn);
        }
    }
    impose_defense(hero, res, jumpable);
    b.imposed_check[stats_comp] = static_cast<int>(res);
    b.turn_defenses.push_back({spot, res});
    bump("defenses_imposed");
    log().write("coop_defense_imposed", JsonObj().add("spot", spot).add("hit", static_cast<int>(hit)).add("owner", static_cast<int>(owner)).add("jumpable", jumpable)
                                         .add("result", static_cast<int>(res)).add("received", got).add("waited_ms", waited_ms).add("why", why), b.turn);
}

// --- Démarrage synchronisé des tours ennemis (hôte, D28) -----------------------------------------------------
// L'hôte est en avance sur ses clients : chaque client attend son début de tour, et rejoue les actions des héros de
// l'hôte avec un décalage (replay_lag_s, action_delay_s). Au tour ennemi qui suit, l'hôte frappe donc les héros d'un
// client avant lui, et fige son image (defense_point) jusqu'à ce que le résultat de ce client arrive : avance + aller-
// retour, ~1 s par coup en vrai (essai à deux PC du 2026-10-24, 3 s au pire). Démarrage synchronisé (D28, par défaut,
// -e33turnalign=0 pour le couper) : l'hôte retient le tour ennemi (dépendance de début de tour : le jeu anime, rien
// n'est figé) jusqu'à ce que chaque client qui a un héros vivant l'ait commencé chez lui (son StateHash du tour, ou un
// autre signe de ce tour), 4 s de jeu au plus (pauses non comptées). Le client est alors en avance d'un aller simple :
// l'hôte n'attend presque plus ses résultats, et le client attend ceux des héros de l'hôte un aller-retour au plus.
// Aucune action automatique, même résultat partout (D11). Pas attendus : joueur bloqué (unresponsive), parti, pas dans
// la partie (chargement, menu principal), sans héros vivant ; ni pendant un contre, ni en fin de combat.
constexpr double kAlignTimeout = 4.0;

// Clients de la partie qui ont un héros vivant dans ce combat et n'ont pas encore commencé ce tour (StateHash).
std::vector<uint8_t> align_missing(uint32_t turn)
{
    auto& b = s.battle;
    std::vector<uint8_t> out;
    for (auto& p : s.peers) {
        if (!in_game(p) || b.unresponsive.count(p.player_id) || (b.acks.count(turn) && b.acks[turn].count(p.player_id))) continue;
        bool hero = false;
        for (auto& [spot, owner] : b.owner_by_spot) {
            if (owner != p.player_id) continue;
            UObject* h = game::find_unit(true, spot);
            if (h && !game::is_dead_fast(h)) hero = true;
        }
        if (hero) out.push_back(p.player_id);
    }
    return out;
}

// Hôte : retient le tour ennemi `turn` (déjà commencé ici : son état est parti) tant qu'un client attendu ne l'a pas
// commencé chez lui. Inscription d'une dépendance de début de tour, résolue par align_tick.
void align_begin(uint32_t turn, const char* how)
{
    auto& b = s.battle;
    if (s.role != Role::Host || !s.turn_align || !b.active || b.ending || b.hold || b.align_active || turn <= 1) return;
    if (b.align_turn == turn) return;   // une seule retenue par tour (délai écoulé : on ne recommence pas)
    if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
    const std::vector<uint8_t> missing = align_missing(turn);
    if (missing.empty()) return;
    UObject* deps = turn_deps();
    ue::Call reg(deps, STR("Register Dependency"));
    if (!reg) return;
    reg.set_object(STR("Dependency"), game::save_manager()).invoke();
    b.align_active = true;
    b.align_turn = turn;
    b.align_since = s.now;
    b.align_paused_s = 0;
    bump("turn_aligns");
    std::string w = "[";
    for (uint8_t p : missing) w += (w.size() > 1 ? "," : "") + std::to_string(p);
    log().write("coop_turn_align", JsonObj().add("turn", turn).add("how", how).add("listening", ue::get_bool(deps, STR("IsListening")))
                                       .raw("waiting_for", w + "]"), turn);
}

void align_tick()
{
    auto& b = s.battle;
    static double last = 0;
    const double dt = s.now - last;
    last = s.now;
    if (!b.align_active) return;
    // Jeu en pause (pause d'un joueur, menu pause) : ce temps ne compte pas dans le délai (rien n'avance de toute façon).
    if (explore::remote_paused() || game::pause_menu_visible()) b.align_paused_s += std::max(0.0, std::min(dt, 1.0));
    const bool done = !b.active || b.ending || align_missing(b.align_turn).empty();
    const bool expired = s.now - b.align_since - b.align_paused_s > kAlignTimeout;
    if (!done && !expired) return;
    b.align_active = false;
    // Diagnostic : le jeu écoutait-il nos dépendances (sinon la retenue n'a rien retenu) ?
    const bool listening = ue::get_bool(turn_deps(), STR("IsListening"));
    const bool triggered = ue::get_bool(turn_deps(), STR("HasTriggered"));
    if (!listening) bump("turn_align_not_listening");
    ue::Call solve(turn_deps(), STR("SolveDependency"));
    if (solve) solve.set_object(STR("Dependency"), game::save_manager()).invoke();
    if (listening) b.play_since = s.now;   // tour vraiment retenu : il se joue à partir d'ici (DefenseRequest.turn_ms)
    const int ms = static_cast<int>((s.now - b.align_since) * 1000.0);
    bump_max("turn_align_max_ms", ms);
    if (!done) bump("turn_align_timeouts");
    std::string missing = "[";
    if (!done) for (uint8_t p : align_missing(b.align_turn)) missing += (missing.size() > 1 ? "," : "") + std::to_string(p);
    log().write("coop_turn_aligned", JsonObj().add("turn", b.align_turn).add("waited_ms", ms).add("timeout", !done)
                                         .add("paused_ms", static_cast<int>(b.align_paused_s * 1000.0)).raw("missing", missing + "]")
                                         .add("listening", listening).add("triggered", triggered), b.align_turn);
}

// Parade en duo : pendant le tour qui finit, des héros d'au moins deux joueurs ont été touchés (attaque de groupe ou
// coups enchaînés sur plusieurs héros) et tous les coups ont été parés -> +kDuoParryAP à chacun de ces héros.
// Calculé sur chaque instance avec les mêmes données (résultat du propriétaire de chaque défense, propriétaires du
// combat), au même point du jeu (début du tour suivant, avant la photo d'état) : aucun message, aucun écart de hash.
void duo_parry_check()
{
    auto& b = s.battle;
    std::vector<std::pair<int, proto::Defense>> hits;
    hits.swap(b.turn_defenses);
    if (!b.duo_parry || hits.empty()) return;
    std::map<int, bool> parried;   // emplacement -> tous ses coups parés
    std::set<uint8_t> owners;
    for (const auto& [spot, d] : hits) {
        const bool ok = d == proto::Defense::Parry || d == proto::Defense::GradientParry;
        auto [it, fresh] = parried.emplace(spot, ok);
        if (!fresh) it->second = it->second && ok;
        owners.insert(owner_of_spot(spot));
    }
    if (owners.size() < 2) return;
    for (const auto& [spot, ok] : parried) {
        if (ok) continue;
        bump("duo_parries_missed");
        log().write("coop_duo_parry_missed", JsonObj().add("spot", spot).add("hits", static_cast<int64_t>(hits.size())), b.turn);
        return;
    }
    std::string names, detail = "[";
    int gained = 0;
    for (const auto& [spot, ok] : parried) {
        UObject* hero = game::find_unit(true, spot);
        if (!hero || game::is_dead_fast(hero)) continue;
        UObject* st = game::stats(hero);
        const double before = ue::get_double(st, STR("CurrentAP"));
        // GainAP du jeu : plafonné aux PA max (SetAP), effet visuel de gain de PA ; SetAP à défaut.
        ue::Call gain(st, STR("GainAP"));
        if (gain) gain.set_int(STR("Amount"), kDuoParryAP).set_int(STR("ChangeReason"), kDuoParryReason).invoke();
        else {
            ue::Call set(st, STR("SetAP"));
            if (set) set.set_int(STR("NewAP"), std::llround(before) + kDuoParryAP).set_int(STR("ChangeReason"), kDuoParryReason).invoke();
        }
        const double after = ue::get_double(st, STR("CurrentAP"));
        gained += static_cast<int>(std::llround(after - before));
        const std::string name = game::hardcoded_name(hero);
        names += (names.empty() ? "" : ", ") + name;
        if (detail.size() > 1) detail += ",";
        detail += JsonObj().add("hero", name).add("spot", spot).add("ap_before", before).add("ap_after", after).add("via_gain", static_cast<bool>(gain)).str();
    }
    if (names.empty()) return;
    bump("duo_parries");
    bump("duo_parry_ap", gained);
    ui::toast("duo_parry", {names, std::to_string(kDuoParryAP)}, 4.0);
    log().write("coop_duo_parry", JsonObj().add("players", static_cast<int64_t>(owners.size())).add("hits", static_cast<int64_t>(hits.size()))
                                    .raw("heroes", detail + "]"), b.turn);
}

// Début de tour d'un personnage (OnTurnStart, ou rattrapage quand le combat local a démarré avant le BattleStart).
void on_turn_start(UObject* actor)
{
    // Client, combat pas encore suivi par le mod (engagé ici avant le BattleStart) : photo de l'état à cet instant, celui
    // de la photo de l'hôte. Le rattrapage peut arriver des secondes plus tard (monde prêt depuis 5 s) : entre-temps le
    // jeu a déjà appliqué les effets du début de tour (PA du tour : +1 PA à Gustave dans le duel du prologue, 2026-10-23),
    // et la comparaison d'une photo tardive « corrigeait » ce gain, puis l'écart revenait au tour suivant.
    if (s.role == Role::Client && !s.battle.active) {
        s.early_turn = sync::snapshot(0, 1);
        s.early_turn_actor = actor;
        s.early_turn_at = s.now;
    }
    s.enemy_turn = actor && !game::is_player(actor);
    if (s.role != Role::Solo && s.battle.active) duo_parry_check();
    // Commandes de défense (parade, esquive, saut) : branchées par le jeu quand il lance lui-même le combat, pas
    // quand le mod le lance (client : TriggerBattle). Essai à deux PC du 2026-10-24 : aucun appui défensif du client
    // n'atteignait le jeu (TryStart* jamais appelés), alors qu'en solo sa parade marche.
    if (s.role != Role::Solo && s.battle.active) {
        UObject* ctrl = game::controller();
        if (ctrl && !ue::get_bool(ctrl, STR("IsDefenseInputBound"))) {
            ue::Call c(ctrl, STR("TryBindDefendInput"));
            const bool called = c && c.invoke();
            const bool bound = ue::get_bool(ctrl, STR("IsDefenseInputBound"));
            bump(bound ? "defense_input_bound" : "defense_input_bind_failed");
            log().write("coop_defense_input_bind", JsonObj().add("called", called).add("bound", bound), s.battle.turn);
        }
        // Diagnostic (2026-10-24 : parade du client toujours sans effet) : contextes d'entrée de combat actifs au début
        // de chaque tour ennemi, pour comparer avec l'hôte et le solo.
        if (actor && !game::is_player(actor) && s.imc_logs < 40) {
            ++s.imc_logs;
            UObject* sub = game::input_subsystem();
            std::string act;
            for (const wchar_t* n : {L"IMC_BattleDefense", L"IMC_Battle_InputBlocker", L"IMC_Battle_PlayerTurn", L"IMC_Battle_MainWheel",
                                     L"IMC_Battle_Targeting", L"IMC_Battle_QTE", L"IMC_Battle_FreeAim", L"IMC_Battle_PauseMenu"}) {
                UObject* imc = ue::find_object((std::wstring(L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/") + n + L"." + n).c_str());
                ue::Call has(sub, STR("HasMappingContext"));
                if (!imc || !has) continue;
                has.set_object(STR("MappingContext"), imc).invoke();
                if (!has.out_bool(STR("ReturnValue"))) continue;
                if (!act.empty()) act += ",";
                act += ue::to_utf8(n) + ":" + std::to_string(has.out_int(STR("OutFoundPriority")));
            }
            log().write("coop_enemy_turn_inputs", JsonObj().add("contexts", act).add("bound", ctrl ? ue::get_bool(ctrl, STR("IsDefenseInputBound")) : false)
                                                    .add("input_enabled", s.input_enabled), s.battle.turn);
        }
    }
    if (s.role == Role::Host) host_turn_start(actor);
    else if (s.role == Role::Client && s.battle.active) client_turn_start(actor);
    // Tour du héros d'un autre joueur : pas de choix d'action ici (son action arrive par le réseau).
    if (s.role != Role::Solo && s.battle.active && s.lock_input) {
        const bool hero = game::is_player(actor);
        const bool mine = hero && controls(game::spot(actor));
        // Combat fini chez l'hôte : plus d'action ici, la fin est imposée (client_force_end_tick).
        const bool over = s.role == Role::Client && s.battle.host_end >= 0;
        set_local_input((!hero || mine) && !over, over ? "host battle over" : "turn start", mine && !over);
    }
}

// Client : équipe de l'hôte imposée avant un combat (le joueur de l'hôte a pu la changer depuis l'instantané).
void apply_host_party(const proto::BattleStart& bs)
{
    if (bs.party.empty() || game::party_names() == bs.party) return;
    std::string err, mine;
    for (auto& n : game::party_names()) mine += (mine.empty() ? "" : ",") + n;
    std::string host;
    for (auto& n : bs.party) host += (host.empty() ? "" : ",") + n;
    const bool ok = game::set_party_names(bs.party, err);
    const bool same = ok && game::party_names() == bs.party;
    bump(same ? "parties_applied" : "party_apply_failures");
    log().write("coop_party_applied", JsonObj().add("local", mine).add("host", host).add("ok", same).add("err", err));
}

// --- Défaite (D33) -------------------------------------------------------------------------------
// Écran de défaite du jeu (WBP_jRPG_GameOverScreen) : d'abord « Envoyer l'équipe de réserve ? » si elle existe, puis
// « Réessayer » / « Abandonner ». Seul l'hôte répond (DefeatChoice). Client : chaque fenêtre de choix est refermée dès
// qu'elle s'ouvre (statut « l'hôte choisit »), puis la réponse de l'hôte est donnée ici :
//   même réponse donnée à l'écran d'ici (le jeu continue avec la réserve, relance le combat ou revient à l'exploration ;
//   le mod suit un combat qui reprend comme un combat engagé ici avant le BattleStart de l'hôte).
// Vu en test (2026-10-24, combat lancé par une action de jeu, non « rejouable ») : réserve refusée -> le jeu répond seul
// « abandonner » (OnRetryEncounterResponse(false), sans fenêtre) -> retour à l'exploration sur place, sans chargement.
// Filet : encore en combat 20 s après l'abandon (écran noir chez TOTOFFF le 2026-10-24) -> instantané de l'hôte demandé.
// -e33defeatabandon=resync : pas d'abandon ici, instantané demandé tout de suite.
// Hôte muet 120 s après l'ouverture d'une fenêtre : le joueur d'ici choisit lui-même.
constexpr double kDefeatHostSilentS = 120.0;

void defeat_reset(uint32_t battle)
{
    s.defeat = State::Defeat{};
    s.defeat.battle = battle;
    s.defeat.at = s.now;
}

// Défaite vue ici (StartBattleEndFlow, résultat 2) : écran de défaite attendu.
void defeat_arm()
{
    auto& d = s.defeat;
    // Choix de l'hôte déjà reçu pour ce combat (il a pu choisir avant notre fin de combat) : gardé.
    if (d.battle != s.battle.id || d.done_kind >= 0) defeat_reset(s.battle.id);
    d.armed = true;
    d.at = s.now;
    d.next_poll = 0;
    log().write("coop_defeat", JsonObj().add("battle", d.battle).add("host_retry", d.host_choice[0]).add("host_reserve", d.host_choice[1])
                                   .add("sync", s.defeat_sync), s.battle.turn);
}

void defeat_on_host_choice(const proto::DefeatChoice& dc)
{
    auto& d = s.defeat;
    if (d.battle != dc.battle_id || d.done_kind >= 0) defeat_reset(dc.battle_id);
    const int k = dc.prompt == 1 ? 1 : 0;
    d.host_choice[k] = dc.yes ? 1 : 0;
    d.host_choice_at[k] = s.now;
    d.next_poll = 0;
    bump("defeat_choices_received");
    log().write("coop_defeat_host_choice", JsonObj().add("battle", dc.battle_id).add("prompt", k).add("yes", static_cast<int>(dc.yes))
                                               .add("armed", d.armed).add("prompt_seen", d.prompt_kind).add("sync", s.defeat_sync));
}

// Fenêtre de choix refermée ici (le joueur ne peut pas choisir) : désactivée (ses touches ne répondent plus) et réduite.
void defeat_hide(UObject* modal)
{
    auto& d = s.defeat;
    {
        game::ModCall mod;
        ue::Call off(modal, STR("DeactivateWidget"));
        if (off) off.invoke();
        ue::Call vis(modal, STR("SetVisibility"));
        if (vis) { vis.set_int(STR("InVisibility"), 1); vis.invoke(); }   // ESlateVisibility::Collapsed
    }
    ++d.hidden;
    bump("defeat_prompts_hidden");
    ui::set_status("defeat", "defeat_host_choosing", {player_name(0)});
    bool still = false;
    game::defeat_modal(game::defeat_screen(), &still);
    log().write("coop_defeat_prompt_hidden", JsonObj().add("battle", d.battle).add("prompt", d.prompt_kind).add("n", d.hidden).add("still_visible", still));
}

// Réponse de l'hôte à la fenêtre k donnée ici. modal : fenêtre encore ouverte (réponse par elle, comme un clic).
void defeat_apply(int k, UObject* screen, UObject* modal)
{
    auto& d = s.defeat;
    d.applied[k] = true;
    d.applied_at = s.now;
    d.prompt_kind = -1;   // prompt_modal gardé : la même fenêtre (parfois encore « active ») n'est pas une nouvelle fenêtre
    bump("defeat_choices_applied");
    const bool yes = d.host_choice[k] == 1;
    const std::string host = player_name(0);
    // Réserve refusée : la fenêtre « Réessayer / Abandonner » (ou l'abandon automatique du jeu) suit, l'attente continue.
    if (k == 0 || yes) { d.done_kind = k; bump("defeat_follows_done"); }
    // Le jeu d'ici a déjà répondu (abandon automatique après la réserve refusée) : rien à redonner.
    if (d.responded[k]) {
        if (k == 0) ui::set_status("defeat", yes ? "defeat_host_retry" : "defeat_host_abandon", {host});
        log().write("coop_defeat_applied", JsonObj().add("battle", d.battle).add("prompt", k).add("yes", yes).add("how", "already").add("ok", true));
        return;
    }
    if (k == 1 || yes || !s.defeat_abandon_resync) {
        const int resp = yes ? game::RespConfirm : game::RespCancel;
        bool ok = false;
        const char* how = "none";
        d.applying = true;
        {
            game::ModCall mod;
            if (modal) {
                ue::Call c(modal, STR("NotifyUserResponse"));
                if (c) { c.set_int(STR("InUserResponse"), resp); ok = c.invoke(); how = "modal"; }
            }
            if (!ok && screen) {
                ue::Call c(screen, k == 1 ? STR("OnReserveTeamModalResponse") : STR("OnRetryUserResponse"));
                if (c) { c.set_int(STR("InUserResponse"), resp); ok = c.invoke(); how = "screen"; }
            }
        }
        d.applying = false;
        if (!ok) bump("defeat_apply_errors");
        if (k == 1 && !yes) ui::set_status("defeat", "defeat_host_choosing", {host});
        else ui::set_status("defeat", k == 1 ? "defeat_host_reserve" : yes ? "defeat_host_retry" : "defeat_host_abandon", {host});
        log().write("coop_defeat_applied", JsonObj().add("battle", d.battle).add("prompt", k).add("yes", yes).add("how", how).add("ok", ok)
                                               .add("response", resp).add("wait_s", d.prompt_at >= 0 ? s.now - d.prompt_at : -1.0));
        return;
    }
    // Abandon : la partie de l'hôte, rechargée chez lui, arrive par un instantané (point sûr de l'hôte, D27).
    publish(proto::ResyncRequest{"host abandoned the battle"});
    bump("defeat_resyncs_requested");
    ui::set_status("defeat", "defeat_host_abandon", {host});
    log().write("coop_defeat_applied", JsonObj().add("battle", d.battle).add("prompt", 0).add("yes", false).add("how", "resync").add("ok", true)
                                           .add("wait_s", d.prompt_at >= 0 ? s.now - d.prompt_at : -1.0));
}

void defeat_tick()
{
    auto& d = s.defeat;
    // Classe de l'écran de défaite chargée dès le premier combat : ses hooks sont posés avant la première défaite.
    static bool screen_class_loaded = false;
    if (!screen_class_loaded && s.battle.active) { screen_class_loaded = true; game::load_asset(game::kGameOverScreenClass); }
    if (s.role != Role::Client || !s.defeat_sync) return;
    if (s.now < d.next_poll) return;
    // Statut retiré une fois le choix suivi (nouveau combat ou combat repris, ou monde rechargé après l'abandon).
    if (d.done_kind >= 0) {
        d.next_poll = s.now + 0.5;
        const bool abandon = d.done_kind == 0 && d.host_choice[0] == 0;
        // Filet : toujours en combat 20 s après l'abandon -> partie de l'hôte par un instantané (point sûr de l'hôte, D27).
        if (abandon && !d.rescue_sent && s.now - d.applied_at > 20.0 && game::in_battle()) {
            d.rescue_sent = true;
            d.applied_at = s.now;   // nouvelle attente (chargement de l'instantané)
            publish(proto::ResyncRequest{"still in battle after the host gave up"});
            bump("defeat_resyncs_requested");
            log().write("coop_defeat_rescue", JsonObj().add("battle", d.battle));
            return;
        }
        const bool done = s.now - d.applied_at > 120.0 ||
                          (!abandon && s.battle.active && s.battle.id != d.battle) ||
                          (abandon && s.now - d.applied_at > 5.0 && game::controller() && !game::in_battle());
        if (done) {
            ui::clear_status("defeat");
            log().write("coop_defeat_done", JsonObj().add("battle", d.battle).add("prompt", d.done_kind).add("after_s", s.now - d.applied_at));
            defeat_reset(0);
        }
        return;
    }
    if (!d.armed && d.host_choice[0] < 0 && d.host_choice[1] < 0) return;
    d.next_poll = s.now + 0.2;   // FindAllOf : pas à chaque image
    UObject* screen = game::defeat_screen();
    bool visible = false;
    UObject* modal = screen ? game::defeat_modal(screen, &visible) : nullptr;
    if (modal && modal != d.prompt_modal) {
        // Nouvelle fenêtre de choix : « Réessayer / Abandonner » si PromptBattleRetry est passé, sinon l'équipe de réserve.
        d.prompt_modal = modal;
        d.prompt_kind = d.retry_prompted ? 0 : 1;
        d.prompt_at = s.now;
        bump("defeat_prompts_seen");
        log().write("coop_defeat_prompt", JsonObj().add("battle", d.battle).add("prompt", d.prompt_kind).add("host_choice", d.host_choice[d.prompt_kind])
                                              .add("visible", visible).add("after_s", s.now - d.at));
    }
    const int k = d.prompt_kind;
    // Réponse de l'hôte connue pour la fenêtre ouverte ici : donnée tout de suite.
    if (k >= 0 && !d.local_allowed && d.host_choice[k] >= 0 && !d.applied[k]) { defeat_apply(k, screen, modal); return; }
    // Le jeu d'ici a déjà répondu à une fenêtre dont l'hôte a donné la réponse (abandon automatique) : suivi noté.
    for (int j = 1; j >= 0; --j)
        if (d.responded[j] && d.host_choice[j] >= 0 && !d.applied[j]) { defeat_apply(j, screen, nullptr); return; }
    // Sans fenêtre vue ici 15 s après la réponse de l'hôte (écran pas encore là, autre déroulé) : réponse donnée à
    // l'écran, réserve d'abord.
    for (int j = 1; j >= 0 && k < 0 && screen && !d.local_allowed; --j)
        if (d.host_choice[j] >= 0 && !d.applied[j] && s.now - d.host_choice_at[j] > 15.0 && (j == 1 || d.host_choice[1] != 1)) {
            log().write("coop_defeat_no_prompt_apply", JsonObj().add("prompt", j));
            defeat_apply(j, screen, nullptr);
            return;
        }
    // Abandon de l'hôte sans écran de défaite ici (combat local pas perdu) : instantané de l'hôte demandé.
    if (!screen && !d.armed && !d.applied[0] && d.host_choice[0] == 0 && s.now - d.host_choice_at[0] > 30.0) {
        d.applied[0] = true;
        d.applied_at = s.now;
        d.done_kind = 0;
        d.rescue_sent = true;
        publish(proto::ResyncRequest{"host gave up a battle not lost here"});
        bump("defeat_resyncs_requested");
        log().write("coop_defeat_rescue", JsonObj().add("battle", d.battle).add("why", "no defeat here"));
        return;
    }
    if (modal && visible && !d.local_allowed) defeat_hide(modal);
    // Hôte muet : le joueur d'ici choisit (« Réessayer / Abandonner » rouverte ; réserve : refusée ici, la suivante s'ouvre).
    if (!d.local_allowed && k >= 0 && d.host_choice[k] < 0 && s.now - d.prompt_at > kDefeatHostSilentS && screen) {
        d.local_allowed = true;
        bump("defeat_local_fallbacks");
        bool ok = false;
        {
            game::ModCall mod;
            if (k == 0) { ue::Call c(screen, STR("PromptBattleRetry")); ok = c && c.invoke(); }
            else { ue::Call c(screen, STR("OnReserveTeamModalResponse")); if (c) { c.set_int(STR("InUserResponse"), game::RespCancel); ok = c.invoke(); } }
        }
        ui::clear_status("defeat");
        ui::toast("defeat_host_silent", {player_name(0)}, 10.0);
        log().write("coop_defeat_local_fallback", JsonObj().add("battle", d.battle).add("prompt", k).add("ok", ok));
    }
    // Diagnostic : écran de défaite sans fenêtre de choix reconnue 20 s après la défaite (autre parent ?).
    if (screen && d.prompt_at < 0 && !d.diag_logged && s.now - d.at > 20.0) {
        d.diag_logged = true;
        std::vector<UObject*> all;
        RC::Unreal::UObjectGlobals::FindAllOf(STR("WBP_ModalPopup_C"), all);
        std::string list = "[";
        for (UObject* m : all) {
            if (list.size() > 1) list += ",";
            UObject* parent = ue::get_object(m, STR("ParentActivatableWidget"));
            list += JsonObj().add("active", ue::get_bool(m, STR("bIsActive"))).add("parent", parent ? ue::short_name(parent) : std::string()).str();
        }
        log().write("coop_defeat_no_prompt", JsonObj().add("screen", ue::short_name(screen)).raw("modals", list + "]"));
    }
}

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();

    // Client : combat lancé par un ennemi de la carte (le nôtre, ou notre pion suiveur qui l'a touché avant que le mod
    // le lance) : même équipe et même engagement que chez l'hôte. Vu le 2026-10-21 : attaque surprise de l'hôte vue
    // comme « pris par surprise » chez le client, et combat avec l'ancienne équipe (Verso seul).
    bus.pre(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle", [](HookCtx& ctx, UFunction* f) {
        if (s.role != Role::Client) return;
        // Le pion suiveur peut engager avant que le mod lance le combat : le jeu se dit « en combat » (transition) et le
        // mod marque le combat lancé, mais StartBattle n'arrive qu'après (vu le 2026-10-21 : ~0,4 s plus tard). On
        // garde donc le dernier BattleStart tant que ce combat n'a pas commencé son premier tour.
        const proto::BattleStart* p = s.pending_start ? &*s.pending_start
                                    : (s.last_start && s.last_start->battle_id == s.battle.id && s.battle.turn == 0 && !s.battle.finalized) ? &*s.last_start
                                    : (s.early_start && s.now - s.early_start_at < 20.0) ? &*s.early_start
                                    : nullptr;
        if (!p) {
            bump("world_battle_without_host_start");
            log().write("coop_world_battle_early", JsonObj().add("enemy", ue::short_name(ctx.Context)));
            return;
        }
        const proto::BattleStart& bs = *p;
        const std::string enemy = ue::short_name(ctx.Context);
        if (bs.world_enemy.empty() || enemy != bs.world_enemy) {
            bump("world_battle_other_enemy");
            log().write("coop_world_battle_other_enemy", JsonObj().add("local", enemy).add("host", bs.world_enemy));
            return;
        }
        apply_host_party(bs);
        int size = 0;
        if (auto* e = static_cast<uint8_t*>(ue::param_address(ctx, f, STR("EngagementType"), &size)); e && size == 1 && *e != bs.engage) {
            log().write("coop_engage_forced", JsonObj().add("local", static_cast<int>(*e)).add("host", static_cast<int>(bs.engage)));
            *e = bs.engage;
            bump("engages_forced");
        }
    });
    // Hôte : avis précoce dès le lancement d'un combat de la carte (le BattleStart complet ne part qu'au premier tour,
    // après l'animation d'entrée ; le pion suiveur du client touchait l'ennemi ~3 s avant, vu le 2026-10-21).
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle", [](HookCtx& ctx, UFunction* f) {
        if (s.role != Role::Host || s.battle.active) return;
        proto::BattleStart n;
        n.battle_id = 0;
        n.world_enemy = ue::short_name(ctx.Context);
        n.engage = static_cast<uint8_t>(ue::read_param_int(ctx, f, STR("EngagementType")).value_or(0));
        n.party = game::party_names();
        host_setup_open(true);   // graine du combat dès l'avis : le pion suiveur du client peut engager avant le BattleStart (D30)
        n.rng_seed = s.setup.seed;
        broadcast(n);
        bump("battle_notices_sent");
        log().write("coop_battle_notice", JsonObj().add("enemy", n.world_enemy).add("engage", static_cast<int>(n.engage)));
    });

    // Lancement d'un combat, quel que soit le chemin (ennemi de la carte, game action, dialogue) : fenêtre des tirages
    // de la mise en place (D30), avant le BeginPlay des héros (roue de Monoco) et l'équipement de début de combat.
    bus.pre(fn(game::path::BattleMgr, L"StartBattleNEW"), [](HookCtx&, UFunction*) {
        if (s.role == Role::Host) host_setup_open(false);
        else if (s.role == Role::Client) client_setup_open();
    });

    // Client : le personnage qui joue est celui de l'hôte, pas celui que le jeu local a calculé. L'ordre dépend de
    // l'initiative, de l'engagement (attaque surprise) et des buffs de vitesse, qui peuvent différer : le 2026-10-20
    // (Steam, 2 PC), l'hôte a ouvert avec Maelle et le client avec Verso, puis chacun attendait l'autre. On attend
    // donc le TurnStart de l'hôte et on remplace le paramètre Character avant que le tour ne commence.
    bus.pre(fn(game::path::BattleMgr, L"StartCharacterTurn"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Host && s.turn_align && !s.battle.ending) {   // D28 : qui joue le prochain tour
            ++s.battle.sct_seq;
            auto** slot = static_cast<UObject**>(ue::param_address(ctx, f, STR("Character")));
            s.battle.next_enemy = slot && *slot && !game::is_player(*slot);
            return;
        }
        if (s.role != Role::Client || s.battle.ending) return;
        ++s.battle.sct_seq;
        if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
        // Premier tour d'un combat engagé ici avant que le mod le lance (pion suiveur) : le combat n'est pas encore
        // « actif », mais l'hôte l'a annoncé (avis précoce ou BattleStart). Vu le 2026-10-21 : sans ça, le client
        // jouait Verso au tour 1 pendant que l'hôte jouait Maelle.
        const bool first = !s.battle.active;
        if (first && !s.pending_start && !(s.early_start && s.now - s.early_start_at < 30.0)) return;
        auto& b = s.battle;
        const uint32_t next = first ? 1 : b.turn + 1;
        // Essai (-e33turndep=1) : TurnStart de l'hôte pas encore là -> au lieu de figer le jeu, on enregistre une
        // dépendance de début de tour ; le jeu attend en continuant d'animer, et le tick la résout à l'arrivée.
        if (!first && s.turn_dep && !b.strict && !b.host_turns.count(next) && b.host_end < 0) {
            bump("actor_checks_deferred");   // vérifié au début du tour (OnTurnStart), sans figer le jeu ici
            return;
        }
        if (!wait_for("turn_actor", [&] {
                if (first) return (s.pending_start || b.active) && b.host_turns.count(1) > 0;
                return b.host_turns.count(next) > 0 || b.host_end >= 0;
            }, 15.0)) {
            bump("actor_wait_timeouts");
            log().write("coop_actor_wait_timeout", JsonObj().add("battle", b.id).add("first", first), next);
            return;
        }
        auto it = b.host_turns.find(next);
        if (it == b.host_turns.end()) return;   // combat fini chez l'hôte
        auto** slot = static_cast<UObject**>(ue::param_address(ctx, f, STR("Character")));
        if (!slot) return;
        UObject* local = *slot;
        const proto::Unit want{it->second.kind, it->second.index};
        if (local && actor_unit(local).kind == want.kind && actor_unit(local).spot == want.spot) return;
        UObject* host_actor = game::find_unit(want.kind == proto::ActorKind::Hero, want.spot);
        if (!host_actor || game::is_dead_fast(host_actor)) {
            bump("actor_force_failures");
            log().write("coop_actor_force_failed", JsonObj().add("local", local ? game::char_class(local) : "").add("host", it->second.actor), next);
            return;
        }
        *slot = host_actor;
        bump("actors_forced");
        log().write("coop_actor_forced", JsonObj().add("local", local ? game::char_class(local) : "").add("host", it->second.actor), next);
    });

    // Diagnostic D28 (hôte, en combat) : déroulé du début de tour (écoutes des dépendances, phases, auditeurs).
    {
        auto diag = [](const char* what) {
            return [what](HookCtx& ctx, UFunction*) {
                if (s.role != Role::Host || !s.battle.active || s.battle.ending) return;
                UObject* deps = turn_deps();
                log().write("coop_turn_flow", JsonObj().add("what", what).add("turn", s.battle.turn).add("self", ue::short_name(ctx.Context))
                                                  .add("is_turn_deps", ctx.Context == deps)
                                                  .add("listening", deps ? ue::get_bool(deps, STR("IsListening")) : false)
                                                  .add("triggered", deps ? ue::get_bool(deps, STR("HasTriggered")) : false)
                                                  .add("align_active", s.battle.align_active), s.battle.turn);
            };
        };
        bus.pre(L"/Game/Gameplay/BP_DependencyRefCounter.BP_DependencyRefCounter_C:StartListenToDependencies", diag("dep_listen"));
        bus.pre(fn(game::path::BattleMgr, L"StartCharacterTurn_Phase2"), diag("phase2"));
        bus.pre(fn(game::path::BattleMgr, L"ProcessNextTurnStartListener"), diag("next_listener"));
    }
    // D28 : le jeu ne retient le début de tour que si un « auditeur » (BattleListeners : effets, pictos…) s'inscrit
    // comme dépendance : pour chacun, ProcessNextTurnStartListener remet le compteur à zéro (Reset), appelle son
    // OnCharacterTurnStart, puis écoute s'il y a une dépendance (StartListenToDependencies). Sans auditeur inscrit
    // (tours 9 et 16 du Lancelier Alpha, 2026-10-24), aucune écoute : une inscription faite plus tôt était effacée par
    // Reset. Inscription juste après le premier Reset du tour ennemi : le jeu écoute alors et attend notre résolution.
    // Client (même raison) : tour ennemi commencé ici avant l'état de l'hôte (vérification différée, -e33turndep) et que le jeu
    // n'aurait pas retenu faute d'auditeur : il le retient maintenant jusqu'à l'état de l'hôte, jeu animé. Sinon le client
    // jouait le tour en avance et figeait son image en attendant les défenses des héros de l'hôte (sens inverse de D28).
    bus.post(L"/Game/Gameplay/BP_DependencyRefCounter.BP_DependencyRefCounter_C:Reset", [](HookCtx& ctx, UFunction*) {
        auto& b = s.battle;
        if (!b.active || b.ending) return;
        if (s.role == Role::Host) {
            if (!s.turn_align || b.align_active || b.align_turn == b.turn || !b.cur_enemy || b.ots_seq != b.sct_seq) return;
            if (ctx.Context != turn_deps()) return;
            align_begin(b.turn, "reset");
            return;
        }
        if (s.role != Role::Client || !s.turn_dep || !s.turn_align || b.strict || b.dep_active) return;
        if (!b.dep_finish_pending || b.dep_turn != b.turn || b.host_sync.count(b.turn) || b.host_end >= 0) return;
        if (!b.dep_actor || game::is_player(b.dep_actor)) return;   // tours ennemis seulement (menu d'un héros jamais retardé)
        if (ctx.Context != turn_deps() || ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
        ue::Call reg(ctx.Context, STR("Register Dependency"));
        if (!reg) return;
        reg.set_object(STR("Dependency"), game::save_manager()).invoke();
        b.dep_active = true;
        bump("turn_deps_registered");
        bump("turn_deps_on_reset");
        log().write("coop_turn_dep", JsonObj().add("turn", b.turn).add("after_turn_start", true).add("how", "reset"), b.turn);
    });
    // Client (-e33turndep, par défaut) : le jeu commence à attendre les dépendances du début de tour -> inscription du mod
    // si l'état de l'hôte pour ce tour n'est pas encore arrivé (résolue par le tick à son arrivée). Hôte : voir D28.
    bus.pre(L"/Game/Gameplay/BP_DependencyRefCounter.BP_DependencyRefCounter_C:StartListenToDependencies", [](HookCtx& ctx, UFunction*) {
        // Hôte, démarrage synchronisé (D28) : tour ennemi retenu tant qu'un client qui a des héros ne l'a pas commencé chez lui.
        if (s.role == Role::Host) {
            auto& b = s.battle;
            if (!s.turn_align || !b.active || b.ending || ctx.Context != turn_deps()) return;
            if (b.hold || b.align_active) return;
            if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
            const bool after = b.ots_seq == b.sct_seq;   // OnTurnStart de ce tour déjà passé ?
            const uint32_t turn = after ? b.turn : b.turn + 1;
            if (turn <= 1 || !(after ? b.cur_enemy : b.next_enemy) || align_missing(turn).empty()) return;
            // OnTurnStart pas encore passé : l'état de ce tour (TurnStart, StateSync) n'est pas parti, le client ne peut pas
            // commencer le tour ; le retenir ici ferait attendre chacun l'autre jusqu'au délai. Pas d'alignement.
            if (!after) {
                bump("turn_align_skipped_early");
                log().write("coop_turn_align_skipped", JsonObj().add("turn", turn).add("why", "before_turn_start"), turn);
                return;
            }
            align_begin(turn, "listen");
            return;
        }
        if (s.role != Role::Client || !s.turn_dep || !s.battle.active || s.battle.strict || s.battle.ending) return;
        if (ctx.Context != turn_deps() || ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
        auto& b = s.battle;
        b.listen_seq = b.sct_seq;
        // Le OnTurnStart de ce tour est-il déjà passé ? (l'ordre varie selon les tours)
        const uint32_t turn = b.ots_seq == b.sct_seq ? b.turn : b.turn + 1;
        if (turn <= 1 || b.dep_active || b.host_sync.count(turn) || b.host_end >= 0) return;
        // OnTurnStart déjà passé et tour déjà vérifié (état de l'hôte arrivé avant, puis consommé) : rien à attendre.
        // Sinon la dépendance attendait un état déjà retiré et retenait le tour jusqu'au suivant (p6_turndep, tour 4).
        if (turn == b.turn && !(b.dep_finish_pending && b.dep_turn == turn)) return;
        ue::Call reg(ctx.Context, STR("Register Dependency"));
        if (!reg) return;
        reg.set_object(STR("Dependency"), game::save_manager()).invoke();
        b.dep_active = true;
        b.dep_turn = turn;
        if (!b.dep_finish_pending) b.dep_since = s.now;
        bump("turn_deps_registered");
        log().write("coop_turn_dep", JsonObj().add("turn", turn).add("after_turn_start", b.ots_seq == b.sct_seq), turn);
    });

    // Début de tour : hôte -> TurnStart + StateHash + StateSync ; client -> contrôle et resynchro éventuelle.
    bus.post(fn(game::path::BattleChar, L"OnTurnStart"), [](HookCtx& ctx, UFunction*) {
        if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;   // contre : pas un tour
        on_turn_start(ctx.Context);
    });

    // Passer le tour / fuir (chemins de l'interface, sans RequestActionExecution) : publiés comme les autres actions.
    for (const auto& [fn_name, type] : {std::pair{L"OnSelectSkip", game::ActSkip}, std::pair{L"OnSelectFlee", game::ActFlee}}) {
        const int t = type;
        bus.pre(fn(game::path::BattleMgr, fn_name), [t](HookCtx&, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active) return;
            UObject* cur = game::current_character();
            if (!cur || !game::is_player(cur) || !controls(game::spot(cur))) return;
            proto::ActionCommand ac;
            ac.battle_id = s.battle.id;
            ac.turn = s.battle.turn;
            ac.actor_spot = static_cast<uint8_t>(game::spot(cur));
            ac.action_type = static_cast<uint8_t>(t);
            bump("actions_published");
            publish(ac);
        });
    }

    // Action choisie ici pour un héros contrôlé ici (test runner ou joueur) : publiée telle quelle.
    bus.pre(fn(game::path::BattleMgr, L"RequestActionExecution"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* cur = game::current_character();
        if (!cur || !game::is_player(cur) || !controls(game::spot(cur))) return;
        UObject* bm = game::battle_manager();
        proto::ActionCommand ac;
        ac.battle_id = s.battle.id;
        ac.turn = s.battle.turn;
        ac.actor_spot = static_cast<uint8_t>(game::spot(cur));
        ac.action_type = static_cast<uint8_t>(ue::get_int(bm, STR("SelectedAction"), 1));
        if (ac.action_type == game::ActItem || ac.action_type == game::ActSkill) ac.secondary = ue::get_name_string(bm, STR("SelectedSecondaryAction"));
        if (auto* arr = static_cast<RC::Unreal::FScriptArray*>(ue::param_address(ctx, f, STR("TargetedCharacters")))) {
            auto** data = static_cast<UObject**>(arr->GetData());
            for (int32_t i = 0; i < arr->Num(); ++i) ac.targets.push_back(sync::unit_of(data[i]));
        }
        // Compétence sur un allié choisie à la main (ex. « Puissant » de Gustave sur Lune, essai du 2026-10-24) : le jeu
        // passe une liste vide, les cibles sont dans le gestionnaire de ciblage. Sans elles, l'autre joueur prenait une
        // cible automatique (le premier ennemi) : buff sur l'Évêque chez lui, sur Lune ici, puis combat qui diverge.
        if (ac.targets.empty() && ac.action_type != game::ActSkip && ac.action_type != game::ActFlee) {
            UObject* tm = ue::get_object(game::controller(), STR("AC_jRPG_TargetingManager"));
            for (UObject* t : ue::get_object_array(tm, STR("TargetedCharacters"))) if (t) ac.targets.push_back(sync::unit_of(t));
            bump(ac.targets.empty() ? "action_targets_empty" : "action_targets_from_targeting");
            log().write("coop_action_targets_fallback", JsonObj().add("action", static_cast<int>(ac.action_type)).add("secondary", ac.secondary)
                                                           .add("targets", static_cast<int64_t>(ac.targets.size())), s.battle.turn);
        }
        bump("actions_published");
        publish(ac);
    });

    // QTE offensifs (D11). Imposer « réussi/parfait » en fin de fenêtre ne reproduit pas un vrai appui (TryActivate a
    // d'autres effets : un coup de plus vu le 2026-10-19). Donc : le propriétaire publie son résultat ET l'instant de son
    // appui dans la fenêtre ; les autres instances, qui jouent l'action ~1 s plus tard, rejouent un vrai appui au même
    // instant (« Try Activate Skill Combo »), avec le verdict « parfait » forcé dans IsInPerfectWindow.
    bus.post(fn(game::path::Timeline, L"OnSkillComboWindowStartReached"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* hero = timeline_hero(ctx.Context);
        if (!hero) return;
        auto& b = s.battle;
        Window w;
        w.hero = hero;
        w.spot = game::spot(hero);
        w.seq = ++b.combo_seq[w.spot];
        w.start = SteadyClock::now();
        w.duration = ue::get_double(ctx.Context, STR("CurrentWindowEndRealTime")) - ue::get_double(ctx.Context, STR("CurrentWindowStartRealTime"));
        w.local = controls(w.spot);
        if (!w.local) {
            const TurnSpotSeq key{b.turn, w.spot, w.seq};
            const ULONGLONG t0 = GetTickCount64();
            const uint8_t owner = owner_of_spot(w.spot);
            // Pas d'attente (image figée) quand aucun résultat ne viendra à temps : action imposée par l'hôte (minuteur ou
            // tour bloqué : le propriétaire ne l'a pas jouée), propriétaire qui ne répond plus, combat fini chez l'hôte.
            const bool forced = b.forced.count({b.turn, w.spot}) > 0;
            const bool skip = forced || (s.role == Role::Host && b.unresponsive.count(owner) > 0);
            const bool got = b.combos.count(key) > 0 ||
                             (!skip && wait_for("combo", [&] { return b.combos.count(key) > 0 || owner_of_spot(w.spot) != owner || (s.role == Role::Client && b.host_end >= 0); },
                                                kComboTimeout) &&
                              b.combos.count(key) > 0);
            const auto waited_ms = static_cast<int64_t>(GetTickCount64() - t0);
            if (got) w.plan = b.combos[key];
            else {
                bump(skip ? "combo_waits_skipped" : (s.role == Role::Client && b.host_end >= 0) ? "combo_waits_battle_over" : "combo_timeouts");
                w.plan = proto::ComboResult{b.id, b.turn, static_cast<uint8_t>(w.spot), w.seq, false, false, proto::kNoPress};
            }
            if (w.plan.success && w.plan.press <= 1000)
                b.presses.push_back({ctx.Context, w.start + std::chrono::microseconds(static_cast<int64_t>(w.duration * w.plan.press * 1000.0))});
            log().write("coop_combo_plan", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", w.plan.success).add("perfect", w.plan.perfect)
                                               .add("press", static_cast<int>(w.plan.press)).add("received", got).add("waited_ms", waited_ms), b.turn);
        }
        b.windows[ctx.Context] = w;
    });
    // Diagnostic : tout appui QTE qui atteint le gestionnaire de combat, avec les fenêtres ouvertes à cet instant.
    bus.pre(fn(game::path::BattleMgr, L"OnComboPressed"), [](HookCtx&, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        std::string open = "[";
        for (const auto& [tl, w] : s.battle.windows) {
            if (open.size() > 1) open += ",";
            open += "{\"spot\":" + std::to_string(w.spot) + ",\"seq\":" + std::to_string(w.seq) + ",\"local\":" + (w.local ? "true" : "false")
                  + ",\"elapsed_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - w.start).count())
                  + ",\"window_ms\":" + std::to_string(static_cast<int64_t>(w.duration * 1000)) + "}";
        }
        log().write("coop_combo_pressed", JsonObj().raw("open", open + "]").add("input_enabled", s.input_enabled), s.battle.turn);
    });
    // Héros d'ici : le premier appui réussi est publié tout de suite, avec son instant dans la fenêtre.
    bus.post(fn(game::path::Timeline, L"TryActivate"), [](HookCtx& ctx, UFunction* f) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end()) return;
        Window& w = it->second;
        const bool ok = ue::read_out_bool(ctx, f, STR("Success")).value_or(false);
        const bool perfect = ue::read_out_bool(ctx, f, STR("Perfect")).value_or(false);
        if (!w.local) {
            w.pressed = true;
            if (ok != w.plan.success || perfect != w.plan.perfect) {
                bump("combo_verdict_mismatches");
                log().write("coop_combo_verdict_mismatch", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", ok).add("perfect", perfect), s.battle.turn);
            }
            return;
        }
        const double elapsed = std::chrono::duration<double>(SteadyClock::now() - w.start).count();
        // Diagnostic (QTE « raté d'emblée » à la manette, 2026-10-20) : chaque appui d'ici, même rejeté.
        log().write("coop_combo_try", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("ok", ok).add("perfect", perfect)
                                          .add("elapsed_ms", static_cast<int64_t>(elapsed * 1000)).add("window_ms", static_cast<int64_t>(w.duration * 1000)), s.battle.turn);
        if (w.published || !ok) return;
        const double frac = w.duration > 0 ? std::clamp(elapsed / w.duration, 0.0, 1.0) : 0.5;
        const proto::ComboResult cr{s.battle.id, s.battle.turn, static_cast<uint8_t>(w.spot), w.seq, true, perfect, static_cast<uint16_t>(std::lround(frac * 1000))};
        w.published = true;
        bump("combos_published");
        log().write("coop_combo_local", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", true).add("perfect", perfect).add("press", static_cast<int>(cr.press)), s.battle.turn);
        publish(cr);
    });
    // Verdict « parfait » d'un appui rejoué : celui du propriétaire (la gigue d'image ne doit pas le changer).
    bus.post(fn(game::path::Timeline, L"IsInPerfectWindow"), [](HookCtx& ctx, UFunction* f) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end() || it->second.local) return;
        ue::write_out_bool(ctx, f, STR("ReturnValue"), it->second.plan.perfect);
    });
    bus.pre(fn(game::path::Timeline, L"OnSkillComboWindowEndReached"), [](HookCtx& ctx, UFunction*) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end() || it->second.local) return;
        Window& w = it->second;
        if (w.plan.success && w.plan.press <= 1000 && !w.pressed) {
            // Appui pas encore rejoué (image trop tardive) : maintenant, avant que la fenêtre ne se ferme.
            std::erase_if(s.battle.presses, [&](const Press& p) { return p.timeline == ctx.Context; });
            ue::Call c(w.hero, STR("Try Activate Skill Combo"));
            if (c) c.invoke();
            bump("combo_late_presses");
        }
        if (w.plan.press == proto::kAutoPress) {   // réussite automatique chez le propriétaire : pas d'appui à rejouer
            ue::set_bool(ctx.Context, STR("IsSkillComboSuccessful"), w.plan.success);
            ue::set_bool(ctx.Context, STR("IsSkillComboPerfect"), w.plan.perfect);
        }
        bump("combos_replayed");
    });
    bus.post(fn(game::path::Timeline, L"OnSkillComboWindowEndReached"), [](HookCtx& ctx, UFunction*) {
        auto it = s.battle.windows.find(ctx.Context);
        if (it == s.battle.windows.end()) return;
        Window w = it->second;
        s.battle.windows.erase(it);
        const bool ok = ue::get_bool(ctx.Context, STR("IsSkillComboSuccessful"));
        const bool perfect = ue::get_bool(ctx.Context, STR("IsSkillComboPerfect"));
        if (w.local) {
            if (w.published) return;
            // Pas d'appui réussi : échec, ou réussite automatique (option d'accessibilité, mode test).
            const proto::ComboResult cr{s.battle.id, s.battle.turn, static_cast<uint8_t>(w.spot), w.seq, ok || perfect, perfect, ok ? proto::kAutoPress : proto::kNoPress};
            bump("combos_published");
            log().write("coop_combo_local", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", cr.success).add("perfect", perfect).add("press", static_cast<int>(cr.press)), s.battle.turn);
            publish(cr);
            return;
        }
        if (ok != w.plan.success || perfect != w.plan.perfect) {
            bump("combo_verdict_mismatches");
            log().write("coop_combo_verdict_mismatch", JsonObj().add("spot", w.spot).add("seq", static_cast<int>(w.seq)).add("success", ok).add("perfect", perfect)
                                                        .add("want_success", w.plan.success).add("want_perfect", w.plan.perfect), s.battle.turn);
        }
    });

    // Visée libre (D11). Le tir part vers le point que renvoie BM.ComputeFreeAim3rdPersonTargetLocation au moment du
    // rayon du projectile (vu en jeu le 2026-10-19). Propriétaire : on publie le début, le point visé (~20 fois/s, pour
    // l'affichage), chaque tir (instant) et son point exact, puis l'annulation. Ailleurs : rejoué avec le même décalage
    // que ses actions, en imposant ce point visé.
    bus.post(fn(game::path::BattleMgr, L"ComputeFreeAim3rdPersonTargetLocation"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        auto* loc = static_cast<double*>(ue::out_param_address(ctx, f, STR("HitLocation")));
        if (!loc) return;
        if (s.fa_rep.started && !s.fa_rep.done && s.fa_rep.have_override) {
            loc[0] = s.fa_rep.point[0]; loc[1] = s.fa_rep.point[1]; loc[2] = s.fa_rep.point[2];
            return;
        }
        if (!s.fa_own.active) return;
        if (s.local_aim) { loc[0] = (*s.local_aim)[0]; loc[1] = (*s.local_aim)[1]; loc[2] = (*s.local_aim)[2]; }   // « joueur » simulé (tests)
        s.fa_own.point = {loc[0], loc[1], loc[2]};
        s.fa_own.have_point = true;
        if (s.fa_own.in_raycast) { s.fa_own.shot_point = s.fa_own.point; s.fa_own.raycast_point = true; }
    });
    auto fa_event = [](proto::FreeAimKind kind, uint16_t seq, const std::array<double, 3>& p) {
        proto::FreeAimEvent e;
        e.battle_id = s.battle.id;
        e.turn = s.battle.turn;
        e.spot = static_cast<uint8_t>(s.fa_own.spot);
        e.kind = kind;
        e.seq = seq;
        e.t_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count());
        e.x = p[0]; e.y = p[1]; e.z = p[2];
        return e;
    };
    bus.pre(fn(game::path::BattleMgr, L"StartFreeAimFromUI"), [fa_event](HookCtx&, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* cur = game::current_character();
        if (!cur || !game::is_player(cur) || !controls(game::spot(cur))) return;
        s.fa_own = FreeAimOwner{};
        s.fa_own.active = true;
        s.fa_own.spot = game::spot(cur);
        s.fa_own.t0 = SteadyClock::now();
        s.fa_diag_at = s.now + 0.5;
        bump("free_aims_published");
        publish(fa_event(proto::FreeAimKind::Start, 0, {}));
    });
    // Diagnostic (2026-10-24 : parade du client sans effet) : appui reçu par le contrôleur (action d'entrée), puis par
    // le gestionnaire de combat, avant TryStartParry / TryStartDodge (journalisés plus haut).
    for (const wchar_t* ev : {L"InpActEvt_IA_Battle_Defense_Parry_K2Node_EnhancedInputActionEvent_1",
                              L"InpActEvt_IA_Battle_Defense_Dodge_K2Node_EnhancedInputActionEvent_0"}) {
        const std::string name = ue::to_utf8(ev).substr(28, 5);   // « Parry » / « Dodge »
        bus.pre(fn(game::path::CtrlWorld, ev), [name](HookCtx&, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active || ++s.defense_path_logs > 120) return;
            log().write("coop_defense_input_event", JsonObj().add("action", name), s.battle.turn);
        });
    }
    for (const wchar_t* f : {L"OnParryAction", L"OnDodgeAction"}) {
        const std::string name = ue::to_utf8(f);
        bus.pre(fn(game::path::BattleMgr, f), [name](HookCtx&, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active || ++s.defense_path_logs > 120) return;
            log().write("coop_defense_bm_action", JsonObj().add("fn", name), s.battle.turn);
        });
    }
    // Diagnostic (2026-10-24 : TOTOFFF vise mais ne tire pas, à la manette) : bouton de tir vu par le héros.
    bus.pre(fn(game::path::BattleChar, L"InpActEvt_IA_Battle_FreeAim_Shoot_K2Node_EnhancedInputActionEvent_6"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active || ++s.shoot_input_logs > 60) return;
        UObject* fa = ue::get_object(ctx.Context, STR("BP_FreeAimShooterBattleComponent"));
        log().write("coop_free_aim_shoot_input", JsonObj().add("spot", game::spot(ctx.Context)).add("mine", controls(game::spot(ctx.Context)))
                                                    .add("aiming", ue::get_bool(fa, STR("IsAiming"))).add("own_active", s.fa_own.active), s.battle.turn);
    });
    bus.pre(fn(game::FreeAimComp, L"OnFreeAimShootAction"), [fa_event](HookCtx& ctx, UFunction*) {
        if (s.role != Role::Solo && s.battle.active && s.fa_own.active && ++s.shoot_input_logs <= 60)
            log().write("coop_free_aim_shoot_action", JsonObj().add("spot", s.fa_own.spot).add("mine", ue::component_owner(ctx.Context) == game::find_unit(true, s.fa_own.spot)), s.battle.turn);
        if (!s.fa_own.active || ue::component_owner(ctx.Context) != game::find_unit(true, s.fa_own.spot)) return;
        publish(fa_event(proto::FreeAimKind::Fire, ++s.fa_own.fires, s.fa_own.point));
    });
    bus.pre(fn(game::FreeAimProjectile, L"TriggerRaycast"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* caster = ue::get_object(ctx.Context, STR("Caster"));
        if (!caster || !game::is_player(caster)) return;
        if (s.fa_own.active && game::spot(caster) == s.fa_own.spot) {
            s.fa_own.in_raycast = true;
            s.fa_own.raycast_point = false;
            s.fa_own.shot_hit = nullptr;
            s.fa_own.shot_part = nullptr;
            return;
        }
        // Rejeu : point exact du tir du propriétaire (attente bornée ; avec le décalage, il est déjà là).
        auto& r = s.fa_rep;
        if (!r.started || r.done || game::spot(caster) != r.key.second) return;
        const uint16_t seq = ++r.raycasts;
        std::optional<proto::FreeAimEvent> shot;   // copie : le vecteur grandit pendant l'attente
        wait_for("free_aim_shot", [&] {
            for (auto& e : s.battle.fa_events[r.key]) if (e.kind == proto::FreeAimKind::Shot && e.seq == seq) { shot = e; return true; }
            return s.role == Role::Client && s.battle.host_end >= 0;   // combat fini chez l'hôte : plus aucun tir ne viendra
        }, 2.0);
        if (!shot) { bump("free_aim_timeouts"); return; }
        std::array<double, 3> p{shot->x, shot->y, shot->z};
        if (shot->target.spot != proto::kAutoSpot) {   // point relatif au combattant touché chez le propriétaire (ou visé par le faux client)
            UObject* unit = game::find_unit(shot->target.kind == proto::ActorKind::Hero, shot->target.spot);
            double a[3];
            // Zone touchée chez le propriétaire (point faible…) : visée là où elle est ici. Sinon (zone absente ici, ancien
            // message) : point relatif au combattant (approché si le point était relatif à la zone).
            const bool on_part = shot->part != 0xFF && free_aim_part_location(free_aim_part_at(unit, shot->part), a);
            if (on_part) bump("free_aim_part_aims");
            else if (shot->part != 0xFF) {
                bump("free_aim_part_missing");
                log().write("coop_free_aim_part_missing", JsonObj().add("seq", static_cast<int>(seq)).add("part", static_cast<int>(shot->part)), s.battle.turn);
            }
            // Zone : visée en son centre (un point de sa surface, visé depuis un endroit un peu différent, la frôlait sans la
            // toucher : essai du 2026-10-24, 16:21) ; les dégâts ne dépendent que de la zone touchée.
            if (on_part) p = {a[0], a[1], a[2]};
            else if (game::location(unit, a)) p = {a[0] + p[0], a[1] + p[1], a[2] + p[2]};
            else {
                bump("free_aim_anchor_missing");
                log().write("coop_free_aim_anchor_missing", JsonObj().add("seq", static_cast<int>(seq)).add("kind", static_cast<int>(shot->target.kind))
                                                                .add("target_spot", static_cast<int>(shot->target.spot)), s.battle.turn);
            }
        }
        r.point = p;
        r.have_override = true;
        r.in_raycast = true;
        r.want = shot->target;
        r.hit_spot = -1;
        bump("free_aim_shots_replayed");
        log().write("coop_free_aim_shot", JsonObj().add("spot", r.key.second).add("seq", static_cast<int>(seq)).add("x", p[0]).add("y", p[1]).add("z", p[2]), s.battle.turn);
    });
    bus.post(fn(game::FreeAimProjectile, L"TriggerRaycast"), [fa_event](HookCtx& ctx, UFunction*) {
        // Tir rejoué : le combattant touché chez le propriétaire l'a-t-il été ici ? Un tir manqué ici seulement laisse
        // l'ennemi plus vivant qu'ailleurs (essai à deux PC du 2026-10-22 : tirs 7, 9 et 14 sur 15 manqués chez le
        // client, Lancelier vivant ici et mort chez l'hôte). Diagnostic ; l'état de l'hôte corrige au tour suivant.
        if (auto& r = s.fa_rep; r.in_raycast) {
            UObject* caster = ue::get_object(ctx.Context, STR("Caster"));
            if (caster && game::is_player(caster) && game::spot(caster) == r.key.second) {
                r.in_raycast = false;
                if (r.want.spot != proto::kAutoSpot && (r.hit_spot < 0 || r.hit_spot != r.want.spot)) {
                    ++r.misses;
                    bump("free_aim_replay_misses");
                    log().write("coop_free_aim_replay_miss", JsonObj().add("spot", r.key.second).add("seq", static_cast<int>(r.raycasts))
                                                                 .add("owner_hit", static_cast<int>(r.want.spot)).add("hit", r.hit_spot)
                                                                 .add("x", r.point[0]).add("y", r.point[1]).add("z", r.point[2]), s.battle.turn);
                }
            }
        }
        if (!s.fa_own.in_raycast) return;
        UObject* caster = ue::get_object(ctx.Context, STR("Caster"));
        if (!caster || game::spot(caster) != s.fa_own.spot) return;
        s.fa_own.in_raycast = false;
        const auto p = s.fa_own.raycast_point ? s.fa_own.shot_point : s.fa_own.point;
        proto::FreeAimEvent e = fa_event(proto::FreeAimKind::Shot, ++s.fa_own.shots, p);
        // Tir qui a touché un combattant : point envoyé relatif à lui (même format que le faux client ; une version
        // précédente retrouve le même point absolu). Le rejeu vise ce combattant même s'il s'est un peu déplacé, et sait
        // qui devait être touché (tir manqué détecté).
        int hit_spot = -1;
        if (UObject* hit = s.fa_own.shot_hit) {
            double a[3];
            if (game::location(hit, a)) {
                e.target = actor_unit(hit);
                hit_spot = e.target.spot;
                // Zone touchée connue (point faible…) : point relatif à elle, le rejeu la vise là où elle est chez lui.
                const int pi = free_aim_part_index(hit, s.fa_own.shot_part);
                double pl[3];
                if (pi >= 0 && free_aim_part_location(s.fa_own.shot_part, pl)) {
                    e.part = static_cast<uint8_t>(pi);
                    a[0] = pl[0]; a[1] = pl[1]; a[2] = pl[2];
                }
                e.x -= a[0]; e.y -= a[1]; e.z -= a[2];
                if (++s.fa_part_logs <= 40)
                    log().write("coop_free_aim_part", JsonObj().add("seq", static_cast<int>(s.fa_own.shots)).add("index", pi).add("sent", e.part != 0xFF)
                                                         .add("part", s.fa_own.shot_part ? ue::short_name(s.fa_own.shot_part) : std::string())
                                                         .add("type", s.fa_own.shot_part ? static_cast<int>(ue::get_int(s.fa_own.shot_part, STR("BodyPartType"), -1)) : -1),
                                s.battle.turn);
            }
        }
        publish(e);
        log().write("coop_free_aim_local_shot", JsonObj().add("seq", static_cast<int>(s.fa_own.shots)).add("x", p[0]).add("y", p[1]).add("z", p[2])
                                                 .add("from_raycast", s.fa_own.raycast_point).add("hit_spot", hit_spot), s.battle.turn);
    });
    bus.post(fn(game::FreeAimComp, L"OnFreeAimProjectileHit"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* target = ue::read_param_object(ctx, f, STR("Character"));
        if (s.fa_own.in_raycast && !s.fa_own.shot_hit) { s.fa_own.shot_hit = target; s.fa_own.shot_part = ue::read_param_object(ctx, f, STR("TargetBodyPartBP")); }
        if (s.fa_rep.in_raycast && s.fa_rep.hit_spot < 0 && target) s.fa_rep.hit_spot = game::spot(target);
        bump("free_aim_hits");
        log().write("coop_free_aim_hit", JsonObj().add("target", game::char_class(target)).add("spot", game::spot(target))
                                          .add("body_part", ue::read_param_int(ctx, f, STR("BodyPart")).value_or(-1)), s.battle.turn);
    });
    for (const wchar_t* name : {L"CancelFreeAimFromUI"}) {
        bus.pre(fn(game::path::BattleMgr, name), [fa_event](HookCtx&, UFunction*) {
            if (!s.fa_own.active) return;
            publish(fa_event(proto::FreeAimKind::Cancel, 0, s.fa_own.point));
            bump("free_aim_cancels_published");
            s.fa_own.active = false;
        });
    }
    bus.post(fn(game::FreeAimComp, L"EndFreeAim"), [fa_event](HookCtx& ctx, UFunction*) {
        if (!s.fa_own.active || ue::component_owner(ctx.Context) != game::find_unit(true, s.fa_own.spot)) return;
        publish(fa_event(proto::FreeAimKind::End, 0, s.fa_own.point));
        s.fa_own.active = false;
    });

    // Défenses : TryParryHitFromCharacter (coups parables) ou ReceiveDamageFromObject (coups sautables).
    bus.pre(fn(game::path::Stats, L"TryParryHitFromCharacter"), [](HookCtx& ctx, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* hero = ue::component_owner(ctx.Context);
        if (!hero || !game::is_player(hero)) return;
        defense_point(ctx.Context, hero, false);
    });
    bus.pre(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        UObject* hero = ue::component_owner(ctx.Context);
        if (!hero || !game::is_player(hero)) return;
        if (!game::incoming_attack_jumpable(ue::read_param_object(ctx, f, STR("DamageBuilder")))) return;
        defense_point(ctx.Context, hero, true);
    });
    bus.post(fn(game::path::Stats, L"TryDodgeHitFromCharacter"), [](HookCtx& ctx, UFunction* f) {
        auto it = s.battle.local_hits.find(ctx.Context);
        if (it == s.battle.local_hits.end()) return;
        it->second.perfect_dodge = ue::read_out_bool(ctx, f, STR("IsPerfectDodge")).value_or(false);
    });
    bus.post(fn(game::path::Stats, L"ReceiveDamageFromObject"), [](HookCtx& ctx, UFunction* f) {
        auto& b = s.battle;
        const bool parried = ue::read_out_bool(ctx, f, STR("ParrySuccess")).value_or(false);
        const bool dodged = ue::read_out_bool(ctx, f, STR("DodgeSuccess")).value_or(false);
        if (auto it = b.local_hits.find(ctx.Context); it != b.local_hits.end()) {
            const LocalHit lh = it->second;
            b.local_hits.erase(it);
            UObject* hero = ue::component_owner(ctx.Context);
            const proto::Defense res = local_result(lh, parried, dodged, ue::get_bool(hero, STR("IsGradientParrying")));
            bump("defenses_published");
            b.turn_defenses.push_back({lh.spot, res});
            log().write("coop_defense_local", JsonObj().add("spot", lh.spot).add("hit", static_cast<int>(lh.hit)).add("result", static_cast<int>(res)), b.turn);
            publish(proto::DefenseResult{b.id, b.turn, static_cast<uint8_t>(lh.spot), lh.hit, res});
            return;
        }
        // Héros d'un autre joueur : le verdict du jeu doit être celui imposé.
        if (auto it = b.imposed_check.find(ctx.Context); it != b.imposed_check.end()) {
            const auto want = static_cast<proto::Defense>(it->second);
            b.imposed_check.erase(it);
            const bool want_parry = want == proto::Defense::Parry || want == proto::Defense::GradientParry;
            const bool want_dodge = want == proto::Defense::Dodge || want == proto::Defense::PerfectDodge || want == proto::Defense::Jump;
            if (parried != want_parry || dodged != want_dodge) {
                bump("defense_verdict_mismatches");
                log().write("coop_defense_verdict_mismatch", JsonObj().add("want", static_cast<int>(want)).add("parried", parried).add("dodged", dodged), b.turn);
            }
        }
    });

    // Entrées de défense parasites sur un héros d'un autre joueur (ex. parade d'un joueur sur une attaque de groupe) :
    // la fenêtre est annulée, seul le résultat du propriétaire compte.
    for (const wchar_t* name : {L"TryStartParry", L"TryStartDodge", L"TryStartJump", L"TryStartGradientParry"}) {
        bus.post(fn(game::path::BattleChar, name), [name](HookCtx& ctx, UFunction*) {
            if (s.role == Role::Solo || !s.battle.active || s.imposing) return;
            UObject* hero = ctx.Context;
            if (!game::is_player(hero)) return;
            if (controls(game::spot(hero))) {
                // Diagnostic (2026-10-24, TOTOFFF à la manette : esquive OK, parade sans animation) : appui du
                // propriétaire vu par le jeu, et blocage des défenses du combat.
                if (++s.defense_try_logs <= 60)
                    log().write("coop_defense_try", JsonObj().add("fn", ue::to_utf8(name)).add("spot", game::spot(hero))
                                                       .add("block_all", ue::get_bool(game::battle_manager(), STR("BlockAllDefensiveMoves"))), s.battle.turn);
                return;
            }
            game::cancel_defense(hero);
            bump("stray_defenses_undone");
            log().write("coop_stray_defense", JsonObj().add("fn", ue::to_utf8(name)).add("spot", game::spot(hero)), s.battle.turn);
        });
    }

    // NE PAS hooker KismetArrayLibrary:Array_Random (custom thunk à paramètres génériques) : le hook UE4SS casse
    // GetRandomAliveHero (GAME-INTERNALS §15). Le ciblage aléatoire est synchronisé plus bas, sur les helpers BP.

    // Tirages de gameplay (D10) : valeur dérivée de la graine de l'hôte, du flux et du rang dans le tour.
    static const RandomFn random_fns[] = {
        {L"RandomIntegerInRange", RandomKind::IntInRange}, {L"RandomInteger", RandomKind::Int}, {L"RandomFloat", RandomKind::Float},
        {L"RandomFloatInRange", RandomKind::FloatInRange}, {L"RandomBool", RandomKind::Bool}, {L"RandomBoolWithWeight", RandomKind::BoolWeight},
    };
    for (const auto& rf : random_fns) {
        const RandomKind kind = rf.kind;
        const std::string fname = ue::to_utf8(rf.name);
        bus.post(std::wstring(L"/Script/Engine.KismetMathLibrary:") + rf.name, [kind, fname](HookCtx& ctx, UFunction* f) {
            if (s.role == Role::Solo) return;
            // Threads d'animation, chargement… : jamais un tirage de gameplay, et aucun accès au jeu permis ici.
            if (!on_game_thread()) { bump("off_thread_draws"); return; }
            trace_random(ctx, fname);
            if (!ctx.RESULT_DECL) return;
            // Avant le tour 1 : mise en place du combat (D30).
            if (!s.battle.active || s.battle.turn == 0) { offturn_draw(ctx, f, kind, fname); return; }
            const std::string stream = gameplay_stream(ctx, fname);
            if (!stream.empty() && draw_allowed(stream)) replace_draw(ctx, f, kind, stream);
            else if (!stream.empty()) log().write("coop_draw_natural", JsonObj().add("stream", stream).add("value", natural_value(ctx, kind)), s.battle.turn);
        });
    }

    // Ciblage aléatoire des ennemis (Array_Random sur les héros vivants) : remplacé par un tirage dérivé de la graine.
    // GetSpecificHero / GetMiddleHero ne tirent au hasard que si le héros voulu est mort.
    struct TargetFn { const wchar_t* name; const wchar_t* out; const wchar_t* spot_param; };
    static const TargetFn target_fns[] = {
        {L"GetRandomAliveHero", L"Output", nullptr},
        {L"GetSpecificHero", L"Character", L"SpotIndex"},
    };
    for (const auto& tf : target_fns) {
        const TargetFn t = tf;
        bus.post(fn(game::path::Targeting, t.name), [t](HookCtx& ctx, UFunction* f) {
            if (s.role == Role::Solo || !s.battle.active || s.battle.turn == 0) return;
            UObject* natural = ue::read_out_object(ctx, f, t.out);
            if (!natural) return;
            if (t.spot_param) {
                const auto want = ue::read_param_int(ctx, f, t.spot_param);
                if (want && *want == game::spot(natural)) return;   // héros voulu vivant : pas de hasard
            }
            std::vector<UObject*> alive;
            for (UObject* h : game::players()) if (!game::is_dead_fast(h)) alive.push_back(h);
            std::sort(alive.begin(), alive.end(), [](UObject* x, UObject* y) { return game::spot(x) < game::spot(y); });
            if (alive.empty()) return;
            int caller_spot = -1;   // ennemi appelant (AcquireTargetRandomHero…), pour un flux par ennemi
            RC::Unreal::FFrame* fr = ctx.TheStack.PreviousFrame();
            for (int depth = 0; fr && caller_spot < 0 && depth < 6; ++depth) {
                caller_spot = cached_enemy_spot(fr->Object());
                RC::Unreal::FFrame* prev = fr->PreviousFrame();
                fr = prev == fr ? nullptr : prev;
            }
            const std::string stream = "Targeting:" + ue::to_utf8(t.name) + "#" + std::to_string(caller_spot);
            uint32_t key = 0;
            uint16_t rank = 0;
            const double u = next_uniform(stream, key, rank);
            UObject* pick = alive[std::min(alive.size() - 1, static_cast<size_t>(u * static_cast<double>(alive.size())))];
            ue::write_out_object(ctx, f, t.out, pick);
            record_draw(stream, key, rank, game::spot(pick), game::spot(natural), static_cast<double>(alive.size()));
            bump("targets_synced");
        });
    }
    bus.post(fn(game::path::Targeting, L"GetRandomCharacterByBuffCritera"), [](HookCtx&, UFunction*) {
        if (s.role == Role::Solo || !s.battle.active) return;
        bump("unsynced_random_targeting");
        log().write("coop_unsynced_targeting", JsonObj().add("fn", "GetRandomCharacterByBuffCritera"), s.battle.turn);
    });

    // Invocations : les nouveaux ennemis entrent dans le cache (leurs tirages d'IA sont de gameplay).
    for (const wchar_t* name : {L"SummonMinions", L"SummonSingleMinion"})
        bus.post(fn(game::path::BattleChar, name), [](HookCtx&, UFunction*) { if (s.battle.active) refresh_enemy_spots(); });

    // Récompenses : l'hôte publie les siennes ; le client compare (mêmes tirages, D10) et prend celles de l'hôte.
    bus.post(fn(game::path::BattleMgr, L"RollBattleRewards"), [](HookCtx& ctx, UFunction* f) {
        if (s.role == Role::Solo || !s.battle.active) return;
        RC::Unreal::FProperty* prop = nullptr;
        void* addr = ue::out_param_address(ctx, f, STR("BattleRewards"), &prop);
        auto* sp = RC::Unreal::CastField<RC::Unreal::FStructProperty>(prop);
        if (!addr || !sp) { bump("rewards_unreadable"); return; }
        proto::BattleRewards mine = read_rewards(addr, sp->GetStruct());
        log().write("coop_rewards", JsonObj().raw("rewards", proto::to_json(mine)), s.battle.turn);
        if (s.role == Role::Host) {
            broadcast(mine);
        } else {
            auto& b = s.battle;
            const ULONGLONG t0 = GetTickCount64();
            wait_for("rewards", [&] { return b.host_rewards.has_value(); }, 5.0);
            // Fin de combat de l'hôte arrivée entre-temps : ses récompenses suivent de peu (~2 s, p3_freeaim 2026-10-22).
            if (!b.host_rewards && b.host_end >= 0) wait_for("rewards_after_end", [&] { return b.host_rewards.has_value(); }, 4.0);
            bump_max("rewards_wait_max_ms", static_cast<int>(GetTickCount64() - t0));
            bump("rewards_checked");
            bool same = b.host_rewards && b.host_rewards->xp == mine.xp && b.host_rewards->gold == mine.gold && b.host_rewards->loot.size() == mine.loot.size();
            for (size_t i = 0; same && i < mine.loot.size(); ++i)
                same = mine.loot[i].item == b.host_rewards->loot[i].item && mine.loot[i].quantity == b.host_rewards->loot[i].quantity;
            if (!same) {
                bump("rewards_mismatches");
                log().write("coop_rewards_mismatch", JsonObj().raw("local", proto::to_json(mine)).raw("host", b.host_rewards ? proto::to_json(*b.host_rewards) : "null"));
                if (b.host_rewards) {
                    if (auto* p = ue::struct_member(sp->GetStruct(), STR("ExperienceGained"))) *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(addr)) = b.host_rewards->xp;
                    if (auto* p = ue::struct_member(sp->GetStruct(), STR("GoldAcquired"))) *static_cast<int32_t*>(p->ContainerPtrToValuePtr<void>(addr)) = b.host_rewards->gold;
                    if (write_loot(addr, sp->GetStruct(), b.host_rewards->loot)) bump("rewards_loot_rewritten");
                    else bump("rewards_loot_unrepaired");
                }
            }
        }
        finalize_battle("rewards");
    });

    bus.pre(fn(game::path::BattleMgr, L"StartBattleEndFlow"), [](HookCtx& ctx, UFunction* f) {
        if (!s.battle.active || s.battle.ending) return;
        const int result = static_cast<int>(ue::read_param_int(ctx, f, STR("BattleEndResult")).value_or(-1));
        s.battle.ending = true;
        s.battle.local_end = result;
        if (s.role == Role::Host) broadcast(proto::BattleEnd{s.battle.id, static_cast<uint8_t>(std::clamp(result, 0, 3))});
        log().write("coop_battle_end", JsonObj().add("battle", s.battle.id).add("result", result).add("desyncs", counter("desyncs")));
        // Sans victoire, pas de récompenses : la fin est traitée tout de suite.
        if (result != game::EndVictory) s.battle.end_flow_at = s.now;
        if (result == game::EndDefeat) defeat_arm();
    });


    // Défaite (D33). Réponse donnée à une fenêtre de l'écran de défaite (k = 0 « Réessayer / Abandonner », 1 équipe de
    // réserve ; tous les chemins : fenêtre, mod) : l'hôte la publie ; chez un client, une réponse qui ne vient pas du mod
    // est une fuite (comptée), remplacée par celle de l'hôte si elle est connue.
    for (int k = 0; k < 2; ++k) {
        const wchar_t* name = k == 0 ? L"OnRetryEncounterResponse" : L"OnReserveTeamResponse";
        const wchar_t* param = k == 0 ? L"ShouldRetry" : L"ShouldSendReserve";
        bus.pre(fn(game::path::BattleMgr, name), [k, param](HookCtx& ctx, UFunction* f) {
            auto* p = static_cast<uint8_t*>(ue::param_address(ctx, f, param));
            const int yes = p ? (*p != 0 ? 1 : 0) : -1;
            auto& d = s.defeat;
            log().write("coop_defeat_response", JsonObj().add("prompt", k).add("yes", yes).add("by_mod", d.applying || game::mod_call_depth() > 0)
                                                    .add("host_choice", d.host_choice[k]).add("local_allowed", d.local_allowed).add("battle", s.battle.id));
            if (s.role == Role::Host) {
                if (!s.defeat_sync || yes < 0) return;
                broadcast(proto::DefeatChoice{s.battle.id, static_cast<uint8_t>(k), static_cast<uint8_t>(yes)});
                bump("defeat_choices_published");
                log().write("coop_defeat_choice_sent", JsonObj().add("battle", s.battle.id).add("prompt", k).add("yes", yes).add("players", connected_players()));
                return;
            }
            if (s.role != Role::Client || !s.defeat_sync) return;
            d.responded[k] = true;
            d.next_poll = 0;
            if (p && d.host_choice[k] >= 0 && yes != d.host_choice[k] && !d.local_allowed) {
                *p = static_cast<uint8_t>(d.host_choice[k]);
                bump("defeat_choices_rewritten");
                log().write("coop_defeat_choice_rewritten", JsonObj().add("prompt", k).add("local", yes).add("host", d.host_choice[k]));
            }
        });
    }
    // Client : clic du joueur d'ici dans une fenêtre de l'écran de défaite (pas le mod, pas le repli « hôte muet ») : fuite.
    bus.pre(L"/Game/UI/Widgets/InGame_Menu/Modal/WBP_ModalPopup.WBP_ModalPopup_C:NotifyUserResponse", [](HookCtx& ctx, UFunction*) {
        auto& d = s.defeat;
        if (s.role != Role::Client || !s.defeat_sync || !d.armed || d.applying || d.local_allowed || game::mod_call_depth() > 0) return;
        if (ue::get_object(ctx.Context, STR("ParentActivatableWidget")) != game::defeat_screen()) return;
        bump("defeat_local_choices");
        log().write("coop_defeat_local_choice", JsonObj().add("prompt", d.prompt_kind).add("modal", ue::short_name(ctx.Context)));
    });
    // « Réessayer / Abandonner » ouverte : la prochaine fenêtre de choix est celle-là (client : refermée à l'image suivante).
    bus.post(std::wstring(game::kGameOverScreenClass) + L":PromptBattleRetry", [](HookCtx&, UFunction*) {
        s.defeat.retry_prompted = true;
        s.defeat.next_poll = 0;
    });
    // Diagnostic : déroulé de la fin de combat perdue (fonctions du jeu, premier paramètre), 80 lignes au plus par défaite.
    {
        const std::wstring screen = std::wstring(game::kGameOverScreenClass) + L":";
        const std::wstring modal = L"/Game/UI/Widgets/InGame_Menu/Modal/WBP_ModalPopup.WBP_ModalPopup_C:";
        const std::wstring bm = game::path::BattleMgr, ctrl = game::path::CtrlWorld;
        const std::pair<std::wstring, const wchar_t*> traced[] = {
            {bm + L"OnAllHeroesKilled", nullptr}, {bm + L"ShowReserveTeamGameOver", nullptr}, {bm + L"OnReserveTeamResponse", L"ShouldSendReserve"},
            {bm + L"OnGameOverScreenClosed", nullptr}, {bm + L"RequestRestartCurrentEncounter", nullptr}, {bm + L"PrepareForEncounterRestart", nullptr},
            {bm + L"LoadRestartState", nullptr}, {bm + L"TriggerAbandonBattle", nullptr}, {bm + L"StartBattleNEW", nullptr},
            {bm + L"SpawnBackupTeam", nullptr}, {bm + L"OnBackupTeamLoaded", nullptr},
            {ctrl + L"RestartCurrentEncounter", nullptr}, {ctrl + L"ResumeExplorationOnBattleEnd", nullptr},
            {screen + L"PromptBattleRetry", nullptr}, {screen + L"OnRetryUserResponse", L"InUserResponse"},
            {screen + L"OnReserveTeamModalResponse", L"InUserResponse"}, {screen + L"DisappearAndRemove", L"FadeBackToBattle"},
            {screen + L"FadeInGameOverScreen", nullptr}, {screen + L"OnUIAction", nullptr}, {screen + L"BP_OnDeactivated", nullptr},
            {modal + L"NotifyUserResponse", L"InUserResponse"},
            {L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:ChangeMap", nullptr},
        };
        for (const auto& t : traced) {
            const std::string name = ue::to_utf8(t.first.substr(t.first.rfind(L':') + 1));
            const wchar_t* param = t.second;
            bus.pre(t.first, [name, param](HookCtx& ctx, UFunction* f) {
                auto& d = s.defeat;
                if (!d.armed && d.host_choice[0] < 0 && d.host_choice[1] < 0) return;   // seulement autour d'une défaite
                if (++d.traces > 80) return;
                int size = 0;
                const auto* v = param ? static_cast<const uint8_t*>(ue::param_address(ctx, f, param, &size)) : nullptr;
                log().write("coop_defeat_trace", JsonObj().add("fn", name).add("arg", v && size >= 1 ? static_cast<int>(*v) : -1)
                                                     .add("self", ue::short_name(ctx.Context)).add("by_mod", game::mod_call_depth() > 0));
            });
        }
    }

    // Fuite : retour à l'exploration sans StartBattleEndFlow (vu le 2026-10-20) -> fin « neutre », même chemin.
    bus.post(fn(game::path::CtrlWorld, L"ResumeExplorationOnBattleEnd"), [](HookCtx&, UFunction*) {
        auto& b = s.battle;
        if (!b.active || b.ending) return;
        b.ending = true;
        b.local_end = game::EndNeutral;
        b.left = true;
        if (s.role == Role::Host) broadcast(proto::BattleEnd{b.id, static_cast<uint8_t>(game::EndNeutral)});
        bump("battles_left");
        log().write("coop_battle_left", JsonObj().add("battle", b.id));
        b.end_flow_at = s.now;
    });
}


// Retour au menu principal par le joueur (menu pause) : la session se termine pour lui. Hôte : tout le monde est prévenu
// (Bye) ; client : l'hôte reprend ses héros.
void subscribe_session()
{
    HookBus::get().pre(game::path::fn(game::path::CtrlWorld, L"BackToTitleMenu"), [](HookCtx&, UFunction*) {
        if (s.role != Role::Solo) coopsave::on_quit_to_title();   // hôte d'une copie : dernière copie
        if (s.role == Role::Solo || s.ended) return;
        log().write("session_quit_to_title", JsonObj().add("role", s.role == Role::Host ? "host" : "client"));
        shutdown(s.role == Role::Host ? "host quit to title" : "client quit to title");
        s.ended = true;
        s.title_requested = true;
        s.ended_at = s.now;
    });
}
// Client : lien vers l'hôte (UDP : -e33connect ; Steam : propriétaire du lobby) et présentation (Hello).
void connect_to_host(const std::string& addr)
{
    s.connect_to = addr;
    Peer host;
    host.link = std::make_unique<net::Link>(*s.transport, s.connect_to);
    host.created_at = s.now;
    host.accepted = true;
    host.player_id = 0;
    wire_link(*host.link);
    // Hôte d'une autre version de protocole : il nous répond avec sa version, on l'affiche.
    host.link->on_protocol_mismatch = [](uint16_t v) {
        const bool first = !s.ended;
        const std::string hv = s.steam && s.lobby.id ? net::steam::lobby_data(s.lobby.id, "mod") : std::string();
        const uint64_t owner = s.lobby.owner;
        end_session("rejected", ui::text("reject_protocol", {std::to_string(v), std::to_string(proto::kProtocolVersion)}));
        if (first) update::on_version_mismatch(v > proto::kProtocolVersion, player_name(0), hv, true, owner);
    };
    s.peers.push_back(std::move(host));
    proto::Hello h;
    h.game_build = s.game_build;
    h.mod_version = kModVersion;
    h.gameplay_mods = s.gameplay_mods;
    h.player_name = s.my_name;
    h.instance_id = GetTickCount64();
    h.join = s.want_join;
    send_to(s.peers.front(), h);
    heartbeat_targets();
    if (s.want_join) ui::set_status("session", "joining");
}

// --- Transport Steam (phase 6) ---------------------------------------------------------------------
// Rien de Steam avant le premier tick (le mod reste inerte les 10 premières secondes). Hôte : lobby « amis seulement »
// créé d'office, F7 ouvre la fenêtre d'invitation de l'overlay ; les membres du lobby sont « ouverts » (punch) pour
// que Steam accepte leurs messages. Client : lobby donné par -e33lobby, par Steam (+connect_lobby, invitation acceptée
// jeu fermé), par une invitation acceptée pendant que le jeu tourne, ou F7 au menu principal (lobby coop d'un ami) ;
// une fois dedans, connexion au propriétaire du lobby (son SteamID) comme en UDP.
void start_session(Role r, const char* why);
void neutral_steam(bool f7);
void back_to_neutral(const char* why);

bool key_pressed(int vk, bool& was_down)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool pressed = down && !was_down && pid == GetCurrentProcessId();
    was_down = down;
    return pressed;
}

void steam_tick()
{
    if (!s.steam) return;
    if (!s.transport) {
        std::string err;
        s.transport = net::steam::make_transport(err);
        if (!s.transport) {
            if (!s.steam_error_shown) {
                s.steam_error_shown = true;
                bump("steam_errors");
                ui::toast("steam_unavailable", {err}, 12.0);
                log().write("coop_steam_error", JsonObj().add("what", err));
            }
            return;
        }
        if (!s.name_from_arg) {
            const std::string pn = net::steam::persona_name();
            if (!pn.empty()) s.my_name = pn;
        }
        log().write("coop_steam_ready", JsonObj().add("steam_id", std::to_string(net::steam::my_id())).add("name", s.my_name)
                                           .add("command_line_lobby", std::to_string(net::steam::command_line_lobby())));
    }
    if (s.steam_selftest && s.now >= s.next_punch) {
        // Auto-test : un message P2P à son propre SteamID par seconde (compté à la réception dans pump_network).
        s.next_punch = s.now + 1.0;
        uint8_t pkt[8] = {'E', '3', '3', 'S'};
        const uint32_t seq = ++s.selftest_seq;
        std::memcpy(pkt + 4, &seq, 4);
        const bool ok = s.transport->send(net::steam::peer_of(net::steam::my_id()), pkt, sizeof pkt);
        bump(ok ? "steam_selftest_sent" : "steam_selftest_send_errors");
    }
    if (!s.steam_lobby) return;
    net::steam::poll_lobby(s.lobby);
    if (s.lobby.state == net::steam::LobbyState::Failed && !s.lobby_error_shown) {
        s.lobby_error_shown = true;
        bump("steam_errors");
        ui::toast("steam_lobby_failed", {s.lobby.error}, 10.0);
        log().write("coop_steam_lobby_failed", JsonObj().add("what", s.lobby.error));
    }
    const bool f7 = key_pressed(VK_F7, s.f7_down);
    if (s.role == Role::Solo) { neutral_steam(f7); return; }   // mode neutre : rôle décidé ici
    if (s.role == Role::Host) {
        if (s.lobby.state == net::steam::LobbyState::None) net::steam::create_lobby(s.lobby, kModVersion, proto::kProtocolVersion);
        if (s.lobby.state == net::steam::LobbyState::Ready && !s.lobby_announced) {
            s.lobby_announced = true;
            ui::toast("session_open_steam", {}, 10.0);
            log().write("coop_steam_lobby", JsonObj().add("lobby", std::to_string(s.lobby.id)));
        }
        if (s.invite_on_ready && s.lobby.state == net::steam::LobbyState::Ready) {   // ouvert par F7 : l'invitation suit
            s.invite_on_ready = false;
            net::steam::invite_dialog(s.lobby);
            bump("steam_invite_dialogs");
        }
        if (f7) {
            if (s.lobby.state == net::steam::LobbyState::Ready) { net::steam::invite_dialog(s.lobby); bump("steam_invite_dialogs"); }
            else if (s.lobby.state == net::steam::LobbyState::Failed) { s.lobby = {}; s.lobby_error_shown = false; }   // nouvel essai
        }
        if (s.now >= s.next_punch && s.lobby.state == net::steam::LobbyState::Ready) {
            s.next_punch = s.now + 1.0;
            const uint64_t me = net::steam::my_id();
            for (uint64_t m : net::steam::lobby_members(s.lobby)) if (m && m != me) net::steam::punch(*s.transport, m);
        }
        (void)net::steam::take_join_request();   // l'hôte n'en a pas l'usage
        return;
    }
    // Client
    if (!s.peers.empty()) {
        if (s.now >= s.next_punch && s.lobby.owner) { s.next_punch = s.now + 1.0; net::steam::punch(*s.transport, s.lobby.owner); }
        return;
    }
    if (s.lobby.state == net::steam::LobbyState::None) {
        uint64_t id = s.lobby_wanted;
        if (!id) id = net::steam::command_line_lobby();
        if (!id) id = net::steam::take_join_request();
        const bool clicked = ui::joinbutton::take_click();   // bouton « Rejoindre <ami> » du menu principal
        if (!id && (f7 || clicked)) {
            if (clicked) log().write("coop_join_button_clicked", JsonObj());
            id = net::steam::find_friend_lobby(kModVersion);
            if (!id) ui::toast("steam_no_friend_lobby", {}, 8.0);
        }
        if (!id) id = remote::auto_join_lobby();   // test à distance : lobby de l'hôte de confiance, au menu principal
        if (id) {
            s.lobby_wanted = 0;
            net::steam::join_lobby(s.lobby, id);
            ui::set_status("session", "joining");
            log().write("coop_steam_join", JsonObj().add("lobby", std::to_string(id)));
        }
        return;
    }
    if (s.lobby.state == net::steam::LobbyState::Failed) {
        s.lobby = {};   // F7 ou une nouvelle invitation pour réessayer
        s.lobby_error_shown = false;
        ui::clear_status("session");
        if (s.runtime_session) back_to_neutral("lobby failed");
        return;
    }
    if (s.lobby.state == net::steam::LobbyState::Ready && s.lobby.owner) {
        net::steam::punch(*s.transport, s.lobby.owner);
        connect_to_host(net::steam::peer_of(s.lobby.owner));
        log().write("coop_steam_connect", JsonObj().add("host", std::to_string(s.lobby.owner)));
    }
}

// --- Mode neutre (D18) -----------------------------------------------------------------------------------------------
// Partie normale sans -e33role : personne n'a de rôle. Au menu principal, une invitation acceptée, +connect_lobby, F7 ou
// le bouton « Rejoindre <ami> » font de ce joueur un client ; dans une partie chargée, F7 l'ouvre au coop (hôte : lobby
// créé alors, fenêtre d'invitation dès qu'il est prêt) ; F10 au menu héberge la copie coop (coopsave). Fin de session
// (retour au menu principal) : retour au mode neutre.
void neutral_steam(bool f7)
{
    if (s.now >= s.next_place_check) {
        s.next_place_check = s.now + 0.5;   // FindFirstOf : pas à chaque image
        const bool menu = game::main_menu() != nullptr && !game::controller();
        const bool world = !menu && game::controller() && game::world_pawn() && !game::map_change_in_progress();
        if (world && !s.ready_hint_shown && !game::in_battle()) {   // partie chargée : rappel de F7, une fois par lancement
            s.ready_hint_shown = true;
            ui::toast("coop_ready_hint", {}, 8.0);
        }
        // Rappel de F7 une seule fois par lancement du jeu (avant : à chaque partie chargée, jugé parasite).
        s.at_menu = menu;
        s.in_world = world;
    }
    uint64_t id = 0;
    const char* why = "";
    if (!s.cmdline_lobby_used) {   // une seule fois : -e33lobby, ou « +connect_lobby » (invitation acceptée jeu fermé)
        s.cmdline_lobby_used = true;
        id = s.lobby_wanted ? s.lobby_wanted : net::steam::command_line_lobby();
        s.lobby_wanted = 0;
        why = "command line lobby";
    }
    if (const uint64_t req = net::steam::take_join_request()) {
        // En pleine partie : rien n'est quitté sans le joueur (sauvegardes) ; la partie de l'ami sera rejointe au menu.
        if (s.in_world) { s.pending_lobby = req; ui::toast("invite_in_game", {}, 10.0); log().write("coop_invite_pending", JsonObj().add("lobby", std::to_string(req))); }
        else { id = req; why = "steam invite"; }
    }
    if (!id && s.pending_lobby && s.at_menu) { id = s.pending_lobby; s.pending_lobby = 0; why = "pending invite"; }
    if (!id && s.rejoin_host && s.at_menu && s.now >= s.next_rejoin_try) {   // relance après une mise à jour
        s.next_rejoin_try = s.now + 3.0;
        if (s.now > s.rejoin_until) s.rejoin_host = 0;
        else if (const uint64_t l = net::steam::friend_lobby(s.rejoin_host)) { id = l; why = "rejoin after update"; s.rejoin_host = 0; }
    }
    const bool clicked = ui::joinbutton::take_click();
    if (!id && ((f7 && s.at_menu) || clicked)) {
        why = clicked ? "join button" : "F7 menu";
        if (clicked) log().write("coop_join_button_clicked", JsonObj());
        id = net::steam::find_friend_lobby(kModVersion);
        if (!id) {   // partie d'un ami avec une autre version : refus clair, puis proposition de mise à jour
            const auto any = net::steam::friend_lobbies(kModVersion, true);
            if (!any.empty()) id = any.front().lobby;
        }
        if (!id && clicked) ui::toast("steam_no_friend_lobby", {}, 8.0);
        if (!id && !clicked) {
            // F7 au menu sans partie d'ami : ouvrir la sienne dès le menu. L'ami rejoint ici, puis la partie lancée
            // (nouvelle partie ensemble avec l'intro, ou sauvegarde chargée) est partagée, comme avec l'ancien -e33role=host.
            start_session(Role::Host, "F7 menu");
            s.invite_on_ready = true;
            bump("coop_opened");
            bump("coop_opened_at_menu");
            ui::toast("coop_opened_menu", {}, 10.0);
            log().write("coop_opened_at_menu", JsonObj());
            return;
        }
    }
    if (id) {
        start_session(Role::Client, why);
        net::steam::join_lobby(s.lobby, id);
        ui::set_status("session", "joining");
        log().write("coop_steam_join", JsonObj().add("lobby", std::to_string(id)).add("why", why));
        return;
    }
    if (f7 && s.in_world) {
        if (game::in_battle()) { ui::toast("open_not_in_battle", {}, 6.0); return; }
        start_session(Role::Host, "F7");
        s.invite_on_ready = true;
        bump("coop_opened");
    }
}

// Session ouverte en jeu terminée, joueur revenu au menu principal : tout ce qui dépend du rôle est remis à zéro. Les
// hooks restent abonnés (ils ne font rien sans rôle) ; le transport Steam est gardé.
void back_to_neutral(const char* why)
{
    log().write("coop_back_to_neutral", JsonObj().add("why", why ? why : "").add("role", s.role == Role::Host ? "host" : s.role == Role::Client ? "client" : "solo"));
    if (s.steam) net::steam::leave_lobby(s.lobby);
    s.peers.clear();
    heartbeat_targets();
    explore::reset();
    builds::reset();
    presence::reset();
    coopsave::set_host(false);
    s.role = Role::Solo;
    s.runtime_session = false;
    s.my_player_id = 0;
    s.session_id = GetTickCount();
    s.connect_to.clear();
    s.accepted = s.rejected = false;
    s.battle = Battle{};
    s.owner_overrides = s.owner_overrides_arg;
    s.pending_start.reset();
    s.last_start.reset();
    s.early_start.reset();
    s.early_start_at = -1;
    s.world_ready_s = 0;
    s.next_trigger_try = 0;
    s.imposing = false;
    s.want_join = false;
    s.input_enabled = true;
    s.input_sub = nullptr;
    s.removed_contexts.clear();
    s.fa_own = FreeAimOwner{};
    s.fa_rep = FreeAimReplay{};
    s.local_aim.reset();
    s.roster.clear();
    s.ended = false;
    s.end_key.clear();
    s.ended_at = -1;
    s.title_requested = false;
    s.hello_at = 0;
    s.takeover = false;
    s.version_rejected.clear();
    s.tombstones.clear();
    s.lobby = {};
    s.lobby_announced = s.lobby_error_shown = false;
    s.next_punch = 0;
    s.invite_on_ready = false;
    s.keys_shown = false;
    s.in_world = false;
    for (const char* slot : {"turn", "pause", "wait", "waited", "peer", "keys", "invite"}) ui::clear_status(slot);
    if (ui::statuses().count("session")) s.session_msg_until = s.now + 12.0;   // message de fin encore lisible un moment
    bump("back_to_neutral");
}

// Options changées en jeu (options.cpp) : réglages d'hôte relus ; l'attente sans blocage seulement hors combat.
void apply_options()
{
    if (!s.options_dirty) return;
    s.duo_parry = arg(L"e33duoparry") != L"0";
    const std::wstring t = arg(L"e33turntimer");
    s.turn_timer_s = static_cast<uint16_t>(t.empty() ? 0 : std::wcstoul(t.c_str(), nullptr, 10));
    if (s.battle.active) return;
    s.turn_dep = arg(L"e33turndep") != L"0";
    s.turn_align = arg(L"e33turnalign") != L"0";
    s.options_dirty = false;
    log().write("coop_options", JsonObj().add("duo_parry", s.duo_parry).add("turn_timer_s", static_cast<int>(s.turn_timer_s)).add("turndep", s.turn_dep)
                                   .add("turnalign", s.turn_align));
}
} // namespace

void reassign_character(size_t index) { if (s.role == Role::Host) cycle_owner(index); }
bool neutral() { return s.neutral && s.role == Role::Solo; }
bool ready() { return s.neutral || s.role != Role::Solo; }
void reload_options() { s.options_dirty = true; }

Role role() { return s.role; }
bool active() { return s.role != Role::Solo; }
int connected_players()
{
    if (s.role == Role::Client) return s.accepted && !s.rejected ? 1 : 0;
    int n = 0;
    for (auto& p : s.peers) if (p.accepted) ++n;
    return n;
}
bool session_ok() { return s.role == Role::Solo || (s.role == Role::Host ? true : (s.accepted && !s.rejected)); }
bool battle_finalized() { return s.battle.finalized; }
// Essai -e33turndep : début de tour retenu tant que l'état de l'hôte n'est pas là (le joueur n'a pas encore le menu ;
// le testeur, qui agit directement, attend aussi).
bool holding() { return s.battle.hold || s.battle.dep_active || s.battle.dep_finish_pending; }
bool session_ended() { return s.ended; }
int defeat_prompt_kind() { return s.defeat.retry_prompted ? 0 : 1; }
bool action_forced(int spot) { return s.battle.forced.count({s.battle.turn, spot}) > 0; }
int remote_turn_open()
{
    if (s.role == Role::Solo || !s.battle.active || s.battle.ending) return -1;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur) || ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return -1;
    const int spot = game::spot(cur);
    if (controls(spot) || s.battle.executed.count({s.battle.turn, spot})) return -1;
    return spot;
}
std::string name_of(uint8_t player) { return player_name(player); }
uint8_t my_player() { return s.my_player_id; }
uint8_t spot_owner(int spot) { return owner_of_spot(spot); }
uint8_t hero_owner(const std::string& name)
{
    if (s.battle.active)
        for (UObject* h : game::players())
            if (game::hardcoded_name(h) == name) return owner_of_spot(game::spot(h));
    return character_owner(name);
}
std::vector<std::pair<uint8_t, std::string>> players()
{
    std::vector<std::pair<uint8_t, std::string>> out;
    for (auto& [id, n] : s.roster) out.emplace_back(id, n.empty() ? player_name(id) : n);
    if (out.empty() || out.front().first != 0) out.insert(out.begin(), {uint8_t{0}, s.role == Role::Host ? s.my_name : player_name(0)});
    return out;
}
bool steam_transport() { return s.steam; }
uint64_t host_lobby()
{
    return s.role == Role::Host && s.steam && s.lobby.state == net::steam::LobbyState::Ready ? s.lobby.id : 0;
}
bool host_lobby_has(uint64_t steam_id)
{
    if (!host_lobby()) return false;
    for (uint64_t m : net::steam::lobby_members(s.lobby)) if (m == steam_id) return true;
    return false;
}
void shutdown(const char* reason)
{
    if (s.role == Role::Solo) return;
    for (auto& p : s.peers) if (p.accepted) send_to(p, proto::Bye{reason ? reason : ""});
    for (int i = 0; i < 3; ++i) {
        for (auto& p : s.peers) if (p.link) p.link->update(s.now + 0.3 * (i + 1));
        Sleep(30);
    }
    if (s.steam) net::steam::leave_lobby(s.lobby);
    log().write("session_shutdown", JsonObj().add("reason", reason ? reason : ""));
}
void set_local_aim(const double* point)
{
    if (point) s.local_aim = std::array<double, 3>{point[0], point[1], point[2]};
    else s.local_aim.reset();
}
const std::map<std::string, int>& counters() { return s.counters; }
const char* mod_version() { return kModVersion; }
int counter(const std::string& name)
{
    auto it = s.counters.find(name);
    return it == s.counters.end() ? 0 : it->second;
}

bool controls_spot(int spot)
{
    if (s.role == Role::Solo) return true;
    if (!s.battle.active) return s.role == Role::Host;
    return controls(spot);
}

namespace {

// Réglages lus une fois au démarrage (indépendants du rôle).
void read_settings()
{
    for (const char* c : {"desyncs", "raw_hash_diffs", "hashes_compared", "gameplay_draws", "draw_mismatches", "draw_count_diffs", "draw_errors", "invalid_messages",
                          "session_ended", "players_left", "heroes_taken_over", "peer_timeouts", "sessions_rejected", "battle_ready_timeouts", "turn_timer_expired",
                          "defense_timeouts", "combo_timeouts", "turn_wait_timeouts", "actor_mismatches", "end_mismatches", "rewards_mismatches",
                          "repair_failures", "defense_verdict_mismatches", "combo_verdict_mismatches", "remote_actions",
                          "setup_draws_replaced", "setup_draws_natural", "start_mech_diffs", "raw_mech_diffs"})
        s.counters[c] = 0;

    s.game_build = read_build_id();
    s.gameplay_mods = read_gameplay_mods();
    s.session_id = GetTickCount();
    if (const std::wstring seed = arg(L"e33seed"); !seed.empty()) s.fixed_seed = std::wcstoull(seed.c_str(), nullptr, 0);
    s.setup_draws = arg(L"e33setupdraws") != L"0";
    s.defeat_sync = arg(L"e33defeatsync") != L"0";
    s.defeat_abandon_resync = arg(L"e33defeatabandon") == L"resync";
    if (const std::wstring d = arg(L"e33actiondelay"); !d.empty()) s.action_delay_s = std::wcstod(d.c_str(), nullptr);
    if (const std::wstring p = arg(L"e33perturb"); !p.empty()) s.perturb_turn = static_cast<uint32_t>(std::wcstoul(p.c_str(), nullptr, 10));
    if (const std::wstring p = arg(L"e33perturbend"); !p.empty()) s.perturb_end_turn = static_cast<uint32_t>(std::wcstoul(p.c_str(), nullptr, 10));
    if (arg(L"e33lockinput") == L"0") s.lock_input = false;
    if (const std::wstring l = arg(L"e33replaylag"); !l.empty()) s.replay_lag_s = std::wcstod(l.c_str(), nullptr);
    s.turn_dep = arg(L"e33turndep") != L"0";   // attente sans figer le jeu, par défaut depuis le 2026-10-22 (-e33turndep=0 : attente bloquante)
    if (const std::wstring t = arg(L"e33turntimer"); !t.empty()) s.turn_timer_s = static_cast<uint16_t>(std::wcstoul(t.c_str(), nullptr, 10));
    s.duo_parry = arg(L"e33duoparry") != L"0";
    if (const std::wstring v = arg(L"e33readyidle"); !v.empty()) s.ready_idle_s = std::wcstod(v.c_str(), nullptr);
    if (const std::wstring v = arg(L"e33stuckturn"); !v.empty()) s.stuck_turn_s = std::wcstod(v.c_str(), nullptr);
    s.turn_align = arg(L"e33turnalign") != L"0";   // démarrage synchronisé des tours ennemis, par défaut depuis le 2026-10-24 (D28)
    s.my_name = ue::to_utf8(arg(L"e33name"));
    s.name_from_arg = !s.my_name.empty();
    ui::init();
    {
        std::stringstream ds(ue::to_utf8(arg(L"e33draws")));
        std::string part;
        while (std::getline(ds, part, ',')) if (!part.empty()) s.draw_filter.push_back(part);
    }
    {
        std::stringstream os(ue::to_utf8(arg(L"e33owners")));
        std::string part;
        while (std::getline(os, part, ',')) {
            const auto c = part.find(':');
            if (c != std::string::npos) s.owner_overrides[part.substr(0, c)] = static_cast<uint8_t>(std::atoi(part.substr(c + 1).c_str()));
        }
        s.owner_overrides_arg = s.owner_overrides;
    }
    if (const std::wstring l = arg(L"e33lobby"); !l.empty()) s.lobby_wanted = std::wcstoull(l.c_str(), nullptr, 10);
    if (arg(L"e33steamlobby") == L"0") s.steam_lobby = false;
    if (arg(L"e33steamselftest") == L"1") s.steam_selftest = true;
}

void coopsave_init(bool host)
{
    // Mode neutre : F10 au menu principal fait de ce joueur l'hôte de la copie (become_host).
    std::function<bool()> become;
    if (s.neutral) become = [] {
        if (s.role == Role::Host) return true;
        if (s.role != Role::Solo) return false;
        start_session(Role::Host, "F10 copy");
        return s.role == Role::Host;
    };
    coopsave::init(host, [](const char* c, int n) { s.counters[c] += n; }, become);
}

// Rôle décidé (option de lancement, ou en jeu en mode neutre) : modules initialisés pour ce rôle, hooks abonnés une
// fois. En mode neutre, les hooks du coop ne sont abonnés qu'ici (partie solo sans session : rien d'accroché).
void start_session(Role r, const char* why)
{
    s.role = r;
    s.my_player_id = 0;
    s.runtime_session = s.neutral;
    if (r == Role::Client && s.neutral) s.want_join = true;   // en jeu, un client rejoint toujours la partie de l'hôte
    if (!s.name_from_arg && (s.my_name.empty() || s.my_name == "Hôte" || s.my_name == "Joueur")) {
        const std::string pn = s.steam && net::steam::loaded() ? net::steam::persona_name() : std::string();
        s.my_name = !pn.empty() ? pn : r == Role::Host ? "Hôte" : "Joueur";
    }
    static bool recorder_open = false;
    if (!recorder_open) {
        recorder_open = true;
        s.recorder.open(ue::to_utf8((std::filesystem::path(launch_args().run_dir) / (r == Role::Host ? L"recording-host.jsonl" : L"recording-client.jsonl")).wstring()));
    }
    log().set_role(r == Role::Host ? "host" : "client");

    std::string mods = "[";
    for (auto& m : s.gameplay_mods) { if (mods.size() > 1) mods += ","; mods += "\"" + json_escape(m) + "\""; }
    log().write("coop_init", JsonObj().add("role", r == Role::Host ? "host" : "client").add("local", s.transport ? s.transport->local_address() : std::string("steam (pending)"))
                                 .add("game_build", s.game_build).add("mod_version", kModVersion).add("duo_parry", s.duo_parry).raw("gameplay_mods", mods + "]")
                                 .add("why", why ? why : "").add("runtime", s.runtime_session));

    if (r == Role::Client && !s.steam) connect_to_host(ue::to_utf8(arg(L"e33connect")));
    explore::Net net;
    net.publish = [](const proto::Message& m, bool reliable) { publish(m, nullptr, reliable); };
    net.send_to = [](uint8_t player, const proto::Message& m) { for (auto& p : s.peers) if (p.accepted && p.player_id == player) send_to(p, m); };
    net.relay = [](const proto::Message& m, uint8_t origin, bool reliable) { for (auto& p : s.peers) if (p.accepted && p.player_id != origin) send_to(p, m, reliable); };
    net.bump = [](const char* c, int n) { s.counters[c] += n; };
    net.name_of = [](uint8_t id) { return player_name(id); };
    net.battle_engaged = [] { return s.pending_start.has_value() || (s.battle.active && !s.battle.finalized); };
    explore::init(r == Role::Host, s.want_join, net);
    {
        builds::Net bn;
        bn.publish = [](const proto::Message& m) { publish(m); };
        bn.send_to = [](uint8_t player, const proto::Message& m) { for (auto& p : s.peers) if (p.accepted && p.player_id == player) send_to(p, m); };
        bn.relay = [](const proto::Message& m, uint8_t origin) { for (auto& p : s.peers) if (in_game(p) && p.player_id != origin) send_to(p, m); };
        bn.bump = [](const char* c, int n) { s.counters[c] += n; };
        bn.name_of = [](uint8_t id) { return player_name(id); };
        bn.owner_of = [](const std::string& n) { return character_owner(n); };
        bn.sharing = [] { if (s.role == Role::Client) return explore::joined() && s.accepted; for (auto& p : s.peers) if (in_game(p)) return true; return false; };
        bn.last_event_age = [] { return explore::last_event_age(); };
        bn.request_resync = [](const std::string& why) { publish(proto::ResyncRequest{why}); };
        builds::init(r == Role::Host, bn);
    }
    if (s.neutral) coopsave::set_host(r == Role::Host);   // déjà initialisé au démarrage (F10 peut appeler d'ici)
    else coopsave_init(r == Role::Host);
    {
        intent::Net in;
        in.publish = [](const proto::Message& m, bool reliable) { publish(m, nullptr, reliable); };
        in.bump = [](const char* c, int n) { s.counters[c] += n; };
        in.name_of = [](uint8_t id) { return player_name(id); };
        in.owner_of_spot = [](int spot) { return owner_of_spot(spot); };
        in.controls = [](int spot) { return controls(spot); };
        in.action_known = [](uint32_t turn, int spot) { return s.battle.commands.count({turn, spot}) > 0 || s.battle.executed.count({turn, spot}) > 0; };
        in.local_aiming = [] { return s.fa_own.active; };
        in.battle = [] { return intent::BattleRef{s.battle.id, s.battle.turn, s.battle.active}; };
        in.remote_aim_active = [] { return s.battle.active && s.fa_rep.started && !s.fa_rep.done && s.fa_rep.key.first == s.battle.turn; };
        in.paused = [] { return s.battle.hold || explore::remote_paused() || game::pause_menu_visible(); };
        intent::init(in);
    }
    if (!s.hooks_subscribed) {
        s.hooks_subscribed = true;
        subscribe();
        intent::subscribe();
        explore::subscribe();
        if (!s.neutral) {   // mode neutre : déjà abonnés au démarrage
            coopsave::subscribe();
            subscribe_session();
            if (r == Role::Client && s.steam) ui::joinbutton::subscribe();   // bouton « Rejoindre <ami> » du menu principal
            ui::joinbutton::subscribe_version();   // version du mod au menu principal
        } else {
            HookBus::get().try_register();   // en pleine partie : classes déjà chargées, accrochées tout de suite
        }
    }
}

} // namespace

void init()
{
    std::wstring r = arg(L"e33role");
    // Test à distance sans options de lancement : client Steam qui rejoint la partie de l'hôte de confiance.
    const bool remote_default = r.empty() && !cmdline::test_mode() && remote::client_enabled();
    if (remote_default) r = L"client";
    Role role = Role::Solo;
    if (r == L"host") role = Role::Host;
    else if (r == L"client") role = Role::Client;
    // En test sans rôle : solo pur, comme avant (aucune lecture, aucun hook).
    if (role == Role::Solo && cmdline::test_mode()) return;

    read_settings();
    update::init();
    if (role == Role::Solo) {
        // Partie normale sans option -e33role : prêt au coop par Steam ; le rôle se décide en jeu (D18).
        s.neutral = true;
        s.steam = true;
        s.rejoin_host = update::take_rejoin();
        s.rejoin_until = s.rejoin_host ? 180.0 : 0;
        coopsave_init(false);
        coopsave::subscribe();
        subscribe_session();
        ui::joinbutton::subscribe();
        ui::joinbutton::subscribe_version();   // version du mod au menu principal
        log().write("coop_neutral", JsonObj().add("mod_version", kModVersion).add("rejoin", std::to_string(s.rejoin_host)));
        return;
    }
    if (role == Role::Client && (arg(L"e33join") == L"1" || remote_default)) s.want_join = true;
    std::string err;
    const std::wstring port_s = arg(L"e33port");
    const uint16_t port = static_cast<uint16_t>(port_s.empty() ? (role == Role::Host ? 7777 : 0) : std::stoi(port_s));
    s.steam = arg(L"e33transport") == L"steam" || remote_default;
    if (!s.steam) {
        s.transport = net::make_udp(port, err);
        if (!s.transport) { log().write("coop_error", JsonObj().add("what", err)); return; }
    }
    // Steam : transport et lobby créés au premier tick (rien dans les 10 premières secondes, voir dllmain), steam_tick.
    start_session(role, "launch option");
}

// Client : lance le combat reçu de l'hôte quand le monde local est chargé et stable depuis quelques secondes.
void start_pending_battle()
{
    if (!s.pending_start || s.now < s.next_trigger_try) return;
    constexpr double kStep = 0.5;   // FindFirstOf parcourt tous les objets : pas à chaque image
    s.next_trigger_try = s.now + kStep;
    // Pas d'appel au jeu (ProcessEvent sur l'instance de jeu) avant qu'un pion d'exploration existe : au démarrage
    // (écran d'avertissement), ces appels ont figé le jeu entier (4 runs clients sur ~20, 2026-10-19).
    if (!game::world_pawn()) { s.world_ready_s = 0; return; }
    const bool ready = game::controller() && !game::map_change_in_progress() && !game::main_menu();
    // Menu du jeu ouvert ici (équipe, pictos, pause : jeu en pause) : lancer le combat dedans a figé le jeu de TOTOFFF
    // (2026-10-24, Lune déformée dans l'écran des pictos). On attend sa fermeture, avec un message.
    const bool in_menu = game::pause_menu_visible();   // pas IsGamePaused : la barrière narrative met aussi le jeu en pause
    if (in_menu) {
        ui::set_status("battle_menu", "battle_close_menu");
        if (!s.menu_wait_logged) { s.menu_wait_logged = true; bump("battle_starts_waiting_menu"); log().write("coop_battle_wait_menu", JsonObj()); }
        s.world_ready_s = 0;
        return;
    }
    if (s.menu_wait_logged) { s.menu_wait_logged = false; ui::clear_status("battle_menu"); }
    s.world_ready_s = ready ? s.world_ready_s + kStep : 0;
    if (s.world_ready_s < 5.0) return;
    const proto::BattleStart bs = *s.pending_start;
    std::string err;
    bool started = game::in_battle();
    const bool already = started;
    if (started) {
        // Notre pion a déjà engagé un combat (il suit le pilote) : ce doit être le même.
        std::string enc, gid;
        int engage = 0;
        if (game::battle_params(enc, gid, engage) && enc != bs.encounter) {
            bump("battle_encounter_mismatches");
            log().write("coop_battle_encounter_mismatch", JsonObj().add("local", enc).add("host", bs.encounter));
        }
    } else if (!bs.world_enemy.empty()) {
        if (UObject* enemy = game::find_world_enemy(bs.world_enemy)) started = game::start_world_battle(enemy, ue::from_utf8(bs.encounter), bs.engage, err);
        else err = "world enemy not found: " + bs.world_enemy;
        if (!started) log().write("coop_error", JsonObj().add("what", "start_world_battle: " + err));
        else bump("world_battles");
    }
    if (!started && !game::trigger_battle(ue::from_utf8(bs.encounter), party_wide(bs.party), err)) {
        log().write("coop_error", JsonObj().add("what", "trigger_battle: " + err));
        s.next_trigger_try = s.now + 2.0;
        return;
    }
    s.pending_start.reset();
    s.battle.active = true;
    log().write("coop_battle_start", JsonObj().add("battle", bs.battle_id).add("encounter", bs.encounter).add("already_engaged", already));
    // Combat engagé ici avant le BattleStart (notre pion suit le pilote) : son premier tour a pu commencer sans le mod.
    // Sans rattrapage, le BattleReady ne part jamais et l'hôte reste figé en attente (vu le 2026-10-20, 2e combat).
    if (UObject* cur = game::current_character(); already && cur && s.battle.turn == 0 && ue::get_bool(cur, STR("IsTurnActive"))) {
        bump("turn_start_catchups");
        // Vérification différée : la photo du vrai début de ce tour, reportée sur l'état de l'hôte (shift_host_state).
        const bool snap = s.early_turn && s.early_turn_actor == cur && s.now - s.early_turn_at < 60.0;
        if (snap) {
            s.battle.dep_start = *s.early_turn;
            s.battle.dep_start.battle_id = s.battle.id;
            s.battle.dep_start.turn = 1;
        }
        log().write("coop_turn_start_catchup", JsonObj().add("actor", game::char_class(cur)).add("turn_start_snapshot", snap)
                                                   .add("snapshot_age_s", snap ? s.now - s.early_turn_at : -1.0));
        s.early_turn.reset();
        on_turn_start(cur);
    }
}

// Visée libre d'un héros d'un autre joueur : rejouée au même rythme que chez lui (décalée de replay_lag_s).
// Renvoie true tant qu'elle est en cours (l'action du tour attend).
bool replay_free_aim(const std::pair<uint32_t, int>& key, UObject* hero)
{
    auto& b = s.battle;
    auto ev = b.fa_events.find(key);
    if (ev == b.fa_events.end() || ev->second.empty()) return false;
    auto& r = s.fa_rep;
    if (r.key != key) r = FreeAimReplay{key};
    if (r.done) return false;
    if (!r.started) {
        if (s.now - b.turn_started_at < s.action_delay_s || s.now - b.fa_arrival[key] < s.replay_lag_s) return true;
        if (!intent::mirror_release(s.now)) return true;   // menus rejoués refermés d'abord (état du début du tour)
        game::ModCall mod;
        ue::Call c(game::battle_manager(), STR("StartFreeAimFromUI"));
        if (c) c.invoke();
        // Le joueur d'ici ne doit ni orienter la caméra ni tirer pour ce héros (vu par Killian le 2026-10-19).
        // DisableFreeAimControls coupe la souris/le stick et le bouton de tir ; nos tirs (OnFreeAimShootAction) et
        // l'annulation (CancelFreeAimFromUI) passent toujours (vérifié en jeu le 2026-10-19).
        ue::Call off(game::battle_manager(), STR("DisableFreeAimControls"));
        if (off) { off.invoke(); bump("free_aim_controls_locked"); }
        r.started = true;
        r.t0 = SteadyClock::now();
        bump("free_aims_replayed");
        log().write("coop_free_aim_start", JsonObj().add("spot", key.second), b.turn);
        return true;
    }
    const auto elapsed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - r.t0).count());
    const auto& events = ev->second;
    // Point visé (affichage) : le plus récent déjà atteint.
    const proto::FreeAimEvent* aim = nullptr;
    for (auto& e : events) if (e.kind == proto::FreeAimKind::Aim && e.t_ms <= elapsed && (!aim || e.t_ms >= aim->t_ms)) aim = &e;
    if (aim && aim->target.spot == proto::kAutoSpot) { r.point = {aim->x, aim->y, aim->z}; r.have_override = true; }
    else if (aim) {   // point relatif à un combattant (faux client)
        double a[3];
        if (game::location(game::find_unit(aim->target.kind == proto::ActorKind::Hero, aim->target.spot), a)) {
            r.point = {a[0] + aim->x, a[1] + aim->y, a[2] + aim->z};
            r.have_override = true;
        }
    }
    // Événements fiables, dans l'ordre.
    game::ModCall mod;   // tirs et fin de la visée rejoués : appels du mod, pas des entrées d'ici
    while (r.next < events.size() && events[r.next].t_ms <= elapsed) {
        const auto& e = events[r.next++];
        if (e.kind == proto::FreeAimKind::Fire) {
            ue::Call c(ue::get_object(hero, STR("BP_FreeAimShooterBattleComponent")), STR("OnFreeAimShootAction"));
            if (c) c.invoke();
        } else if (e.kind == proto::FreeAimKind::Cancel || e.kind == proto::FreeAimKind::End) {
            // Fin chez le propriétaire : si la visée est encore ouverte ici, on l'annule pareil.
            if (ue::get_bool(ue::get_object(hero, STR("BP_FreeAimShooterBattleComponent")), STR("IsAiming"))) {
                ue::Call c(game::battle_manager(), STR("CancelFreeAimFromUI"));
                if (c) c.invoke();
            }
            r.done = true;
            r.have_override = false;
            bump("free_aim_replays_ended");
            log().write("coop_free_aim_end", JsonObj().add("spot", key.second).add("kind", static_cast<int>(e.kind)).add("raycasts", static_cast<int>(r.raycasts)), b.turn);
            return true;
        }
    }
    // Filet : le propriétaire a déjà choisi une autre action et tous ses événements de visée sont joués, sans fin
    // reçue (ancienne version, chemin de sortie inconnu) : on ferme la visée ici pour ne pas bloquer son action.
    if (r.next >= events.size() && b.commands.count(key)) {
        if (ue::get_bool(ue::get_object(hero, STR("BP_FreeAimShooterBattleComponent")), STR("IsAiming"))) {
            ue::Call c(game::battle_manager(), STR("CancelFreeAimFromUI"));
            if (c) c.invoke();
        }
        r.done = true;
        r.have_override = false;
        bump("free_aim_closed_by_action");
        bump("free_aim_replays_ended");
        log().write("coop_free_aim_end", JsonObj().add("spot", key.second).add("kind", -1).add("raycasts", static_cast<int>(r.raycasts)), b.turn);
    }
    return true;
}

// Tour d'un héros contrôlé par un autre joueur : exécuter sa commande (reçue de lui ou relayée par l'hôte).
void run_remote_action()
{
    auto& b = s.battle;
    if (!b.active || b.ending) return;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur)) return;
    const int spot = game::spot(cur);
    const std::pair<uint32_t, int> key{b.turn, spot};
    // Héros d'ici : le joueur agit lui-même, sauf action imposée par l'hôte (minuteur de tour écoulé).
    if (controls(spot) && !b.forced.count(key)) return;
    if (b.executed.count(key) || b.hold) return;
    if (replay_free_aim(key, cur)) return;   // visée libre du propriétaire d'abord (elle ne termine pas le tour)
    auto it = b.commands.find(key);
    if (it == b.commands.end()) {
        // Hôte : attaque automatique imposée au héros d'un autre joueur et diffusée (il la jouera en arrivant à ce tour).
        auto force_attack = [&] {
            proto::ActionCommand ac;
            ac.battle_id = b.id;
            ac.turn = b.turn;
            ac.actor_spot = static_cast<uint8_t>(spot);
            ac.action_type = game::ActAttack;
            ac.targets = {{proto::ActorKind::Enemy, proto::kAutoSpot}};
            ac.forced = true;
            b.forced.insert(key);
            b.commands[key] = ac;
            b.command_time[key] = s.now - s.replay_lag_s;   // pas de décalage : personne n'attend ce QTE
            broadcast(ac);
        };
        const bool running = !game::pause_menu_visible() && !explore::remote_paused();
        // Minuteur de tour (option, désactivé par défaut) : l'hôte impose une attaque automatique au héros du joueur muet.
        if (s.role == Role::Host && b.turn_timer_s > 0 && running) {
            const double left = b.turn_timer_s - b.timer_elapsed;
            ui::set_status("turn", "turn_timer", {player_name(owner_of_spot(spot)), std::to_string(static_cast<int>(std::max(0.0, std::ceil(left))))});
            if (left <= 0) {
                force_attack();
                bump("turn_timer_expired");
                ui::toast("turn_forced", {player_name(owner_of_spot(spot))});
                log().write("coop_turn_timer_expired", JsonObj().add("spot", spot).add("timer_s", static_cast<int>(b.turn_timer_s)), b.turn);
                return;
            }
        }
        // Tour bloqué (sans minuteur) : preuve positive que le jeu du propriétaire n'est pas dans ce tour : lien vivant,
        // mais aucun signe de ce tour venu de lui (ni StateHash, ni menu, aperçu, visée, action) depuis stuck_turn_s de
        // temps de jeu (pauses exclues), sans raison visible (chargement, reconnexion : on attend). Un joueur qui réfléchit,
        // fouille ses menus ou lit une description a déjà envoyé son StateHash au début du tour : jamais concerné, même
        // longtemps. Attaque automatique, et ses défenses / QTE ne sont plus attendus tant qu'il ne répond pas.
        if (s.role == Role::Host && running && !controls(spot)) {
            const uint8_t owner = owner_of_spot(spot);
            const auto act = presence::activity(owner, s.now);
            const bool reached = b.acks.count(b.turn) && b.acks[b.turn].count(owner);
            if (!reached && b.timer_elapsed >= s.stuck_turn_s && act != presence::Activity::Loading && act != presence::Activity::Joining &&
                act != presence::Activity::Reconnecting) {
                force_attack();
                b.unresponsive.insert(owner);
                bump("turn_stuck_forced");
                ui::toast("turn_forced_stuck", {player_name(owner)});
                log().write("coop_turn_stuck", JsonObj().add("spot", spot).add("player", static_cast<int>(owner)).add("waited_s", b.timer_elapsed)
                                                   .add("activity", presence::activity_name(act)), b.turn);
                return;
            }
        }
        if (!b.action_wait_logged && s.now - b.turn_started_at > 30.0) {
            b.action_wait_logged = true;
            bump("action_waits_long");
            log().write("coop_action_wait", JsonObj().add("spot", spot).add("waited_s", s.now - b.turn_started_at), b.turn);
        }
        return;
    }
    if (s.now - b.turn_started_at < s.action_delay_s) return;
    if (s.now - b.command_time[key] < s.replay_lag_s) return;
    if (explore::remote_paused() || game::pause_menu_visible()) return;   // jeu en pause : l'action attend la reprise
    // Menus du propriétaire rejoués ici (miroir) : refermés d'abord, l'action part du même état qu'avant le miroir.
    // Clé de l'action : fermeture dans la même image que l'exécution (D29).
    if (!controls(spot) && !intent::mirror_release(s.now, std::to_string(it->second.action_type) + "|" + it->second.secondary)) return;
    const proto::ActionCommand cmd = it->second;
    b.executed.insert(key);
    if (controls(spot)) set_local_input(false, "forced action");   // action imposée sur un héros d'ici : le joueur ne joue plus ce tour
    std::vector<UObject*> targets;
    bool auto_target = cmd.targets.empty();
    for (auto& t : cmd.targets) {
        if (t.spot == proto::kAutoSpot) { auto_target = true; continue; }
        if (UObject* o = game::find_unit(t.kind == proto::ActorKind::Hero, t.spot)) targets.push_back(o);
    }
    const std::wstring secondary = ue::from_utf8(cmd.secondary);
    // Une commande que le jeu du propriétaire n'aurait pas permise (objet épuisé, PA insuffisants) signale un écart.
    {
        int targeting = -1, cost = 0;
        const bool no_item = cmd.action_type == game::ActItem && game::battle_item_count(secondary) <= 0;
        const double ap = ue::get_double(game::stats(cur), STR("CurrentAP"));
        const bool no_ap = cmd.action_type == game::ActSkill && game::skill_info(cur, secondary, targeting, cost) && ap < cost;
        if (no_item || no_ap) {
            bump("remote_action_invalid");
            log().write("coop_remote_action_invalid", JsonObj().add("spot", spot).add("secondary", cmd.secondary).add("no_item", no_item).add("no_ap", no_ap)
                                                         .add("ap", ap).add("cost", cost).add("hero", game::char_class(cur)), b.turn);
        }
    }
    if (auto_target) targets = game::auto_targets(cur, cmd.action_type, secondary);
    std::string err;
    // Pas de réussite automatique des QTE : chaque fenêtre reçoit le résultat du propriétaire.
    if (!game::perform_action(cmd.action_type, secondary, targets, 0, err)) {
        bump("remote_action_errors");
        log().write("coop_error", JsonObj().add("what", "perform_action: " + err), b.turn);
        return;
    }
    bump("remote_actions");
    std::string ts = "[";
    for (UObject* t : targets) { if (ts.size() > 1) ts += ","; ts += "\"" + game::char_class(t) + "#" + std::to_string(game::spot(t)) + "\""; }
    log().write("coop_remote_action", JsonObj().add("spot", spot).add("action", static_cast<int>(cmd.action_type)).add("secondary", cmd.secondary)
                                         .raw("targets", ts + "]").add("auto_target", auto_target), b.turn);
}

// Diagnostic (2026-10-24 : parade du client sans effet) : à chaque image en combat, apparition / disparition des
// contextes de défense, de blocage et de visée libre, et du drapeau IsDefenseInputBound. Comparer hôte et client.
static void defense_context_watch()
{
    static int logs = 0;
    static int last[4] = {-2, -2, -2, -2};   // -1 absent, sinon priorité ; dernier : drapeau bound
    if (!s.battle.active || s.role == Role::Solo) { last[0] = last[1] = last[2] = last[3] = -2; return; }
    UObject* sub = game::input_subsystem();
    if (!sub) return;
    static const wchar_t* names[] = {L"IMC_BattleDefense", L"IMC_Battle_InputBlocker", L"IMC_Battle_FreeAim"};
    static UObject* imcs[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (!imcs[i]) imcs[i] = ue::find_object((std::wstring(L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/") + names[i] + L"." + names[i]).c_str());
        if (!imcs[i]) continue;
        ue::Call has(sub, STR("HasMappingContext"));
        if (!has) return;
        has.set_object(STR("MappingContext"), imcs[i]).invoke();
        const int now = has.out_bool(STR("ReturnValue")) ? static_cast<int>(has.out_int(STR("OutFoundPriority"))) : -1;
        if (now == last[i]) continue;
        last[i] = now;
        if (++logs > 400) continue;
        log().write("coop_imc_change", JsonObj().add("imc", ue::to_utf8(names[i])).add("active", now >= 0).add("priority", now)
                                           .add("input_enabled", s.input_enabled), s.battle.turn);
        // Contexte de défense ouvert : ses touches (une fois), widgets CommonUI activés et type d'entrée (manette ?).
        if (i == 0 && now >= 0) {
            static bool mapped = false;
            JsonObj o;
            if (!mapped) { mapped = true; o.add("mappings", game::imc_mappings(imcs[0])); }
            log().write("coop_defense_ui_state", o.add("input_type", game::common_input_type()).add("widgets", game::active_common_widgets())
                                                    .add("rb", game::active_contexts_with_key("Gamepad_RightShoulder"))
                                                    .add("rt", game::active_contexts_with_key("Gamepad_RightTrigger"))
                                                    .add("e", game::active_contexts_with_key("E")), s.battle.turn);
        }
    }
    UObject* ctrl = game::controller();
    const int bound = ctrl ? (ue::get_bool(ctrl, STR("IsDefenseInputBound")) ? 1 : 0) : -1;
    if (bound != last[3]) {
        last[3] = bound;
        if (++logs <= 400) log().write("coop_imc_change", JsonObj().add("imc", "IsDefenseInputBound").add("active", bound == 1).add("input_enabled", s.input_enabled), s.battle.turn);
    }
}

// Activité des autres joueurs (écrans d'attente) : chargement (battements « occupé ») et silence du lien, par pair
// direct. Ligne d'information quand un joueur charge sans qu'on l'attende (« TOTOFF charge votre partie… 12 s »).
static void peer_activity_tick()
{
    std::vector<uint8_t> ids;
    for (auto& p : s.peers) {
        if (!p.link || !p.accepted || !p.drop.empty()) continue;
        const double heard = p.link->last_heard();
        presence::set_link(p.player_id, p.link->busy(s.now), heard >= 0 ? s.now - heard : 0.0, s.now);
        ids.push_back(p.player_id);
    }
    // Client pas encore entré : son écran d'attente est déjà la ligne « session ».
    const bool show = !ui::statuses().count("wait") && (s.role == Role::Host || explore::joined());
    ui::set_status_text("peer", "peer_loading", show ? presence::loading_text(ids, s.now, player_name) : std::string());
}

// Diagnostic (essai à deux PC du 2026-10-23) : jeu relancé par un script (relaunch.ps1, installateur) -> aucun salon
// d'ami vu pendant 10 min, alors qu'un lancement à la main le trouve en quelques secondes. Au menu, toutes les 30 s
// tant qu'aucun salon n'est trouvé, puis une fois quand un salon l'est : amis, GetFriendGamePlayed de l'hôte de
// confiance et de l'ami du rejoin.txt, état de persona ; processus (parents, ligne de commande, variables Steam*) au
// premier enregistrement et à celui du salon trouvé, pour comparer lancement à la main et lancement par script.
static void lobby_diag_tick(bool found)
{
    static double next = 0;
    static bool found_logged = false, process_logged = false;
    if (found ? found_logged : s.now < next) return;
    next = s.now + 30.0;
    if (found) found_logged = true;
    std::vector<uint64_t> focus{remote::trusted_host()};
    if (s.rejoin_host && s.rejoin_host != focus.front()) focus.push_back(s.rejoin_host);
    JsonObj o;
    o.add("found", found).add("t", s.now).raw("steam", net::steam::friends_diag_json(focus));
    if (!process_logged || found) o.raw("process", net::steam::process_diag_json());
    process_logged = true;
    log().write("coop_lobby_diag", o);
}

// Client Steam au menu principal : bouton « Rejoindre <ami> » quand un ami a ouvert une partie coop (liste d'amis
// relue toutes les 5 s, seulement au menu). Le clic passe par le même chemin que F7 (steam_tick).
static void join_button_tick()
{
    static double next_menu_check = 0, next_scan = 0;
    static bool at_menu = false;
    static std::string friend_name;
    const bool eligible = (s.role == Role::Client || (s.role == Role::Solo && s.neutral)) && s.steam && s.steam_lobby && s.transport && s.peers.empty() &&
                          s.lobby.state == net::steam::LobbyState::None && !s.lobby_wanted;
    if (!eligible) { friend_name.clear(); at_menu = false; (void)ui::joinbutton::take_click(); }   // pas de clic en attente
    else if (s.now >= next_menu_check) {
        next_menu_check = s.now + 0.5;   // FindFirstOf : pas à chaque image
        at_menu = game::main_menu() != nullptr;
        if (!at_menu) friend_name.clear();
        else if (s.now >= next_scan) {
            next_scan = s.now + 5.0;
            // Mode neutre : aussi la partie d'un ami d'une autre version (le refus dira qui doit mettre à jour).
            auto lobbies = net::steam::friend_lobbies(kModVersion, s.neutral);
            std::stable_sort(lobbies.begin(), lobbies.end(), [](const auto& a, const auto& b) { return a.compatible && !b.compatible; });
            const std::string n = lobbies.empty() ? std::string() : lobbies.front().name;
            if (n != friend_name) log().write("coop_friend_lobby", JsonObj().add("friend", n).add("count", static_cast<int64_t>(lobbies.size())));
            friend_name = n;
            lobby_diag_tick(!lobbies.empty());
        }
    }
    ui::joinbutton::tick(s.now, friend_name);
    // Plan B : bouton impossible à placer dans le menu du jeu -> carte d'invitation (touche F7).
    if (!friend_name.empty() && ui::joinbutton::failed()) ui::set_status("invite", "friend_invite", {friend_name});
    else ui::clear_status("invite");
}

// Client : l'hôte a fini le combat (BattleEnd) mais le jeu d'ici ne l'a pas fini (ennemi encore vivant ici après un écart :
// essai à deux PC du 2026-10-22, Lancelier tué chez l'hôte seulement). L'hôte décide : fin imposée ici, par étapes, chacune
// tentée si la précédente n'a pas lancé la fin du jeu (StartBattleEndFlow -> s.battle.ending) :
//   1. BM.ForceBattleEnd(résultat de l'hôte), dès qu'un tour recommence ici ou 5 s après le BattleEnd (le jeu d'ici finit
//      normalement de lui-même dans ce délai : il n'a que le décalage de rejeu de retard) ;
//   2. BM.StartBattleEndFlow(résultat), 3 s plus tard ;
//   3. victoire : ennemis restants mis à mort (Stats.SetDead), 4 s plus tard ; le jeu conclut à la fin du tour ;
//   4. 10 s plus tard : combat clos pour le mod (entrées rendues) et nouvel instantané demandé à l'hôte (ResyncRequest :
//      il l'envoie hors combat, le client le recharge et sort du combat).
// Les récompenses restent celles de l'hôte (RollBattleRewards) ; la clôture passe par les chemins habituels.
constexpr double kForceEndGrace = 5.0;

void client_force_end_tick()
{
    auto& b = s.battle;
    if (s.role != Role::Client || !b.active || b.finalized || b.host_end < 0 || b.ending || b.end_flow_at >= 0) return;
    if (!game::in_battle()) return;   // déjà sorti du combat : clôture par la règle « host_end » du tick
    if (game::pause_menu_visible()) return;
    const double since = s.now - b.host_end_at;
    auto step_log = [&](const char* what, bool ok, const std::string& how) {
        bump("battle_end_forced_steps");
        log().write("coop_force_end", JsonObj().add("battle", b.id).add("step", b.force_step).add("what", what).add("ok", ok).add("how", how)
                                          .add("host_end", b.host_end).add("since_host_end_s", since).add("turn_restarted", b.force_now), b.turn);
    };
    std::string how;
    switch (b.force_step) {
    case 0: {
        if (!b.force_now && since < kForceEndGrace) return;
        b.force_step = 1;
        b.force_at = s.now;
        bump("battles_end_forced");
        const bool ok = game::force_battle_end(b.host_end, false, how);
        step_log("ForceBattleEnd", ok, how);
        return;
    }
    case 1: {
        if (s.now - b.force_at < 3.0) return;
        b.force_step = 2;
        b.force_at = s.now;
        const bool ok = game::force_battle_end(b.host_end, true, how);
        step_log("StartBattleEndFlow", ok, how);
        return;
    }
    case 2: {
        if (s.now - b.force_at < 4.0) return;
        b.force_step = 3;
        b.force_at = s.now;
        int killed = 0, failed = 0;
        if (b.host_end == game::EndVictory)
            for (UObject* e : game::enemies())
                if (!game::is_dead_fast(e)) (game::kill_unit(e) ? killed : failed)++;
        step_log("kill_enemies", failed == 0, std::to_string(killed) + " killed, " + std::to_string(failed) + " failed");
        return;
    }
    case 3: {
        if (s.now - b.force_at < 10.0) return;
        b.force_step = 4;
        step_log("resync", true, "finalize + ResyncRequest");
        bump("battle_end_resyncs");
        finalize_battle("host_end_forced");
        publish(proto::ResyncRequest{"battle still running after host BattleEnd"});
        return;
    }
    default:
        return;
    }
}

// Mode neutre (aucun rôle) : Steam prêt, bouton « Rejoindre », F7 / F10, copie coop. Le rôle décidé, tick() prend la suite.
static void neutral_tick(double dt)
{
    s.game_thread = GetCurrentThreadId();
    advance_clock(dt);
    hb.last_tick_ms = steady_ms();
    steam_tick();
    if (s.role != Role::Solo) return;
    join_button_tick();
    ui::tick(s.now);
    coopsave::tick(s.now, true);
    apply_options();
    if (s.session_msg_until >= 0 && s.now > s.session_msg_until) { s.session_msg_until = -1; ui::clear_status("session"); }
}

// À-coups d'image hors combat (2026-10-22 : « freeze » chez le client quand l'hôte court, sans journal en exploration).
// Une ligne par à-coup (> 80 ms, au plus une par seconde) avec le travail du mod dans l'image en cause (étapes de
// l'image précédente, contenue dans ce dt), et un résumé toutes les 30 s (pas une ligne par image).
struct ExploreFrames {
    double since = -1;
    int frames = 0;
    double dt_sum = 0, dt_max = 0;
    int h50 = 0, h80 = 0, h150 = 0, h300 = 0;
    double mod_sum = 0, mod_max = 0;
    std::vector<std::pair<const char*, double>> step_max;
    double last_line = -10;
    int suppressed = 0;
};
static ExploreFrames ef;

static std::string steps_json(const prof::Frame& f, double min_ms, size_t max)
{
    auto v = f.steps;
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    JsonObj o;
    size_t n = 0;
    for (auto& [name, ms] : v) {
        if (ms < min_ms || n++ >= max) break;
        o.add(name, std::round(ms * 100.0) / 100.0);
    }
    return o.str();
}

static void explore_frames_tick(double dt)
{
    if (s.battle.active) { ef = ExploreFrames{}; return; }   // combat : frame_hitch (plus haut)
    const prof::Frame& last = prof::last();
    if (ef.since < 0) ef.since = s.now;
    ++ef.frames;
    ef.dt_sum += dt;
    ef.dt_max = std::max(ef.dt_max, dt);
    if (dt > 0.05) ++ef.h50;
    if (dt > 0.08) ++ef.h80;
    if (dt > 0.15) ++ef.h150;
    if (dt > 0.30) ++ef.h300;
    ef.mod_sum += last.total_ms;
    ef.mod_max = std::max(ef.mod_max, last.total_ms);
    for (auto& [name, ms] : last.steps) {
        bool found = false;
        for (auto& [n, m] : ef.step_max) if (n == name) { m = std::max(m, ms); found = true; break; }
        if (!found) ef.step_max.emplace_back(name, ms);
    }
    // Images > 50 ms pendant un mouvement (suivi, D23) ; « mod » : le travail du mod en fait au moins la moitié.
    if (dt > 0.05 && dt < 3.0 && explore::motion_recent() && !game::map_change_in_progress()) {
        bump("explore_hitches_moving");
        if (last.total_ms > 500.0 * dt) bump("explore_hitches_mod");
    }
    if (dt > 0.08 && dt < 3.0) {   // au-delà : chargement (écran figé attendu)
        bump("explore_frame_hitches");
        if (s.now - ef.last_line < 1.0) ++ef.suppressed;
        else if (!game::map_change_in_progress()) {
            ef.last_line = s.now;
            log().write("frame_hitch_explore", JsonObj().add("dt_ms", static_cast<int64_t>(dt * 1000.0))
                                                   .add("mod_ms", std::round(last.total_ms * 100.0) / 100.0)
                                                   .raw("steps", steps_json(last, 0.2, 8))
                                                   .add("heavy", prof::last_heavy())
                                                   .add("pilot", explore::is_pilot())
                                                   .add("follow_error", static_cast<int64_t>(explore::follow_error()))
                                                   .add("suppressed", ef.suppressed));
            ef.suppressed = 0;
        }
    }
    if (s.now - ef.since >= 30.0) {
        prof::Frame mx;
        mx.steps = ef.step_max;
        log().write("explore_frame_stats", JsonObj().add("s", std::round((s.now - ef.since) * 10.0) / 10.0).add("frames", ef.frames)
                                               .add("dt_avg_ms", std::round(ef.dt_sum / ef.frames * 10000.0) / 10.0)
                                               .add("dt_max_ms", static_cast<int64_t>(ef.dt_max * 1000.0))
                                               .add("over_50ms", ef.h50).add("over_80ms", ef.h80).add("over_150ms", ef.h150).add("over_300ms", ef.h300)
                                               .add("mod_avg_ms", std::round(ef.mod_sum / ef.frames * 1000.0) / 1000.0)
                                               .add("mod_max_ms", std::round(ef.mod_max * 100.0) / 100.0)
                                               .raw("step_max_ms", steps_json(mx, 0.5, 12))
                                               .add("pilot", explore::is_pilot()));
        const double keep_line = ef.last_line;
        ef = ExploreFrames{};
        ef.last_line = keep_line;
    }
}

void tick(double dt)
{
    if (s.role == Role::Solo) { if (s.neutral) neutral_tick(dt); return; }
    s.game_thread = GetCurrentThreadId();
    advance_clock(dt);
    if (const int64_t st = hb.stall_of.exchange(0); st) {
        log().write("game_thread_resumed", JsonObj().add("stalled_ms", steady_ms() - st).add("battle", s.battle.active));
        if (s.battle.active) bump("game_thread_stalls_battle");   // gel de plus de 5 s en plein combat (hors chargement)
    }
    hb.last_tick_ms = steady_ms();
    heartbeat_start();
    if (!s.ended) explore_frames_tick(dt);
    { prof::Scope p("steam"); steam_tick(); }
    { prof::Scope p("join_button"); join_button_tick(); }
    { prof::Scope p("ui"); ui::tick(s.now); }
    { prof::Scope p("coopsave"); coopsave::tick(s.now, !s.ended); }   // copie coop (D15) : aussi après la fin de session (retour au menu)
    apply_options();
    if (s.ended) {
        // Client : session finie. Message affiché, puis retour au menu principal (la partie chargée est celle de l'hôte).
        if (!s.title_requested && s.now - s.ended_at > 6.0 && game::controller() && !game::main_menu()) {
            s.title_requested = true;
            const bool ok = game::back_to_title();
            log().write("session_back_to_title", JsonObj().add("ok", ok));
        }
        if (s.title_requested && game::main_menu() && s.now - s.ended_at > 20.0) ui::clear_status("session");
        // Session ouverte en jeu : de retour au menu principal, le joueur redevient « prêt au coop » (mode neutre).
        if (s.runtime_session && s.now - s.ended_at > 3.0 && s.now >= s.next_place_check) {
            s.next_place_check = s.now + 0.5;
            if (!game::controller() && game::main_menu()) back_to_neutral(s.end_key.empty() ? "quit to title" : s.end_key.c_str());
        }
        return;
    }
    // À-coups d'image en combat (> 50 ms) : un appui de QTE peut sembler « raté d'emblée » si le jeu saccade.
    if (s.battle.active) bump_max("battle_frame_max_ms", static_cast<int>(dt * 1000.0));   // image la plus longue en combat
    if (s.battle.active && dt > 0.05) {
        bump("frame_hitches");
        log().write("frame_hitch", JsonObj().add("dt_ms", static_cast<int64_t>(dt * 1000.0)).add("in_combo_window", !s.battle.windows.empty()), s.battle.turn);
    }
    { prof::Scope p("net"); pump_network(); }
    if (s.ended) return;
    // Client : les ennemis de la carte ne détectent, ne poursuivent ni n'engagent plus personne ici. Le combat ne part
    // que de l'hôte (BattleStart), avec son équipe et son engagement. Vu le 2026-10-21 : l'ennemi du client fonçait sur
    // le pion suiveur et lançait le combat 3,5 s avant l'attaque surprise de l'hôte (« pris par surprise » chez le client).
    // Toutes les 3 s (FindAllOf parcourt tous les objets), seule tâche lourde de son image ; pion pris par le contrôleur
    // (avant : deux FindFirstOf de plus, pion du monde et menu principal, à chaque passage).
    if (static double next_passive = 0; s.role == Role::Client && s.now >= next_passive && prof::claim_heavy("passive_enemies")) {
        next_passive = s.now + 3.0;
        prof::Scope p("passive_enemies");
        if (game::explore_pawn() && !game::in_battle() && !game::map_change_in_progress()) {
            const int n = game::passive_world_enemies();
            static int last_n = -1;
            if (n != last_n) { last_n = n; log().write("coop_world_enemies_passive", JsonObj().add("count", n)); }
        }
    }
    // Rappel des touches du mod tant que le menu pause est ouvert.
    {
        const bool show = game::pause_menu_visible() && !owners_ui::shows_key_hints();   // panneau Coop : il porte déjà les touches
        if (show != s.keys_shown) {
            s.keys_shown = show;
            if (show) ui::set_status("keys", s.role == Role::Host ? "keys_host" : "keys_client");
            else ui::clear_status("keys");
        }
    }
    // Hôte, hors combat, fenêtre du jeu au premier plan : F5 choisit un personnage de l'équipe, F6 le donne au joueur
    // suivant (menu de réattribution du brief, version touches). Diffusé aux clients ; vaut pour les builds et les
    // combats suivants. Avant : F6 seul passait aussi au personnage suivant à chaque appui (peu pratique, Killian).
    {
        static bool f5_down = false, f6_down = false;
        static size_t selected = SIZE_MAX;   // aucun : le premier F5 choisit le premier personnage
        const bool d5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        const bool d6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        const bool press5 = d5 && !f5_down, press6 = d6 && !f6_down;
        DWORD pid = 0;
        if (press5 || press6) GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool ok = (press5 || press6) && pid == GetCurrentProcessId() && s.role == Role::Host && !s.battle.active;
        if (const auto party = ok ? game::party_names() : std::vector<std::string>{}; !party.empty()) {
            if (press5 || selected == SIZE_MAX) {   // F6 sans choix préalable : choisit d'abord
                selected = (selected + 1) % party.size();
                const std::string& ch = party[selected];
                ui::toast("owner_selected", {ch, player_name(character_owner(ch))});
            } else {
                cycle_owner(selected % party.size());
            }
        }
        f5_down = d5;
        f6_down = d6;
    }
    // Héros repris à un joueur parti : si c'est le tour de l'un d'eux, le joueur de l'hôte le joue (entrées rendues).
    if (s.takeover && s.battle.active) {
        s.takeover = false;
        UObject* cur = game::current_character();
        if (cur && game::is_player(cur) && controls(game::spot(cur)) && !s.battle.executed.count({s.battle.turn, game::spot(cur)})) {
            set_local_input(true, "takeover", true);
            turn_indicator(actor_unit(cur));
            log().write("coop_takeover_turn", JsonObj().add("spot", game::spot(cur)), s.battle.turn);
        }
    }
    // Démarrage coordonné : jeu en pause au premier tour tant qu'un joueur de la partie n'a pas commencé ce combat.
    if (auto& b = s.battle; b.hold) {
        // Qui on attend et pourquoi (chargement, cinématique, menu, reconnexion ; sinon « entre en combat »).
        std::vector<uint8_t> who;
        for (auto& p : s.peers) if (in_game(p) && !b.ready.count(p.player_id)) who.push_back(p.player_id);
        // Inactivité continue de tous les joueurs attendus : rien ne les retient, ils devraient déjà être là.
        bool idle = !who.empty();
        for (uint8_t w : who) if (presence::activity(w, s.now) != presence::Activity::Ready) idle = false;
        b.hold_idle_s = idle ? b.hold_idle_s + dt : 0.0;
        const bool expired = s.now - b.hold_since > kReadyTimeout;
        const bool idle_out = b.hold_idle_s >= s.ready_idle_s;
        if (who.empty() || expired || idle_out) {
            b.hold = false;
            explore::hold_pause("battle_start", false);
            ui::clear_status("wait");
            if (expired || idle_out) {
                // Jamais figé pour toujours : le combat commence sans eux ; ils suivront en arrivant (BattleStart reçu).
                bump("battle_ready_timeouts");
                if (idle_out && !expired) bump("battle_ready_idle_timeouts");
                std::string names;
                for (uint8_t w : who) names += (names.empty() ? "" : ", ") + player_name(w);
                ui::toast("battle_started_without", {names}, 8.0);
            }
            bump("battle_holds_released");
            bump_max("battle_hold_max_ms", static_cast<int>((s.now - b.hold_since) * 1000.0));
            log().write("coop_battle_hold_release", JsonObj().add("waited_s", s.now - b.hold_since).add("timeout", expired).add("idle_timeout", idle_out)
                                                         .add("idle_s", b.hold_idle_s), b.turn);
        } else {
            if (!explore::pause_held("battle_start")) {
                explore::hold_pause("battle_start", true);
                bump("battle_holds");
                log().write("coop_battle_hold", JsonObj(), b.turn);
            }
            if (s.now - std::max(b.hold_since, b.hold_logged_at) >= 15.0) {
                b.hold_logged_at = s.now;
                std::string ids, acts;
                for (uint8_t w : who) {
                    if (!ids.empty()) { ids += ","; acts += ","; }
                    ids += std::to_string(w);
                    acts += presence::activity_name(presence::activity(w, s.now));
                }
                log().write("coop_battle_hold_waiting", JsonObj().add("waited_s", s.now - b.hold_since).add("players", ids).add("activities", acts)
                                                             .add("idle_s", b.hold_idle_s), b.turn);
            }
            ui::set_status_text("wait", "waiting_players", presence::wait_text(who, s.now, player_name, "wait_battle_prep"));
        }
    }
    // Diagnostic (duel du prologue, 2026-10-22) : la barrière narrative ne doit jamais tenir pendant un combat coop engagé.
    {
        const bool engaged = s.pending_start.has_value() || (s.battle.active && !s.battle.finalized);
        if (engaged && explore::pause_held("barrier")) {
            if (s.barrier_battle_since < 0) s.barrier_battle_since = s.now;
            else if (s.barrier_battle_since > 0 && s.now - s.barrier_battle_since > 2.0) {
                s.barrier_battle_since = 0;   // une fois par épisode
                bump("barrier_holds_in_battle");
                log().write("coop_barrier_in_battle", JsonObj().add("pending_start", s.pending_start.has_value()).add("active", s.battle.active), s.battle.turn);
            }
        } else {
            s.barrier_battle_since = -1;
        }
    }
    peer_activity_tick();
    defense_context_watch();
    if (s.battle.active && !s.battle.hold && !explore::remote_paused() && !game::pause_menu_visible()) s.battle.timer_elapsed += dt;
    if (s.battle.active && !s.input_enabled) enforce_input_block();
    if (s.role == Role::Client) start_pending_battle();
    { prof::Scope p("explore"); explore::tick(s.now); }
    { prof::Scope p("builds"); builds::tick(s.now); }
    run_remote_action();
    // Aperçu de ce que préparent les joueurs, caméra de la visée libre (envoyée par le propriétaire, rejouée ailleurs).
    { prof::Scope p("intent"); intent::tick(s.now); }
    if (s.fa_own.active && s.battle.active)
        intent::aim_owner_tick(s.now, s.fa_own.spot, static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count()));
    {
        const auto& r = s.fa_rep;
        intent::AimReplay ar;
        const bool replaying = s.battle.active && r.started && !r.done && r.key.first == s.battle.turn;
        if (replaying) {
            ar.hero = game::find_unit(true, r.key.second);
            ar.spot = r.key.second;
            ar.elapsed_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - r.t0).count());
            ar.have_point = r.have_override;
            std::copy(r.point.begin(), r.point.end(), ar.point);
        }
        intent::aim_camera_tick(s.now, dt, replaying ? &ar : nullptr);
    }
    // Visée libre d'un héros d'ici quittée par un autre chemin que CancelFreeAimFromUI / EndFreeAim (touche
    // d'annulation de la visée : vu le 2026-10-21, l'autre joueur restait en visée et l'action suivante attendait).
    if (s.fa_own.active) {
        const bool aiming = ue::get_bool(ue::get_object(game::find_unit(true, s.fa_own.spot), STR("BP_FreeAimShooterBattleComponent")), STR("IsAiming"));
        if (aiming) s.fa_own.seen_aiming = true;
        else if (s.fa_own.seen_aiming) {
            proto::FreeAimEvent e;
            e.battle_id = s.battle.id;
            e.turn = s.battle.turn;
            e.spot = static_cast<uint8_t>(s.fa_own.spot);
            e.kind = proto::FreeAimKind::Cancel;
            e.t_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count());
            e.x = s.fa_own.point[0]; e.y = s.fa_own.point[1]; e.z = s.fa_own.point[2];
            publish(e);
            s.fa_own.active = false;
            bump("free_aim_exits_detected");
            bump("free_aim_cancels_published");
            log().write("coop_free_aim_local_exit", JsonObj().add("spot", s.fa_own.spot), s.battle.turn);
        }
    }
    // Diagnostic (TOTOFFF vise mais ne tire pas, à la manette) : touches de la visée et contexte qui reçoit RT / clic.
    if (s.fa_diag_at > 0 && s.now >= s.fa_diag_at) {
        s.fa_diag_at = -1;
        static bool mapped = false;
        UObject* fa_imc = ue::find_object(L"/Game/Gameplay/Inputs/InputMappingProfiles/Battle/IMC_Battle_FreeAim.IMC_Battle_FreeAim");
        JsonObj o;
        if (!mapped) { mapped = true; o.add("mappings", game::imc_mappings(fa_imc)); }
        o.add("active", s.fa_own.active).add("input_enabled", s.input_enabled)
         .add("rt_top", game::top_context_for_key("Gamepad_RightTrigger")).add("rt", game::active_contexts_with_key("Gamepad_RightTrigger"))
         .add("rb", game::active_contexts_with_key("Gamepad_RightShoulder")).add("lmb_top", game::top_context_for_key("LeftMouseButton"))
         .add("input_type", game::common_input_type()).add("widgets", game::active_common_widgets());
        // Tir (RT) lié aussi par un autre contexte actif (ex. sprint de l'exploration) : il peut ne jamais arriver.
        const std::string rt = game::active_contexts_with_key("Gamepad_RightTrigger");
        bump(rt.find(",") != std::string::npos ? "free_aim_rt_shared" : "free_aim_rt_alone");
        log().write("coop_free_aim_inputs", o, s.battle.turn);
    }
    // Visée libre d'un héros d'ici : point visé envoyé ~20 fois par seconde (non fiable, pour l'affichage chez les autres).
    if (s.fa_own.active && s.fa_own.have_point && s.now >= s.fa_own.next_aim_s) {
        s.fa_own.next_aim_s = s.now + 0.05;
        proto::FreeAimEvent e;
        e.battle_id = s.battle.id;
        e.turn = s.battle.turn;
        e.spot = static_cast<uint8_t>(s.fa_own.spot);
        e.kind = proto::FreeAimKind::Aim;
        e.t_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.fa_own.t0).count());
        e.x = s.fa_own.point[0]; e.y = s.fa_own.point[1]; e.z = s.fa_own.point[2];
        publish(e, nullptr, false);
    }
    // Essai -e33turndep=1 : vérification différée faite et dépendance résolue dès que l'état de l'hôte est là.
    if (auto& b = s.battle; b.dep_active || b.dep_finish_pending) {
        const bool ready = b.host_sync.count(b.dep_turn) || b.host_end >= 0 || !b.active;
        const bool expired = s.now - b.dep_since > 15.0;
        if (ready || expired) {
            // D'abord la vérification (acteur, état, réparation), puis le jeu reprend le tour.
            if (b.dep_finish_pending && ready && b.active && b.turn == b.dep_turn && b.dep_actor) client_turn_finish(b.dep_actor);
            b.dep_finish_pending = false;
            b.dep_start = {};
            if (expired && !ready) { bump("turn_dep_timeouts"); log().write("coop_turn_dep_timeout", JsonObj().add("turn", b.dep_turn), b.dep_turn); }
            if (b.dep_active) {
                bump("turn_deps_held");
                dep_solve(ready ? "host_state" : "timeout");
                // SolveDependency conclut déjà l'écoute quand c'était la dernière dépendance ; ses drapeaux (IsListening,
                // HasTriggered) ne sont pas encore à jour juste après : un CheckDependencies immédiat relançait la suite du tour
                // une 2e fois (l'ennemi tirait deux fois sa cible, tours 21 et 28 de p3_party, 2026-10-22). Pas de vérification forcée (voir plus bas).
                b.dep_recheck_at = s.now + 0.5;
            }
        }
    }
    // Diagnostic seulement : 0,5 s après la résolution, les drapeaux disent parfois « écoute toujours » alors que la suite
    // du tour est déjà partie ; un CheckDependencies forcé la relançait une 2e fois (tours 21 et 28 de p3_party).
    if (auto& b = s.battle; b.dep_recheck_at >= 0 && s.now >= b.dep_recheck_at) {
        b.dep_recheck_at = -1;
        UObject* deps = b.active && !b.dep_active && b.turn == b.dep_turn ? turn_deps() : nullptr;
        if (deps && ue::get_bool(deps, STR("IsListening")) && !ue::get_bool(deps, STR("HasTriggered"))) {
            bump("turn_deps_still_listening");
            log().write("coop_turn_dep_still_listening", JsonObj().add("turn", b.dep_turn), b.turn);
        }
    }
    if (s.role == Role::Host) align_tick();   // D28 : tour ennemi relâché quand les clients l'ont commencé
    client_force_end_tick();
    defeat_tick();
    // Fin sans victoire : clôture dès que le résultat de l'hôte est là (8 s au plus).
    if (auto& b = s.battle; b.end_flow_at >= 0 && !b.finalized && (s.role == Role::Host || b.host_end >= 0 || s.now - b.end_flow_at > 8.0))
        finalize_battle("end_flow");
    // Combat fini chez l'hôte sans fin locale (client bloqué, ou carte rechargée en suivant l'hôte) : clôture quand
    // même, sinon l'état de ce combat déborde sur le suivant (vu le 2026-10-20 : 2e combat compté comme le tour 2 du 1er).
    if (auto& b = s.battle; s.role == Role::Client && b.active && !b.finalized && b.host_end_at >= 0 && b.end_flow_at < 0 &&
                            (!game::in_battle() || (b.host_end != game::EndVictory && s.now - b.host_end_at > 15.0))) {
        bump("battles_closed_by_host_end");
        finalize_battle("host_end");
    }
    // Appuis de QTE rejoués pour les héros des autres joueurs (instant de leur propriétaire).
    auto& presses = s.battle.presses;
    for (size_t i = 0; i < presses.size();) {
        if (SteadyClock::now() < presses[i].at) { ++i; continue; }
        const Press p = presses[i];
        presses.erase(presses.begin() + static_cast<std::ptrdiff_t>(i));
        auto it = s.battle.windows.find(p.timeline);
        if (it == s.battle.windows.end() || it->second.pressed) continue;
        ue::Call c(it->second.hero, STR("Try Activate Skill Combo"));
        if (c) c.invoke();
        bump("combo_presses_replayed");
    }
}

} // namespace e33::coop
