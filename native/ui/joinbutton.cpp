#include "joinbutton.hpp"

#include <atomic>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

// Structure lue dans le dump du menu principal (build 23765773) : TitleScreen.WidgetTree contient ButtonsBox
// (VerticalBox, 4 VerticalBoxSlot) ; chaque ligne est un Overlay qui porte un WBP_BaseButton_C (ContinueButton,
// NewGameButton, SettingsButton, QuitButton) ; les libellés (WBP_HeadingText_C : ContinueText…) sont dans l'arbre du
// titre, donc posés dans l'emplacement ButtonContent (UNamedSlot) de chaque bouton. Pas d'InsertChildAt en Blueprint :
// pour être en tête, on vide ButtonsBox et on remet les lignes du jeu après la nôtre, avec leurs réglages d'emplacement.

using namespace RC::Unreal;

namespace e33::ui::joinbutton {

namespace {

constexpr const wchar_t* kTitle = L"/Game/UI/Widgets/MainMenu/WBP_MM_TitleScreen.WBP_MM_TitleScreen_C:";

struct G {
    UObject* menu = nullptr;    // WBP_MM_MainMenu_C au moment de l'insertion
    UObject* title = nullptr;
    UObject* box = nullptr;
    UObject* row = nullptr;     // notre ligne dans ButtonsBox (Overlay comme celles du jeu, ou le bouton lui-même)
    UObject* btn = nullptr;
    UObject* label = nullptr;
    UObject* sub = nullptr;
    std::string name;
    bool failed = false;        // définitif après kMaxAttempts essais
    int attempts = 0;
    double next = 0;
    int text_passes = 0;        // libellés reposés au tick suivant (le Construct du widget peut remettre son texte)
    bool title_focus = false;   // un bouton de l'écran titre avait le focus au dernier passage
    bool has_focus = false, hovered = false;
    bool enter_down = false, mouse_down = false;
} g;

std::atomic<bool> g_clicked{false};
constexpr int kMaxAttempts = 5;   // un essai toutes les 2 s (écran titre pas encore prêt), puis plan B

void fail(const char* what)
{
    ++g.attempts;
    if (g.attempts >= kMaxAttempts) g.failed = true;
    log().write("ui_join_button_error", JsonObj().add("what", what).add("attempt", g.attempts).add("final", g.failed));
}

UObject* call_obj(UObject* o, const wchar_t* fn)
{
    ue::Call c(o, fn);
    if (!c || !c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

bool call_bool(UObject* o, const wchar_t* fn)
{
    ue::Call c(o, fn);
    return c && c.invoke() && c.out_bool(STR("ReturnValue"));
}

bool make_text(const std::string& s, std::vector<uint8_t>& out)
{
    UObject* ktl = ue::find_object(STR("/Script/Engine.Default__KismetTextLibrary"));
    ue::Call c(ktl, STR("Conv_StringToText"));
    if (!c) return false;
    const std::wstring w = ue::from_utf8(s);
    c.set_string(STR("InString"), w.c_str());
    if (!c.invoke()) return false;
    const size_t n = c.param_size(STR("ReturnValue"));
    const auto* p = static_cast<const uint8_t*>(c.param_ptr(STR("ReturnValue")));
    if (!p || n == 0) return false;
    out.assign(p, p + n);
    return true;
}

void set_text(UObject* w, const std::string& text)
{
    std::vector<uint8_t> ft;
    ue::Call c(w, STR("Set Text"));   // WBP_HeadingText_C / WBP_WrappingText_C (nom avec une espace, vu dans le dump)
    if (c && make_text(text, ft)) c.set_raw(STR("InText"), ft.data()).invoke();
}

UObject* create(UObject* cls, UObject* pc)
{
    UObject* wbl = ue::find_object(STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
    ue::Call c(wbl, STR("Create"));
    if (!c || !cls) return nullptr;
    c.set_object(STR("WorldContextObject"), pc).set_object(STR("WidgetType"), cls).set_object(STR("OwningPlayer"), pc);
    if (!c.invoke()) return nullptr;
    return c.out_object(STR("ReturnValue"));
}

// Copie de propriétés de réglage (valeurs simples : nombres, booléens, pointeurs d'objet/classe, structs sans chaîne).
void copy_props(UObject* dst, UObject* src, std::initializer_list<const wchar_t*> names)
{
    for (const wchar_t* n : names) {
        FProperty* ps = ue::prop(src, n);
        FProperty* pd = ue::prop(dst, n);
        if (!ps || !pd || ps->GetSize() != pd->GetSize()) continue;
        if (auto* bs = CastField<FBoolProperty>(ps)) {
            if (auto* bd = CastField<FBoolProperty>(pd)) bd->SetPropertyValueInContainer(dst, bs->GetPropertyValueInContainer(src));
            continue;
        }
        std::memcpy(pd->ContainerPtrToValuePtr<void>(dst), ps->ContainerPtrToValuePtr<void>(src), static_cast<size_t>(ps->GetSize()));
    }
}

// TSoftObjectPtr (0x28 : pointeur faible + chemin d'asset + FString de sous-chemin) : copié sans la chaîne, et
// seulement si elle est vide chez le modèle (cas d'une texture).
void copy_soft(UObject* dst, UObject* src, const wchar_t* name)
{
    FProperty* ps = ue::prop(src, name);
    FProperty* pd = ue::prop(dst, name);
    if (!ps || !pd || ps->GetSize() != 0x28 || pd->GetSize() != 0x28) return;
    const auto* s = static_cast<const uint8_t*>(ps->ContainerPtrToValuePtr<void>(src));
    int32_t sub_num = 0;
    std::memcpy(&sub_num, s + 24 + 8, sizeof sub_num);
    if (sub_num != 0) return;
    std::memcpy(pd->ContainerPtrToValuePtr<void>(dst), s, 24);
}

// Réglages d'un emplacement de panneau (VerticalBoxSlot, OverlaySlot) : lus, puis rejoués par les setters (Slate à jour).
struct SlotProps { std::map<std::wstring, std::vector<uint8_t>> v; };
constexpr std::pair<const wchar_t*, const wchar_t*> kSlotSetters[] = {
    {L"Padding", L"SetPadding"}, {L"HorizontalAlignment", L"SetHorizontalAlignment"}, {L"VerticalAlignment", L"SetVerticalAlignment"}, {L"Size", L"SetSize"},
};
const wchar_t* setter_param(const wchar_t* prop)
{
    if (!std::wcscmp(prop, L"Padding")) return L"InPadding";
    if (!std::wcscmp(prop, L"HorizontalAlignment")) return L"InHorizontalAlignment";
    if (!std::wcscmp(prop, L"VerticalAlignment")) return L"InVerticalAlignment";
    return L"InSize";
}

SlotProps read_slot(UObject* widget)
{
    SlotProps out;
    UObject* slot = ue::get_object(widget, STR("Slot"));
    for (auto& [p, setter] : kSlotSetters) {
        FProperty* fp = ue::prop(slot, p);
        if (!fp) continue;
        const auto* b = static_cast<const uint8_t*>(fp->ContainerPtrToValuePtr<void>(slot));
        out.v[p].assign(b, b + fp->GetSize());
    }
    return out;
}

void apply_slot(UObject* slot, const SlotProps& sp)
{
    for (auto& [p, setter] : kSlotSetters) {
        auto it = sp.v.find(p);
        if (it == sp.v.end()) continue;
        ue::Call c(slot, setter);
        if (c && c.param_size(setter_param(p)) == it->second.size()) c.set_raw(setter_param(p), it->second.data()).invoke();
    }
}

UObject* add_child(UObject* panel, UObject* child, const SlotProps* sp)
{
    ue::Call c(panel, STR("AddChild"));
    if (!c) return nullptr;
    c.set_object(STR("Content"), child);
    if (!c.invoke()) return nullptr;
    UObject* slot = c.out_object(STR("ReturnValue"));
    if (slot && sp) apply_slot(slot, *sp);
    return slot;
}

void set_labels()
{
    if (g.label) set_text(g.label, ui::text("join_button", {g.name}));
    if (g.sub) set_text(g.sub, ui::text("join_button_sub"));
}

void forget()
{
    const bool had = g.row != nullptr;
    const bool failed = g.failed;
    g = G{};
    g.failed = failed;
    if (had) log().write("ui_join_button", JsonObj().add("state", "forgotten"));
}

void remove()
{
    if (g.row && g.box) {
        ue::Call c(g.box, STR("RemoveChild"));
        if (c) c.set_object(STR("Content"), g.row).invoke();
        if (g.sub) { ue::Call d(g.box, STR("RemoveChild")); if (d) d.set_object(STR("Content"), g.sub).invoke(); }
        log().write("ui_join_button", JsonObj().add("state", "removed"));
    }
    const bool failed = g.failed;
    UObject* menu = g.menu;
    g = G{};
    g.failed = failed;
    g.menu = menu;
}

bool insert(UObject* menu)
{
    UObject* title = ue::get_object(menu, STR("TitleScreen"));
    UObject* box = ue::get_object(title, STR("ButtonsBox"));
    UObject* tmpl = ue::get_object(title, STR("NewGameButton"));          // toujours visible (« Continuer » peut être masqué)
    UObject* tmpl_label = ue::get_object(title, STR("NewGameText"));
    UObject* tmpl_sub = ue::get_object(title, STR("PressAnyButtonText"));
    if (!title || !box || !tmpl || !tmpl_label) { fail("title screen widgets not found"); return false; }
    UObject* pc = call_obj(tmpl, STR("GetOwningPlayer"));
    if (!pc) { fail("no owning player"); return false; }
    // Ligne du modèle : enfant direct de ButtonsBox qui porte le bouton (le bouton, ou un conteneur à un seul niveau).
    UObject* tmpl_row = tmpl;
    UObject* parent = call_obj(tmpl, STR("GetParent"));
    if (parent != box) {
        tmpl_row = parent;
        if (!tmpl_row || call_obj(tmpl_row, STR("GetParent")) != box) { fail("unexpected button hierarchy"); return false; }
    }
    // Bouton : même classe et mêmes réglages de style que « Nouvelle partie » (jamais ses délégués : ils lancent une partie).
    UObject* btn = create(tmpl->GetClassPrivate(), pc);
    UObject* label = create(tmpl_label->GetClassPrivate(), pc);
    if (!btn || !label) { fail("Create failed"); return false; }
    copy_props(btn, tmpl, {STR("MinWidth"), STR("MinHeight"), STR("Style"), STR("bHideInputAction"), STR("bSelectable"),
                           STR("bShouldSelectUponReceivingFocus"), STR("bInteractableWhenSelected"), STR("bToggleable"),
                           STR("bTriggerClickedAfterSelection"), STR("ClickMethod"), STR("TouchMethod"), STR("PressMethod"),
                           STR("HasHoverStain"), STR("BackgroundMaterialInstance"), STR("ContentPadding"), STR("IsSelectedInstant"),
                           STR("HoverStainTransform"), STR("HoverStainScalePercent"), STR("SD_OnNavigated"), STR("HidePressedAnim"),
                           STR("HideBackgroundWhenNotFocused"), STR("SD_OnClicked"), STR("HoverStainMaterialInstance"),
                           STR("MaxHoverScale"), STR("ShouldCallFocusedHold"), STR("FocusedHoldTime")});
    copy_soft(btn, tmpl, STR("HoverStainTexture"));
    copy_props(label, tmpl_label, {STR("IsUpper"), STR("FirstLetterStyle"), STR("HeadingStyle"), STR("MobileFontSizeMultipler")});
    UObject* content = ue::get_object(btn, STR("ButtonContent"));
    ue::Call setc(content, STR("SetContent"));
    if (!setc) { fail("ButtonContent not found"); return false; }
    setc.set_object(STR("Content"), label).invoke();
    UObject* row = btn;
    if (tmpl_row != tmpl) {
        FStaticConstructObjectParameters params{tmpl_row->GetClassPrivate(), tmpl_row->GetOuterPrivate()};
        row = UObjectGlobals::StaticConstructObject(params);
        if (!row) { fail("row container construct failed"); return false; }
        if (!add_child(row, btn, nullptr)) { fail("row AddChild failed"); return false; }
        const SlotProps inner = read_slot(tmpl);
        if (UObject* slot = ue::get_object(btn, STR("Slot"))) apply_slot(slot, inner);
    }
    UObject* sub = nullptr;
    if (tmpl_sub) {
        sub = create(tmpl_sub->GetClassPrivate(), pc);
        if (sub) copy_props(sub, tmpl_sub, {STR("TextStyle"), STR("TextMinDesiredWidth"), STR("TextMaxDesiredWidth"), STR("Justification"),
                                            STR("ReduceTextSizeWhenWrapping"), STR("WrappingFontSizePercentage"), STR("MobileFontSizeMultiplier")});
    }
    // Réordonner : notre ligne (et son sous-titre) d'abord, puis celles du jeu, chacune avec ses réglages d'emplacement.
    const std::vector<UObject*> rows = [&] {
        ue::Call c(box, STR("GetAllChildren"));
        if (!c || !c.invoke()) return std::vector<UObject*>{};
        return c.out_object_array(STR("ReturnValue"));
    }();
    if (rows.empty()) { fail("ButtonsBox empty"); return false; }
    std::vector<std::pair<UObject*, SlotProps>> saved;
    for (UObject* r : rows) saved.emplace_back(r, read_slot(r));
    const SlotProps row_slot = read_slot(tmpl_row);
    ue::Call clear(box, STR("ClearChildren"));
    if (!clear) { fail("ClearChildren not found"); return false; }
    clear.invoke();
    add_child(box, row, &row_slot);
    if (sub) {
        SlotProps sp = row_slot;
        sp.v.erase(L"Padding");   // collé sous le bouton
        add_child(box, sub, &sp);
        ue::Call op(sub, STR("SetRenderOpacity"));
        if (auto* v = static_cast<float*>(op ? op.param_ptr(STR("InOpacity")) : nullptr)) { *v = 0.7f; op.invoke(); }   // float
    }
    for (auto& [r, sp] : saved) add_child(box, r, &sp);
    g.menu = menu;
    g.title = title;
    g.box = box;
    g.row = row;
    g.btn = btn;
    g.label = label;
    g.sub = sub;
    g.text_passes = 2;
    g.title_focus = false;
    log().write("ui_join_button", JsonObj().add("state", "inserted").add("rows", static_cast<int64_t>(rows.size()))
                                      .add("row_class", ue::short_name(tmpl_row->GetClassPrivate())).add("subtitle", sub != nullptr));
    return true;
}

// Version du mod au menu principal : petit texte en bas à droite, juste à gauche du numéro du jeu (BuildText), même
// police, couleur et ombre. Posé dans le premier panneau Overlay ou CanvasPanel qui contient BuildText (ou un de ses
// ancêtres), puis recalé à chaque passage (largeur du numéro connue après la mise en page, texte remis par le jeu).
// On ne déréférence jamais un widget gardé d'un ancien menu : on compare seulement les pointeurs aux enfants vivants.
UObject* g_version = nullptr;
int g_version_errors = 0;
constexpr float kVersionGap = 12.f;

void version_error(const char* what)
{
    ++g_version_errors;
    log().write("ui_version_error", JsonObj().add("what", what).add("count", static_cast<int64_t>(g_version_errors)));
}

// Copie brute d'une propriété de src dans le paramètre d'un setter de dst (tailles identiques), puis appel.
void copy_to_setter(UObject* dst, const wchar_t* fn, const wchar_t* param, UObject* src, const wchar_t* prop)
{
    FProperty* fp = ue::prop(src, prop);
    ue::Call c(dst, fn);
    if (!c || !fp || c.param_size(param) != static_cast<size_t>(fp->GetSize())) return;
    c.set_raw(param, fp->ContainerPtrToValuePtr<void>(src)).invoke();
}

// Résultat brut d'un getter (struct) passé tel quel à un setter.
void getter_to_setter(UObject* src, const wchar_t* get, UObject* dst, const wchar_t* set, const wchar_t* param)
{
    ue::Call g(src, get);
    ue::Call s(dst, set);
    if (!g || !s || !g.invoke() || g.param_size(STR("ReturnValue")) != s.param_size(param)) return;
    s.set_raw(param, g.param_ptr(STR("ReturnValue"))).invoke();
}

// Composantes d'un FVector2D renvoyé (doubles en UE5, floats sinon).
bool vec2_out(ue::Call& c, double& x, double& y)
{
    const void* p = c.param_ptr(STR("ReturnValue"));
    if (!p) return false;
    if (c.param_size(STR("ReturnValue")) == 16) { x = static_cast<const double*>(p)[0]; y = static_cast<const double*>(p)[1]; return true; }
    if (c.param_size(STR("ReturnValue")) == 8) { x = static_cast<const float*>(p)[0]; y = static_cast<const float*>(p)[1]; return true; }
    return false;
}

void set_vec2(UObject* o, const wchar_t* fn, const wchar_t* param, double x, double y)
{
    ue::Call c(o, fn);
    void* d = c.param_ptr(param);
    if (!d) return;
    if (c.param_size(param) == 16) { static_cast<double*>(d)[0] = x; static_cast<double*>(d)[1] = y; }
    else if (c.param_size(param) == 8) { static_cast<float*>(d)[0] = static_cast<float>(x); static_cast<float*>(d)[1] = static_cast<float>(y); }
    else return;
    c.invoke();
}

void set_tb_text(UObject* tb, const std::string& s)
{
    std::vector<uint8_t> ft;
    ue::Call c(tb, STR("SetText"));
    if (c && make_text(s, ft) && c.param_size(STR("InText")) == ft.size()) c.set_raw(STR("InText"), ft.data()).invoke();
}

UObject* make_text_block(UObject* outer)
{
    auto* cls = static_cast<UClass*>(ue::find_object(STR("/Script/UMG.TextBlock")));
    if (!cls || !outer) return nullptr;
    static int seq = 0;
    const std::wstring name = L"E33Coop_Version_" + std::to_wstring(++seq);
    FStaticConstructObjectParameters p{cls, outer};
    p.Name = FName(name.c_str(), FNAME_Add);
    return UObjectGlobals::StaticConstructObject(p);
}

// Cale notre texte à gauche de anchor (enfant direct de panel). Renvoie false si la largeur n'est pas encore connue.
bool position_version(UObject* panel, UObject* anchor, UObject* w)
{
    ue::Call ds(anchor, STR("GetDesiredSize"));
    double aw = 0, ah = 0;
    if (!ds || !ds.invoke() || !vec2_out(ds, aw, ah) || aw <= 0) return false;
    UObject* aslot = ue::get_object(anchor, STR("Slot"));
    UObject* slot = ue::get_object(w, STR("Slot"));
    if (!aslot || !slot) return false;
    if (ue::is_a(panel, STR("CanvasPanel"))) {
        getter_to_setter(aslot, STR("GetAnchors"), slot, STR("SetAnchors"), STR("InAnchors"));
        ue::Call al(aslot, STR("GetAlignment"));
        ue::Call po(aslot, STR("GetPosition"));
        double ax = 0, ay = 0, px = 0, py = 0;
        if (!al || !al.invoke() || !vec2_out(al, ax, ay) || !po || !po.invoke() || !vec2_out(po, px, py)) return false;
        ue::Call as(slot, STR("SetAutoSize"));
        if (as) as.set_bool(STR("InbAutoSize"), true).invoke();
        set_vec2(slot, STR("SetAlignment"), STR("InAlignment"), 1.0, ay);   // bord droit de notre texte calé
        set_vec2(slot, STR("SetPosition"), STR("InPosition"), px - ax * aw - kVersionGap, py);
        return true;
    }
    // Overlay : mêmes alignements et marges que le numéro, marge droite augmentée de sa largeur + écart.
    SlotProps sp = read_slot(anchor);
    auto it = sp.v.find(L"Padding");
    if (it != sp.v.end() && it->second.size() == 16) {
        float m[4];
        std::memcpy(m, it->second.data(), sizeof m);
        m[2] += static_cast<float>(aw) + kVersionGap;
        std::memcpy(it->second.data(), m, sizeof m);
    }
    sp.v.erase(L"Size");
    apply_slot(slot, sp);
    ue::Call hz(slot, STR("SetHorizontalAlignment"));
    if (hz) hz.set_int(STR("InHorizontalAlignment"), 2).invoke();   // HAlign_Right
    return true;
}

// Diagnostic (2026-10-24) : « placed » vrai mais version absente des captures de l'écran titre. Pour chaque menu
// principal et chaque BuildText vivant : dans le viewport / visible, chaîne des parents, et état de notre texte.
// Plafonné (kVersionDiagMax lignes).
int g_version_diag = 0;
constexpr int kVersionDiagMax = 8;

std::string widget_state(UObject* w)
{
    if (!w) return "null";
    ue::Call v(w, STR("GetVisibility"));
    const int64_t vis = v && v.invoke() ? v.out_int(STR("ReturnValue"), -1) : -1;
    std::string s = ue::short_name(w) + " vis=" + std::to_string(vis) + " shown=" + (call_bool(w, STR("IsVisible")) ? "1" : "0") +
                    " op=" + std::to_string(ue::get_double(w, STR("RenderOpacity"), -1.0));
    ue::Call ds(w, STR("GetDesiredSize"));
    double dx = 0, dy = 0;
    if (ds && ds.invoke() && vec2_out(ds, dx, dy)) s += " size=" + std::to_string(static_cast<int>(dx)) + "x" + std::to_string(static_cast<int>(dy));
    return s;
}

// Chaîne des parents d'un widget jusqu'à son UserWidget (outer du WidgetTree), avec l'état de chacun.
std::string widget_chain(UObject* w)
{
    std::string s;
    UObject* last = w;
    for (int i = 0; i < 12 && w; ++i) {
        if (!s.empty()) s += " < ";
        s += ue::short_name(w->GetClassPrivate()) + ":" + widget_state(w);
        last = w;
        w = call_obj(w, STR("GetParent"));
    }
    // UserWidget propriétaire : outer du WidgetTree qui contient la racine.
    UObject* tree = last ? last->GetOuterPrivate() : nullptr;
    UObject* uw = tree ? tree->GetOuterPrivate() : nullptr;
    if (uw) s += " | owner " + ue::short_name(uw) + " viewport=" + (call_bool(uw, STR("IsInViewport")) ? "1" : "0") + " " + widget_state(uw);
    return s;
}

void version_diag(const char* why, UObject* menu, UObject* w)
{
    if (g_version_diag >= kVersionDiagMax) return;
    ++g_version_diag;
    JsonObj o;
    o.add("why", why).add("n", static_cast<int64_t>(g_version_diag)).add("menu", menu ? ue::short_name(menu) : std::string("null"))
     .add("controller", game::controller() != nullptr);
    std::vector<UObject*> menus;
    UObjectGlobals::FindAllOf(STR("WBP_MM_MainMenu_C"), menus);
    std::string ms;
    for (UObject* m : menus) ms += ue::short_name(m) + (call_bool(m, STR("IsInViewport")) ? "(vp) " : " ");
    o.add("menus", ms);
    // Tous les BuildText vivants (le numéro visible peut appartenir à un autre widget que celui où l'on se pose).
    std::vector<UObject*> texts;
    UObjectGlobals::FindAllOf(STR("TextBlock"), texts);
    int k = 0;
    for (UObject* t : texts) {
        if (!t || ue::short_name(t) != "BuildText" || k >= 4) continue;
        o.add("build" + std::to_string(k++), widget_chain(t));
    }
    o.add("ours", w ? widget_chain(w) : std::string("none"));
    log().write("ui_version_diag", o);
}

void place_version(UObject* menu)
{
    if (g_version_errors >= 3 || !menu) return;
    UObject* build = ue::get_object(menu, STR("BuildText"));
    if (!build) { version_error("build_text"); return; }
    // Premier ancêtre posé dans un panneau à plusieurs enfants où l'on sait se placer.
    UObject* anchor = build;
    UObject* panel = call_obj(build, STR("GetParent"));
    for (int i = 0; i < 6 && panel && !ue::is_a(panel, STR("Overlay")) && !ue::is_a(panel, STR("CanvasPanel")); ++i) {
        anchor = panel;
        panel = call_obj(panel, STR("GetParent"));
    }
    if (!panel) { version_error("parent"); return; }
    ue::Call all(panel, STR("GetAllChildren"));
    if (!all || !all.invoke()) return;
    UObject* w = nullptr;
    for (UObject* c : all.out_object_array(STR("ReturnValue"))) if (c && c == g_version) w = c;
    const bool fresh = w == nullptr;
    if (fresh) {
        w = make_text_block(panel);
        if (!w) { version_error("create"); return; }
        ue::Call add(panel, STR("AddChild"));
        if (!add || !add.set_object(STR("Content"), w).invoke() || !add.out_object(STR("ReturnValue"))) { version_error("add"); return; }
        copy_to_setter(w, STR("SetFont"), STR("InFontInfo"), build, STR("Font"));
        copy_to_setter(w, STR("SetColorAndOpacity"), STR("InColorAndOpacity"), build, STR("ColorAndOpacity"));
        copy_to_setter(w, STR("SetShadowOffset"), STR("InShadowOffset"), build, STR("ShadowOffset"));
        copy_to_setter(w, STR("SetShadowColorAndOpacity"), STR("InShadowColorAndOpacity"), build, STR("ShadowColorAndOpacity"));
        ue::Call j(w, STR("SetJustification"));
        if (j) j.set_int(STR("InJustification"), 2).invoke();   // ETextJustify::Right
        g_version = w;
    }
    set_tb_text(w, std::string("E33 Coop ") + E33COOP_VERSION);   // reposé à chaque passage (Construct peut le remettre)
    const bool placed = position_version(panel, anchor, w);
    // Invisible tant que la largeur du numéro est inconnue (sinon il le chevaucherait un instant).
    ue::Call op(w, STR("SetRenderOpacity"));
    if (auto* v = static_cast<float*>(op ? op.param_ptr(STR("InOpacity")) : nullptr)) {
        *v = placed ? static_cast<float>(ue::get_double(build, STR("RenderOpacity"), 1.0)) : 0.f;
        op.invoke();
    }
    if (fresh) {
        UObject* aslot = ue::get_object(anchor, STR("Slot"));
        log().write("ui_version", JsonObj().add("version", E33COOP_VERSION).add("parent", ue::short_name(panel->GetClassPrivate()))
                                        .add("anchor", ue::short_name(anchor)).add("anchor_class", ue::short_name(anchor->GetClassPrivate()))
                                        .add("slot", aslot ? ue::short_name(aslot->GetClassPrivate()) : std::string()).add("placed", placed));
        version_diag("fresh", menu, w);
    }
    // Diagnostic : état relu 5 passages plus tard puis toutes les 10 (mise en page faite).
    static int passes = 0;
    if (!fresh && (++passes == 5 || passes % 10 == 0)) version_diag("pass", menu, w);
}

bool any_focus(std::initializer_list<UObject*> ws)
{
    for (UObject* w : ws) if (w && call_bool(w, STR("HasAnyUserFocus"))) return true;
    return false;
}

} // namespace

void subscribe()
{
    auto& bus = HookBus::get();
    // Clic : CommonButtonBase::HandleButtonClicked est lié en délégué dynamique au bouton interne (souris, Entrée,
    // touche de validation de la manette) : il passe par ProcessEvent.
    bus.pre(L"/Script/CommonUI.CommonButtonBase:HandleButtonClicked", [](HookCtx& ctx, UFunction*) {
        if (g.btn && ctx.Context == g.btn) g_clicked = true;
    });
    // Focus voulu à l'activation de l'écran titre : notre bouton.
    bus.post(std::wstring(kTitle) + L"BP_GetDesiredFocusTarget", [](HookCtx& ctx, UFunction* f) {
        if (g.btn && ctx.Context == g.title) ue::write_out_object(ctx, f, STR("ReturnValue"), g.btn);
    });
}

void tick(double now, const std::string& friend_name)
{
    // Version du mod : aussi posée au menu principal sans attendre l'activation de l'écran titre (après « appuyez sur une
    // touche » seulement) ; une fois par seconde, hors partie chargée (pas de contrôleur du monde), comme le mode neutre.
    if (static double next_version = 0; now >= next_version) {
        next_version = now + 1.0;
        UObject* m = game::main_menu();
        if (m && !game::controller()) place_version(m);
        else if (static bool said = false; m && !said) { said = true; version_diag("controller_present", m, g_version); }
    }
    // Repli clavier / souris (chaque image ; le focus et le survol sont relus 4 fois par seconde) : Entrée sur notre
    // bouton sélectionné, clic gauche dessus. Le hook du clic suffit normalement ; un double signal est sans effet.
    if (g.btn) {
        const bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        const bool mouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool fg = pid == GetCurrentProcessId();
        // Front montant : focus / survol relus à l'instant (l'état mis en cache peut dater de 0,25 s).
        if (fg && enter && !g.enter_down && g.has_focus && call_bool(g.btn, STR("HasAnyUserFocus"))) g_clicked = true;
        if (fg && mouse && !g.mouse_down && g.hovered && call_bool(g.btn, STR("IsHovered"))) g_clicked = true;
        g.enter_down = enter;
        g.mouse_down = mouse;
    }
    if (now < g.next) return;
    g.next = now + 0.25;
    UObject* menu = friend_name.empty() && !g.row ? nullptr : game::main_menu();
    if (g.menu && menu != g.menu) forget();   // menu détruit (partie chargée) : nos widgets avec lui
    if (!menu || friend_name.empty()) {
        if (g.row) remove();
        return;
    }
    if (!g.row) {
        if (g.failed) return;
        g.name = friend_name;
        if (!insert(menu)) { g.next = now + 2.0; return; }
        set_labels();
        return;
    }
    if (friend_name != g.name || g.text_passes > 0) {
        g.name = friend_name;
        if (g.text_passes > 0) --g.text_passes;
        set_labels();
    }
    // Sélectionné par défaut : quand le focus arrive sur la liste (fin de « appuyez sur une touche », retour des
    // paramètres), il va sur notre bouton ; ensuite le joueur navigue librement.
    g.has_focus = call_bool(g.btn, STR("HasAnyUserFocus"));
    g.hovered = call_bool(g.btn, STR("IsHovered"));
    const bool title_focus = g.has_focus || any_focus({ue::get_object(g.title, STR("ContinueButton")), ue::get_object(g.title, STR("NewGameButton")),
                                                       ue::get_object(g.title, STR("SettingsButton")), ue::get_object(g.title, STR("QuitButton"))});
    if (title_focus && !g.title_focus && !g.has_focus) {
        ue::Call f(g.btn, STR("SetFocus"));
        if (f) { f.invoke(); log().write("ui_join_button", JsonObj().add("state", "focused")); }
    }
    g.title_focus = title_focus;
}

bool shown() { return g.row != nullptr; }
bool failed() { return g.failed; }
bool take_click() { return g_clicked.exchange(false); }

void subscribe_version()
{
    static bool done = false;
    if (done) return;
    done = true;
    HookBus::get().post(std::wstring(kTitle) + L"BP_GetDesiredFocusTarget", [](HookCtx& ctx, UFunction*) {
        if (ctx.Context) place_version(game::main_menu());
    });
}

} // namespace e33::ui::joinbutton
