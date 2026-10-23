// Options du mod en jeu (settings.ini) et répartition des personnages au clavier, sans gêner la navigation du jeu :
//  - F9 : ligne suivante (personnages de l'équipe pour l'hôte hors combat, puis options) ; après la dernière, fermé ;
//  - Maj+F9 : changer la ligne choisie (personnage -> joueur suivant, comme F6 ; option -> valeur suivante, enregistrée).
// Dans le menu pause, la sélection est surlignée dans le panneau « Coop » (owners_ui) ; ailleurs, une carte la montre.
// Le panneau est aussi navigable (owners_ui, D22) : la ligne qui a le focus devient la ligne choisie, et inversement ;
// clic / Entrée / A = change(), gauche / droite = change(-1 / +1).
// Options d'hôte (parade en duo, minuteur) : lecture seule chez un client (la valeur de l'hôte voyage dans BattleStart).
// Touches coupées en mode test (le testeur pilote par select / change).
#pragma once

#include <string>
#include <vector>

namespace e33::options {

struct Row {
    std::string label, value;
    bool hero = false;          // ligne de personnage (sinon option)
    bool editable = true;
    bool selected = false;
};

void tick(double dt);                  // game thread
std::vector<Row> rows();               // lignes sélectionnables (personnages puis options), dans l'ordre
int selected();                        // -1 : aucune
// Testeur et clavier
void select(int index);
// Change la ligne choisie ; false si impossible (lecture seule, en combat…). dir < 0 : valeur précédente (option) ;
// un personnage passe toujours au joueur suivant (même chemin que F6).
bool change(int dir = 1);
void keep_alive();                     // la ligne reste choisie (focus du panneau) : pas d'oubli après 20 s
int count(const std::string& name);    // compteurs (panel_changes, option_changes…)

} // namespace e33::options
