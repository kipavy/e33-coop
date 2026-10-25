// Partie interne du module coop, partagée entre ses fichiers (coop*.cpp). Ne pas inclure ailleurs.
// Découpage de l'ancien coop.cpp (2026-10-25) : même code, réparti par thème.
#pragma once

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


namespace e33::coop::detail {

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

struct OwnerDone {                                         // fin d'action chez le propriétaire d'un héros (D36)
    uint16_t hits = 0;                                     // coups reçus par ce héros chez lui pendant ce tour
    proto::Unit actor{proto::ActorKind::Enemy, proto::kAutoSpot};   // combattant du tour chez lui (contrôle)
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
    // Fin d'action ennemie (D36) : combattant du tour en cours ici, fins d'action reçues des propriétaires, dernier tour publié.
    proto::Unit turn_actor{proto::ActorKind::Enemy, proto::kAutoSpot};
    std::map<std::pair<uint32_t, int>, OwnerDone> owner_done;          // (tour, emplacement) -> fin chez le propriétaire
    uint32_t done_sent_turn = 0;
    // État
    std::map<uint32_t, uint64_t> local_hash;
    std::map<uint32_t, uint64_t> remote_hash;
    std::map<uint32_t, proto::StateSync> hashed_state;     // état exact haché pour chaque tour (journal desync)
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
    double battle_request_at = -1e9;   // client meneur : dernière demande de combat envoyée à l'hôte (D34)
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
    bool repairing = false;                                // réparation d'état en cours : ses tirages (ActivateBuff…) ne prennent pas de rang du tour
    bool lock_input = true;                                // -e33lockinput=0 : laisser les entrées pendant les tours des autres
    bool turn_dep = false;                                 // -e33turndep=1 : attente du TurnStart par dépendance de début de tour (essai)
    double ready_idle_s = kReadyIdleTimeout;               // hôte : -e33readyidle=N
    double stuck_turn_s = kStuckTurnTimeout;               // hôte : -e33stuckturn=N
    bool turn_align = true;                                // hôte : démarrage synchronisé des tours ennemis (-e33turnalign=0 : coupé, D28)
    bool enemy_done = true;                                // fin d'action ennemie publiée et utilisée (-e33enemydone=0 : coupé, D36)
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
    double menu_wait_next_log = 0;
    double menu_widget_since = -1;
    double fa_diag_at = -1;                                // diagnostic : contextes 0,5 s après le début d'une visée libre d'ici
    bool enemy_turn = false;                               // tour en cours : un ennemi (tous les rôles)
    int imc_logs = 0;                                      // diagnostic : contextes d'entrée au début des tours ennemis
    int defense_path_logs = 0;                             // diagnostic : appuis de défense (contrôleur, gestionnaire)
    UObject* input_sub = nullptr;                          // EnhancedInputLocalPlayerSubsystem (pris au blocage)
    std::map<UObject*, int> removed_contexts;              // contextes d'entrée retirés pendant le blocage -> priorité
    std::map<std::string, int> counters;
    FreeAimOwner fa_own;                                   // visée d'un héros d'ici
    std::pair<uint32_t, int> fa_own_ended{0, -1};          // (tour, emplacement) d'une visée d'ici finie : pas de reprise tardive tant que IsAiming reste vrai
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

extern State s;

inline void bump(const char* name, int n = 1) { s.counters[name] += n; }

struct Heartbeat {
    std::mutex mu;
    std::vector<std::pair<std::string, uint32_t>> targets;   // adresse, époque de notre lien vers elle
    std::atomic<int64_t> last_tick_ms{0};
    std::atomic<bool> started{false};
    std::atomic<int64_t> stall_of{0};   // diagnostic : last_tick_ms du gel du thread de jeu déjà journalisé (0 : aucun)
};
extern Heartbeat hb;

enum class RandomKind { IntInRange, Int, Float, FloatInRange, Bool, BoolWeight };
struct RandomFn { const wchar_t* name; RandomKind kind; };
constexpr size_t kTurnContexts = 3;
constexpr double kAlignTimeout = 4.0;
constexpr double kDefeatHostSilentS = 120.0;
constexpr double kForceEndGrace = 5.0;

// --- Fonctions internes (définies dans les coop*.cpp) ---
std::wstring arg(const wchar_t* key);
std::filesystem::path game_root();
uint32_t read_build_id();
std::vector<std::string> read_gameplay_mods();
void send_to(Peer& p, const proto::Message& m, bool reliable = true);
bool in_game(const Peer& p);
void broadcast(const proto::Message& m);
void publish(const proto::Message& m, const Peer* origin = nullptr, bool reliable = true);
std::string player_name(uint8_t id);
uint8_t character_owner(const std::string& name);
std::string owners_summary();
void toast_owners_if_changed();
proto::Owners character_owner_map();
void cycle_owner(size_t index);
int64_t steady_ms();
void heartbeat_targets();
void heartbeat_start();
uint8_t owner_of_spot(int spot);
bool controls(int spot);
proto::Unit actor_unit(UObject* actor);
int free_aim_part_index(UObject* actor, UObject* part);
UObject* free_aim_part_at(UObject* actor, int index);
bool free_aim_part_location(UObject* part, double out[3]);
void refresh_enemy_spots();
int cached_enemy_spot(UObject* o);
std::string hex64(uint64_t v);
void check_hash(uint32_t turn);
void handle(Peer& p, proto::Message& m);
void on_player_left(uint8_t player, const std::string& name, const std::string& why);
void end_session(const std::string& key, const std::string& detail);
void defeat_on_host_choice(const proto::DefeatChoice& dc);
void turn_indicator(const proto::Unit& u);
void set_local_input(bool enabled, const char* why, bool local_turn = false);
void wire_link(net::Link& l);
void sweep_peers();
void pump_network();
void advance_clock(double dt);
void bump_max(const char* name, int v);
bool wait_parry_enabled();
bool parry_key_down();
bool handle_battle_common(Peer& p, proto::Message& m);
uint8_t free_player_id();
void send_roster();
void reject(Peer& p, proto::RejectReason reason, const std::string& detail);
void subscribe_session();
void connect_to_host(const std::string& addr);
void apply_options();
void read_settings();
void coopsave_init(bool host);
void start_session(Role r, const char* why);
bool on_game_thread();
UObject* script_caller(HookCtx& ctx, UFunction** node_out = nullptr);
bool setup_window();
void trace_random(HookCtx& ctx, const std::string& fname);
uint64_t splitmix64(uint64_t x);
double derived_uniform_seed(uint64_t seed, uint32_t turn, uint32_t key, uint16_t rank);
double derived_uniform(uint32_t turn, uint32_t key, uint16_t rank);
uint32_t fnv32(const std::string& str);
double next_uniform(const std::string& stream, uint32_t& key, uint16_t& rank);
void record_draw(const std::string& stream, uint32_t key, uint16_t rank, double v, double a, double b);
std::string gameplay_stream(HookCtx& ctx, const std::string& fname, bool setup = false);
double next_setup_uniform(const std::string& stream, uint32_t& key, uint16_t& rank);
void record_setup_draw(const std::string& stream, uint32_t key, uint16_t rank, double v, double a, double b);
void replace_draw(HookCtx& ctx, UFunction* f, RandomKind kind, const std::string& stream, bool setup = false);
bool draw_allowed(const std::string& stream);
double natural_value(HookCtx& ctx, RandomKind kind);
void compare_draws(uint32_t turn);
void setup_reset(uint64_t seed, bool seeded, const std::string& source);
void host_setup_open(bool notice);
void client_setup_open();
void setup_close();
void compare_setup_draws();
void check_start_mech(const proto::StateSync& mine, const proto::StateSync& host);
bool mech_differs(const proto::StateSync& mine, const proto::StateSync& host);
void offturn_draw(HookCtx& ctx, UFunction* f, RandomKind kind, const std::string& fname);
std::vector<std::wstring> party_wide(const std::vector<std::string>& v);
uint8_t default_owner(int index, int spot, const std::string& name);
void begin_turn_common(UObject* actor);
void align_begin(uint32_t turn, const char* how);
void host_turn_start(UObject* actor);
void perturb_state();
void perturb_end();
void client_turn_finish(UObject* actor);
UObject* turn_deps();
void dep_solve(const char* why);
void client_turn_start(UObject* actor);
proto::StateSync shift_host_state(const proto::StateSync& host, const proto::StateSync& start, const proto::StateSync& now);
UObject* blocked_context(size_t i);
std::optional<int> context_priority(UObject* imc);
void enforce_input_block();
void restore_turn_contexts();
std::vector<uint8_t> align_missing(uint32_t turn);
void align_tick();
void duo_parry_check();
void on_turn_start(UObject* actor);
void apply_host_party(const proto::BattleStart& bs);
UObject* timeline_hero(UObject* timeline);
bool impose_defense(UObject* hero, proto::Defense d, bool jumpable);
proto::Defense local_result(const LocalHit& lh, bool parried, bool dodged, bool gradient);
void defense_point(UObject* stats_comp, UObject* hero, bool jumpable);
void enemy_action_done(const char* why);
void on_enemy_action_done(Peer& p, const proto::EnemyActionDone& d);
proto::BattleRewards read_rewards(void* st_ptr, RC::Unreal::UStruct* st);
bool write_loot(void* st_ptr, RC::Unreal::UStruct* st, const std::vector<proto::LootEntry>& loot);
void finalize_battle(const char* why);
void defeat_reset(uint32_t battle);
void defeat_arm();
void defeat_hide(UObject* modal);
void defeat_apply(int k, UObject* screen, UObject* modal);
void defeat_tick();
void subscribe();
void neutral_steam(bool f7);
void back_to_neutral(const char* why);
bool key_pressed(int vk, bool& was_down);
void steam_tick();

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

} // namespace e33::coop::detail

namespace e33::coop {
using namespace detail;
void client_force_end_tick();
void start_pending_battle();
bool replay_free_aim(const std::pair<uint32_t, int>& key, UObject* hero);
void run_remote_action();
} // namespace e33::coop
