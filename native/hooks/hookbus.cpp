#include "hookbus.hpp"

#include <exception>

#include "../common/jsonlog.hpp"
#include "log.hpp"
#include "ue.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace e33 {

namespace {

void dispatch_pre(HookCtx& ctx, void* custom)
{
    auto* e = static_cast<HookBus::Entry*>(custom);
    for (auto& f : e->pre) {
        try { f(ctx, e->fn); } catch (const std::exception& ex) {
            log().write("hook_exception", JsonObj().add("path", ue::to_utf8(e->path)).add("phase", "pre").add("what", ex.what()));
        } catch (...) {
            log().write("hook_exception", JsonObj().add("path", ue::to_utf8(e->path)).add("phase", "pre"));
        }
    }
}

void dispatch_post(HookCtx& ctx, void* custom)
{
    auto* e = static_cast<HookBus::Entry*>(custom);
    for (auto& f : e->post) {
        try { f(ctx, e->fn); } catch (const std::exception& ex) {
            log().write("hook_exception", JsonObj().add("path", ue::to_utf8(e->path)).add("phase", "post").add("what", ex.what()));
        } catch (...) {
            log().write("hook_exception", JsonObj().add("path", ue::to_utf8(e->path)).add("phase", "post"));
        }
    }
}

} // namespace

HookBus& HookBus::get()
{
    static HookBus bus;
    return bus;
}

HookBus::Entry& HookBus::entry(const std::wstring& path)
{
    for (auto& e : entries_) if (e->path == path) return *e;
    entries_.push_back(std::make_unique<Entry>());
    entries_.back()->path = path;
    return *entries_.back();
}

void HookBus::pre(const std::wstring& path, HookFn fn) { entry(path).pre.push_back(std::move(fn)); }
void HookBus::post(const std::wstring& path, HookFn fn) { entry(path).post.push_back(std::move(fn)); }

bool HookBus::try_register()
{
    bool all = true;
    for (auto& up : entries_) {
        Entry& e = *up;
        if (e.registered) continue;
        e.fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, e.path.c_str());
        if (!e.fn) { all = false; continue; }
        try {
            // Jamais de callback nul : une std::function vide casse le dispatcher global d'UE4SS.
            e.ids = UObjectGlobals::RegisterHook(e.fn, &dispatch_pre, &dispatch_post, &e);
            e.registered = true;
            log().write("hook_registered", JsonObj().add("path", ue::to_utf8(e.path)));
        } catch (const std::exception& ex) {
            log().write("hook_error", JsonObj().add("path", ue::to_utf8(e.path)).add("what", ex.what()));
            e.fn = nullptr;
            all = false;
        }
    }
    return all;
}

void HookBus::unregister_all()
{
    for (auto& up : entries_) {
        if (up->registered && up->fn) {
            try { UObjectGlobals::UnregisterHook(up->fn, up->ids); } catch (...) {}
        }
        up->registered = false;
    }
}

} // namespace e33
