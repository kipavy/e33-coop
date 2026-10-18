// Journal JSON global de l'instance (un fichier par instance).
#pragma once

#include <string>

#include "../common/jsonlog.hpp"

namespace e33 {

JsonLog& log();

// Paramètres de lancement propres au mod (-e33run=..., -e33test=..., -e33role=...).
struct LaunchArgs {
    std::wstring run_dir;     // dossier des journaux/résultats du run
    std::wstring test;        // chemin du scénario de test (vide = pas de test runner)
    std::wstring role;        // solo | host | client
    std::wstring instance;    // identifiant d'instance (défaut : role)
};
const LaunchArgs& launch_args();

// Dossier du mod (…/ue4ss/Mods/E33Coop).
std::wstring mod_dir();

} // namespace e33
