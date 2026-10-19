#include "ue.hpp"

#include <malloc.h>

#include <Helpers/String.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>

using namespace RC;
using namespace RC::Unreal;

namespace e33::ue {

std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring from_utf8(std::string_view s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string full_name(UObject* o) { return o ? to_utf8(o->GetFullName()) : std::string("null"); }
std::string short_name(UObject* o) { return o ? to_utf8(o->GetName()) : std::string("null"); }

UObject* find_object(const wchar_t* path) { return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path); }

FProperty* prop(UObject* o, const wchar_t* name) { return o ? o->GetPropertyByNameInChain(name) : nullptr; }

FProperty* struct_member(UStruct* s, std::wstring_view name)
{
    if (!s) return nullptr;
    const std::wstring prefix = std::wstring(name) + L"_";
    for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated)) {
        const std::wstring n = p->GetName();
        if (n == name || n.rfind(prefix, 0) == 0) return p;
    }
    return nullptr;
}

double get_double(UObject* o, const wchar_t* name, double def)
{
    auto* v = value_ptr<double>(o, name);
    return v ? *v : def;
}

int64_t get_int(UObject* o, const wchar_t* name, int64_t def)
{
    FProperty* p = prop(o, name);
    if (!p) return def;
    void* v = p->ContainerPtrToValuePtr<void>(o);
    switch (p->GetSize()) {
    case 1: return *static_cast<uint8_t*>(v);
    case 2: return *static_cast<int16_t*>(v);
    case 4: return *static_cast<int32_t*>(v);
    case 8: return *static_cast<int64_t*>(v);
    default: return def;
    }
}

bool get_bool(UObject* o, const wchar_t* name, bool def)
{
    auto* bp = CastField<FBoolProperty>(prop(o, name));
    return bp ? bp->GetPropertyValueInContainer(o) : def;
}

bool set_bool(UObject* o, const wchar_t* name, bool v)
{
    auto* bp = CastField<FBoolProperty>(prop(o, name));
    if (!bp) return false;
    bp->SetPropertyValueInContainer(o, v);
    return true;
}

bool set_int(UObject* o, const wchar_t* name, int64_t v)
{
    FProperty* p = prop(o, name);
    if (!p) return false;
    void* d = p->ContainerPtrToValuePtr<void>(o);
    switch (p->GetSize()) {
    case 1: *static_cast<uint8_t*>(d) = static_cast<uint8_t>(v); return true;
    case 2: *static_cast<int16_t*>(d) = static_cast<int16_t>(v); return true;
    case 4: *static_cast<int32_t*>(d) = static_cast<int32_t>(v); return true;
    case 8: *static_cast<int64_t*>(d) = v; return true;
    default: return false;
    }
}

UObject* get_object(UObject* o, const wchar_t* name)
{
    auto* v = value_ptr<UObject*>(o, name);
    return v ? *v : nullptr;
}

std::vector<UObject*> get_object_array(UObject* o, const wchar_t* name)
{
    std::vector<UObject*> out;
    auto* arr = value_ptr<FScriptArray>(o, name);
    if (!arr) return out;
    auto** data = static_cast<UObject**>(arr->GetData());
    const int32 n = arr->Num();
    for (int32 i = 0; i < n; ++i) out.push_back(data[i]);
    return out;
}

std::string get_name_string(UObject* o, const wchar_t* name)
{
    auto* v = value_ptr<FName>(o, name);
    return v ? to_utf8(v->ToString()) : std::string();
}

// --- Call -------------------------------------------------------------------------

Call::Call(UObject* target, const wchar_t* function_name) : target_(target)
{
    if (!target_) return;
    fn_ = target_->GetFunctionByNameInChain(function_name);
    if (!fn_) return;
    const int32 sz = fn_->GetPropertiesSize();
    buf_.assign(static_cast<size_t>(sz > 0 ? sz : 8) + 16, 0);
}

FProperty* Call::param(const wchar_t* name) { return fn_ ? fn_->FindProperty(FName(name, FNAME_Find)) : nullptr; }

void* Call::param_ptr(const wchar_t* name)
{
    FProperty* p = param(name);
    return p ? p->ContainerPtrToValuePtr<void>(buf_.data()) : nullptr;
}

Call& Call::set_object(const wchar_t* name, UObject* v)
{
    if (void* d = param_ptr(name)) *static_cast<UObject**>(d) = v;
    return *this;
}

Call& Call::set_bool(const wchar_t* name, bool v)
{
    if (auto* bp = CastField<FBoolProperty>(param(name))) bp->SetPropertyValueInContainer(buf_.data(), v);
    return *this;
}

Call& Call::set_int(const wchar_t* name, int64_t v)
{
    FProperty* p = param(name);
    if (!p) return *this;
    void* d = p->ContainerPtrToValuePtr<void>(buf_.data());
    switch (p->GetSize()) {
    case 1: *static_cast<uint8_t*>(d) = static_cast<uint8_t>(v); break;
    case 2: *static_cast<int16_t*>(d) = static_cast<int16_t>(v); break;
    case 4: *static_cast<int32_t*>(d) = static_cast<int32_t>(v); break;
    case 8: *static_cast<int64_t*>(d) = v; break;
    default: break;
    }
    return *this;
}

Call& Call::set_double(const wchar_t* name, double v)
{
    if (void* d = param_ptr(name)) *static_cast<double*>(d) = v;
    return *this;
}

Call& Call::set_fname(const wchar_t* name, const wchar_t* v)
{
    if (void* d = param_ptr(name)) *static_cast<FName*>(d) = FName(v, FNAME_Add);
    return *this;
}

Call& Call::set_string(const wchar_t* name, const wchar_t* v)
{
    if (void* d = param_ptr(name)) *static_cast<FString*>(d) = FString{FromCharTypePtr<TCHAR>(v)};
    return *this;
}

Call& Call::set_object_array(const wchar_t* name, const std::vector<UObject*>& v)
{
    void* d = param_ptr(name);
    if (!d) return *this;
    auto* arr = new (d) FScriptArray{};
    if (!v.empty()) {
        arr->AddZeroed(static_cast<int32>(v.size()), static_cast<int32>(sizeof(UObject*)), static_cast<uint32>(alignof(UObject*)));
        auto** data = static_cast<UObject**>(arr->GetData());
        for (size_t i = 0; i < v.size(); ++i) data[i] = v[i];
    }
    return *this;
}

bool Call::invoke()
{
    if (!fn_ || !target_) return false;
    target_->ProcessEvent(fn_, buf_.data());
    return true;
}

bool Call::out_bool(const wchar_t* name, bool def)
{
    auto* bp = CastField<FBoolProperty>(param(name));
    return bp ? bp->GetPropertyValueInContainer(buf_.data()) : def;
}

UObject* Call::out_object(const wchar_t* name)
{
    void* d = param_ptr(name);
    return d ? *static_cast<UObject**>(d) : nullptr;
}

// --- Cadres de hooks -------------------------------------------------------------

std::optional<bool> read_out_bool(UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name)
{
    if (!fn) return std::nullopt;
    auto* bp = CastField<FBoolProperty>(fn->FindProperty(FName(name, FNAME_Find)));
    if (!bp) return std::nullopt;
    void* addr = FindOutParamValueAddress(ctx.TheStack, bp);
    if (!addr) return std::nullopt;
    return bp->GetPropertyValue(addr);
}

bool write_out_bool(UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name, bool v)
{
    if (!fn) return false;
    auto* bp = CastField<FBoolProperty>(fn->FindProperty(FName(name, FNAME_Find)));
    if (!bp) return false;
    void* addr = FindOutParamValueAddress(ctx.TheStack, bp);
    if (!addr) return false;
    bp->SetPropertyValue(addr, v);
    return true;
}

UObject* read_param_object(UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name)
{
    if (!fn) return nullptr;
    FProperty* p = fn->FindProperty(FName(name, FNAME_Find));
    void* locals = ctx.TheStack.Locals();
    if (!p || !locals) return nullptr;
    return *static_cast<UObject**>(p->ContainerPtrToValuePtr<void>(locals));
}

void* param_address(UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name, int* size)
{
    if (!fn) return nullptr;
    FProperty* p = fn->FindProperty(FName(name, FNAME_Find));
    void* locals = ctx.TheStack.Locals();
    if (!p || !locals) return nullptr;
    if (size) *size = p->GetSize();
    return p->ContainerPtrToValuePtr<void>(locals);
}

std::optional<int64_t> read_param_int(UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name)
{
    if (!fn) return std::nullopt;
    FProperty* p = fn->FindProperty(FName(name, FNAME_Find));
    void* locals = ctx.TheStack.Locals();
    if (!p || !locals) return std::nullopt;
    void* v = p->ContainerPtrToValuePtr<void>(locals);
    switch (p->GetSize()) {
    case 1: return *static_cast<uint8_t*>(v);
    case 4: return *static_cast<int32_t*>(v);
    case 8: return *static_cast<int64_t*>(v);
    default: return std::nullopt;
    }
}

UObject* component_owner(UObject* component)
{
    if (!component) return nullptr;
    Call c(component, STR("GetOwner"));
    if (c && c.invoke()) {
        if (UObject* o = c.out_object(STR("ReturnValue"))) return o;
    }
    static UClass* actor_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.Actor"));
    return actor_class ? component->GetTypedOuter(actor_class) : nullptr;
}

} // namespace e33::ue
