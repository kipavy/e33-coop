// Carte du monde : pilote sur Esquie (vol, nage, plongée) suivi par un pion qui ne l'est pas (2026-10-22, D19).
// Le pion d'un suiveur ne peut pas reproduire la monture (aucune fonction « monter » connue) : conduit par
// AddMovementInput vers un pilote en l'air, il tombait, nageait, était téléporté, retombait (« entre et sort de l'eau
// 20 fois par seconde »). Tant que l'état du pilote n'est pas reproductible, le pion du suiveur est tenu en
// « fantôme » : caché, sans collision ni gravité, placé sur le pilote à chaque image (rien n'est écrit dans la save,
// et la copie coop n'est pas refaite pendant ce temps). Rendu dès que le pilote revient dans un état reproductible.
#pragma once

#include <functional>
#include <string>

#include "../common/minijson.hpp"

#include <Unreal/UObject.hpp>

#include "../protocol/messages.hpp"

namespace e33::mount {

using Bump = std::function<void(const char* counter, int n)>;

// Pilote : mode de déplacement, monture et classe du pion dans PilotState (journal à chaque changement).
void fill_pilot(RC::Unreal::UObject* pawn, proto::PilotState& m, double now);
// Pilote sans pion d'exploration (pion possédé d'une autre classe, ex. monture) : journal de la classe possédée.
void pilot_without_pawn(double now);
// Suiveur : true si le pion est tenu en fantôme cette image (follow_tick ne le conduit pas alors).
// age : âge (s) du dernier état du pilote.
bool follow(RC::Unreal::UObject* pawn, const proto::PilotState& p, double age, double now, const Bump& bump);
// À chaque image, après le suivi : rend le pion si le suivi s'est arrêté (pilote, combat, niveau, fin de session).
void tick(double now);
void release(const char* why);   // rend le pion tout de suite si possible
bool ghosted();
void reset();

// Banc d'essai (étape {"do":"mount","op":…} des scénarios) : diagnostic de la monture, trace des appels, interaction ou
// appel de fonction sur Esquie / le pion / le contrôleur, déplacement du pion possédé. true quand l'étape est finie.
bool test_step(const json::Value& st, double step_t, bool first, std::string& err);
int count(const std::string& name);   // compteurs du banc (métrique "mount:<nom>")

} // namespace e33::mount
