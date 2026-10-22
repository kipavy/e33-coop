// Options du mod en jeu (settings.ini) et répartition des personnages au clavier, sans gêner la navigation du jeu :
//  - F9 : ligne suivante (personnages de l'équipe pour l'hôte hors combat, puis options) ; après la dernière, fermé ;
//  - Maj+F9 : changer la ligne choisie (personnage -> joueur suivant, comme F6 ; option -> valeur suivante, enregistrée).
// Dans le menu pause, la sélection est surlignée dans le panneau « Coop » (owners_ui) ; ailleurs, une carte la montre.
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
bool change();                         // change la ligne choisie ; false si impossible (lecture seule, en combat…)
int count(const std::string& name);    // compteurs (panel_changes, option_changes…)

} // namespace e33::options
