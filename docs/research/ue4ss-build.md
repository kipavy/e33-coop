# Compiler un mod C++ UE4SS en CI (recherche du 2026-10-18)

## Résumé
- Voie officielle : RE-UE4SS a le sous-module privé `deps/first/Unreal = git@github.com:Re-UE4SS/UEPseudo.git` (code dérivé d'UE). Accès seulement si le compte GitHub est lié à un compte Epic Games. Le token de kipavy reçoit un 404 aujourd'hui → compte non lié.
- En CI une fois lié : `actions/checkout` avec `submodules: recursive` + `token: ${{ secrets.UEPSEUDO_PAT }}` (PAT classique scope `repo` du compte lié ; checkout réécrit `git@github.com:` en HTTPS). Clé de déploiement impossible ; PAT fine-grained généralement non plus.
- Sans UEPseudo : (A) en-têtes écrits à la main + lib d'import générée depuis les exports de `UE4SS.dll` (RC::Unreal y est intégralement exporté, ~4249 symboles) — exemple `DrSkillIssue/RSDragonWildsMod` (`cmake/UE4SS.def`, `src/UnrealAbi.hpp`, `llvm-dlltool`/`lib /def`) ; (B) DLL autonome avec sa propre réflexion + MinHook (`VOTV-MP/Multivoid`) ; (C) module C Lua chargé par `require` (UE4SS ajoute `Scripts/?.dll` au cpath, mais Lua 5.4.7 modifié lié statiquement, `lua_*` non exportés, `RegisterHook` Lua sur fonctions BP = post seulement).

## Construction officielle
- CMake ≥ 3.22 (défaut ; xmake en voie d'abandon), Ninja, MSVC ≥ 14.43 (C++23), Rust ≥ 1.73 (patternsleuth). Build type `Game__Shipping__Win64`, cibles `UE4SS`, `proxy`, mod.
- Mod : `add_subdirectory(RE-UE4SS)` puis `add_library(X SHARED …)` + `target_link_libraries(X PUBLIC UE4SS)`. Installation `ue4ss/Mods/X/dlls/main.dll`, activé par `mods.txt` ou `enabled.txt`. Disposition v4 : tout sous `<Win64>/ue4ss/`, seul `dwmapi.dll` à côté de l'exe.
- Releases : v3.0.1 (2024) obsolète ; builds expérimentaux archivés, ex. `https://github.com/UE4SS-RE/RE-UE4SS/releases/download/experimental/UE4SS_v3.0.1-1181-g83330635.zip`. Aucun zip ne contient d'en-têtes ni de `.lib`.
- Exemples CI : `SSyl/AbioticFactor-SteamOnScreenKeyboard` (build.yml), `ASEAN-Motor-Club/MTDediMod` (msvc_build.yml), upstream `cmake_build_ue4ss.yml`.

## ABI
- Le mod doit être compilé contre le **commit exact** de l'`UE4SS.dll` livré, même CRT, Release. Les builds expérimentaux cassent l'ABI (alignement FName 8→4, layouts runtime de FSoftObjectPath/FText, TObjectPtr). Pratique : épingler et livrer `UE4SS.dll` + `dwmapi.dll` avec le mod. Vérification possible : `dumpbin /imports` du mod vs `/exports` d'UE4SS.

## Version pour E33
- Expérimental ≥ 1176 (fix layout `TSoftObjectPtr` UE 5.4, issue #1433) ; aujourd'hui 1181 (`83330635`). Pas de CustomGameConfig nécessaire pour E33.
- Mod C++ E33 existant : `github.com/uuuu790/E33-AutoParryAnim` (Nexus 917), build 1108, testé 1152, hooke `AC_jRPG_CharacterBattleStats_C:TryParryHitFromCharacter`.
- À surveiller : issue #1463 (ouverte) — sur Windows 11 26200.x, `ProcessLocalScriptFunction` résolu sur un slot IAT a fait planter Stalker 2 ; le contournement `HookProcessLocalScriptFunction=0` désactive aussi les hooks BP. La machine de dev est en 26200.

## API C++ essentielle
- `CppUserModBase` : `on_unreal_init` (avant : rien du namespace Unreal), `on_update` (**thread d'UE4SS, pas le game thread**), `on_lua_start(mod_name, lua, main_lua, async_lua, hook_lua)`, `on_cpp_mods_loaded`, `register_keydown_event`. Exports `start_mod` / `uninstall_mod`.
- Hooks BP : `UObjectGlobals::RegisterHook(fn, pre, post, data)` (callback `(UnrealScriptFunctionCallableContext& ctx, void*)` : `ctx.TheStack.Locals()`, `ctx.RESULT_DECL`) ; `UnregisterHook`. Leçons du mod E33 : ne jamais passer `nullptr` comme callback (une std::function vide casse le dispatcher global) ; out-params forcés dans le post via `FindOutParamValueAddress(ctx.TheStack, prop)` ; réessayer si `FindProperty` renvoie null.
- Globaux : `Hook::RegisterProcessEventPreCallback/PostCallback`, `Hook::RegisterEngineTickPreCallback` (game thread → appliquer le réseau là), `BeginPlay`, `EndPlay`, `LoadMap`, `ProcessConsoleExecCallback`.
- Objets : `StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Pawn:IsPlayerControlled"))`, `FindFirstOf`, `FindAllOf`, `GetFunctionByNameInChain`.
- Appels : `obj->ProcessEvent(fn, &params)` ; buffer via `fn->GetPropertiesSize()` + `FindProperty(...)->ContainerPtrToValuePtr<T>(parms)`.
- Propriétés : `GetValuePtrByPropertyNameInChain<T>`, `GetPropertyByNameInChain`, `TFieldRange<FProperty>`.
- Lua ↔ C++ : `lua.register_function("Nom", fonction_sans_capture)` dans `on_lua_start` ; ne toucher un `lua_State` que depuis son thread (issue #1445).

## URLs
RE-UE4SS https://github.com/UE4SS-RE/RE-UE4SS · issues #1433 #1463 #1445 · template https://github.com/UE4SS-RE/UE4SSCPPTemplate · https://github.com/uuuu790/E33-AutoParryAnim · https://github.com/DrSkillIssue/RSDragonWildsMod · https://github.com/VOTV-MP/Multivoid
