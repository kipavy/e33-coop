// Registre de hooks sur des fonctions Blueprint/natives, par chemin complet.
// Plusieurs abonnés par fonction ; l'enregistrement est réessayé tant que la classe n'est pas chargée.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FFrame.hpp>

namespace e33 {

using HookCtx = RC::Unreal::UnrealScriptFunctionCallableContext;
using HookFn = std::function<void(HookCtx&, RC::Unreal::UFunction*)>;

class HookBus {
public:
    static HookBus& get();

    // Abonne un callback avant (pre) / après (post) l'exécution de la fonction.
    void pre(const std::wstring& path, HookFn fn);
    void post(const std::wstring& path, HookFn fn);

    // À appeler régulièrement : enregistre les hooks dont la fonction est désormais chargée.
    // Renvoie true quand tout est enregistré.
    bool try_register();
    void unregister_all();
    // Abonnements pas encore enregistrés (ajoutés après coup : rôle décidé en jeu, D18).
    bool pending() const { return pending_; }

    struct Entry {
        std::wstring path;
        RC::Unreal::UFunction* fn = nullptr;
        std::pair<int, int> ids{-1, -1};
        bool registered = false;
        std::vector<HookFn> pre, post;
    };

private:
    Entry& entry(const std::wstring& path);
    std::vector<std::unique_ptr<Entry>> entries_;
    bool pending_ = false;
};

} // namespace e33
