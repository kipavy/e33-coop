#include "remote_proto.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/jsonlog.hpp"
#include "../common/minijson.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

namespace e33::remote {

namespace {

struct Out {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) { const auto* c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + n); }
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void str(std::string_view s) { const size_t n = std::min<size_t>(s.size(), 0xFFFF); u16(static_cast<uint16_t>(n)); raw(s.data(), n); }
    void lstr(const void* p, size_t n) { u32(static_cast<uint32_t>(n)); raw(p, n); }
};

// Lecture bornée : toute lecture hors limites met le curseur en erreur.
struct In {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    bool raw(void* out, size_t k) { if (!ok || k > n - i) { ok = false; return false; } std::memcpy(out, p + i, k); i += k; return true; }
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    std::string str(size_t max)
    {
        const uint16_t k = u16();
        if (!ok || k > max || k > n - i) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p + i), k);
        i += k;
        return s;
    }
    std::string lstr(size_t max)
    {
        const uint32_t k = u32();
        if (!ok || k > max || k > n - i) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p + i), k);
        i += k;
        return s;
    }
};

std::string lower(std::string_view s)
{
    std::string o(s);
    for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// Sérialisation d'une valeur JSON (scénario réécrit).
void dump(const json::Value& v, std::string& o)
{
    using T = json::Value::Type;
    switch (v.type) {
    case T::Null: o += "null"; break;
    case T::Bool: o += v.b ? "true" : "false"; break;
    case T::Number: {
        char buf[40];
        if (std::isfinite(v.num) && v.num == std::floor(v.num) && std::fabs(v.num) < 9e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.num));
        else std::snprintf(buf, sizeof buf, "%.17g", std::isfinite(v.num) ? v.num : 0.0);
        o += buf;
        break;
    }
    case T::String: o += '"' + json_escape(v.str) + '"'; break;
    case T::Array:
        o += '[';
        for (size_t i = 0; i < v.arr.size(); ++i) { if (i) o += ','; dump(v.arr[i], o); }
        o += ']';
        break;
    case T::Object: {
        o += '{';
        bool first = true;
        for (const auto& [k, x] : v.obj) { if (!first) o += ','; first = false; o += '"' + json_escape(k) + "\":"; dump(x, o); }
        o += '}';
        break;
    }
    }
}

json::Value str_value(const std::string& s) { json::Value v; v.type = json::Value::Type::String; v.str = s; return v; }

} // namespace

std::vector<uint8_t> encode(const Packet& p)
{
    Out o;
    o.u32(kMagic);
    o.u16(kVersion);
    o.u8(static_cast<uint8_t>(p.op));
    switch (p.op) {
    case Op::Hello: o.str(p.mod_version); o.u16(p.protocol); o.u32(p.flags); o.str(p.name); break;
    case Op::Command: o.u32(p.req); o.str(p.cmd); o.lstr(p.arg.data(), p.arg.size()); break;
    case Op::Reply: o.u32(p.req); o.u8(p.ok ? 1 : 0); o.u8(p.final ? 1 : 0); o.lstr(p.json.data(), p.json.size()); break;
    case Op::File: o.u32(p.req); o.str(p.path); o.u64(p.size); o.u64(p.offset); o.str(p.sha256); o.lstr(p.data.data(), p.data.size()); break;
    case Op::Done: o.u32(p.req); o.u32(p.count); o.u64(p.total); o.str(p.sha256); break;
    }
    return std::move(o.b);
}

std::optional<Packet> decode(const uint8_t* data, size_t len, std::string& err)
{
    In in{data, len};
    const uint32_t magic = in.u32();
    const uint16_t ver = in.u16();
    const uint8_t op = in.u8();
    if (!in.ok || magic != kMagic) { err = "not an E33R packet"; return std::nullopt; }
    if (ver == 0) { err = "bad version"; return std::nullopt; }
    // Version plus récente : mêmes champs en tête (règle du format), la suite est ignorée.
    Packet p;
    p.op = static_cast<Op>(op);
    switch (p.op) {
    case Op::Hello: p.mod_version = in.str(64); p.protocol = in.u16(); p.flags = in.u32(); p.name = in.str(128); break;
    case Op::Command: p.req = in.u32(); p.cmd = in.str(32); p.arg = in.lstr(kMaxArg); break;
    case Op::Reply: {
        p.req = in.u32();
        const uint8_t ok = in.u8(), fin = in.u8();
        if (ok > 1 || fin > 1) in.ok = false;
        p.ok = ok == 1; p.final = fin == 1;
        p.json = in.lstr(kMaxJson);
        break;
    }
    case Op::File: {
        p.req = in.u32(); p.path = in.str(260); p.size = in.u64(); p.offset = in.u64(); p.sha256 = in.str(64);
        const std::string d = in.lstr(kMaxChunk);
        p.data.assign(d.begin(), d.end());
        if (in.ok && (p.offset > p.size || p.data.size() > p.size - p.offset)) in.ok = false;
        break;
    }
    case Op::Done: p.req = in.u32(); p.count = in.u32(); p.total = in.u64(); p.sha256 = in.str(64); break;
    default: err = "unknown op " + std::to_string(op); return std::nullopt;
    }
    if (!in.ok) { err = "truncated or invalid packet"; return std::nullopt; }
    return p;
}

bool safe_relative(std::string_view path)
{
    if (path.empty() || path.size() > 240 || path.front() == '/') return false;
    int depth = 0;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || ++depth > 8) return false;
        if (part.back() == '.' || part.back() == ' ' || part.front() == ' ') return false;   // noms ambigus sous Windows
        for (char c : part) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '.' || c == '_' || c == '-' || c == '+' || c == ' ' || c == '(' || c == ')';
            if (!ok) return false;   // ni '\\', ni ':', ni '~', ni caractère de contrôle
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool update_dest_allowed(std::string_view path)
{
    if (!safe_relative(path)) return false;
    const std::string l = lower(path);   // NTFS ignore la casse
    if (l == "dwmapi.dll" || l == "ue4ss/ue4ss.dll") return true;
    const std::string mod = "ue4ss/mods/e33coop/";
    if (l.rfind(mod, 0) != 0 || l.size() == mod.size()) return false;
    const std::string rest = l.substr(mod.size());
    if (rest.rfind("remote/", 0) == 0 || rest.rfind("logs/", 0) == 0 || rest == "remotetest.txt" || rest == "remote" || rest == "logs") return false;
    return true;
}

bool safe_id(std::string_view id)
{
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

bool validate_scenario(std::string_view text, std::string& sanitized, std::string& err, std::vector<std::string>* notes)
{
    auto note = [&](const std::string& n) { if (notes) notes->push_back(n); };
    if (text.size() > kMaxScenario) { err = "scénario trop gros"; return false; }
    json::Value v;
    if (!json::parse(text, v, err)) { err = "JSON invalide : " + err; return false; }
    if (!v.is_object()) { err = "le scénario n'est pas un objet JSON"; return false; }
    // Save : uniquement les emplacements de test / coop, jamais les saves perso (SaveGames\<SteamID>\…).
    if (const auto& s = v["save"]; !s.is_null()) {
        const std::string sv = s.as_string("");
        const size_t slash = sv.find('/');
        const std::string dir = slash == std::string::npos ? "" : sv.substr(0, slash);
        const std::string name = slash == std::string::npos ? "" : sv.substr(slash + 1);
        const bool ok = (dir == "e33coop" || dir == "e33coop-test") && !name.empty() && name.size() <= 64 &&
                        std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
        if (!s.is_string() || (!sv.empty() && !ok)) { err = "save refusée (seulement e33coop/<nom> ou e33coop-test/<nom>) : " + sv; return false; }
    }
    // Étapes d'exploration : seulement celles du runner.
    static const char* steps[] = {"wait", "wait_world", "wait_join", "move", "interact", "choose", "wait_dialogue_end", "change_map", "world_battle",
                                  "battle", "trigger", "set_party", "reassign", "economy", "build", "wait_follow", "zone_action", "skip_cinematic",
                                  "wait_key", "request_pilot", "end"};
    if (const auto& ex = v["explore"]; !ex.is_null()) {
        if (!ex.is_array()) { err = "explore doit être une liste"; return false; }
        for (size_t i = 0; i < ex.size(); ++i) {
            const std::string d = ex[i]["do"].as_string("");
            if (!ex[i].is_object() || std::none_of(std::begin(steps), std::end(steps), [&](const char* x) { return d == x; })) {
                err = "étape inconnue : " + d;
                return false;
            }
        }
    }
    // Options de lancement : liste fermée.
    static const char* allowed[] = {"e33role", "e33transport", "e33join", "e33name", "e33lang", "e33turntimer", "e33seed", "e33actiondelay",
                                    "e33replaylag", "e33lockinput", "e33turndep", "e33owners", "e33draws", "e33perturb", "e33perturbworld",
                                    "e33lobby", "e33steamlobby"};
    json::Value args;
    args.type = json::Value::Type::Array;
    bool has_role_client = false, has_transport = false;
    if (const auto& la = v["launch_args"]; !la.is_null()) {
        if (!la.is_array()) { err = "launch_args doit être une liste"; return false; }
        for (size_t i = 0; i < la.size(); ++i) {
            if (!la[i].is_string()) { err = "launch_args : chaîne attendue"; return false; }
            const std::string a = la[i].as_string();
            if (a.rfind("-e33", 0) != 0) { note("option hors mod ignorée : " + a); continue; }
            const size_t eq = a.find('=');
            const std::string key = lower(a.substr(1, eq == std::string::npos ? std::string::npos : eq - 1));
            const std::string val = eq == std::string::npos ? "" : a.substr(eq + 1);
            if (key == "e33connect" || key == "e33port") { note("option réseau directe retirée : " + a); continue; }
            if (std::none_of(std::begin(allowed), std::end(allowed), [&](const char* x) { return key == x; })) { err = "option refusée : " + a; return false; }
            if (key == "e33role") { if (val != "client") { err = "seul -e33role=client est permis"; return false; } has_role_client = true; }
            if (key == "e33transport") { if (val != "steam") { err = "seul -e33transport=steam est permis"; return false; } has_transport = true; }
            if (val.size() > 256) { err = "option trop longue : " + key; return false; }
            args.arr.push_back(str_value(a));
        }
    }
    if (has_role_client && !has_transport) { args.arr.push_back(str_value("-e33transport=steam")); note("transport Steam ajouté"); }
    json::Value out = v;
    out.obj["launch_args"] = args;
    for (const char* k : {"fakepeer", "fakepeers"}) if (out.obj.erase(k)) note(std::string("champ ignoré : ") + k);
    sanitized.clear();
    dump(out, sanitized);
    return true;
}

// --- SHA-256 (BCrypt de Windows) ---------------------------------------------------------------

namespace {
BCRYPT_ALG_HANDLE sha_alg()
{
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) a = nullptr;
        return a;
    }();
    return alg;
}
} // namespace

Sha256::Sha256()
{
    BCRYPT_ALG_HANDLE alg = sha_alg();
    if (!alg) return;
    DWORD len = 0, got = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof len, &got, 0);
    obj_.resize(len ? len : 1);
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, obj_.data(), static_cast<ULONG>(obj_.size()), nullptr, 0, 0) == 0) h_ = h;
}

Sha256::~Sha256()
{
    if (h_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(h_));
}

void Sha256::update(const void* data, size_t len)
{
    const auto* p = static_cast<const uint8_t*>(data);
    while (h_ && len > 0) {
        const ULONG n = static_cast<ULONG>(std::min<size_t>(len, 1u << 30));
        BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(h_), const_cast<PUCHAR>(p), n, 0);
        p += n;
        len -= n;
    }
}

std::string Sha256::hex()
{
    if (!h_) return {};
    uint8_t d[32]{};
    BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(h_), d, sizeof d, 0);
    BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(h_));
    h_ = nullptr;
    std::string s;
    char b[3];
    for (uint8_t x : d) { std::snprintf(b, sizeof b, "%02x", x); s += b; }
    return s;
}

std::string sha256_hex(const void* data, size_t len)
{
    Sha256 h;
    h.update(data, len);
    return h.hex();
}

std::string manifest_text(std::vector<ManifestEntry> entries)
{
    std::sort(entries.begin(), entries.end(), [](const ManifestEntry& a, const ManifestEntry& b) { return a.path < b.path; });
    std::string t;
    for (auto& e : entries) t += e.sha256 + " " + std::to_string(e.size) + " " + e.path + "\n";
    return t;
}

std::string manifest_hash(const std::vector<ManifestEntry>& entries)
{
    const std::string t = manifest_text(entries);
    return sha256_hex(t.data(), t.size());
}

} // namespace e33::remote
