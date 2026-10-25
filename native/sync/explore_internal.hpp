// Partie interne du module d'exploration, partagée entre ses fichiers (explore*.cpp). Ne pas inclure ailleurs.
// Découpage de l'ancien explore.cpp (2026-10-25) : même code, réparti par thème.
#pragma once

#include "explore.hpp"

#include "builds.hpp"
#include "coopsave.hpp"
#include "mount.hpp"
#include "presence.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string_view>
#include <vector>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../common/prof.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/reflect.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"

#include <Unreal/Hooks.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include "../ui/ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

using RC::Unreal::UObject;
using RC::Unreal::UFunction;

namespace e33::explore::detail {

constexpr const wchar_t* kHostSnapshotSlot = L"e33coop/snapshot";   // hôte : instantané envoyé

constexpr const wchar_t* kCoopSlot = L"e33coop/host";               // client : emplacement coop (jamais ses saves solo)

constexpr double kPilotPeriod = 0.05;                               // PilotState ~20 par seconde

constexpr double kPollPeriod = 0.2;                                 // dialogues, cinématiques, menu pause

constexpr double kTeleportDistance = 800;                           // écart au-delà duquel le suiveur est téléporté

constexpr double kBarrierTimeout = 600;   // les cinématiques durent jusqu'à ~5 min (intro de Renoir : 295 s)

constexpr double kIntroMax = 900;         // nouvelle partie : intro comptée comme cinématique au plus 15 min (filet si sa fin n'est pas vue)

// Cinématique ou dialogue commencé chez un autre joueur, pas ici : délai avant la pause de la barrière, pour que le
// suiveur atteigne le déclencheur et commence la même scène (sinon il reste figé en dehors, ne la déclenche qu'après
// la fin chez l'autre, et c'est l'autre qui attend ensuite : scènes « en décalé », essai à deux PC du 2026-10-22).
constexpr double kBarrierGrace = 6.0;

// InGame : client accepté sans « rejoindre » (tests de combat) : il participe d'emblée, sans instantané.
enum class JoinState { None, Pending, Waiting, Sent, Joined, InGame };

// resync : client entré qui a demandé un nouvel instantané ; il reste dans la partie (combats compris) jusqu'à l'envoi.
struct PeerJoin { JoinState state = JoinState::None; double since = 0; bool wait_sent = false; bool resync = false; };

struct Inbound {                                  // client : instantané en cours de réception
    uint32_t xfer = 0;
    uint32_t size = 0;
    uint64_t hash = 0;
    std::string level;
    std::vector<uint8_t> data;
    uint32_t received = 0;
};

struct S {
    bool host = false;
    bool active = false;
    Net net;
    uint8_t me = 0;
    double now = 0;
    // Rejoindre
    bool want_join = false;
    std::map<uint8_t, PeerJoin> peers;            // hôte
    uint32_t next_xfer = 1;
    std::optional<Inbound> inbound;               // client
    enum class ClientJoin { None, Receiving, Loading, WaitWorld, Joined } cj = ClientJoin::None;
    double cj_since = 0;
    double world_stable = 0;
    uint32_t joined_xfer = 0;
    double cj_calm_since = -1;                    // client : début du calme (ni dialogue ni cinématique) avant de charger
    int cj_load_tries = 0;                        // client : chargements demandés pour cet instantané
    double cj_next_diag = 0;                      // client : prochain journal de l'attente du monde
    int join_stuck_test = 0;                      // test : -e33joinstuck=N, monde vu « pas prêt » pendant les N premiers chargements
    int cj_loads_total = 0;                       // client : chargements demandés dans la session (tous instantanés)
    double next_char_fix = 0;                     // suiveur : prochain essai de copie du personnage affiché
    // Pilote
    uint8_t pilot = 0;
    uint32_t pilot_seq = 0;
    double next_pilot_send = 0;
    std::optional<proto::PilotState> last_pilot;  // suiveur
    double last_pilot_at = 0;
    double follow_error = -1;
    // Sprint du suiveur (2026-10-22 : le pilote sprinte à ~720 u/s, le suiveur restait en course à ~350 u/s et était
    // téléporté toutes les ~2,2 s à 800 u d'écart : les « freeze » vus quand l'hôte court). Suivi : FollowCtl (D23).
    bool follow_sprint = false;                   // sprint demandé au pion suiveur
    bool sprint_touched = false;                  // le mod a déjà changé l'allure de ce pion
    int sprint_method = 0;                        // 2 SetDesiredGait (0 : échec)
    int gait_before = -1;                         // allure voulue avant le sprint (rendue ensuite)
    double sprint_since = -1;
    bool sprint_logged = false;
    // Vitesse du pion local (pilote ou suiveur), pour reporter les tâches lourdes pendant le mouvement
    double local_speed = 0;
    double local_speed_at = -1;
    double last_motion_at = -100;                 // pilote ou pion local en mouvement (> 50 u/s) pour la dernière fois
    // Client : resynchro (rechargement de 5-6 s) attendue jusqu'à l'arrêt du mouvement (essai du 2026-10-22 : deux
    // rechargements en plein sprint de l'hôte, vus comme des « freeze »).
    std::string resync_deferred;                  // parties en écart (vide : aucune demande en attente)
    double resync_deferred_since = -1;
    double level_mismatch_since = -1;
    // Changements de niveau (suiveur) : MapChange en attente, point d'apparition du pilote par niveau, niveau attendu
    std::optional<proto::MapChange> pending_map;
    std::map<std::string, std::string> map_spawns;
    std::string expect_level;
    double expect_level_at = -1;
    bool f8_down = false;
    // Interactions, dialogues, transitions (rejoués chez les suiveurs)
    uint32_t seq = 0;
    bool replaying = false;
    std::deque<proto::WorldInteract> interactions;
    std::deque<std::pair<proto::DialogueChoice, double>> choices;
    bool choice_pending = false;                  // question posée (StartChoice), pas encore répondue
    int choice_hover_sent = -1, choice_hover_want = -1, choice_hover_shown = -1;   // choix survolé (pilote / voulu / affiché ici)
    int choice_diag = 0;
    bool merchant_open = false, merchant_locked = false, merchant_close = false;   // fenêtre du marchand (pilote : ouverte ; suiveur : miroir posé, fermeture demandée)
    int merchant_sent = -1, merchant_want = -1, merchant_shown = -1;
    int merchant_diag = 0;
    int merchant_buy = -1;            // suiveur : achat du pilote à rejouer (index de l'article)
    double merchant_seen_at = -1;     // suiveur : premier message du marchand du pilote (sélection), pour ouvrir la fenêtre
    bool merchant_open_tried = false; // suiveur : ouverture de secours déjà tentée pour cette visite
    bool sequence_playing = false;                // OnSequenceStarted reçu, pas encore OnSequenceFinished
    // Narration (barrière) et pause
    double next_poll = 0;
    bool local_narr = false;
    proto::NarrativeKind local_kind = proto::NarrativeKind::Dialogue;
    uint32_t narr_seq = 0;
    std::map<uint8_t, bool> remote_narr;
    std::map<uint8_t, double> remote_narr_since;  // début de la narration en cours de chaque autre joueur
    std::map<uint8_t, double> remote_narr_end;    // fin de la dernière narration de chaque autre joueur
    double local_narr_end = -1;                   // fin de la dernière narration d'ici
    bool barrier_paused = false;
    double barrier_since = -1;
    bool local_pause = false;
    std::map<uint8_t, bool> remote_pause;
    bool remote_paused_game = false;
    // Nouvelle partie ensemble : intro (vidéo d'ouverture) en cours ici, comptée comme une cinématique pour la barrière
    bool intro = false;
    double intro_since = -1;
    int intro_sequences_done = 0;                 // cinématiques du prologue terminées depuis le début de l'intro
    double intro_calm_since = -1;                 // pion jouable, sans cinématique, depuis
    // Combat d'exploration
    std::string last_enemy;
    int last_engage = 0;
    double last_enemy_at = -1;
    std::map<UObject*, int> removed_locomotion;   // suiveur : contextes de déplacement retirés -> priorité
    // Pause partagée (raisons) et robustesse (phase 5)
    std::set<std::string> pause_reasons;
    bool pause_applied = false;
    double last_chunk_at = -1;                    // client : réception de l'instantané en cours
    bool ended = false;
    // Hash d'état d'exploration
    double last_event_at = 0;
    double next_world_check = 0;
    uint64_t last_world_sent = 0;
    double last_world_sent_at = -1000;
    uint32_t world_seq = 0;
    std::optional<proto::WorldHash> pending_world;   // client : dernière empreinte de l'hôte, à comparer au calme
    double pending_world_at = 0;
    int world_mismatch_streak = 0;
    std::set<std::string> world_ignored;          // client : parties qu'un instantané n'a pas réparées (plus de resynchro)
    std::set<std::string> resync_parts;           // client : parties en écart de la dernière demande d'instantané
    double resync_at = -1;                        // client : instant de cette demande (-1 : aucune en cours)
    std::map<std::string, int> resync_failures;   // client : instantanés de suite qui n'ont pas réparé cette partie
    int perturb_gold = 0;                         // -e33perturbworld=N (test)
    double cj_joined_at = 0;
    double waited_since = -1;                     // joueur en retard : les autres ont fini depuis
    double last_world_compute = -1000;            // dernière lecture de l'état d'exploration (hôte et client)
    std::vector<proto::HashPart> last_world_parts; // hôte : dernière empreinte envoyée
    uint64_t copy_ok_hash = 0;                    // client : empreinte vérifiée égale à celle de l'hôte, copie coop à faire
    double copy_ok_at = -1;
};
extern S s;

// Personnage affiché en exploration, par pion (hook de LoadAndActivateCharacter) : gardé hors session (le pion
// charge son personnage avant l'ouverture de la partie coop).
extern std::map<UObject*, std::string> g_char_of;
// Statistiques du suivi, journalisées toutes les 15 s (coop_follow_stats), pas une ligne par image.
struct FollowStats {
    double since = -1;
    // PilotState reçus (chez le suiveur)
    int rx = 0, rx_same_frame = 0, rx_gap_100 = 0, rx_gap_250 = 0, rx_gap_500 = 0, seq_lost = 0, rx_old = 0;
    double rx_gap_sum = 0, rx_gap_max = 0, last_rx = -1;
    uint32_t last_seq = 0;
    // Conduite du pion
    int ticks = 0, starts = 0, stops = 0, teleports = 0, sprints = 0, sprint_fallbacks = 0;
    double err_sum = 0, err_max = 0, age_max = 0, pilot_speed_max = 0, own_speed_sum = 0, own_speed_max = 0;
    int own_speed_n = 0;
    double sprint_s = 0, sprint_gait_ok_s = 0, starved_s = 0, last_t = -1;
};

extern FollowStats fstats;
// --- Suivi fluide du pilote (2026-10-23, « freeze » vus par le client quand l'hôte court, D23) -------------
// Avant : cible extrapolée depuis la dernière position reçue + lissage exponentiel, échelle d'entrée approximative,
// sprint par SprintAction (sans effet sur le pion suiveur : repli SetDesiredGait après 0,6 s, retard de ~200 u jamais
// rattrapé puisque le suiveur n'allait pas plus vite que le pilote), téléportation fixe à 800 u.
// Maintenant :
//  - tampon d'interpolation : chaque position est placée à son instant d'envoi (PilotState.t_ms, horloge du pilote ;
//    à défaut l'instant de réception). Le pion vise la trajectoire du pilote telle qu'elle était kRenderDelay avant la
//    position la plus récente arrivée « à l'heure » (transit minimal) : courbe d'Hermite entre deux positions, sans la
//    gigue du réseau ; prolongée au plus kExtrapolateMax si une position tarde (pointe réseau) ;
//  - commande en vitesse : vitesse du pilote à cet instant + rattrapage proportionnel à l'écart. En course : échelle
//    d'entrée = vitesse voulue / MaxWalkSpeed. En sprint : allure voulue Sprint (SetDesiredGait, directement), entrée
//    pleine (l'ALS n'accorde le sprint qu'au-delà de 0,9) et MaxWalkSpeed = vitesse voulue (jusqu'à 1,2 x le sprint
//    pour rattraper un retard), rendue à l'ALS à la sortie du sprint ;
//  - téléportation seulement si elle se justifie : trou de données ou saut du pilote (« gap »), écart > 800 u qui ne
//    se résorbe pas en 1 s ou > 1600 u, pion bloqué 2 s, écart vertical > 400 u pendant 0,5 s ;
//  - mesures de fluidité en compteurs (follow_metrics), vérifiées par les scénarios p8_*.
constexpr double kRenderDelay = 0.10;        // retard de rendu au-delà du transit minimal (horloge du pilote)

constexpr double kRenderDelayNoClock = 0.12; // sans horloge (ancien enregistrement) : instants de réception

constexpr double kExtrapolateMax = 0.5;      // position en retard (pointe réseau) : trajectoire prolongée au plus d'autant

constexpr double kFollowGain = 2.0;          // rattrapage : vitesse ajoutée par unité d'écart (1/s)

constexpr double kCatchUpMax = 1.2;          // vitesse maximale = 1,2 x sprint pendant un rattrapage

struct PilotSample { double t = 0; double pos[3]{}; double vel[3]{}; double yaw = 0; };

struct FollowCtl {
    std::deque<PilotSample> buf;            // positions du pilote, triées par instant d'envoi
    uint8_t pilot = 0xFF;
    std::string level;
    bool clock = false;                     // instants = horloge du pilote (sinon réception)
    double off = 0;                         // réception - envoi, minimal (s)
    int off_high = 0;
    double last_arrival = -1;
    double discontinuity_at = -100;         // trou de données / saut du pilote : téléportation « gap » permise
    double teleport_at = -100;
    double tp_from[3]{};                    // position du pion juste avant la dernière téléportation
    int tp_fails = 0;                       // téléportations annulées par le jeu d'affilée (pion revenu sur place)
    double tp_block_until = -1;
    // Conduite
    bool moving = false;
    bool sprint = false;
    double sprint_low_since = -1;
    double sprint_set = 0;                  // vitesse de sprint écrite (lissée)
    double ctl_last = -1;
    double next_gait_assert = 0;
    float base_max = 0;                     // MaxWalkSpeed avant la première écriture du mod
    bool max_written = false;
    game::GaitSpeeds speeds{};
    bool speeds_ok = false;
    bool speeds_logged = false;
    double speeds_at = -100;
    double far_since = -1, far_err0 = 0;
    double dz_since = -1;
    double stuck_since = -1;
    double follow_since = -1;               // pion conduit depuis
    // Mesures
    double rv_move_since = -1;              // vitesse rendue du pilote > 150 depuis
    double rv_fast_since = -1;              // > 300 depuis
    double stall_since = -1; bool stall_counted = false;
    double frozen_since = -1;
    double dip_since = -1; bool dip_counted = false;
    double rest_since = -1;
    int cls = -2; double cls_since = 0;     // classe d'allure du pilote rendu (marche, course, sprint) et depuis quand
    double last_drop_at = -10;
    double last_event_log = -10;
    std::deque<std::array<double, 3>> hist; // (instant, vitesse du pion, vitesse rendue du pilote) sur 0,3 s
};
extern FollowCtl fc;

// Position et vitesse du pilote à l'instant r (horloge du tampon). starved : r au-delà de la dernière position ;
// stale : au-delà de la prolongation permise (pilote considéré arrêté là où on l'a prolongé).
struct Rendered { double pos[3]{}; double vel[3]{}; double yaw = 0; bool starved = false; bool stale = false; };

// --- Changements de niveau : mesures (2026-10-23) -------------------------------------------------
// Chez le suiveur, pour chaque MapChange du pilote : délai avant le début du chargement d'ici, nombre de chargements,
// durée du chargement d'ici, arrivée par rapport au pilote (sa première position dans le nouveau niveau), écart au pilote
// à la reprise du suivi et temps pour le rattraper, téléportations, arrêts, pause et images lentes du mod juste après
// l'arrivée. Une ligne coop_map_transition par transition, et des compteurs pour les scénarios (« max » en ms).
struct MapTrans {
    bool open = false;
    std::string level, spawn, how;   // how : followed, already (chargement déjà en route), same_level, deferred
    int expect_loads = 1;
    double rx_at = -1;               // MapChange reçu
    double load_at = -1;             // premier chargement d'ici (avant rx_at : pion passé par le même déclencheur)
    int loads = 0;                   // chargements commencés ici pendant la transition
    double arrived_at = -1;          // monde prêt ici, dans le niveau visé
    double pilot_at = -1;            // première position du pilote reçue dans le niveau visé
    double follow_at = -1, err0 = -1, caught_at = -1;   // premier suivi après l'arrivée, son écart, écart < 250 u
    int teleports = 0, starts = 0, stops = 0, mod_slow = 0, slow = 0;
    double paused_s = 0, mod_max_ms = 0;
    double ticked_s = 0;             // temps de jeu réel après l'arrivée (images < 0,5 s : sans les gels de chargement)
};

extern MapTrans mt;
extern bool load_in;
extern double load_fall_at;
extern double load_rise_at;
extern std::string load_dest;
extern double prev_now;
extern std::string tp_level;
extern bool tp_off;
extern double tp_at;
extern int tp_count;
struct MapMax { int delay = 0, late = 0, load = 0, err = 0, catchup = 0, paused = 0, mod = 0; };
extern MapMax mmax;

constexpr double kMapWindow = 12.0;   // fenêtre d'observation après l'arrivée (et celle du pilote)

// --- Entrées de déplacement d'un suiveur ------------------------------------------------------------
// Le pion d'un suiveur est conduit par le mod (AddMovementInput) : les touches de déplacement locales sont coupées
// en retirant les contextes de locomotion (même méthode qu'en combat, vu le 2026-10-20 : seul RemoveMappingContext
// coupe vraiment une touche). Caméra, menus et dialogues restent libres. Rendus quand on redevient pilote.
constexpr const wchar_t* kLocomotionContexts[] = {
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_Default_ALS.IMC_Locomotion_Default_ALS",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_Crouch.IMC_Locomotion_Crouch",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_CustomMovement.IMC_Locomotion_CustomMovement",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_Locomotion_WorldMapCharacter.IMC_Locomotion_WorldMapCharacter",
    // Phase 5 : interactions du suiveur (touche d'interaction : PNJ, coffres…) et choix de dialogue pendant une question
    // (la réponse vient du pilote). Indices 4 et 5 : voir context_wanted_locked.
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_UI_Exploration.IMC_UI_Exploration",
    L"/Game/Gameplay/Inputs/InputMappingProfiles/IMC_UI_Dialogue.IMC_UI_Dialogue",
};

// --- Scènes synchronisées (2026-10-23) -------------------------------------------------------------
// Essai à deux PC du 2026-10-22 : le pilote déclenche une cinématique (zone traversée au bas de la corde) ; le suiveur,
// un peu derrière, n'atteint pas la zone, la barrière le fige, et il ne joue la scène qu'après la fin chez le pilote
// (« en décalé »). Le pilote publie donc la scène qu'il commence (TriggerCinematic) et sa cause ; le suiveur la
// déclenche aussitôt par la même zone (TryExecuteGameActionOnOverlap : état de la zone tenu par le jeu, pas de double
// lecture), sinon téléporte son pion sur la position du pilote au déclenchement (il traverse la même zone).
// La barrière ne sert plus qu'à attendre la fin ; son délai de grâce reste le filet (pair ancien, scène non retrouvée).
struct SceneSync {
    // Dernières causes possibles vues ici (pilote) : zone « au passage », interaction publiée, game action exécutée.
    std::string zone, zone_action; double zone_at = -1;
    std::string interaction; double interaction_at = -1;
    std::string action; double action_at = -1;
    std::string cine_action;                      // action de jeu qui a appelé TriggerCinematic (pile d'appels)
    UObject* zone_in_call = nullptr;              // zone dans TryExecuteGameActionOnOverlap (pré -> post)
    uint32_t seq = 0;
    // Dernière cinématique commencée ici
    std::string local_scene; double local_scene_at = -1;
    // Suiveur : scène du pilote à déclencher ici
    std::optional<proto::SceneStart> pending; double pending_at = -1;
    double scene_rx_at = -1;                      // dernier SceneStart reçu (mesure du retard)
    std::string acted_scene; double acted_at = -1; int acted_method = 0; bool acted_checked = true;
    // Suiveur : cinématique passée par le pilote, à passer ici aussi (réessayée : le jeu ignore un passage trop tôt)
    double skip_at = -1, next_skip_try = 0; int skip_tries = 0; bool skip_seen_playing = false;
    double skip_called_at = -1;                   // SkipCinematic appelé ici (passage effectif si la séquence finit juste après)
    // Mesures (tests) : début de la scène ici après le début chez un autre, fin ici après la fin chez lui
    std::map<uint8_t, double> remote_end_at;
    int lag_max_ms = 0, end_late_max_ms = 0, barrier_wait_max_ms = 0;
};

extern SceneSync sc;
extern DWORD g_explore_thread;
constexpr double kSceneCauseWindow = 60.0;   // cause retenue si vue dans la minute qui précède la cinématique

// --- Hash d'état d'exploration (phase 5) -----------------------------------------------------------
// L'hôte résume son état d'exploration (or, inventaire, objets du monde utilisés, ennemis vaincus, drapeaux de
// progression…) quand la partie est calme (point sûr, aucune interaction depuis quelques secondes). Chaque client
// compare au même point calme ; deux écarts de suite -> nouvel instantané au prochain point sûr (ResyncRequest).
struct WorldPart { const char* name; const wchar_t* field; };

constexpr WorldPart kWorldParts[] = {
    {"gold", L"Gold"}, {"inventory2", L"Inventory"},   // inventory2 : nouvelle empreinte (minuscules, sans tenues), 2026-10-23
     {"interacted2", L"InteractedObjects"}, {"battled", L"BattledEnemies"},
    {"flags", L"NamedIDsStates"}, {"gpe", L"GPE_States"}, {"cinematics", L"InteractedCinematics"}, {"dialogs", L"InteractedDialogs"},
};

// --- Fonctions internes (définies dans les explore*.cpp) ---
uint64_t fnv64(const std::vector<uint8_t>& b);
std::string hex64(uint64_t h);
void follow_stats_flush(bool force);
void follow_stats_rx(const proto::PilotState& p);
void bump(const char* c, int n = 1);
void bump_max(const char* name, int& cur, int v);
void mt_follow(double err, bool teleported, int started, int stopped);
bool zone_fix();
bool sharing();
void touch();
void send_world_resync(std::string differ);
bool world_ready();
bool resync_calm();
bool safe_point();
void send_snapshot(uint8_t player);
void host_join_tick();
void start_intro();
void follow_new_game();
void client_join_tick();
void on_chunk(const proto::SaveChunk& c);
void pilot_tick();
void gauge_max(const char* name, double v);
void follow_buf_reset(const char* why);
void follow_push(const proto::PilotState& p);
bool follow_render(double r, Rendered& out);
void max_speed_restore(UObject* pawn);
void sprint_start(UObject* pawn);
void sprint_off(UObject* pawn);
void sprint_stop(UObject* pawn);
void sprint_release();
void follow_event(const char* what, double v, double rv, double own, double gap);
void follow_metrics(const Rendered& R, double rv, double own, double gap_now, double rest_gap, double pilot_now_speed);
void teleport_follower(UObject* pawn, const Rendered& R, const char* why, double err, double dz, double rv, double own_speed);
void follow_tick();
void set_pilot(uint8_t p, const char* why);
void key_tick();
std::string object_path(UObject* o);
void replay_tick();
double wnow();
double mt_window_start();
bool mt_in_window();
void mt_close(const char* why);
void load_begin(const std::string& dest, const std::string& tag);
void load_end();
void load_edge_tick();
void map_tick();
bool is_menu_level(const std::string& level);
void apply_map_change(const proto::MapChange& mc);
void on_map_change(const proto::MapChange& mc);
void pending_map_tick();
UObject* input_sub();
UObject* locomotion_context(size_t i);
std::optional<int> context_priority(UObject* sub, UObject* imc);
bool restore_in_battle();
bool context_wanted_locked(size_t i);
void movement_lock_tick();
void pilot_stuck_tick();
std::vector<UObject*> choice_options();
bool widget_flag(UObject* w, const wchar_t* fn);
UObject* merchant_window();
std::vector<UObject*> merchant_entries(UObject* win);
void merchant_tick();
void choice_hover_tick();
void apply_pause();
bool others_in_narrative();
std::vector<uint8_t> narrative_players();
std::vector<uint8_t> known_players();
void waited_line_tick(bool narr);
void gpe_process_event(RC::Unreal::UObject* ctx, RC::Unreal::UFunction* fn, void* parms);
void gpe_pe_hook(RC::Unreal::Hook::TCallbackIterationData<void>&, RC::Unreal::UObject* ctx, RC::Unreal::UFunction* fn, void* parms);
std::string scene_key(const std::string& id, const std::string& seq);
UObject* zone_overlap_action(UObject* zone);
void read_cinematic_params(RC::Unreal::UnrealScriptFunctionCallableContext& ctx, UFunction* f, std::string& id, std::string& seq);
void on_local_scene(proto::NarrativeKind kind, const std::string& id, const std::string& seq);
bool scene_test_no_follow();
void scene_tick();
void poll_tick();
reflect::Leaves inventory_leaves();
reflect::Leaves interacted_leaves(reflect::Leaves in);
std::vector<proto::HashPart> world_parts(std::map<std::string, reflect::Leaves>* leaves = nullptr);
std::string leaves_line(const reflect::Leaves& l);
std::vector<std::string> split_csv(const std::string& csv);
void log_world_leaves(const std::string& part, const reflect::Leaves& l, uint32_t seq);
bool quiet(double secs);
void copy_tick();
void world_hash_tick();
void world_resync_tick();
void perturb_world_tick();

} // namespace e33::explore::detail

namespace e33::explore {
using namespace detail;
} // namespace e33::explore
