// Lecture générique de valeurs UE par réflexion (structs, tableaux, maps, sets, objets par nom) : liste triée de
// feuilles « chemin = valeur ». Sert au hash d'état d'exploration (or, inventaire, drapeaux…) et aux builds des
// personnages. Lecture mémoire seulement (aucun ProcessEvent) : utilisable n'importe où sur le game thread.
// Les maps et sets sont triés par clé : deux instances avec le même contenu donnent la même liste.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

namespace e33::reflect {

using Leaves = std::vector<std::pair<std::string, std::string>>;

// Feuilles d'une valeur de propriété (value = adresse de la valeur, pas du conteneur).
void flatten(RC::Unreal::FProperty* p, const void* value, const std::string& path, Leaves& out, int depth = 0);
// Feuilles d'un champ d'objet par nom (vide si le champ n'existe pas).
Leaves field(RC::Unreal::UObject* o, const wchar_t* name);
// Toutes les propriétés d'un objet (sa classe et ses parents), sauf celles dont le nom contient une sous-chaîne exclue.
Leaves object_fields(RC::Unreal::UObject* o, const std::vector<std::wstring>& exclude = {});
uint64_t hash(const Leaves& l);
// Écarts entre deux listes (chemins présents d'un seul côté ou de valeurs différentes), au plus max.
std::string diff_json(const Leaves& mine, const Leaves& theirs, size_t max = 20);
// Nom de membre lisible : "Quantity_12_8F3A…" -> "Quantity".
std::string clean_name(const std::wstring& n);

// Parcourt une map d'un objet (champ TMap) : visit(adresse de la clé, adresse de la valeur, propriété clé, propriété valeur).
bool map_pairs(RC::Unreal::UObject* o, const wchar_t* field, std::vector<std::pair<void*, void*>>& pairs, RC::Unreal::FProperty** key_prop, RC::Unreal::FProperty** value_prop);

} // namespace e33::reflect
