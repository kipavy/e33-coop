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

// Panneau navigable (lignes = boutons du jeu). Pour le testeur : focus, clic et gauche / droite par les fonctions du
// jeu (SetFocus, HandleButtonClicked, diffusion de la navigation contrainte), jamais de vraies touches.
int panel_buttons();               // lignes en boutons dans le menu pause ouvert (0 : panneau en texte ou absent)
int focused_row();                 // indice options::rows() de la ligne qui a le focus, -1 sinon
bool menu_has_focus();             // un bouton du menu du jeu a le focus
bool test_focus(int row);          // SetFocus sur la ligne ; -1 : retour au menu du jeu
bool test_click(int row);          // clic (souris, Entrée, A)
bool test_nav(int row, int dir);   // gauche (dir < 0) / droite sur la ligne
bool test_back();                  // Retour (événement du bouton Retour du menu pause, porte du mod) ; false si pas de porte

} // namespace e33::owners_ui
