// Interactions et choix de dialogue rejoués, miroir du marchand.
#include "explore_internal.hpp"

namespace e33::explore::detail {

// --- Interactions, choix, transitions rejoués -----------------------------------------------------
std::string object_path(UObject* o)
{
    // full_name : "Classe /Game/...:PersistentLevel.Nom" -> chemin après le premier espace
    const std::string f = ue::full_name(o);
    const auto sp = f.find(' ');
    return sp == std::string::npos ? f : f.substr(sp + 1);
}

void replay_tick()
{
    if (!s.host && s.want_join && s.cj != S::ClientJoin::Joined) return;
    if (!s.interactions.empty() && world_ready()) {
        const auto wi = s.interactions.front();
        s.interactions.pop_front();
        // Tir du pilote sur un objet du monde : même coup ici (FreeAimHit avec notre pion, au centre de l'objet).
        if (wi.actor.rfind("freeaim:", 0) == 0) {
            const std::string path = wi.actor.substr(8);
            UObject* target = ue::find_object(ue::from_utf8(path).c_str());
            bool ok = false;
            if (target) {
                ue::Call c(target, STR("FreeAimHit"));
                double loc[3]{};
                game::location(target, loc);
                if (c && c.param_size(STR("HitLocation")) == sizeof(loc)) {
                    s.replaying = true;
                    ok = c.set_object(STR("PlayerCharacterWorld"), game::world_pawn()).set_raw(STR("HitLocation"), loc).set_bool(STR("IsDisabled"), false).invoke();
                    s.replaying = false;
                }
            }
            bump(ok ? "freeaim_hits_replayed" : "freeaim_hit_errors"); touch();
            log().write("coop_freeaim_world_replayed", JsonObj().add("actor", path).add("found", target != nullptr).add("ok", ok));
            return;
        }
        UObject* target = ue::find_object(ue::from_utf8(wi.actor).c_str());
        if (!target) {
            bump("interactions_missing");
            log().write("coop_interaction_missing", JsonObj().add("actor", wi.actor));
        } else {
            s.replaying = true;
            const bool ok = game::interact(target, wi.instant);
            s.replaying = false;
            bump(ok ? "interactions_replayed" : "interaction_errors"); touch();
            log().write("coop_interaction_replayed", JsonObj().add("actor", wi.actor).add("ok", ok));
        }
    }
    if (!s.choices.empty()) {
        auto& [dc, at] = s.choices.front();
        if (s.choice_pending) {
            s.replaying = true;
            const bool ok = game::choose_dialogue(dc.index);
            s.replaying = false;
            bump(ok ? "choices_replayed" : "choice_errors"); touch();
            log().write("coop_choice_replayed", JsonObj().add("index", dc.index).add("ok", ok).add("waited_s", s.now - at));
            s.choices.pop_front();
        } else if (s.now - at > 120) {
            bump("choice_timeouts");
            log().write("coop_choice_timeout", JsonObj().add("index", dc.index));
            s.choices.pop_front();
        }
    }
}

// Choix de dialogue survolé par le pilote (demande de Killian, 2026-10-24) : le suiveur voit la même option en
// surbrillance avant le choix. Options = WBP_Dialogue_ChoiceOption_C visibles, rangées par leur place dans leur parent.
std::vector<UObject*> choice_options()
{
    std::vector<UObject*> all, out;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("WBP_Dialogue_ChoiceOption_C"), all);
    std::vector<std::pair<int, UObject*>> v;
    for (UObject* o : all) {
        if (!o) continue;
        ue::Call vis(o, STR("IsVisible"));
        if (!vis || !vis.invoke() || !vis.out_bool(STR("ReturnValue"))) continue;
        ue::Call par(o, STR("GetParent"));
        UObject* parent = par && par.invoke() ? par.out_object(STR("ReturnValue")) : nullptr;
        int idx = static_cast<int>(v.size()) + 100;
        if (parent) { ue::Call ci(parent, STR("GetChildIndex")); if (ci) { ci.set_object(STR("Content"), o); if (ci.invoke()) idx = static_cast<int>(ci.out_int(STR("ReturnValue"), idx)); } }
        v.emplace_back(idx, o);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [i, o] : v) out.push_back(o);
    return out;
}

bool widget_flag(UObject* w, const wchar_t* fn)
{
    ue::Call c(w, fn);
    return c && c.invoke() && c.out_bool(STR("ReturnValue"));
}

// Marchand (essai à deux PC du 2026-10-24, demande de Killian) : le choix « Acheter » du pilote est rejoué chez le
// suiveur, qui avait alors sa propre fenêtre du marchand, entrées libres (achats séparés, combat du marchand lancé chez
// lui). Désormais la fenêtre du suiveur est un miroir : ses boutons (acheter, combattre, partir) sont désactivés, il voit
// l'article sélectionné par le pilote (DialogueChoice.hover 2 = index de l'article), et elle se ferme quand le pilote
// ferme la sienne (hover 3). Les achats du pilote lui arrivent par la synchro de l'inventaire et de l'or (D13).
UObject* merchant_window()
{
    UObject* win = ue::get_object(ue::get_object(game::controller(), STR("BP_DialogueSystemComponent")), STR("MerchantWindow"));
    if (!win) return nullptr;
    ue::Call c(win, STR("IsActivated"));
    return c && c.invoke() && c.out_bool(STR("ReturnValue")) ? win : nullptr;
}

std::vector<UObject*> merchant_entries(UObject* win)
{
    ue::Call c(ue::get_object(win, STR("DynamicEntryBox_Items")), STR("GetAllEntries"));
    if (!c || !c.invoke()) return {};
    return c.out_object_array(STR("ReturnValue"));
}

void merchant_tick()
{
    static double next = 0;
    if (s.now < next || !sharing()) return;
    next = s.now + 0.1;
    UObject* win = merchant_window();
    if (is_pilot()) {
        if (!win) {
            if (s.merchant_open) { s.merchant_open = false; s.net.publish(proto::DialogueChoice{++s.seq, 0, 3}, true); log().write("coop_merchant_closed", JsonObj()); }
            return;
        }
        if (!s.merchant_open) { s.merchant_open = true; s.merchant_sent = -1; s.merchant_diag = 0; log().write("coop_merchant_opened", JsonObj()); }
        UObject* focused = ue::get_object(win, STR("FocusedMerchantItem"));
        const auto entries = merchant_entries(win);
        int idx = -1;
        for (size_t i = 0; i < entries.size(); ++i) if (entries[i] == focused) idx = static_cast<int>(i);
        // FocusedMerchantItem ne désigne pas forcément l'entrée (rien envoyé en 0.6.23) : focus ou survol de l'entrée.
        for (size_t i = 0; i < entries.size() && idx < 0; ++i)
            if (widget_flag(entries[i], STR("HasAnyUserFocus")) || widget_flag(entries[i], STR("HasFocusedDescendants")) || widget_flag(entries[i], STR("IsHovered"))) idx = static_cast<int>(i);
        static double diag_next = 0;
        if (s.merchant_diag < 8 && s.now >= diag_next && ++s.merchant_diag)
            log().write("coop_merchant_focus_diag", JsonObj().add("entries", static_cast<int64_t>(entries.size())).add("focused", focused ? ue::short_name(focused->GetClassPrivate()) : std::string("null"))
                                                       .add("entry_class", entries.empty() ? std::string() : ue::short_name(entries[0]->GetClassPrivate())).add("idx", idx));
        if (s.merchant_diag <= 8 && s.now >= diag_next) diag_next = s.now + 2.0;
        if (idx >= 0 && idx != s.merchant_sent && idx <= 255) {
            s.merchant_sent = idx;
            s.net.publish(proto::DialogueChoice{++s.seq, idx, 2}, true);
            bump("merchant_focus_published");
        }
        return;
    }
    if (!win) {
        s.merchant_locked = false; s.merchant_shown = -1;
        // Fenêtre absente alors que le pilote est chez le marchand depuis 6 s (ex. dialogue relancé après le combat du
        // marchand, pas rejoué ici, 2026-10-24) : ouverte directement sur le marchand le plus proche.
        if (s.merchant_seen_at >= 0 && !s.merchant_open_tried && s.now - s.merchant_seen_at > 6.0 && !s.choice_pending && s.choices.empty()) {
            s.merchant_open_tried = true;
            std::vector<UObject*> comps;
            RC::Unreal::UObjectGlobals::FindAllOf(STR("BP_MerchantComponent_C"), comps);
            double me[3]{}, best = 1e30;
            UObject* merchant = nullptr;
            const bool have_me = game::location(game::world_pawn(), me);
            for (UObject* c : comps) {
                UObject* owner = ue::component_owner(c);
                double p[3]{};
                if (!owner || !game::location(owner, p)) continue;
                const double d = have_me ? (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]) : 0;
                if (d < best) { best = d; merchant = owner; }
            }
            bool ok = false;
            if (merchant) {
                ue::Call c(ue::get_object(game::controller(), STR("BP_DialogueSystemComponent")), STR("OpenMerchantWindow"));
                if (c && c.set_object(STR("Merchant"), merchant).invoke()) ok = c.out_bool(STR("IsOpened"));
            }
            bump(ok ? "merchant_windows_opened" : "merchant_open_errors");
            log().write("coop_follower_merchant_open", JsonObj().add("ok", ok).add("merchants", static_cast<int64_t>(comps.size()))
                                                         .add("merchant", merchant ? ue::short_name(merchant) : std::string("null")).add("dist", std::sqrt(best)));
        }
        return;
    }
    if (!s.merchant_locked) {
        s.merchant_locked = true;
        s.merchant_open_tried = true;   // fenêtre ouverte par le dialogue : pas d'ouverture de secours
        s.merchant_close = false;
        for (const wchar_t* b : {STR("BuyItemButton"), STR("FightMerchantButton"), STR("LeaveButton")}) {
            ue::Call c(ue::get_object(win, b), STR("SetIsInteractionEnabled"));
            if (c) c.set_bool(STR("bInIsInteractionEnabled"), false).invoke();
        }
        bump("merchant_mirrors");
        log().write("coop_follower_merchant_mirror", JsonObj().add("entries", static_cast<int64_t>(merchant_entries(win).size())));
        ui::toast("pilot_at_merchant");
    }
    if (s.merchant_close) {
        s.merchant_close = false;
        UObject* leave = ue::get_object(win, STR("LeaveButton"));
        bool ok = false;
        { ue::Call c(win, STR("BndEvt__WBP_Merchant_Panel_LeaveButton_K2Node_ComponentBoundEvent_5_CommonButtonBaseClicked__DelegateSignature"));
          if (c && leave) ok = c.set_object(STR("Button"), leave).invoke(); }
        if (!ok) { ue::Call d(win, STR("DeactivateWidget")); ok = d && d.invoke(); }
        bump(ok ? "merchant_windows_closed" : "merchant_close_errors");
        log().write("coop_follower_merchant_closed", JsonObj().add("ok", ok));
        return;
    }
    const auto entries = merchant_entries(win);
    // Boutons du suiveur désactivés à chaque passage : le jeu les réactive à chaque changement d'article (0.6.28 :
    // TOTOFFF a pu acheter lui-même pendant que Killian menait).
    for (const wchar_t* b : {STR("BuyItemButton"), STR("FightMerchantButton"), STR("LeaveButton")}) {
        ue::Call c(ue::get_object(win, b), STR("SetIsInteractionEnabled"));
        if (c) c.set_bool(STR("bInIsInteractionEnabled"), false).invoke();
    }
    auto read_gold = [] { auto* g = ue::value_ptr<int32_t>(game::gi(), STR("Gold")); return g ? static_cast<int64_t>(*g) : int64_t(-1); };
    static int64_t last_gold = -1;   // or vu au passage précédent (achat fait par le suiveur lui-même)
    // Achat du pilote rejoué ici par le bouton Acheter du panneau (même chemin que chez le pilote : achat, or et stock
    // affichés à jour), l'article du pilote étant sélectionné. Repli : TryBuyItem direct (0.6.28 : or changé, mais
    // fenêtre pas redessinée). Or et inventaire communs (D13).
    if (s.merchant_buy >= 0 && s.merchant_buy < static_cast<int>(entries.size())) {
        UObject* row = entries[static_cast<size_t>(s.merchant_buy)];
        s.merchant_buy = -1;
        if (ue::get_object(win, STR("FocusedMerchantItem")) != row) { ue::Call p(win, STR("OnMerchantItemFocused")); if (p) p.set_object(STR("Widget"), row).invoke(); }
        const int64_t gold_before = read_gold();
        std::string how = "button";
        bool ok = false;
        s.replaying = true;
        if (ue::get_object(win, STR("FocusedMerchantItem")) == row) {
            ue::Call c(win, STR("BndEvt__WBP_Merchant_Panel_BuyItemButton_K2Node_ComponentBoundEvent_3_CommonButtonBaseClicked__DelegateSignature"));
            UObject* buy = ue::get_object(win, STR("BuyItemButton"));
            ok = c && buy && c.set_object(STR("Button"), buy).invoke() && read_gold() != gold_before;
        }
        if (!ok) {
            how = "direct";
            using namespace RC::Unreal;
            auto* sp = CastField<FStructProperty>(ue::prop(row, STR("MerchantItem")));
            void* item = sp ? sp->ContainerPtrToValuePtr<void>(row) : nullptr;
            FProperty* name_p = sp ? ue::struct_member(sp->GetStruct(), STR("MerchantItemRowName")) : nullptr;
            ue::Call c(ue::get_object(win, STR("MerchantComponent")), STR("TryBuyItem"));
            if (c && name_p && c.param_size(STR("MerchantItemRowName")) == static_cast<size_t>(name_p->GetSize()))
                ok = c.set_raw(STR("MerchantItemRowName"), name_p->ContainerPtrToValuePtr<void>(item)).invoke();
            for (UObject* r : entries) { ue::Call u(r, STR("UpdateContent")); if (u) u.invoke(); }
        }
        s.replaying = false;
        // Or affiché : valeur réelle (le panneau peut garder l'ancienne).
        if (UObject* ga = ue::get_object(win, STR("WBP_GoldAmount"))) { ue::Call g(ga, STR("SetCurrentValue")); if (g) g.set_int(STR("Value"), read_gold()).invoke(); }
        bump(ok ? "merchant_buys_replayed" : "merchant_buy_errors");
        log().write("coop_follower_merchant_buy", JsonObj().add("ok", ok).add("how", how).add("gold_before", gold_before).add("gold_after", read_gold()));
        s.merchant_shown = -1;   // lignes peut-être reconstruites : sélection reposée
        last_gold = read_gold();
        return;
    }
    // Achat fait par le suiveur lui-même malgré tout (or qui baisse sans achat du pilote) : signalé.
    if (const int64_t g = read_gold(); last_gold >= 0 && g < last_gold && !s.replaying) {
        bump("merchant_follower_spent");
        log().write("coop_follower_merchant_spent", JsonObj().add("gold_before", last_gold).add("gold_after", g));
    }
    last_gold = read_gold();
    // Sélection du pilote : seule sa ligne accepte le focus (le suiveur ne peut plus naviguer), focus sur son bouton
    // (surbrillance), panneau mis à jour (nom, stock, détails) s'il ne suit pas.
    if (s.merchant_want >= 0 && s.merchant_want < static_cast<int>(entries.size())) {
        UObject* row = entries[static_cast<size_t>(s.merchant_want)];
        UObject* focused = ue::get_object(win, STR("FocusedMerchantItem"));
        if (s.merchant_want != s.merchant_shown || (focused && focused != row)) {
            for (size_t i = 0; i < entries.size(); ++i) {
                ue::Call fc(ue::get_object(entries[i], STR("WBP_BaseButton")), STR("SetIsFocusable"));
                if (fc) fc.set_bool(STR("bInIsFocusable"), entries[i] == row).invoke();
            }
            UObject* button = ue::get_object(row, STR("WBP_BaseButton"));
            { ue::Call c(button ? button : row, STR("SetFocus")); if (c) c.invoke(); }
            const bool by_focus = ue::get_object(win, STR("FocusedMerchantItem")) == row;
            if (!by_focus) { ue::Call p(win, STR("OnMerchantItemFocused")); if (p) p.set_object(STR("Widget"), row).invoke(); }
            static int logged = 0;
            if (logged++ < 20)
                log().write("coop_follower_merchant_focus", JsonObj().add("idx", s.merchant_want).add("by_focus", by_focus).add("snapback", s.merchant_want == s.merchant_shown)
                                                              .add("panel_ok", ue::get_object(win, STR("FocusedMerchantItem")) == row));
            s.merchant_shown = s.merchant_want;
        }
    }
}

void choice_hover_tick()
{
    static double next = 0;
    if (!s.choice_pending || !sharing() || s.now < next) return;
    next = s.now + 0.1;
    const auto opts = choice_options();
    if (opts.empty()) return;
    if (s.choice_diag++ == 0) {
        std::string names;
        for (UObject* o : opts) names += ue::short_name(o) + (widget_flag(o, STR("HasFocusedDescendants")) || widget_flag(o, STR("HasAnyUserFocus")) ? "(f)" : "") +
                                         (widget_flag(o, STR("IsHovered")) ? "(h)" : "") + " ";
        log().write("coop_choice_options", JsonObj().add("count", static_cast<int64_t>(opts.size())).add("options", names).add("pilot", is_pilot()));
    }
    if (is_pilot()) {
        int idx = -1;
        for (size_t i = 0; i < opts.size() && idx < 0; ++i)
            if (widget_flag(opts[i], STR("HasAnyUserFocus")) || widget_flag(opts[i], STR("HasFocusedDescendants")) || widget_flag(opts[i], STR("IsHovered"))) idx = static_cast<int>(i);
        if (idx >= 0 && idx != s.choice_hover_sent) {
            s.choice_hover_sent = idx;
            s.net.publish(proto::DialogueChoice{++s.seq, idx, 1}, true);
            bump("choice_hovers_published");
        }
        return;
    }
    const int want = s.choice_hover_want;
    if (want < 0 || want == s.choice_hover_shown || want >= static_cast<int>(opts.size())) return;
    // Focus de l'interface sur l'option voulue : surbrillance du jeu. Les entrées de choix du suiveur sont coupées
    // (contexte de choix retiré), il ne peut pas valider.
    ue::Call c(opts[static_cast<size_t>(want)], STR("SetFocus"));
    const bool ok = c && c.invoke();
    s.choice_hover_shown = want;
    bump(ok ? "choice_hovers_shown" : "choice_hover_errors");
    if (s.choice_diag < 40) { ++s.choice_diag; log().write("coop_choice_hover", JsonObj().add("index", want).add("ok", ok)); }
}

} // namespace e33::explore::detail
