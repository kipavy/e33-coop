// Bouton « Rejoindre <ami> » en tête de la liste du menu principal (WBP_MM_TitleScreen.ButtonsBox), dans le style des
// boutons du jeu : un WBP_BaseButton_C neuf, réglé comme « Nouvelle partie » (mêmes propriétés de style), avec un
// WBP_HeadingText_C dans son emplacement ButtonContent et un sous-titre « partie coop ouverte ». Sélectionné par défaut.
// Le clic (souris, Entrée, manette) est seulement signalé (take_click) : la jonction reste celle de F7 (coop.cpp).
// Game thread uniquement.
#pragma once

#include <string>

namespace e33::ui::joinbutton {

void subscribe();                                      // hooks : clic du bouton, focus par défaut de l'écran titre
void tick(double now, const std::string& friend_name); // nom vide : pas de partie d'ami -> bouton retiré
bool shown();
bool failed();                                         // insertion impossible : la carte d'invitation prend le relais
bool take_click();

} // namespace e33::ui::joinbutton
