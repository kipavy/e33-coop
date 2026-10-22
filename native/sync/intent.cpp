#include "intent.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FStrProperty.hpp>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"
#include "state.hpp"

using namespace RC::Unreal;

namespace e33::intent {

namespace {

constexpr double kPollS = 0.1;              // relevé de l'intention locale (10 fois/s au plus)
constexpr double kAimViewS = 0.05;          // caméra de visée envoyée ~20 fois/s
constexpr const char* kSlot = "turn_intent";   // après « Tour de X » dans la carte d'état (lignes triées par nom)

Net g_net;
bool g_aim_cam = true;                      // -e33aimcam=0 : visée rejouée avec la caméra du jeu (comportement d'avant)

void bump(const char* c, int n = 1) { if (g_net.bump) g_net.bump(c, n); }
BattleRef battle() { return g_net.battle ? g_net.battle() : BattleRef{}; }

// --- Textes du jeu --------------------------------------------------------------------------

// FText -> UTF-8 (KismetTextLibrary::Conv_TextToString). La copie brute du FText n'est jamais détruite (pas de
// compteur touché) ; la chaîne rendue n'est pas libérée : appels rares (noms mis en cache).
std::string ftext_utf8(const void* ftext)
{
    if (!ftext) return {};
    UObject* ktl = ue::find_object(STR("/Script/Engine.Default__KismetTextLibrary"));
    ue::Call c(ktl, STR("Conv_TextToString"));
    if (!c) return {};
    c.set_raw(STR("InText"), ftext);
    if (!c.invoke()) return {};
    const auto* s = static_cast<const FString*>(c.param_ptr(STR("ReturnValue")));
    const wchar_t* w = s ? **s : nullptr;
    return w ? ue::to_utf8(w) : std::string();
}

std::map<std::string, std::string> g_skill_names, g_item_names;
std::map<std::tuple<uint32_t, int, int>, std::string> g_unit_names;   // (combat, camp, emplacement) -> nom affiché

std::string skill_name(UObject* hero, const std::string& id)
{
    auto it = g_skill_names.find(id);
    if (it != g_skill_names.end()) return it->second;
    std::string name;
    ue::Call c(game::stats(hero), STR("FindEquippedSkillByID"));
    if (c) {
        const std::wstring w = ue::from_utf8(id);
        c.set_fname(STR("SkillID"), w.c_str()).invoke();
        if (UObject* asset = c.out_object(STR("Skill"))) name = ftext_utf8(ue::value_ptr<void>(asset, STR("Name")));
    }
    if (name.empty()) name = id;   // attaque gradient ou compétence absente de la liste équipée : identifiant brut
    g_skill_names[id] = name;
    return name;
}

// Nom d'un objet : GI.Inventory[i].ItemStaticData.Item_DisplayName de la ligne dont Item_HardcodedName vaut id.
std::string item_name(const std::string& id)
{
    auto it = g_item_names.find(id);
    if (it != g_item_names.end()) return it->second;
    std::string name;
    UObject* g = game::gi();
    if (auto* ap = CastField<FArrayProperty>(ue::prop(g, STR("Inventory")))) {
        auto* inner = CastField<FStructProperty>(ap->GetInner());
        auto* arr = ap->ContainerPtrToValuePtr<FScriptArray>(g);
        auto* stat = inner ? CastField<FStructProperty>(ue::struct_member(inner->GetStruct(), STR("ItemStaticData"))) : nullptr;
        FProperty* idp = stat ? ue::struct_member(stat->GetStruct(), STR("Item_HardcodedName")) : nullptr;
        FProperty* disp = stat ? ue::struct_member(stat->GetStruct(), STR("Item_DisplayName")) : nullptr;
        auto* data = arr ? static_cast<uint8_t*>(arr->GetData()) : nullptr;
        for (int32 i = 0; data && idp && disp && i < arr->Num(); ++i) {
            void* st = stat->ContainerPtrToValuePtr<void>(data + static_cast<size_t>(i) * inner->GetSize());
            if (ue::to_utf8(static_cast<FName*>(idp->ContainerPtrToValuePtr<void>(st))->ToString()) != id) continue;
            name = ftext_utf8(disp->ContainerPtrToValuePtr<void>(st));
            break;
        }
    }
    if (name.empty()) name = id;
    g_item_names[id] = name;
    return name;
}

std::string unit_name(uint32_t battle_id, const proto::Unit& u)
{
    const auto key = std::make_tuple(battle_id, static_cast<int>(u.kind), static_cast<int>(u.spot));
    auto it = g_unit_names.find(key);
    if (it != g_unit_names.end()) return it->second;
    UObject* o = game::find_unit(u.kind == proto::ActorKind::Hero, u.spot);
    if (!o) return "?";
    std::string name = ftext_utf8(ue::value_ptr<void>(game::stats(o), STR("CharacterDisplayName")));
    if (name.empty()) name = game::hardcoded_name(o);
    if (name.empty()) name = game::char_class(o);
    g_unit_names[key] = name;
    return name;
}

// --- Intention d'un héros d'ici (propriétaire) ----------------------------------------------

struct Local {
    uint32_t battle = 0, turn = 0;
    int spot = -1;
    uint8_t action = 0;                     // choisi pendant ce tour (jamais la valeur restée du tour d'avant)
    std::string secondary;
    bool done = false;                      // action lancée : plus d'aperçu
    bool sent = false;
    uint16_t seq = 0;
    proto::IntentPreview last;
    double next_poll = 0;
};
Local g_loc;

// Tour d'un héros d'ici en cours : état local (remis à zéro à chaque nouveau tour), nul sinon.
Local* local_turn()
{
    const BattleRef b = battle();
    if (!b.active) return nullptr;
    UObject* cur = game::current_character();
    if (!cur || !game::is_player(cur)) return nullptr;
    const int spot = game::spot(cur);
    if (!g_net.controls || !g_net.controls(spot)) return nullptr;
    if (g_loc.battle != b.id || g_loc.turn != b.turn || g_loc.spot != spot) {
        g_loc = Local{};
        g_loc.battle = b.id;
        g_loc.turn = b.turn;
        g_loc.spot = spot;
    }
    if (g_loc.done || (g_net.action_known && g_net.action_known(b.turn, spot))) return nullptr;
    return &g_loc;
}

bool same(const proto::IntentPreview& a, const proto::IntentPreview& b)
{
    if (a.action_type != b.action_type || a.aiming != b.aiming || a.secondary != b.secondary || a.targets.size() != b.targets.size()) return false;
    for (size_t i = 0; i < a.targets.size(); ++i)
        if (a.targets[i].kind != b.targets[i].kind || a.targets[i].spot != b.targets[i].spot) return false;
    return true;
}

void poll_local(double now)
{
    Local* l = local_turn();
    if (!l || now < l->next_poll) return;
    l->next_poll = now + kPollS;
    proto::IntentPreview p;
    p.battle_id = l->battle;
    p.turn = l->turn;
    p.spot = static_cast<uint8_t>(l->spot);
    p.action_type = l->action;
    p.secondary = l->secondary;
    p.aiming = g_net.local_aiming && g_net.local_aiming();
    // Cibles : celles du gestionnaire de ciblage tant qu'il cible (curseur du jeu, flèches gauche/droite ou souris).
    UObject* tm = ue::get_object(game::controller(), STR("AC_jRPG_TargetingManager"));
    if (tm && ue::get_bool(tm, STR("TargetingEnabled?")))
        for (UObject* t : ue::get_object_array(tm, STR("TargetedCharacters")))
            if (t && p.targets.size() < 16) p.targets.push_back(sync::unit_of(t));
    const bool empty = p.action_type == 0 && !p.aiming && p.targets.empty();
    if (same(p, l->last) && (l->sent || empty)) return;
    if (!l->sent && empty) return;   // début de tour : rien à montrer
    p.seq = ++l->seq;
    l->last = p;
    l->sent = true;
    if (g_net.publish) g_net.publish(p, true);
    bump("intent_previews_sent");
    log().write("intent_local", JsonObj().raw("msg", proto::to_json(p)), l->turn);
}

// --- Aperçu des autres (observateur) --------------------------------------------------------

std::map<std::tuple<uint32_t, uint32_t, int>, proto::IntentPreview> g_remote;   // (combat, tour, emplacement) -> dernier
double g_next_show = 0;
bool g_shown = false;

void show(double now)
{
    if (now < g_next_show) return;
    g_next_show = now + kPollS;
    const BattleRef b = battle();
    const proto::IntentPreview* p = nullptr;
    UObject* cur = b.active ? game::current_character() : nullptr;
    if (cur && game::is_player(cur)) {
        const int spot = game::spot(cur);
        auto it = g_remote.find({b.id, b.turn, spot});
        if (it != g_remote.end() && !(g_net.controls && g_net.controls(spot)) && !(g_net.action_known && g_net.action_known(b.turn, spot))) p = &it->second;
    }
    std::string what;
    std::vector<std::string> names;
    if (p) {
        if (p->aiming) what = ui::text("intent_free_aim");
        else switch (p->action_type) {
            case game::ActAttack: what = ui::text("intent_attack"); break;
            case game::ActSkill: what = p->secondary.empty() ? ui::text("intent_skill_menu") : skill_name(cur, p->secondary); break;
            case game::ActItem: what = p->secondary.empty() ? ui::text("intent_item_menu") : item_name(p->secondary); break;
            default: break;   // passer / fuir : immédiats, rien à prévisualiser
        }
        for (auto& t : p->targets) names.push_back(unit_name(b.id, t));
    }
    if (what.empty() && names.empty()) {
        if (g_shown) ui::clear_status(kSlot);
        g_shown = false;
        return;
    }
    std::string targets;
    for (size_t i = 0; i < names.size() && i < 3; ++i) targets += (i ? ", " : "") + names[i];
    if (names.size() > 3) targets += " +" + std::to_string(names.size() - 3);
    const std::string player = g_net.name_of && g_net.owner_of_spot ? g_net.name_of(g_net.owner_of_spot(p->spot)) : "?";
    if (what.empty()) ui::set_status(kSlot, "intent_targets", {player, targets});
    else if (targets.empty()) ui::set_status(kSlot, "intent_action", {player, what});
    else ui::set_status(kSlot, "intent_target", {player, what, targets});
    g_shown = true;
}

// --- Caméra de la visée libre ------------------------------------------------------------------

// Caméra du propriétaire reçue, par (combat, tour, emplacement) ; une nouvelle visée (t_ms qui repart de 0) remplace l'ancienne.
std::map<std::tuple<uint32_t, uint32_t, int>, std::vector<proto::AimView>> g_views;
double g_next_view = 0;

struct Cam {
    UObject* actor = nullptr;               // CameraActor du mod
    UObject* pc = nullptr;                  // contrôleur au moment de la création (monde)
    UObject* prev = nullptr;                // cible de vue du jeu avant la visée
    bool engaged = false;
    double engaged_at = 0, next_reassert = 0, destroy_at = -1;
    bool placed = false;
    double pos[3]{}, pitch = 0, yaw = 0;
    bool from_owner = false;
};
Cam g_cam;

void set_float(ue::Call& c, const wchar_t* name, float v)
{
    if (c.param_size(name) == 4) if (auto* d = static_cast<float*>(c.param_ptr(name))) *d = v;
}

UObject* view_target(UObject* pc)
{
    ue::Call c(pc, STR("GetViewTarget"));
    if (!c || !c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

bool set_view_target(UObject* pc, UObject* target, float blend)
{
    ue::Call c(pc, STR("SetViewTargetWithBlend"));
    if (!c || !target) return false;
    c.set_object(STR("NewViewTarget"), target).set_int(STR("blendFunc"), 4 /* VTBlend_EaseInOut */).set_bool(STR("bLockOutgoing"), false);
    set_float(c, STR("BlendTime"), blend);
    set_float(c, STR("BlendExp"), 2.0f);
    return c.invoke();
}

// CameraActor du moteur, créé dans le monde du contrôleur (BeginDeferredActorSpawnFromClass + FinishSpawningActor).
UObject* spawn_camera(UObject* pc, const double pos[3])
{
    UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
    UObject* cls = ue::find_object(STR("/Script/Engine.CameraActor"));
    ue::Call b(gps, STR("BeginDeferredActorSpawnFromClass"));
    if (!b || !cls) { log().write("aim_camera_error", JsonObj().add("what", "spawn functions or CameraActor class not found")); return nullptr; }
    // FTransform (doubles, alignés sur 16) : rotation (quaternion), translation, échelle.
    alignas(16) double t[12] = {0, 0, 0, 1, pos[0], pos[1], pos[2], 0, 1, 1, 1, 0};
    if (b.param_size(STR("SpawnTransform")) != sizeof t) {
        log().write("aim_camera_error", JsonObj().add("what", "unexpected FTransform size").add("size", static_cast<int64_t>(b.param_size(STR("SpawnTransform")))));
        return nullptr;
    }
    b.set_object(STR("WorldContextObject"), pc).set_object(STR("ActorClass"), cls).set_raw(STR("SpawnTransform"), t)
        .set_int(STR("CollisionHandlingOverride"), 1 /* AlwaysSpawn */).set_object(STR("Owner"), nullptr).set_int(STR("TransformScaleMethod"), 1);
    if (!b.invoke()) return nullptr;
    UObject* actor = b.out_object(STR("ReturnValue"));
    if (!actor) { log().write("aim_camera_error", JsonObj().add("what", "spawn returned null")); return nullptr; }
    ue::Call f(gps, STR("FinishSpawningActor"));
    if (f) f.set_object(STR("Actor"), actor).set_raw(STR("SpawnTransform"), t).set_int(STR("TransformScaleMethod"), 1).invoke();
    // Pas de bandes noires : l'image garde le format de l'écran.
    ue::set_bool(ue::get_object(actor, STR("CameraComponent")), STR("bConstrainAspectRatio"), false);
    return actor;
}

// Réglages d'image de la caméra d'épaule du héros (celle de la visée du jeu) : post-traitement (exposition…) et champ de vision.
void copy_camera_look(UObject* from, UObject* to)
{
    if (!from || !to) return;
    for (const wchar_t* name : {STR("PostProcessSettings"), STR("PostProcessBlendWeight"), STR("FieldOfView")}) {
        FProperty* dst = ue::prop(to, name);
        if (dst && dst == ue::prop(from, name)) dst->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(to), dst->ContainerPtrToValuePtr<void>(from));
    }
}

void place_camera(const double pos[3], double pitch, double yaw)
{
    ue::Call c(g_cam.actor, STR("K2_SetActorLocationAndRotation"));
    if (!c) return;
    if (auto* d = static_cast<double*>(c.param_ptr(STR("NewLocation")))) { d[0] = pos[0]; d[1] = pos[1]; d[2] = pos[2]; }
    if (auto* r = static_cast<double*>(c.param_ptr(STR("NewRotation")))) { r[0] = pitch; r[1] = yaw; r[2] = 0; }
    c.set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
}

double wrap180(double a)
{
    while (a > 180) a -= 360;
    while (a < -180) a += 360;
    return a;
}

// Pose de repli : derrière l'épaule droite du héros, regard vers le point visé.
bool fallback_pose(UObject* hero, const double aim[3], double pos[3], double& pitch, double& yaw)
{
    double h[3];
    if (!game::location(hero, h)) return false;
    double fx = aim[0] - h[0], fy = aim[1] - h[1];
    const double n = std::sqrt(fx * fx + fy * fy);
    if (n < 1) { fx = 1; fy = 0; } else { fx /= n; fy /= n; }
    const double rx = -fy, ry = fx;   // droite (UE : X avant, Y droite, Z haut)
    pos[0] = h[0] - fx * 260 + rx * 70;
    pos[1] = h[1] - fy * 260 + ry * 70;
    pos[2] = h[2] + 80;
    const double dx = aim[0] - pos[0], dy = aim[1] - pos[1], dz = aim[2] - pos[2];
    yaw = std::atan2(dy, dx) * 180.0 / 3.14159265358979;
    pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0 / 3.14159265358979;
    return true;
}

void release_camera(double now, const char* why)
{
    if (!g_cam.engaged) return;
    g_cam.engaged = false;
    UObject* pc = game::controller();
    if (pc && pc == g_cam.pc) {
        // Le jeu a déjà repris la vue (fin de visée) : on ne touche à rien ; sinon retour à sa vue d'avant.
        const UObject* cur = view_target(pc);
        if ((cur == g_cam.actor || now - g_cam.engaged_at < 1.0) && g_cam.prev) set_view_target(pc, g_cam.prev, 0.4f);
        g_cam.destroy_at = now + 1.5;   // après le fondu
    } else {
        g_cam = Cam{};   // autre monde : la caméra a disparu avec l'ancien
    }
    bump("aim_camera_released");
    log().write("aim_camera_released", JsonObj().add("why", why));
}

} // namespace

void init(const Net& net)
{
    g_net = net;
    g_aim_cam = cmdline::arg(L"e33aimcam") != L"0";
}

void subscribe()
{
    using game::path::fn;
    auto& bus = HookBus::get();
    const wchar_t* bm = game::path::BattleMgr;
    // Roue : action choisie (attaque, compétences, objets…) puis compétence / objet. Valeurs lues après l'appel.
    bus.post(fn(bm, L"SelectAction"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) {
            l->action = static_cast<uint8_t>(ue::get_int(game::battle_manager(), STR("SelectedAction"), 0));
            l->secondary.clear();
        }
    });
    bus.post(fn(bm, L"SelectSecondaryAction"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) l->secondary = ue::get_name_string(game::battle_manager(), STR("SelectedSecondaryAction"));
    });
    // Page des compétences ouverte (aucune choisie encore).
    bus.post(fn(bm, L"OnSelectSkillMenu"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) { l->action = 5; l->secondary.clear(); }
    });
    // Retour en arrière : ciblage annulé (retour à la liste, ou à la roue pour l'attaque), liste des compétences fermée.
    bus.post(fn(bm, L"OnCancelTargeting"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) {
            if (!l->secondary.empty()) l->secondary.clear();
            else l->action = 0;
        }
    });
    bus.post(fn(bm, L"OnSkillSelectionClosed"), [](HookCtx&, UFunction*) {
        if (Local* l = local_turn()) { l->action = 0; l->secondary.clear(); }
    });
    // Action lancée : l'aperçu s'arrête (l'action elle-même part par le module coop).
    for (const wchar_t* name : {L"RequestActionExecution", L"OnSelectSkip", L"OnSelectFlee"})
        bus.pre(fn(bm, name), [](HookCtx&, UFunction*) { if (Local* l = local_turn()) l->done = true; });
}

void receive(const proto::IntentPreview& m)
{
    const BattleRef b = battle();
    if (m.battle_id != b.id) return;
    auto& slot = g_remote[{m.battle_id, m.turn, m.spot}];
    if (slot.seq != 0 && m.seq <= slot.seq) return;   // en retard
    slot = m;
    bump("intent_previews_received");
    // Ménage : tours passés et combats précédents.
    for (auto it = g_remote.begin(); it != g_remote.end();) {
        if (std::get<0>(it->first) != b.id || std::get<1>(it->first) + 1 < b.turn) it = g_remote.erase(it);
        else ++it;
    }
}

void receive(const proto::AimView& m)
{
    const BattleRef b = battle();
    if (m.battle_id != b.id) return;
    auto& v = g_views[{m.battle_id, m.turn, m.spot}];
    if (!v.empty() && m.t_ms + 500 < v.back().t_ms) v.clear();   // nouvelle visée dans le même tour
    if (v.size() < 4000) v.push_back(m);
    bump("aim_views_received");
    for (auto it = g_views.begin(); it != g_views.end();) {
        if (std::get<0>(it->first) != b.id || std::get<1>(it->first) + 1 < b.turn) it = g_views.erase(it);
        else ++it;
    }
}

void tick(double now)
{
    if (!battle().active) {
        if (!g_remote.empty() || !g_views.empty()) { g_remote.clear(); g_views.clear(); g_unit_names.clear(); }
    }
    poll_local(now);
    show(now);
}

void aim_owner_tick(double now, int spot, uint32_t t_ms)
{
    if (now < g_next_view) return;
    g_next_view = now + kAimViewS;
    const BattleRef b = battle();
    UObject* cm = ue::get_object(game::controller(), STR("PlayerCameraManager"));
    if (!cm || !b.active) return;
    proto::AimView v;
    v.battle_id = b.id;
    v.turn = b.turn;
    v.spot = static_cast<uint8_t>(spot);
    v.t_ms = t_ms;
    {
        ue::Call c(cm, STR("GetCameraLocation"));
        if (!c || !c.invoke()) return;
        const auto* d = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
        if (!d) return;
        v.x = d[0]; v.y = d[1]; v.z = d[2];
    }
    {
        ue::Call c(cm, STR("GetCameraRotation"));
        if (!c || !c.invoke()) return;
        const auto* r = static_cast<const double*>(c.param_ptr(STR("ReturnValue")));
        if (!r) return;
        v.pitch = r[0]; v.yaw = r[1];
    }
    {
        ue::Call c(cm, STR("GetFOVAngle"));
        if (c && c.invoke()) v.fov = c.out_double(STR("ReturnValue"), 0);
        if (!std::isfinite(v.fov) || v.fov < 0 || v.fov > 180) v.fov = 0;
    }
    if (g_net.publish) g_net.publish(v, false);
    bump("aim_views_sent");
}

void aim_camera_tick(double now, double dt, const AimReplay* r)
{
    // Caméra rendue : destruction après le fondu, dans le même monde seulement.
    if (!g_cam.engaged && g_cam.actor && g_cam.destroy_at > 0 && now >= g_cam.destroy_at) {
        if (game::controller() == g_cam.pc) {
            ue::Call d(g_cam.actor, STR("K2_DestroyActor"));
            if (d) d.invoke();
        }
        g_cam = Cam{};
    }
    if (!r || !r->hero || !g_aim_cam) { release_camera(now, "replay_over"); return; }
    UObject* pc = game::controller();
    if (!pc) return;
    if (g_cam.actor && pc != g_cam.pc) g_cam = Cam{};   // changement de monde : l'acteur n'existe plus
    // Pose voulue : caméra du propriétaire au même instant de sa visée, sinon derrière l'épaule vers le point visé.
    const BattleRef b = battle();
    double pos[3]{}, pitch = 0, yaw = 0, fov = 0;
    bool have = false, from_owner = false;
    auto it = g_views.find({b.id, b.turn, r->spot});
    if (it != g_views.end()) {
        const proto::AimView* best = nullptr;
        for (auto& v : it->second) if (v.t_ms <= r->elapsed_ms && (!best || v.t_ms >= best->t_ms)) best = &v;
        double h[3];
        // Garde-fou : la caméra du propriétaire doit être près du même héros ici (arène identique).
        if (best && game::location(r->hero, h) && std::hypot(best->x - h[0], best->y - h[1], best->z - h[2]) < 2000) {
            pos[0] = best->x; pos[1] = best->y; pos[2] = best->z;
            pitch = best->pitch; yaw = best->yaw; fov = best->fov;
            have = from_owner = true;
        }
    }
    if (!have && r->have_point) have = fallback_pose(r->hero, r->point, pos, pitch, yaw);
    if (!have) return;   // rien à montrer encore : la vue du jeu reste
    if (!g_cam.actor) {
        g_cam = Cam{};
        g_cam.actor = spawn_camera(pc, pos);
        g_cam.pc = pc;
        if (!g_cam.actor) { g_aim_cam = false; return; }   // échec : on n'insiste pas (comportement d'avant)
    }
    g_cam.destroy_at = -1;
    if (!g_cam.engaged || !g_cam.placed) {
        std::copy(pos, pos + 3, g_cam.pos);
        g_cam.pitch = pitch;
        g_cam.yaw = yaw;
        g_cam.placed = true;
    } else {
        // Lissage (paquets non fiables, ~20 par seconde).
        const double a = 1.0 - std::exp(-dt * 12.0);
        for (int i = 0; i < 3; ++i) g_cam.pos[i] += (pos[i] - g_cam.pos[i]) * a;
        g_cam.pitch += wrap180(pitch - g_cam.pitch) * a;
        g_cam.yaw += wrap180(yaw - g_cam.yaw) * a;
    }
    place_camera(g_cam.pos, g_cam.pitch, g_cam.yaw);
    if (fov > 1) if (auto* f = ue::value_ptr<float>(ue::get_object(g_cam.actor, STR("CameraComponent")), STR("FieldOfView"))) *f = static_cast<float>(fov);
    if (from_owner != g_cam.from_owner) {
        g_cam.from_owner = from_owner;
        log().write("aim_camera_source", JsonObj().add("from_owner", from_owner).add("spot", r->spot));
    }
    if (!g_cam.engaged) {
        copy_camera_look(ue::get_object(r->hero, STR("Camera_Shoulder")), ue::get_object(g_cam.actor, STR("CameraComponent")));
        if (fov > 1) if (auto* f = ue::value_ptr<float>(ue::get_object(g_cam.actor, STR("CameraComponent")), STR("FieldOfView"))) *f = static_cast<float>(fov);
        g_cam.prev = view_target(pc);
        set_view_target(pc, g_cam.actor, 0.35f);
        g_cam.engaged = true;
        g_cam.engaged_at = now;
        g_cam.next_reassert = now + 1.0;
        bump("aim_camera_engaged");
        log().write("aim_camera_engaged", JsonObj().add("spot", r->spot).add("from_owner", from_owner).add("prev", ue::short_name(g_cam.prev))
                                             .add("x", pos[0]).add("y", pos[1]).add("z", pos[2]).add("pitch", pitch).add("yaw", yaw));
        return;
    }
    // Le jeu a remis sa propre vue pendant la visée (caméra de visée, de combat…) : on reprend la main, sans à-coup.
    if (now >= g_cam.next_reassert) {
        g_cam.next_reassert = now + 0.5;
        if (UObject* cur = view_target(pc); cur != g_cam.actor) {
            set_view_target(pc, g_cam.actor, 0.2f);
            bump("aim_camera_reasserted");
            log().write("aim_camera_reasserted", JsonObj().add("game_view", ue::short_name(cur)));
        }
    }
}

} // namespace e33::intent
