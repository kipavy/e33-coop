// Abonnements aux fonctions du jeu (hooks) de l'exploration.
#include "explore_internal.hpp"

namespace e33::explore {

void subscribe()
{
    auto& bus = HookBus::get();
    // Interactions du pilote avec les objets du monde (implémentations de BPI_Interaction:OnInteractionRequested).
    static const wchar_t* const kInteractables[] = {
        L"/Game/Gameplay/Exploration/Interaction/BP_InteractionObjectBase.BP_InteractionObjectBase_C:",
        L"/Game/Gameplay/GPE/Chests/BP_Chest_Regular.BP_Chest_Regular_C:",
        L"/Game/LevelTools/BP_GrapplePoint.BP_GrapplePoint_C:",
        L"/Game/LevelTools/Climbing/BP_Holds.BP_Holds_C:",
        L"/Game/LevelTools/Journals/BP_DocumentActor.BP_DocumentActor_C:",
        L"/Game/LevelTools/Journals/BP_ReadingCube.BP_ReadingCube_C:",
        L"/Game/LevelTools/LightSafeAreaMecanic/BP_LightSafeArea.BP_LightSafeArea_C:",
        L"/Game/LevelTools/MovementZone/BP_GameActionTriggerZone.BP_GameActionTriggerZone_C:",
        L"/Game/LevelTools/NPCs/BP_jRPG_NPC_Dialog.BP_jRPG_NPC_Dialog_C:",
        L"/Game/LevelTools/Rope/BP_GPE_Rope.BP_GPE_Rope_C:",
        L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:",
        L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_NPC_Base.BP_jRPG_NPC_Base_C:",
        L"/Game/jRPGTemplate/Blueprints/Other/World/BP_jRPG_InteractionObject_Base.BP_jRPG_InteractionObject_Base_C:",
    };
    // Personnage affiché en exploration (changement de personnage, chargement du pion) : noté pour le PilotState.
    bus.pre(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Character_World.BP_jRPG_Character_World_C:LoadAndActivateCharacter",
            [](HookCtx& ctx, UFunction* f) {
                const std::string name = ue::read_param_fname(ctx, f, STR("CharacterName"));
                if (name.empty() || name == "None") return;
                if (g_char_of.size() > 64) g_char_of.clear();   // pions des niveaux quittés
                g_char_of[ctx.Context] = name;
            });
    for (const wchar_t* cls : kInteractables) {
        bus.pre(std::wstring(cls) + L"OnInteractionRequested", [](HookCtx& ctx, UFunction* f) {
            if (s.replaying || !s.active || !sharing()) return;
            const bool instant = ue::read_param_int(ctx, f, STR("Instant")).value_or(0) != 0;
            const std::string path = object_path(ctx.Context);
            if (!is_pilot()) {
                bump("local_actions_on_follower");
                log().write("coop_follower_interaction", JsonObj().add("actor", path));
                return;
            }
            s.net.publish(proto::WorldInteract{++s.seq, path, instant}, true);
            bump("interactions_published"); touch();
            sc.interaction = path; sc.interaction_at = s.now;   // cause possible d'une cinématique (SceneStart)
            log().write("coop_interaction", JsonObj().add("actor", path).add("instant", instant));
        });
    }
    // Achat du pilote chez le marchand : signalé au suiveur (DialogueChoice.hover 4), qui voit l'article dans son miroir.
    bus.post(L"/Game/Gameplay/Inventory/Merchant/BP_MerchantComponent.BP_MerchantComponent_C:TryBuyItem", [](HookCtx&, UFunction*) {
        if (!s.active || !sharing() || !is_pilot()) return;
        s.net.publish(proto::DialogueChoice{++s.seq, std::max(0, s.merchant_sent), 4}, true);
        bump("merchant_buys_published");
        log().write("coop_merchant_buy", JsonObj().add("index", s.merchant_sent));
    });
    // Drapeau persistant écrit ici (D38) : publié s'il vient du pilote (pas d'un rejeu, ni d'une écriture du mod).
    // Session du 2026-10-25 : NID_CuratorTutorial posé chez le pilote seulement (branche de dialogue / d'interface vue
    // une seule fois), jamais chez le suiveur -> empreinte « flags » différente, instantané complet.
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:WritePersistentFlag", [](HookCtx& ctx, UFunction* f) {
        if (!s.active || s.ended) return;
        UObject* id = ue::read_param_object(ctx, f, STR("NamedID"));
        const bool value = ue::read_param_int(ctx, f, STR("Value")).value_or(0) != 0;
        if (id) on_local_flag(id, value);
    });
    // Tirs du pilote en exploration (point 4 de la liste de Killian : sphères de peinture, cages, boutons) : le projectile
    // qui touche un objet du monde cible du tir (interface BPI_FreeAimTarget_World : FreeAimHit) est publié en
    // WorldInteract « freeaim: » ; le suiveur appelle FreeAimHit sur le même objet. Les combattants du monde (ennemis,
    // pions) sont exclus : un tir sur un ennemi lance un combat, qui suit son propre chemin.
    bus.post(L"/Game/Gameplay/WorldSkills/BP_FreeAimProjectile_Base.BP_FreeAimProjectile_Base_C:OnCollision", [](HookCtx& ctx, UFunction* f) {
        if (!s.active || !sharing() || s.replaying || game::in_battle()) return;
        UObject* target = ue::read_param_object(ctx, f, STR("OtherActor"));
        static int logged = 0;
        const bool hittable = target && ue::Call(target, STR("FreeAimHit")) && !ue::is_a(target, STR("BP_jRPG_Enemy_World_Base_Seamless_C")) &&
                              !ue::is_a(target, STR("BP_jRPG_Character_World_C"));
        if (logged < 40 && ++logged)
            log().write("coop_freeaim_world_hit", JsonObj().add("target", target ? ue::short_name(target) : std::string("null"))
                                                     .add("class", target ? ue::short_name(target->GetClassPrivate()) : std::string()).add("hittable", hittable).add("pilot", is_pilot()));
        if (!hittable || !is_pilot()) return;
        const std::string path = object_path(target);
        s.net.publish(proto::WorldInteract{++s.seq, "freeaim:" + path, false}, true);
        bump("freeaim_hits_published"); touch();
    });
    RC::Unreal::Hook::RegisterProcessEventPreCallback(&gpe_pe_hook, {false, false, STR("E33Coop"), STR("ExploreGPE")});
    bus.pre(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:OnChoiceMade", [](HookCtx& ctx, UFunction* f) {
        s.choice_pending = false;
        s.choice_hover_want = -1;
        if (s.replaying || !s.active || !sharing()) return;
        const int index = static_cast<int>(ue::read_param_int(ctx, f, STR("ChoiceIndex")).value_or(-1));
        if (!is_pilot()) {
            bump("local_actions_on_follower");
            log().write("coop_follower_choice", JsonObj().add("index", index));
            return;
        }
        if (index < 0) return;
        s.net.publish(proto::DialogueChoice{++s.seq, index}, true);
        bump("choices_published"); touch();
        log().write("coop_choice", JsonObj().add("index", index));
    });
    // Question de dialogue : CurrentChoices reste vide pendant la question (vu le 2026-10-20), on suit StartChoice.
    bus.post(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:StartChoice", [](HookCtx&, UFunction*) {
        s.choice_pending = true;
        s.choice_hover_sent = s.choice_hover_shown = -1;   // le survol voulu reçu avant la question ici est gardé
        s.choice_diag = 0;
        log().write("coop_choice_asked", JsonObj());
    });
    bus.post(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceStarted", [](HookCtx&, UFunction*) {
        s.sequence_playing = true;
        log().write("coop_sequence_started", JsonObj());
    });
    bus.post(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceFinished", [](HookCtx&, UFunction*) {
        s.sequence_playing = false;
        if (s.intro) ++s.intro_sequences_done;
        log().write("coop_sequence_finished", JsonObj());
    });
    // Scènes synchronisées : causes vues ici (zone « au passage », game action), début et passage d'une cinématique.
    bus.pre(L"/Game/LevelTools/MovementZone/BP_GameActionTriggerZone.BP_GameActionTriggerZone_C:TryExecuteGameActionOnOverlap", [](HookCtx& ctx, UFunction*) {
        sc.zone_in_call = ctx.Context;
    });
    bus.post(L"/Game/LevelTools/MovementZone/BP_GameActionTriggerZone.BP_GameActionTriggerZone_C:TryExecuteGameActionOnOverlap", [](HookCtx&, UFunction*) {
        sc.zone_in_call = nullptr;
    });
    bus.pre(L"/Game/Gameplay/GameActionsSystem/BP_GameActionExecutorComponent.BP_GameActionExecutorComponent_C:ExecuteGameActionWrapperFromTriggerZone", [](HookCtx&, UFunction*) {
        if (!sc.zone_in_call) return;
        UObject* action = zone_overlap_action(sc.zone_in_call);
        sc.zone = object_path(sc.zone_in_call);
        sc.zone_action = action ? object_path(action) : std::string();
        sc.zone_at = s.now;
        log().write("coop_zone_executed", JsonObj().add("zone", sc.zone).add("action", sc.zone_action).add("replaying", s.replaying));
    });
    bus.pre(L"/Game/Gameplay/GameActionsSystem/BP_GameActionExecutorComponent.BP_GameActionExecutorComponent_C:ExecuteGameAction", [](HookCtx& ctx, UFunction* f) {
        UObject* a = ue::read_param_object(ctx, f, STR("GameAction"));
        if (!a) return;
        sc.action = object_path(a);
        sc.action_at = s.now;
    });
    bus.pre(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:TriggerCinematic", [](HookCtx& ctx, UFunction* f) {
        std::string id, seq;
        read_cinematic_params(ctx, f, id, seq);
        // Action de jeu qui lance cette cinématique (pile d'appels) : le suiveur l'exécute si sa zone ne lance rien.
        sc.cine_action.clear();
        std::string frames;
        RC::Unreal::FFrame* fr = ctx.TheStack.PreviousFrame();
        for (int depth = 0; fr && depth < 8; ++depth) {
            UObject* o = fr->Object();
            const std::string cls = o ? ue::short_name(o->GetClassPrivate()) : std::string();
            if (frames.size() < 400) frames += cls + ";";
            if (sc.cine_action.empty() && o && cls.find("GameAction") != std::string::npos && cls.find("Component") == std::string::npos)
                sc.cine_action = object_path(o);
            RC::Unreal::FFrame* prev = fr->PreviousFrame();
            fr = prev == fr ? nullptr : prev;
        }
        log().write("coop_cinematic_caller", JsonObj().add("action", sc.cine_action).add("frames", frames));
        on_local_scene(proto::NarrativeKind::Cinematic, id, seq);
    });
    // Dialogue (D24) : ActivateDialogue(Dialogue) du composant de dialogue, quelle que soit sa cause (PNJ, zone, action).
    bus.pre(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:ActivateDialogue", [](HookCtx& ctx, UFunction* f) {
        UObject* d = ue::read_param_object(ctx, f, STR("Dialogue"));
        if (!d) return;
        on_local_scene(proto::NarrativeKind::Dialogue, "", object_path(d));
    });
    // Passage : le jeu ignore parfois SkipCinematic (trop tôt, scène impossible à passer) ; on ne publie qu'un passage
    // effectif, vu à la fin de la séquence peu après l'appel (OnSequenceFinished, avant la transition de sortie).
    bus.pre(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:SkipCinematic", [](HookCtx&, UFunction*) {
        sc.skip_called_at = s.now;
        log().write("coop_skip_called", JsonObj().add("playing", game::playing_cinematic()).add("remote", sc.skip_at >= 0));
    });
    bus.post(L"/Game/Gameplay/CinematicSystem/BP_CinematicSystem.BP_CinematicSystem_C:OnSequenceFinished", [](HookCtx&, UFunction*) {
        if (sc.skip_called_at < 0 || s.now - sc.skip_called_at > 5.0) return;
        sc.skip_called_at = -1;
        if (!s.active || s.ended || !is_pilot() || !sharing() || sc.skip_at >= 0) return;
        s.net.publish(proto::SceneSkip{s.me, ++sc.seq, sc.local_scene}, true);
        bump("scene_skips_published");
        log().write("coop_scene_skip", JsonObj().add("scene", sc.local_scene));
    });
    bus.post(L"/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:EndDialogue", [](HookCtx&, UFunction*) { s.choice_pending = false; });
    // Nouvelle partie ensemble : l'hôte lance une nouvelle partie -> les clients qui attendent au menu suivent.
    bus.pre(L"/Game/Gameplay/Save/BP_SaveManager.BP_SaveManager_C:StartNewGame", [](HookCtx&, UFunction*) {
        if (!s.active || s.ended) return;
        start_intro();
        if (!s.host) return;   // client : c'est la nôtre (follow_new_game)
        int n = 0;
        for (auto& [player, pj] : s.peers) {
            if (pj.state != JoinState::Pending && pj.state != JoinState::Waiting) continue;
            s.net.send_to(player, proto::JoinWait{"new_game"});
            pj.state = JoinState::Waiting;
            pj.wait_sent = true;
            ++n;
        }
        bump("new_games_announced");
        log().write("coop_new_game", JsonObj().add("players_following", n));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:OnInitialCinematicCompleted", [](HookCtx&, UFunction*) {
        if (!s.intro) return;
        s.intro = false;
        bump("intros_finished");
        log().write("coop_intro_finished", JsonObj().add("after_s", s.now - s.intro_since));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:ChangeMap", [](HookCtx&, UFunction*) {
        if (s.replaying || !s.active || s.ended || !is_pilot() || !sharing()) return;   // pas pendant le chargement de l'instantané
        std::string level, tag;
        if (!game::map_destination(level, tag) || is_menu_level(level)) return;   // retour au menu : pas une transition
        s.net.publish(proto::MapChange{++s.seq, level, tag}, true);
        bump("map_changes_published"); touch();
        log().write("coop_map_change", JsonObj().add("level", level).add("spawn", tag));
    });
    bus.post(L"/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle", [](HookCtx& ctx, UFunction* f) {
        s.last_enemy = ue::short_name(ctx.Context);
        s.last_engage = static_cast<int>(ue::read_param_int(ctx, f, STR("EngagementType")).value_or(0));
        s.last_enemy_at = s.now;
        log().write("coop_world_battle", JsonObj().add("enemy", s.last_enemy).add("engage", s.last_engage));
    });
}

} // namespace e33::explore
