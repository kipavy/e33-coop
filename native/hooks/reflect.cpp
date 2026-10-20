#include "reflect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>

#include <Unreal/UClass.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FStrProperty.hpp>
#include <Unreal/Property/FNameProperty.hpp>
#include <Unreal/Property/FMapProperty.hpp>
#include <Unreal/Property/FSetProperty.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/Core/Containers/Set.hpp>

#include "../common/jsonlog.hpp"
#include "ue.hpp"

namespace e33::reflect {

using namespace RC::Unreal;

namespace {
constexpr int kMaxDepth = 8;
constexpr int32 kMaxElems = 20000;

std::string num(double v)
{
    // Arrondi : les flottants recalculés de part et d'autre peuvent différer au dernier bit.
    char b[48];
    std::snprintf(b, sizeof b, "%.3f", std::round(v * 1000.0) / 1000.0);
    return b;
}

// Feuilles d'une valeur rendues en une seule chaîne (clé de tri d'une entrée de map ou de set).
std::string as_key(FProperty* p, const void* v, int depth)
{
    Leaves l;
    flatten(p, v, "", l, depth + 1);
    std::string k;
    for (auto& [path, val] : l) { if (!k.empty()) k += ","; k += path.empty() ? val : path + "=" + val; }
    return k;
}
} // namespace

std::string clean_name(const std::wstring& n)
{
    // Membres de structs BP : Nom_<n>_<GUID de 32 hexadécimaux>
    std::wstring s = n;
    const auto u = s.find(L'_');
    const auto u2 = s.rfind(L'_');
    if (u != std::wstring::npos && u2 != std::wstring::npos && u2 > u && s.size() - u2 - 1 == 32 &&
        std::all_of(s.begin() + static_cast<std::ptrdiff_t>(u2) + 1, s.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; }))
        s = s.substr(0, u);
    return ue::to_utf8(s);
}

void flatten(FProperty* p, const void* v, const std::string& path, Leaves& out, int depth)
{
    if (!p || !v || depth > kMaxDepth) return;
    if (auto* bp = CastField<FBoolProperty>(p)) { out.emplace_back(path, bp->GetPropertyValue(v) ? "1" : "0"); return; }
    if (auto* np = CastField<FNumericProperty>(p)) {
        if (np->IsFloatingPoint()) {
            out.emplace_back(path, num(p->GetSize() == 4 ? static_cast<double>(*static_cast<const float*>(v)) : *static_cast<const double*>(v)));
        } else {
            int64_t x = 0;
            switch (p->GetSize()) {
            case 1: x = *static_cast<const uint8_t*>(v); break;
            case 2: x = *static_cast<const int16_t*>(v); break;
            case 4: x = *static_cast<const int32_t*>(v); break;
            case 8: x = *static_cast<const int64_t*>(v); break;
            default: break;
            }
            out.emplace_back(path, std::to_string(x));
        }
        return;
    }
    if (CastField<FEnumProperty>(p)) {
        int64_t x = 0;
        std::memcpy(&x, v, std::min<size_t>(sizeof x, static_cast<size_t>(p->GetSize())));
        out.emplace_back(path, std::to_string(x));
        return;
    }
    if (CastField<FNameProperty>(p)) { out.emplace_back(path, ue::to_utf8(static_cast<const FName*>(v)->ToString())); return; }
    if (CastField<FStrProperty>(p)) {
        const auto* s = static_cast<const FString*>(v);
        const wchar_t* c = **s;
        out.emplace_back(path, c ? ue::to_utf8(c) : std::string());
        return;
    }
    if (CastField<FObjectPropertyBase>(p)) {
        UObject* o = *static_cast<UObject* const*>(v);
        out.emplace_back(path, o ? ue::to_utf8(o->GetName()) : "null");
        return;
    }
    if (auto* sp = CastField<FStructProperty>(p)) {
        UStruct* st = sp->GetStruct();
        if (!st) return;
        for (FProperty* m : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeSuper))
            flatten(m, m->ContainerPtrToValuePtr<void>(const_cast<void*>(v)), path + "." + clean_name(m->GetName()), out, depth + 1);
        return;
    }
    if (auto* ap = CastField<FArrayProperty>(p)) {
        const auto* arr = static_cast<const FScriptArray*>(v);
        FProperty* inner = ap->GetInner();
        const int32 n = std::min(arr->Num(), kMaxElems);
        out.emplace_back(path + "#", std::to_string(arr->Num()));
        const auto* data = static_cast<const uint8_t*>(arr->GetData());
        for (int32 i = 0; i < n && data && inner; ++i)
            flatten(inner, data + static_cast<size_t>(i) * inner->GetSize(), path + "[" + std::to_string(i) + "]", out, depth + 1);
        return;
    }
    if (auto* mp = CastField<FMapProperty>(p)) {
        auto* m = const_cast<FScriptMap*>(static_cast<const FScriptMap*>(v));
        const FScriptMapLayout& layout = mp->GetMapLayout();
        FProperty* kp = mp->GetKeyProp();
        FProperty* vp = mp->GetValueProp();
        std::map<std::string, Leaves> sorted;
        const int32 max = std::min(m->GetMaxIndex(), kMaxElems);
        for (int32 i = 0; i < max; ++i) {
            if (!m->IsValidIndex(i)) continue;
            const auto* pair = static_cast<const uint8_t*>(m->GetData(i, layout));
            Leaves l;
            flatten(vp, pair + layout.ValueOffset, "", l, depth + 1);
            sorted[as_key(kp, pair, depth)] = std::move(l);
        }
        out.emplace_back(path + "#", std::to_string(m->Num()));
        for (auto& [k, l] : sorted)
            for (auto& [sub, val] : l) out.emplace_back(path + "{" + k + "}" + sub, val);
        return;
    }
    if (auto* setp = CastField<FSetProperty>(p)) {
        auto* st = const_cast<FScriptSet*>(static_cast<const FScriptSet*>(v));
        const FScriptSetLayout& layout = setp->GetSetLayout();
        FProperty* ep = setp->GetElementProp();
        std::vector<std::string> keys;
        const int32 max = std::min(st->GetMaxIndex(), kMaxElems);
        for (int32 i = 0; i < max; ++i)
            if (st->IsValidIndex(i)) keys.push_back(as_key(ep, st->GetData(i, layout), depth));
        std::sort(keys.begin(), keys.end());
        out.emplace_back(path + "#", std::to_string(keys.size()));
        for (auto& k : keys) out.emplace_back(path + "{" + k + "}", "1");
        return;
    }
    // Texte localisé, délégués, pointeurs souples… : ignorés (non comparables ou sans intérêt pour l'état).
}

Leaves field(UObject* o, const wchar_t* name)
{
    Leaves l;
    if (!o) return l;
    FProperty* p = o->GetPropertyByNameInChain(name);
    if (p) flatten(p, p->ContainerPtrToValuePtr<void>(o), ue::to_utf8(name), l);
    return l;
}

Leaves object_fields(UObject* o, const std::vector<std::wstring>& exclude)
{
    Leaves l;
    if (!o) return l;
    for (FProperty* p : TFieldRange<FProperty>(o->GetClassPrivate(), EFieldIterationFlags::IncludeSuper)) {
        const std::wstring n = p->GetName();
        if (std::any_of(exclude.begin(), exclude.end(), [&](const std::wstring& x) { return n.find(x) != std::wstring::npos; })) continue;
        flatten(p, p->ContainerPtrToValuePtr<void>(o), clean_name(n), l);
    }
    return l;
}

uint64_t hash(const Leaves& l)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xFF;
        h *= 1099511628211ull;
    };
    for (auto& [p, v] : l) { mix(p); mix(v); }
    return h;
}

std::string diff_json(const Leaves& mine, const Leaves& theirs, size_t max)
{
    std::map<std::string, std::string> a(mine.begin(), mine.end()), b(theirs.begin(), theirs.end());
    std::string out = "[";
    size_t n = 0;
    auto add = [&](const std::string& path, const std::string& x, const std::string& y) {
        if (n++ >= max) return;
        if (out.size() > 1) out += ",";
        out += JsonObj().add("path", path).add("mine", x).add("theirs", y).str();
    };
    for (auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end()) add(k, v, "<absent>");
        else if (it->second != v) add(k, v, it->second);
    }
    for (auto& [k, v] : b) if (!a.count(k)) add(k, "<absent>", v);
    return out + "]";
}

} // namespace e33::reflect
