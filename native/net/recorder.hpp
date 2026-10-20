// Enregistrement des échanges d'une session (une ligne JSON par message) pour le rejeu par fakepeer.
// Format : {"t":s,"dir":"h2c"|"c2h","type":"…","hex":"<octets encodés>","msg":{…lisible…}}
#pragma once

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "../common/jsonlog.hpp"
#include "../protocol/messages.hpp"

namespace e33::net {

inline std::string to_hex(const std::vector<uint8_t>& b)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

inline std::vector<uint8_t> from_hex(const std::string& s)
{
    std::vector<uint8_t> b;
    auto v = [](char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(static_cast<uint8_t>(v(s[i]) << 4 | v(s[i + 1])));
    return b;
}

class Recorder {
public:
    bool open(const std::string& path)
    {
        if (path.empty()) return true;
        f_.open(path, std::ios::binary);
        t0_ = std::chrono::steady_clock::now();
        return f_.good();
    }
    void write(const char* dir, const proto::Message& m)
    {
        if (!f_.is_open()) return;
        // Les octets d'une save ne sont jamais enregistrés (repo public, données du joueur) : le faux hôte
        // renvoie l'instantané depuis un fichier local, annoncé par la ligne SaveOffer (taille + hash).
        if (std::holds_alternative<proto::SaveChunk>(m)) return;
        const double now_t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        // Positions du pilote (~20 par seconde, non fiables) : 5 par seconde suffisent au rejeu.
        if (std::holds_alternative<proto::PilotState>(m)) {
            if (now_t - last_pilot_t_ < 0.2) return;
            last_pilot_t_ = now_t;
        }
        wire::Writer w;
        proto::encode(m, w);
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        f_ << JsonObj().add("t", t).add("dir", dir).add("type", proto::type_name(proto::type_of(m))).add("hex", to_hex(w.data())).raw("msg", proto::to_json(m)).str() << "\n";
        f_.flush();
    }

private:
    std::ofstream f_;
    std::chrono::steady_clock::time_point t0_;
    double last_pilot_t_ = -1;
};

} // namespace e33::net
