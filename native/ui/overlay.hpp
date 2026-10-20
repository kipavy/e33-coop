// Affichage des textes du mod à l'écran (game thread). Implémentation : voir overlay.cpp.
#pragma once

#include <string>

namespace e33::ui::overlay {

void set_line(const std::string& slot, const std::string& text);   // texte vide = retirer la ligne
void toast(const std::string& text, double seconds);
void tick(double now);

} // namespace e33::ui::overlay
