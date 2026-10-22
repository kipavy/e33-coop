// « Qui joue qui » à l'écran (maquette variante C, validée par Killian le 2026-10-22) :
//  - en combat : un petit losange avec l'initiale du joueur à côté de chaque portrait de héros de l'ordre des tours ;
//  - menu pause : panneau « Coop » (héros -> joueur, réserve, session) et rappel des touches du mod en bas.
// Widgets UMG natifs (SizeBox, Border, TextBlock…) construits à l'exécution et glissés dans les widgets du jeu
// (aucun asset cuit). Introuvable -> rien n'est affiché, jamais d'erreur vers le jeu.
#pragma once

#include <string>

namespace e33::owners_ui {

void tick(double dt);              // game thread, après coop::tick
bool pause_panel_shown();          // panneau affiché dans le menu pause
bool shows_key_hints();            // coop actif et panneau possible : le rappel des touches en carte devient inutile
int count(const std::string& name); // compteurs pour les tests (badges_built, badges_shown, pause_panels_built…)

} // namespace e33::owners_ui
