// Journal JSON par ligne, partagé entre le mod et les outils (aucune dépendance UE).
// Une ligne = {"ts":…, "inst":…, "role":…, "ev":…, "turn":…, "data":{…}, "hash":…}
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace e33 {

inline std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// Petit constructeur d'objet JSON : JsonObj().add("hp", 12).add("name", "Lune").str()
class JsonObj {
public:
    JsonObj& add(std::string_view k, std::string_view v) { key(k); body_ += '"' + json_escape(v) + '"'; return *this; }
    JsonObj& add(std::string_view k, const char* v) { return add(k, std::string_view(v ? v : "")); }
    JsonObj& add(std::string_view k, const std::string& v) { return add(k, std::string_view(v)); }
    JsonObj& add(std::string_view k, bool v) { key(k); body_ += v ? "true" : "false"; return *this; }
    JsonObj& add(std::string_view k, int64_t v) { key(k); body_ += std::to_string(v); return *this; }
    JsonObj& add(std::string_view k, int v) { return add(k, static_cast<int64_t>(v)); }
    JsonObj& add(std::string_view k, uint32_t v) { return add(k, static_cast<int64_t>(v)); }
    JsonObj& add(std::string_view k, uint64_t v) { key(k); body_ += std::to_string(v); return *this; }
    JsonObj& add(std::string_view k, double v)
    {
        key(k);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.10g", v);
        body_ += buf;
        return *this;
    }
    JsonObj& raw(std::string_view k, std::string_view json) { key(k); body_ += json; return *this; }
    std::string str() const { return "{" + body_ + "}"; }

private:
    void key(std::string_view k)
    {
        if (!body_.empty()) body_ += ',';
        body_ += '"' + json_escape(k) + "\":";
    }
    std::string body_;
};

class JsonLog {
public:
    JsonLog() = default;
    ~JsonLog() { close(); }

    bool open(const std::string& path, std::string instance, std::string role)
    {
        std::lock_guard lk(mu_);
        if (f_) std::fclose(f_);
        f_ = std::fopen(path.c_str(), "ab");
        instance_ = std::move(instance);
        role_ = std::move(role);
        return f_ != nullptr;
    }
    void set_role(std::string role) { std::lock_guard lk(mu_); role_ = std::move(role); }
    void close() { std::lock_guard lk(mu_); if (f_) { std::fclose(f_); f_ = nullptr; } }

    void write(std::string_view ev, const JsonObj& data = {}, int64_t turn = -1, uint64_t hash = 0)
    {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        char tsbuf[32];
        std::snprintf(tsbuf, sizeof tsbuf, "%lld.%06lld", static_cast<long long>(now / 1000000), static_cast<long long>(now % 1000000));
        JsonObj line;
        line.raw("ts", tsbuf)
            .add("inst", instance_)
            .add("role", role_)
            .add("ev", ev)
            .add("turn", turn)
            .raw("data", data.str());
        if (hash) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
            line.add("hash", std::string_view(buf));
        }
        std::lock_guard lk(mu_);
        if (!f_) return;
        const std::string s = line.str() + "\n";
        std::fwrite(s.data(), 1, s.size(), f_);
        std::fflush(f_);
    }

private:
    std::mutex mu_;
    std::FILE* f_ = nullptr;
    std::string instance_ = "?";
    std::string role_ = "solo";
};

} // namespace e33
