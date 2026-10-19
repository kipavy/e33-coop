// État du combat partagé par toutes les instances : instantané, hash, comparaison et réparation (D11).
// Le hash est calculé sur l'instantané (même code chez l'hôte et chez les clients).
#pragma once

#include <cstdint>
#include <string>

#include "../protocol/messages.hpp"

namespace RC::Unreal { class UObject; }

namespace e33::sync {

// Instantané de tous les combattants (héros puis ennemis, triés par emplacement) et de la jauge gradient.
proto::StateSync snapshot(uint32_t battle_id, uint32_t turn);
// Hash : PV et PA arrondis, bouclier, brise, mort, buffs (hors agrégateurs), mécanique propre des héros
// (perfection, posture, taches…), jauge gradient, combattant actif.
uint64_t hash(const proto::StateSync& st, proto::Unit actor);
// Différences champ par champ (JSON, pour les logs) ; vide si identiques au sens du hash.
std::string diff(const proto::StateSync& local, const proto::StateSync& host);

struct RepairStats { int hp = 0, ap = 0, shield = 0, breakbar = 0, death = 0, buffs = 0, mech = 0, gradient = 0, failed = 0; };
// Applique l'état de l'hôte là où il diffère. Renvoie ce qui a été corrigé (et les échecs).
RepairStats repair(const proto::StateSync& local, const proto::StateSync& host);
std::string repair_json(const RepairStats& r);

proto::Unit unit_of(RC::Unreal::UObject* character);

} // namespace e33::sync
