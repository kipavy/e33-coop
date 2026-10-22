#pragma once
// Mesure du travail du mod par image (diagnostic des à-coups en exploration, 2026-10-22) et répartition des tâches
// périodiques coûteuses : une seule par image (sinon contrôle des builds, ressources, ennemis passifs et empreinte
// d'exploration, tous à 2 s, pouvaient tomber sur la même image).
//
// Usage : prof::frame_begin() au début du tick moteur, prof::Scope s("étape") autour des étapes, prof::frame_end()
// à la fin. prof::last() donne les durées de l'image précédente (celle contenue dans le dt de l'image courante).

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace e33::prof {

using Clock = std::chrono::steady_clock;

struct Frame {
    std::vector<std::pair<const char*, double>> steps;   // étape -> ms (inclusif ; une étape répétée est cumulée)
    double total_ms = 0;                                 // tout le tick du mod
};

struct State {
    Frame cur, last;
    Clock::time_point t0{};
    uint64_t frame = 0;
    uint64_t heavy_frame = UINT64_MAX;   // image où une tâche lourde a déjà tourné
    const char* heavy_name = "";
};

inline State& st()
{
    static State s;
    return s;
}

inline void frame_begin()
{
    auto& s = st();
    ++s.frame;
    s.cur.steps.clear();
    s.cur.total_ms = 0;
    s.t0 = Clock::now();
}

inline void frame_end()
{
    auto& s = st();
    s.cur.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - s.t0).count();
    std::swap(s.last, s.cur);
}

inline const Frame& last() { return st().last; }
inline uint64_t frame() { return st().frame; }
// Tâche lourde de l'image précédente ("" si aucune).
inline const char* last_heavy() { return st().heavy_frame + 1 == st().frame ? st().heavy_name : ""; }

inline void add(const char* name, double ms)
{
    auto& v = st().cur.steps;
    for (auto& [n, t] : v)
        if (n == name) { t += ms; return; }   // mêmes chaînes littérales : comparaison de pointeurs suffisante
    v.emplace_back(name, ms);
}

class Scope {
public:
    explicit Scope(const char* name) : name_(name), t0_(Clock::now()) {}
    ~Scope() { add(name_, std::chrono::duration<double, std::milli>(Clock::now() - t0_).count()); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* name_;
    Clock::time_point t0_;
};

// Tâche périodique lourde : true si aucune autre n'a tourné dans cette image (elle la prend). Sinon, la tâche réessaie
// à l'image suivante sans avancer son échéance.
inline bool claim_heavy(const char* name)
{
    auto& s = st();
    if (s.heavy_frame == s.frame) return false;
    s.heavy_frame = s.frame;
    s.heavy_name = name;
    return true;
}

} // namespace e33::prof
