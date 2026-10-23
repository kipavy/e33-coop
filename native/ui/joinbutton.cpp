#include "joinbutton.hpp"

#include <atomic>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

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

// --- Version du mod au menu principal -----------------------------------------------------------------
// Posée quand l'écran titre s'active (son BP_GetDesiredFocusTarget : objet vivant à cet instant), en dernier dans
// ButtonsBox, dans le style du sous-titre « appuyez sur une touche ». On ne déréférence jamais un widget gardé d'un
// ancien menu : on compare seulement les pointeurs aux enfants vivants de la liste.
namespace {
UObject* g_version = nullptr;
int g_version_errors = 0;

void place_version(UObject* title)
{
    if (g_version_errors >= 3) return;
    UObject* box = ue::get_object(title, STR("ButtonsBox"));
    UObject* tmpl = ue::get_object(title, STR("PressAnyButtonText"));
    if (!box || !tmpl) { ++g_version_errors; log().write("ui_version_error", JsonObj().add("what", "widgets")); return; }
    ue::Call all(box, STR("GetAllChildren"));
    if (!all || !all.invoke()) return;
    for (UObject* c : all.out_object_array(STR("ReturnValue")))
        if (c && c == g_version) { set_text(c, std::string("E33 Coop ") + E33COOP_VERSION); return; }   // déjà là : texte reposé (Construct peut le remettre)
    UObject* pc = call_obj(tmpl, STR("GetOwningPlayer"));
    UObject* w = pc ? create(tmpl->GetClassPrivate(), pc) : nullptr;
    if (!w) { ++g_version_errors; log().write("ui_version_error", JsonObj().add("what", "create")); return; }
    copy_props(w, tmpl, {STR("TextStyle"), STR("TextMinDesiredWidth"), STR("TextMaxDesiredWidth"), STR("Justification"),
                         STR("ReduceTextSizeWhenWrapping"), STR("WrappingFontSizePercentage"), STR("MobileFontSizeMultiplier")});
    if (!add_child(box, w, nullptr)) { ++g_version_errors; log().write("ui_version_error", JsonObj().add("what", "add")); return; }
    set_text(w, std::string("E33 Coop ") + E33COOP_VERSION);
    ue::Call op(w, STR("SetRenderOpacity"));
    if (auto* v = static_cast<float*>(op ? op.param_ptr(STR("InOpacity")) : nullptr)) { *v = 0.45f; op.invoke(); }
    g_version = w;
    log().write("ui_version", JsonObj().add("version", E33COOP_VERSION));
}
} // namespace

void subscribe_version()
{
    static bool done = false;
    if (done) return;
    done = true;
    HookBus::get().post(std::wstring(kTitle) + L"BP_GetDesiredFocusTarget", [](HookCtx& ctx, UFunction*) {
        if (ctx.Context) place_version(ctx.Context);
    });
}

} // namespace e33::ui::joinbutton
