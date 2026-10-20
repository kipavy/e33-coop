#include "overlay.hpp"

#include <deque>
#include <map>

namespace e33::ui::overlay {

namespace {
std::map<std::string, std::string> g_lines;
struct T { std::string text; double seconds; double until = -1; };
std::deque<T> g_toasts;
double g_now = 0;
} // namespace

// Premier jet : textes gardés en mémoire (journalisés par ui.cpp). Le rendu à l'écran est branché ici.
void set_line(const std::string& slot, const std::string& text)
{
    if (text.empty()) g_lines.erase(slot);
    else g_lines[slot] = text;
}

void toast(const std::string& text, double seconds) { g_toasts.push_back({text, seconds}); }

void tick(double now)
{
    g_now = now;
    while (!g_toasts.empty() && g_toasts.front().until >= 0 && now > g_toasts.front().until) g_toasts.pop_front();
    if (!g_toasts.empty() && g_toasts.front().until < 0) g_toasts.front().until = now + g_toasts.front().seconds;
}

} // namespace e33::ui::overlay
