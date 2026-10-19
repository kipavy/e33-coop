// Aides de réflexion UE (via UE4SS) : lecture/écriture de propriétés par nom, appel de UFunction
// avec un tampon de paramètres, recherche de membres de structs BP à suffixe GUID.
// Tout est « fail soft » : un nom introuvable renvoie nullptr/false, jamais d'exception vers le jeu.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

namespace e33::ue {

using RC::Unreal::FProperty;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
using RC::Unreal::UStruct;

std::string to_utf8(const std::wstring& w);
std::wstring from_utf8(std::string_view s);

// Nom complet / court d'un objet en UTF-8 ("null" si nul).
std::string full_name(UObject* o);
std::string short_name(UObject* o);

UObject* find_object(const wchar_t* path);

// Propriété par nom exact dans la chaîne de classes de l'objet.
FProperty* prop(UObject* o, const wchar_t* name);
// Membre d'un UStruct par nom exact, ou par préfixe "Nom_" (membres de structs BP : Nom_<n>_<GUID>).
FProperty* struct_member(UStruct* s, std::wstring_view name);

template <typename T>
T* value_ptr(UObject* o, const wchar_t* name)
{
    if (!o) return nullptr;
    FProperty* p = prop(o, name);
    return p ? static_cast<T*>(p->ContainerPtrToValuePtr<void>(o)) : nullptr;
}

double get_double(UObject* o, const wchar_t* name, double def = 0);
int64_t get_int(UObject* o, const wchar_t* name, int64_t def = 0);   // int32/int64/byte/enum
bool get_bool(UObject* o, const wchar_t* name, bool def = false);
UObject* get_object(UObject* o, const wchar_t* name);
std::vector<UObject*> get_object_array(UObject* o, const wchar_t* name);
std::string get_name_string(UObject* o, const wchar_t* name);       // FName -> UTF-8
bool set_bool(UObject* o, const wchar_t* name, bool v);
bool set_int(UObject* o, const wchar_t* name, int64_t v);

// Appel d'une UFunction avec un tampon de paramètres construit par nom.
class Call {
public:
    Call(UObject* target, const wchar_t* function_name);
    explicit operator bool() const { return fn_ != nullptr; }

    Call& set_object(const wchar_t* param, UObject* v);
    Call& set_bool(const wchar_t* param, bool v);
    Call& set_int(const wchar_t* param, int64_t v);   // int32/int64/byte/enum selon la taille
    Call& set_double(const wchar_t* param, double v);
    Call& set_fname(const wchar_t* param, const wchar_t* v);
    Call& set_string(const wchar_t* param, const wchar_t* v);   // FString
    Call& set_object_array(const wchar_t* param, const std::vector<UObject*>& v);
    // Accès brut à la valeur d'un paramètre (pour les structs).
    void* param_ptr(const wchar_t* param);

    bool invoke();   // ProcessEvent ; false si la fonction est introuvable

    bool out_bool(const wchar_t* param, bool def = false);
    UObject* out_object(const wchar_t* param);

private:
    UObject* target_ = nullptr;
    UFunction* fn_ = nullptr;
    std::vector<uint8_t> buf_;
    FProperty* param(const wchar_t* name);
};

// Lit/écrit un out-param bool du cadre d'un hook (via FindOutParamValueAddress).
std::optional<bool> read_out_bool(RC::Unreal::UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name);
bool write_out_bool(RC::Unreal::UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name, bool v);
// Lit un paramètre d'entrée (objet) depuis les locales du cadre d'un hook.
UObject* read_param_object(RC::Unreal::UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name);
std::optional<int64_t> read_param_int(RC::Unreal::UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name);
// Adresse brute d'un paramètre dans les locales du cadre d'un hook (taille dans *size), nullptr si introuvable.
void* param_address(RC::Unreal::UnrealScriptFunctionCallableContext& ctx, UFunction* fn, const wchar_t* name, int* size = nullptr);

// Propriétaire (AActor) d'un composant : GetOwner() ou premier outer qui est un Actor.
UObject* component_owner(UObject* component);

} // namespace e33::ue
