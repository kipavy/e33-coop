// Interface du mod : textes en français et en anglais (table de traduction simple), indicateurs persistants
// (« tour de X », « pause demandée par X », attente) et messages temporaires (départ d'un joueur, fin de session).
// Chaque changement est journalisé (ui_status / ui_toast) : les tests vérifient les textes sans rien voir à l'écran.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace e33::ui {

enum class Lang { Fr, En };

void init();                 // langue : -e33lang=fr|en, sinon langue de l'interface Windows (français -> Fr, sinon En)
Lang lang();
void set_lang(Lang l);
// Texte traduit ; {0}, {1}… remplacés par args. Clé inconnue : la clé elle-même.
std::string text(const std::string& key, const std::vector<std::string>& args = {});

// Indicateur persistant d'un emplacement (ex. "turn", "pause", "session") ; clé vide = effacer.
void set_status(const std::string& slot, const std::string& key, const std::vector<std::string>& args = {});
void clear_status(const std::string& slot);
// Message temporaire (secondes).
void toast(const std::string& key, const std::vector<std::string>& args = {}, double seconds = 6.0);
void tick(double now);       // game thread : affichage
const std::map<std::string, std::string>& statuses();   // textes affichés (slot -> texte)
int toasts_shown();
int count(const std::string& key);   // nombre d'affichages de ce texte (indicateur ou message), pour les tests

} // namespace e33::ui
