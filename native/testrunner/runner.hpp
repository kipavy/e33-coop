// Test runner : joue un scénario de combat sans intervention.
// Activé par -e33test=<scenario.json>. Écrit <run>/result.json puis ferme le jeu.
#pragma once

#include <string>

namespace e33::testrunner {

// À appeler une fois (on_unreal_init) : lit le scénario et abonne les hooks.
void init();
// À appeler à chaque tick moteur (game thread).
void tick(float dt);
bool active();

} // namespace e33::testrunner
