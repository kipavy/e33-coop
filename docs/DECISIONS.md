# Décisions d'architecture

Chaque entrée : la décision, sa date, ses raisons, et ce qui la remettrait en cause.
Les décisions imposées par le brief (`BRIEF.md`) ne sont pas répétées ici, sauf quand il fallait les préciser.

---

## D1 — Pas de fork du mod Kouzukii : on part de zéro et on s'en sert de référence (2026-10-18)

**Décision** : on ne forke pas `Kouzukii/expedition33-coop`. On réutilise sa *connaissance* (carte des hooks, chemins d'objets, cas spéciaux d'attribution des personnages), créditée dans la doc, sans reprendre de code ni d'assets.

**Raisons** (étude complète : `research/kouzukii-study.md`) :
1. **Modèle incompatible.** Son cœur est une seule instance du jeu dont on change, à chaque tour, la manette qui alimente l'unique PlayerController (`SetPlayerPlatformUserId` sur PC0). En ligne, les manettes distantes n'existent pas chez l'hôte : les hooks de tour, la bascule « Start » et l'add-on de boutons seraient tous à jeter.
2. **Chaîne d'outils fragile et exclusive.** Le mod remplace 7 Blueprints entiers du jeu par des copies patchées (`ue-hook-blueprints`). Ces copies doivent être reconstruites à chaque patch (déjà 2 crashs de mise à jour : issues #2 et #5), entrent en conflit avec tout autre mod qui touche ces assets, et embarquent du contenu du jeu (non redistribuable).
3. **Double système de hooks.** On aurait de toute façon besoin d'UE4SS pour le réseau. Or UE4SS C++ hooke ces mêmes fonctions Blueprint à l'exécution (`UObjectGlobals::RegisterHook`), sans remplacer d'asset, et atteint aussi le natif. Vérifié en jeu le 2026-10-18 : les hooks sur `OnTurnStart`, `TryStartParry`, `TryParryHitFromCharacter`, `ReceiveDamage`… s'enregistrent et se déclenchent.
4. **Ce qui compte pour nous n'y est pas** : résultats de QTE offensifs, autorité sur l'aléatoire et les dégâts, capture des décisions de l'IA, réplication. Tout est à écrire dans les deux cas.

**Réutilisé** : chemins `/Game/jRPGTemplate/...` des classes de combat, idée « filtrer `TryStart*` par propriétaire » pour les défenses, `MapBattleSpotToUser` et ses cas spéciaux (combats solo/marchand via `OverrideParty`, prologue « Sophie »), points d'injection UI (réglages, indicateurs de joueur). Crédit : Kouzukii (MIT).

**Remise en cause si** : jamais pour l'architecture ; on pourra lire ses graphes à nouveau pour un point d'UI précis.

---

## D2 — Build C++ : RE-UE4SS en sous-module épinglé, compilé en CI GitHub Actions (2026-10-18)

**Décision** : `native/third_party/RE-UE4SS` épinglé sur `83330635` (experimental 1181), le même commit que le `UE4SS.dll` installé. La CI (`windows-latest`, MSVC VS 18, Ninja, `Game__Shipping__Win64`) compile `UE4SS`, `proxy` (dwmapi.dll) et `E33Coop`, et publie l'ensemble comme artefact. On livre **notre** `UE4SS.dll` avec le mod.

**Raisons** : aucune toolchain locale (règle de Killian) ; l'ABI C++ d'UE4SS n'est pas stable entre builds expérimentaux, donc mod et DLL doivent venir du même commit ; ≥ 1176 nécessaire pour UE 5.4 (issue UE4SS #1433). Le sous-module privé UEPseudo exige un compte GitHub lié à Epic : fait par Killian le 2026-10-18, secret `UEPSEUDO_PAT` dans le repo privé.

**Alternative écartée** : en-têtes réécrits + lib d'import générée depuis les exports d'`UE4SS.dll` (approche RSDragonWildsMod). Gardée en plan B si l'accès UEPseudo est perdu.

**Point de vigilance** : issue UE4SS #1463 (Windows 11 26200 + hook `ProcessLocalScriptFunction`) — pas reproduite ici, les hooks BP fonctionnent sur cette machine (26200).

---

## D3 — Lua réservé à l'exploration et aux scénarios ; logique dans le C++ (2026-10-18)

**Décision** : le mod Lua `E33Explore` (télécommande `cmd.lua` → game thread, dumps, hooks de log) est un outil de dev. Les recettes trouvées en Lua (déclencher un combat, attaquer, charger une save) sont portées en C++ dans `native/`.

**Raisons** : les hooks Lua sur fonctions BP sont post-seulement, sans écriture des out-params ; plusieurs mods E33 rapportent des plantages avec beaucoup de Lua sur le game thread ; le C++ partage le code du protocole avec les outils.

---

## D4 — Saves de test hors du dossier Steam Cloud, chargées par nom de slot relatif (2026-10-18)

**Constat** : les noms de slot de `UGameplayStatics` sont relatifs à `%LOCALAPPDATA%\Sandfall\Saved\SaveGames\` ; le jeu y ajoute lui-même le dossier SteamID (`76561198355797267/EXPEDITION_0`). Le dossier SteamID est synchronisé par Steam Auto-Cloud.

**Décision** :
- Les saves de test vivent dans `SaveGames\e33coop-test\` (hors du dossier SteamID, donc hors Cloud), et sont chargées par `LoadGameFromSlot("e33coop-test/<nom>", 0)` puis `BP_SaveManager_C:LoadGameFromSaveFile(SaveGame, SlotName)`. Vérifié en jeu.
- Avant tout chargement de test : `SaveManager.EnableAutoSave = false`.
- Les fichiers de Killian dans le dossier SteamID sont en **lecture seule** pendant le développement (garde-fou en plus des backups). Exception : `steam_autocloud.vdf`, que Steam doit pouvoir écrire.
- `reset-env` ne touche jamais au dossier SteamID.

---

## D5 — Rejoindre une partie : instantané mémoire du jeu, pas la save disque (2026-10-18, à valider en phase 4)

**Constat** (dump) : `BP_SaveManager_C:CreateSaveGameDataFromGameState(out BP_SaveGameData_C*)` construit en mémoire les données de save depuis l'état courant, et `LoadGameFromSaveData(BP_SaveGameData_C*)` charge une partie depuis ces données. `UGameplayStatics::SaveGameToMemory` / `LoadGameFromMemory` (exposés par UE4SS) sérialisent un `USaveGame` en octets.

**Décision (plan A)** : au point sûr, l'hôte appelle `CreateSaveGameDataFromGameState`, l'emballe dans un objet de save (`BP_SaveGameObject_V8_C`), le sérialise via `SaveGameToMemory` et l'envoie ; le client désérialise et appelle `LoadGameFromSaveData` (ou `LoadGameFromSaveFile`), autosave désactivée → aucune écriture dans ses slots solo. L'emplacement « dédié au coop » est donc d'abord en mémoire ; on l'écrit aussi dans `SaveGames\e33coop\` pour la reconnexion.

**Plan B** (si l'instantané hors point de sauvegarde pose problème — p. ex. QuickSave (Nexus 421) signale qu'une save *en combat* corrompt) : on ne prend l'instantané qu'en exploration (c'est déjà la règle du point sûr) ; à défaut, dernière save de l'hôte + delta (position/zone via `MapToLoad` + `SpawnPointTagToLoadAt` et téléportation, `InteractedObjects`, inventaire, drapeaux de quête).

---

## D6 — Lancement du jeu par Steam (`steam.exe -applaunch 1903340 …`) (2026-10-18)

**Décision** : le lanceur de tests démarre le jeu via `steam.exe -applaunch 1903340 <args>` (arguments transmis à `Expedition33_Steam.exe` puis au jeu), jamais en lançant directement l'exe Shipping (pas de contournement de la protection Steam).

**Observations** : premier lancement bloqué par une fenêtre Steam (« ShowInterstitials ») validée par Killian ; ensuite le lancement est automatique. `-windowed -ResX=1280 -ResY=720` ne semble pas appliqué (rendu en 1920x1080) — à creuser (`GameUserSettings.ini` dédié aux tests). `-nullrhi` : pas encore testé.

---

## D7 — Captures d'écran par `PrintWindow` sur la fenêtre `UnrealWindow` (2026-10-18)

**Décision** : `tools/dev/screenshot.ps1` capture uniquement la fenêtre de rendu du jeu (même masquée), jamais l'écran entier — Killian peut utiliser son PC pendant les tests sans que son bureau soit capturé.

---

## D8 — Modèle de synchro du combat (phase 2, 2026-10-18)

**Principe** : chaque instance fait tourner sa propre simulation du combat (animations, caméra, UI) ; l'hôte est la seule source de vérité pour ce qui est calculé (dégâts, crits, PV, choix de l'IA, ordre des tours). Les clients rejouent puis **écrasent** leur état avec les résultats de l'hôte (`ActionResult.hits[].hp_after`, plus tard buffs/PA) — les « setters de réparation » déjà validés par E33 Versus (`CurrentHP`, `SetAP`, `SetCurrentShieldPoints`…).

**Tour d'un héros d'un client** : le client choisit l'action et résout ses QTE offensifs localement → `ActionCommand` (action, compétence, cibles, `combo_hits`/`combo_perfect`) → l'hôte exécute la même action avec `DEBUG_ComboAutoSuccessCount = combo_hits` (même chemin que le test runner) → `ActionResult` diffusé.

**Coup ennemi sur un héros d'un client** (le cœur du projet) :
1. Le client joue l'attaque sur son instance et résout la défense **localement**, au bon timing, au moment de son propre `TryParryHitFromCharacter` → `DefenseResult{hit k, résultat}` immédiatement.
2. L'hôte, à son propre `TryParryHitFromCharacter` pour le coup k d'un héros distant : si le résultat est déjà arrivé, il l'applique ; sinon il **attend** (pompe le réseau dans le hook, max `timeout_ms`, défaut 3 s → échec). Résultat « parade » ⇒ l'hôte appelle `TryStartParry` sur le défenseur dans le pré-hook, ce qui donne naturellement `Successful=true` (validé en phase 1, sans forcer le verdict) ; « échec » ⇒ rien.
3. Les coups sont identifiés par (combat, tour, n° de coup sur les héros de ce joueur), compté de la même façon des deux côtés.
4. Attaque de groupe : un `DefenseResult` par joueur concerné ; un joueur qui contrôle plusieurs héros envoie un seul résultat pour tous ses héros touchés par ce coup (règle du brief).

**Pourquoi attendre dans le hook** : le verdict de `TryParryHitFromCharacter` est synchrone. L'attente vaut ~RTT/2 par coup (le client voit l'attaque en même temps que l'hôte) ; à mesurer en phase 2. Si c'est perceptible, plan B : ralentir le temps de l'hôte (dilatation) pendant les fenêtres de coup d'un héros distant.

**Détection de désync** : à chaque début de tour, `StateHash` = FNV-1a 64 de (PV arrondis, PA, bouclier, mort) des héros puis des ennemis, dans l'ordre des emplacements, + acteur courant. En cas d'écart : log détaillé des deux états puis resynchronisation complète depuis l'hôte (phase 3).

**Hors phase 2** : forcer l'IA ennemie du client sur le choix de l'hôte (`PerformActionByAI`), compétences, objets, buffs — phase 3.

---

## D9 — Décisions de l'IA ennemie : l'hôte tire, les clients rejouent les tirages (2026-10-19, à implémenter en phase 3)

**Constat** : pas de `FRandomStream` ; l'IA d'un ennemi choisit sa compétence par `KismetMathLibrary::RandomIntegerInRange` appelé depuis son propre Blueprint, et sa cible via `FL_TargetingHelpers_C:GetRandomAliveHero` (`Array_Random`). Les tirages cosmétiques (VFX, caméra, répliques) utilisent le même générateur global, en nombre variable selon la machine → impossible de simplement « seeder » le RNG.

**Décision** : intercepter les fonctions de tirage natives (`RandomIntegerInRange`, `RandomInteger`, `RandomFloat(InRange)`, `RandomBool*`) **et** les helpers de ciblage BP (`GetRandomAliveHero`, `AcquireTargetRandomHero`, `GetRandomEnemy`) en post-hook, et filtrer par **appelant** (objet du cadre de script : un combattant, un skill script, un `BP_BattleDamages`) :
- hôte : enregistre chaque tirage de gameplay (appelant, fonction, n°, valeur) et l'envoie avec le tour ;
- client : remplace la valeur retournée par celle de l'hôte, au même rang pour le même appelant (attente bornée si elle n'est pas encore arrivée) ;
- tests : un scénario peut imposer une séquence de tirages → runs reproductibles à l'identique.

**Pourquoi pas forcer directement la compétence** : il faudrait connaître les événements `Skill_*` de chaque ennemi (des centaines de BP). Rejouer les tirages est générique : même état + mêmes tirages ⇒ même décision.

**Critère de fin de phase 1 en attendant** : même *verdict* sur 3 runs (toutes les vérifications vertes, ordres de défense tous obéis), pas mêmes compteurs.

---

## D10 — Tirages de gameplay : graine de l'hôte + flux par appelant, sans attente réseau (2026-10-19, amende D9)

**Constats (traces `rng_trace`, run hôte du 2026-10-19 10:30)** :
- Un combat de 6 tours fait environ 450 appels aux fonctions de tirage. L'immense majorité est cosmétique : AnimBP (visage, `ABP_Battle_Global`), sons (`BP_Audio_Spline_Random*`), répliques (`BP_BattleLineGatherer_C`), caméra (`AC_jRPG_DynamicCameraManager_C`), FX. Leur nombre varie d'une machine à l'autre.
- Les tirages de gameplay viennent de quelques appelants : l'IA ennemie (`PerformActionByAI` → `RandomIntegerInRange` dans l'ubergraph de l'ennemi, 1 par tour ennemi), les critiques (`BP_BattleDamages_C:RollCriticalHit`, à chaque coup), les effets de Luminas (`BP_LU_*:ProcessDealtRawDamages`) et l'activation de buffs (`AC_jRPG_CharacterBattleStats_C:ActivateBuff`).
- Avec D9 tel quel, le client attendrait les valeurs de l'hôte **en bloquant le thread de jeu**. Or, quand c'est le client qui attaque, il joue l'action *avant* l'hôte, donc chaque jet de critique coûterait environ RTT/2 de gel. C'est inacceptable : la raison d'être du projet est le zéro latence.

**Décision** :
- L'hôte choisit une **graine** par combat (dans `BattleStart`, protocole v3 ; `-e33seed=N` pour des runs reproductibles).
- Un tirage est « de gameplay » selon son premier appelant BP (liste ci-dessus). Les autres ne sont pas touchés.
- Chaque tirage de gameplay appartient à un **flux** = `Classe:Fonction BP:Fonction de tirage` (+ n° d'ennemi). Sa valeur vaut `f(graine, tour, flux, rang dans le tour)` et elle est convertie selon la fonction (`Min`/`Max`/`Weight` lus dans le cadre). Elle **remplace** le résultat natif, sur l'hôte comme sur les clients.
- Le nombre variable de tirages cosmétiques ne décale donc rien, et personne n'attend le réseau.
- **Vérification** : l'hôte publie chaque tirage (`Draws` : flux, rang, valeur). À chaque début de tour, le client compare les tirages du tour précédent ; tout écart (`coop_draw_mismatch`) fait échouer le test et nomme le flux fautif.
- L'hôte reste l'autorité : c'est lui qui choisit la graine, et ses PV écrasent toujours ceux des clients (`ActionResult`). Un écart de PV n'est plus silencieux : `hp_overrides` doit valoir 0 côté client.

**Limites connues** :
- `Array_Random` (ciblage `GetRandomAliveHero`, variantes d'anim) n'est pas remplacé, et **ne doit pas être hooké** : le hook UE4SS casse le ciblage des ennemis (GAME-INTERNALS §15). Le ciblage sera synchronisé en phase 3 au niveau de `GetRandomAliveHero` (avec un seul héros, sans effet).
- La liste des appelants de gameplay sera complétée au fil des compétences (phase 3).

**Remise en cause si** : un tirage de gameplay dépend d'un appelant que l'on ne sait pas classer de façon stable, ou si les ordres d'appel dans un même flux diffèrent entre machines (détecté immédiatement par `coop_draw_mismatch`).

---

## D11 — Combat à plusieurs joueurs : rejeu symétrique, résultats relayés par l'hôte, état complet à chaque tour (2026-10-19, phase 3, précise D8)

**Constat** : avec plusieurs héros, chaque instance doit jouer aussi les héros des **autres** joueurs (animations, caméra, ordre des tours), pas seulement l'IA ennemie. Recalculer leurs dégâts ne pose pas de problème (D10 rend les tirages identiques), mais leurs **décisions** (action, cibles) et leurs **timings** (QTE offensifs, défenses) n'existent que chez leur propriétaire.

**Décision** :
- **Identité** : un combattant = (camp, `BattleSpotIndex`), sur toutes les instances (plus d'index dans `Enemies`, qui bouge quand un ennemi meurt). Les flux de tirage d'un ennemi sont suffixés par son emplacement.
- **Actions** : le propriétaire publie `ActionCommand` (type, compétence/objet, cibles) au `RequestActionExecution` de son héros ; l'hôte la relaie aux autres. Chaque autre instance l'exécute au tour de ce héros (mêmes `SelectAction` / `SelectSecondaryAction` / `RequestActionExecution`), sans réussite automatique des QTE.
- **QTE offensifs** : chez le propriétaire, le résultat de chaque fenêtre (`IsSkillComboSuccessful` / `IsSkillComboPerfect`) est lu à la fin de la fenêtre et publié (`ComboResult`, n° de fenêtre du héros dans le tour). Ailleurs, il est **imposé** dans le pré-hook de `BP_BattleSequenceTimeline_C:OnSkillComboWindowEndReached` (attente bornée à 3 s, en pratique nulle : l'autre instance est en avance).
- **Défenses** : même schéma, par (tour, héros, n° de coup reçu par ce héros) : le propriétaire publie le verdict de son jeu (`DefenseResult` : parade, parade gradient, esquive, esquive parfaite, saut, échec) ; l'hôte le relaie ; les autres instances l'**imposent** avant le test du jeu (`TryStartParry` / `TryStartDodge` avec un début recalé pour une esquive non parfaite / `TryStartJump`, ou annulation de la fenêtre pour un échec). Une entrée de défense locale sur le héros d'un autre joueur est annulée. Une seule décision de défense par « vague » (même attaquant, même image) : une parade d'un joueur couvre tous ses héros touchés par ce coup.
- **État** : à chaque début de tour, l'hôte envoie un **état complet** (`StateSync` : PV, PA, bouclier, brise, mort, buffs hors agrégateurs, jauge gradient) et son hash. Le client compare son état **avant toute correction** (`raw_hash_diffs`, log champ par champ) et ne corrige que ce qui diffère (setters du jeu ; buffs ajoutés/retirés/ajustés). Un écart qui subsiste après correction est une désync (`desyncs`).
- **Ciblage aléatoire des ennemis** : post-hook de `FL_TargetingHelpers_C:GetRandomAliveHero` (et `GetSpecificHero` quand le héros voulu est mort) : la cible est remplacée par un tirage dérivé de la graine (D10) parmi les héros vivants triés par emplacement. `Array_Random` n'est toujours pas hooké.
- **Fin de combat** : l'hôte publie le résultat (`BattleEnd`) et ses récompenses (`BattleRewards`, lues dans l'out-param de `RollBattleRewards`). Les tirages de butin sont des tirages de gameplay (D10), donc identiques ; le client compare et prend les valeurs de l'hôte en cas d'écart (`rewards_mismatches`).
  - Ajout du 2026-10-22 (essai à deux PC) : le résultat de l'hôte fait foi. Si le jeu du client ne finit pas de lui-même (un tour y recommence, ou 5 s après le `BattleEnd`), la fin est imposée par étapes (`BM.ForceBattleEnd`, `StartBattleEndFlow`, ennemis restants mis à mort, puis nouvel instantané). Un ennemi vivant chez le client mais absent de l'état de l'hôte est mis à mort à la réparation ; les ennemis morts ne comptent pas dans le hash.
- **Lien** : les messages fiables de plus de 1 000 octets sont fragmentés (réassemblés dans l'ordre), pour rester sous la MTU d'Internet.

**Pourquoi pas « l'hôte calcule tout et les clients recopient l'état »** : les clients doivent voir les vraies animations (parades, sauts, QTE réussis ou ratés) aux bons moments ; recopier l'état seul donnerait des combats visuellement faux. Le rejeu des décisions + les tirages communs donnent la même simulation, et l'état complet de l'hôte reste le garde-fou (détection et correction).

**Remise en cause si** : une mécanique dépend d'une entrée continue (visée libre : rotation, tirs) — elle sera rejouée par ses tirs et leurs impacts, pas par la rotation (à faire) ; ou si une attente bornée devient perceptible en jeu réel (mesurée par `waited_ms` dans les logs).

---

## D12 — Session et exploration : instantané de l'hôte, pion piloté, interactions rejouées, barrière narrative (2026-10-20, phase 4)

**Mesuré en jeu le 2026-10-20 (TEST_B, Vallons fleuris puis camp)** :
- `SaveManager:CreateSaveGameDataFromGameState` → `BP_SaveGameObject_V8_C` construit puis `ConvertFromSaveGameData` → `GameplayStatics:SaveGameToSlot(obj, "e33coop/SNAP", 0)` écrit une save valide de 438 Ko dans `SaveGames\e33coop\` (hors dossier SteamID). Le point d'apparition vaut `Level.SpawnPoint.Generic.Dynamic` et `LastTransform` la position exacte du pion. Rechargée par `LoadGameFromSlot` + `LoadGameFromSaveFile` (chemin des saves de test), le pion réapparaît **exactement** à cette position. Le plan A de D5 tient, sans `SaveGameToMemory`.
- Pion d'exploration (`BP_jRPG_Character_World_C`, `ALSCharacterMovementComponent`) : `AddMovementInput` appelé par le code le fait marcher (467 u en 2 s, animation de marche), `K2_SetActorLocation(..., teleport)` le téléporte.
- Combat d'exploration : `BP_jRPG_Enemy_World_Base_Seamless_C:StartBattle(EngageType)` sur un ennemi du monde lance le vrai combat (vraie arène). `AC_jRPG_BattleManager_C:StartBattleNEW(FFBattleStartParams)` est appelé, puis `CurrentBattleStartParams` donne `EncounterRow.RowName`, `EnemyGlobalID` (`ObjectID_Enemy_<niveau>_<classe>_<guid>`) et `EngagementType`. La rencontre choisie est dans `SelectedEncounter` de l'ennemi. Les noms d'acteurs du niveau (`..._UAID_...`) sont stables.
- Transition : `FL_jRPG_CustomFunctionLibrary_C:ChangeMapByAssetName(LevelAssetName, SpawnTag, ctx)` change de niveau (camp en ~4 s) ; la chaîne observée est `PrepareToChangeMap` (contrôleur, GI) → `GI:ChangeMap` → `Change Map Internal` → `ChangeMapByAssetName`, puis `OnAreaChanged` dans le nouveau niveau. Le chargement d'une save passe aussi par `ChangeMapByAssetName`.
- Interactions : `OnInteractionRequested(PersonnageMonde, Instant)` sur un PNJ (`BP_jRPG_NPC_Dialog_C`) lance son dialogue ; `BP_DialogueSystemComponent_C:OnChoiceMade(i)` choisit une réponse ; `ActivateDialogue` / `EndDialogue` encadrent le dialogue.

**Décision** :
1. **Rejoindre** : au premier point sûr (pion d'exploration, hors combat, chargement, dialogue et cinématique), l'hôte écrit l'instantané dans `SaveGames\e33coop\snapshot` et l'envoie par morceaux (`SaveOffer` taille + hash, `SaveChunk`). Le client l'écrit dans `SaveGames\e33coop\host` (emplacement coop), le charge autosave coupée, puis annonce `JoinReady`. Ses saves solo ne sont jamais ouvertes en écriture. Si le client arrive pendant un combat, l'hôte répond `JoinWait` et envoie l'instantané à la fin. Même chemin pour revenir après une déconnexion.
2. **Pilote** : un seul pilote (l'hôte au départ). Il diffuse `PilotState` (niveau, position, rotation, vitesse) ~20 fois par seconde, sans fiabilité. Chez les autres, les entrées de déplacement locales sont coupées et le pion est conduit par `AddMovementInput` vers la position prédite, avec une téléportation au-delà d'un écart. `PilotRequest` / `PilotChange` : n'importe qui peut prendre la main, l'hôte arbitre.
3. **Interactions et dialogues** : les objets du monde sont identifiés par leur chemin complet (stable). Le pilote publie chaque `OnInteractionRequested` et chaque `OnChoiceMade` ; les autres instances font le même appel sur leur copie. Comme en combat (D11), chaque instance rejoue les décisions et calcule elle-même les effets (butin, drapeaux, dialogues, cinématiques déclenchées).
4. **Transitions** : le pilote publie `MapChange` (niveau + point d'apparition, lus sur le GI au `ChangeMap`) ; les autres appellent `ChangeMapByAssetName`, sauf s'ils sont déjà en route vers ce niveau (le pion suiveur a pu traverser le même déclencheur).
5. **Cinématiques et dialogues** : joués localement. Chaque instance annonce leur début et leur fin (`NarrativeState`). Celle qui termine avant les autres reste en pause de jeu jusqu'à ce que tout le monde ait fini (`BarrierRelease` de l'hôte, ou délai maximal en cas de pair muet). Précisé le 2026-10-22 (essai à deux PC) : une instance qui n'a pas commencé la scène des autres n'est mise en pause qu'après 6 s de grâce (le suiveur doit pouvoir la déclencher) ; jamais de barrière pendant un combat coop engagé (le démarrage coordonné attend les joueurs). Depuis D20 (2026-10-23), le suiveur déclenche aussitôt la scène du pilote (`SceneStart`).
6. **Combat d'exploration** : l'hôte lit les paramètres au `StartBattleNEW` et les ajoute à `BattleStart` (ennemi du monde, `EnemyGlobalID`, engagement). Le client lance le même ennemi (même rencontre imposée dans `SelectedEncounter`, puis `StartBattle`) ; à défaut, il passe par la recette du test runner avec le même `EnemyGlobalID`. Si son pion a déjà engagé le même combat, il ne le relance pas.
7. **Pause** : en combat, la pause d'un joueur met tout le monde en pause (`PauseState`, qui a mis en pause). En exploration, seule la pause du pilote arrête les autres (leur pion suit, donc il s'arrête de lui-même).
8. **Tests** : l'enregistrement d'une session ne contient pas les octets de la save (repo public, données de Killian) : le faux hôte lit l'instantané depuis `.tools/testsaves/` et le reprend du hash noté dans `SaveOffer`.

**Remise en cause si** : la conduite du pion suiveur dérive trop (escaliers, sauts, escalade) : téléportation plus fréquente ; ou si rejouer une interaction chez un suiveur produit un effet différent de l'hôte (détecté par un hash d'état d'exploration : or, objets, drapeaux).

---

## D13 — Builds et ressources communes en coop (2026-10-20, phase 5, choix de Killian)

**Décision** :
- Chaque joueur gère les personnages qu'il contrôle (niveau, attributs, compétences, équipement, Luminas). Répartition hors combat = même règle qu'en combat (k-ième personnage de l'équipe au k-ième joueur, l'hôte garde le reste), réattribuable.
- Chaque joueur ouvre ses menus sur sa propre instance. Ses opérations (attribuer un point, débloquer/équiper une compétence, équiper un objet…) sont envoyées à l'hôte, qui vérifie (propriétaire, conditions du jeu) puis les rejoue chez tout le monde, comme les décisions de combat (D11). Une opération sur un personnage d'un autre joueur est refusée et annulée chez son auteur.
- L'inventaire, l'or et les matériaux sont **communs** (save de l'hôte). **Option 1 retenue par Killian** : tout le monde peut dépenser (améliorations d'armes, achats, consommables) ; l'hôte arbitre dans l'ordre d'arrivée, une dépense devenue impossible est refusée et annulée.
- Filet de sécurité : l'empreinte des personnages entre dans le hash d'état d'exploration ; un écart persistant est réparé par un nouvel instantané.

> Idée notée le 2026-10-20 (Killian), **pas retenue pour l'instant** : exploration libre (chacun déplace son personnage, avatars des autres joueurs visibles, combat lancé par n'importe qui avec téléportation des autres, regroupement aux transitions et cinématiques). Faisable mais de la taille d'une phase entière (ennemis du monde à synchroniser) : à reconsidérer après la phase 6.

---

## D14 — Robustesse de la session (2026-10-20, phase 5)

**Décision** :
1. **Lien** : chaque `Link` tire une **époque** ; un changement d'époque à la même adresse = le pair a redémarré (nouveau processus, ou même compte Steam). Pendant un chargement (thread de jeu bloqué 30 à 60 s), un thread à part envoie des paquets « occupé » (`ack = 0xFFFFFFFF`, aucun message). Perte : 15 s de silence total, ou 300 s de seuls « occupé ». Les 6 premiers octets d'un paquet (magic, protocole) ne changeront plus : un pair d'une autre version reçoit un paquet de la nôtre et affiche les deux versions.
2. **Départ d'un client** : l'hôte le retire, reprend ses héros (`Owners`), joue lui-même un tour en attente, prévient les autres (`Roster`, message). Retour = chemin « rejoindre » (`JoinWait` en combat, instantané après ; un instantané devenu périmé par un combat commencé pendant le chargement est refait).
3. **Départ de l'hôte** : `Bye`, ou silence → fin de session côté client, message clair, retour au menu principal (la partie chargée est celle de l'hôte). Pas de migration d'hôte (brief).
4. **Démarrage coordonné** : l'hôte tient son premier tour en pause jusqu'au `BattleReady` de chaque client de la partie. Pause du jeu partagée entre plusieurs raisons (barrière, pause d'un joueur, démarrage) et **réappliquée** si le jeu l'enlève (fin de cinématique).
5. **Hash d'exploration** (or, inventaire, objets utilisés, ennemis vaincus, drapeaux, GPE, cinématiques et dialogues vus) au calme ; deux écarts de suite → nouvel instantané. Filet de sécurité, pas mécanisme principal.
6. **Builds par état** (D13) : empreinte de `SaveCharacterState` hors menu ; état sérialisé par le jeu (save réduite aux personnages) ; propriétaire vérifié par l'hôte ; modification d'un personnage d'un autre joueur annulée avec l'état de l'hôte. Pourquoi pas le rejeu des clics : les menus modifient parfois les données directement (Luminas).
7. **Ressources communes par variations** (D13 option 1) : or, quantités, niveaux d'équipement relevés au calme ; une variation sans événement rejoué entre deux relevés est locale (achat, amélioration) et part en `EconomyDelta` ; l'hôte vérifie qu'elle est possible, l'applique avec les fonctions du jeu et la relaie ; sinon refus et resynchro de l'auteur.
8. **Textes** : cartes « toast » du jeu (`WBP_ToastCard`), une persistante pour les indicateurs, une par message ; table FR/EN dans `native/ui`.

**Remise en cause si** : un achat coïncide souvent avec un événement rejoué (la variation est alors absorbée et la resynchro reprend l'état de l'hôte) ; ou si `LoadCharacterState` ne reproduit plus l'empreinte de l'émetteur (compteur `build_hash_mismatches`).

---

## D15 — Sauvegarde côté client : copie coop à jour, hébergeable (2026-10-22, demande de Killian)

**Constat** : la progression commune n'existait que dans la save de l'hôte. Si l'hôte est absent (ou change), son ami ne peut pas continuer, et peut tout perdre.

**Décision** :
1. **Où** : `SaveGames\e33coop\copies\<hôte>.sav` + `<hôte>.json` (nom affiché, date, niveau, origine) + `<hôte>.prev.sav` (copie précédente). Nom de fichier = nom de l'hôte en ASCII ; une copie par hôte, la plus récente proposée. Jamais dans `SaveGames\<SteamID>\`. Les tests écrivent dans `e33coop\test-copies\` (vidé par `reset-env`), jamais dans les vraies copies.
2. **Client, à la réception** d'un instantané (rejoindre, resynchro) : les octets vérifiés deviennent la copie.
3. **Client, ensuite** : à chaque empreinte d'exploration **identique** à celle de l'hôte (D14.5, donc au calme, à un point sûr), la copie est refaite localement (`CreateSaveGameDataFromGameState`, comme l'instantané de l'hôte) si l'empreinte a changé depuis la dernière copie (au plus une fois par minute, `-e33copymin`) ou au bout de 5 min (`-e33copyevery`).
   Pourquoi pas un nouvel instantané demandé à l'hôte à chaque autosave : il faudrait un message de plus (version de protocole), ~440 Ko par envoi, et aujourd'hui recevoir un instantané recharge la partie du client. La copie locale ne coûte rien sur le réseau, ne recharge rien, et n'est écrite que quand l'état du client est vérifié égal à celui de l'hôte (or, inventaire, objets, ennemis, drapeaux, GPE, cinématiques, dialogues ; builds par D13).
4. **Héberger la copie** : options de lancement de l'hôte (`-e33role=host -e33transport=steam`) ; au menu principal, la ligne « Coop : F10 pour héberger la partie coop de X (copie du JJ/MM HH:MM) » ; **F10** charge la copie (autosave coupée), le lobby s'ouvre comme d'habitude (F7 pour inviter). La progression va dans la copie : à chaque instantané envoyé, après chaque demande de sauvegarde du jeu (autosave, drapeau) au calme suivant, toutes les 5 min au calme, et au retour au menu (menu pause). Un client lancé en `-e33role=client` voit seulement un rappel au menu (comment l'héberger).
5. **Saves perso protégées** tant que la partie chargée vient de l'hôte ou d'une copie : autosave coupée au chargement (comme avant), **retenue coupée** (relevé chaque seconde, `coop_autosave_reenabled`), recoupée avant toute demande de sauvegarde du jeu (`RequestAutoSave`, `ForceAutoSave`, `RequestSaveInternal`, `SaveGameToFile`), et filet : un `AsyncSaveGameToSlot` hors `e33coop` voit son objet de save retiré (l'écriture échoue, `coop_personal_saves_blocked`). La garde se lève au retour au menu principal, au chargement d'une save hors `e33coop` ou à une nouvelle partie (sauf nouvelle partie suivie, remarquée juste après).

**Non vérifié en jeu** : l'appel effectif de ces fonctions de sauvegarde (noms tirés du dump), le nom de leurs paramètres (repli : premier paramètre chaîne / objet), et l'échec propre d'un `AsyncSaveGameToSlot` sans objet. Le filet ne devrait jamais servir.

**Remise en cause si** : la copie locale du client diffère de la progression de l'hôte sur un point que l'empreinte ne couvre pas (quêtes, carte du monde) : passer à l'instantané de l'hôte envoyé à chaque autosave (option écartée au point 3).

## D16 — Bonus « parade en duo » (2026-10-22, demande validée par Killian)

**Décision** :
- Pendant un tour, si des héros d'**au moins deux joueurs** sont touchés (attaque de groupe, ou coups enchaînés sur plusieurs héros) et que **tous** les coups reçus sont parés (parade ou parade gradient), chacun de ces héros gagne **+1 PA** (`Stats.GainAP(1, raison 0)`, plafonné par le jeu aux PA max) ; toast « Parade en duo ! +1 PA (héros) » chez tout le monde.
- **Calcul local et identique partout** plutôt qu'un message de l'hôte : chaque instance connaît le résultat retenu pour chaque coup (le sien publié, ou celui du propriétaire imposé, D11) et les propriétaires du combat. Le bonus est appliqué au début du tour suivant (`OnTurnStart`, avant la photo d'état et le hash), donc au même point du jeu chez l'hôte et chez les clients : pas d'écart de hash, pas de réparation. Un délai de défense dépassé (échec imposé d'un côté seulement) reste rattrapé par `StateSync`.
- Option de l'hôte, active par défaut, `-e33duoparry=0` pour la couper ; envoyée dans `BattleStart.duo_parry` (champ final facultatif : absent = coupé, donc les anciens enregistrements rejouent sans bonus). Les clients suivent toujours le réglage de l'hôte.

**Remise en cause si** : la raison 0 de `GainAP` déclenche un effet indésirable (pictos liés à une raison précise) ; ou si `duo_parry_ap` montre un gain différent de 1 (journal `coop_duo_parry` : PA avant/après).

## D17 — Miroir des menus de combat (2026-10-22, demande de Killian)

**Décision** :
- Pendant le tour du héros d'un autre joueur, l'observateur voit les **menus du jeu** de ce joueur (roue, compétences, objets, gradient, colonne, ciblage et curseur) au lieu d'une ligne de texte. Le propriétaire envoie son état (`MenuState`, type 36, fiable, à chaque changement, 15 fois/s au plus) ; l'observateur appelle les fonctions de l'interface du jeu (pages de `WBP_HUD_WheelsContainer`, `BM.SelectAction` / `SelectSecondaryAction` pour ouvrir le ciblage, groupes du `AC_jRPG_TargetingManager` pour le curseur, `BM.OnCancelTargeting` pour le retour).
- **Jamais de confirmation par le miroir** : ni `RequestActionExecution`, ni `ConfirmTarget`, ni `StartFreeAimFromUI`. L'action part toujours par `ActionCommand` (inchangé) ; juste avant son exécution (et avant le rejeu d'une visée libre), le miroir referme tout (ciblage annulé, roue revenue à sa page du début du tour) : l'exécution part du même état qu'avant le miroir. `SelectAction` / `SelectSecondaryAction` sont les appels que l'exécution fait de toute façon.
- Entrées d'ici pendant ces tours : en plus des contextes Enhanced Input retirés (phase 5), navigation d'interface du jeu coupée (`BP_UINavigationComponent.DisableNavigation`) et clics sur la roue 3D coupés (`LGUIWorldSpaceRaycaster.DeactivateRaycaster`), rendus en fin de tour ou quand le menu pause est ouvert (`-e33inputguard=0` pour revenir à avant).
- Correctifs après le premier essai en jeu (2026-10-22, `p7_menumirror_host`, branche `fix/menu-mirror`) :
  - le **pré-ciblage** de la roue des actions (curseur sur l'ennemi dès le début du tour, `TargetingEnabled?` vrai avec `IsPretargeting`) n'est plus pris pour le ciblage d'une action : le miroir l'« annulait » en boucle (`menu_mirror_gave_up` « cancel: ») et n'ouvrait jamais pages ni colonnes. `MenuState.targeting` ne vaut que pour le ciblage d'une action choisie ;
  - une touche ou un clic d'ici confirmait le ciblage ouvert par le miroir (`ConfirmTarget` puis `RequestActionExecution` hors mod, l'action du héros de l'autre joueur partait ici). Garde renforcée pendant les tours des autres : entrées du ciblage retirées (`WBP_HUD_BattleScreen.UnregisterTargetingInput` dès que `IsTargetingRegistered` est levé, et juste après chaque choix rejoué), invites cliquables (`WBP_HUD_TargetingControls`, `WBP_BattleInput_PlayerTurn`) désactivées, et en dernier recours ciblage refermé (`BM.OnCancelTargeting`) avant une `ConfirmTarget` qui ne vient pas du mod (un crochet UE4SS ne peut pas annuler l'appel). Pas de ciblage rejoué si `UnregisterTargetingInput` manque. Journal `input_route` : chemin pris par une entrée qui fuit ;
  - garde réarmée et comptée à chaque tour d'un héros d'un autre joueur (tours consécutifs compris).
- Une partie impossible à rejouer (fonction introuvable, refus du jeu 3 fois) : ligne de texte pour ce tour. `-e33menumirror=0` : ligne de texte seulement. Visée libre : ligne de texte puis caméra rejouée (inchangé).
- Preuve : scénarios `p7_menumirror_host` (captures chaque seconde ; entrées d'ici injectées, `spam_local_input`, `local_input_leaks` = 0) et `p7_menumirror_off_host` (témoin sans miroir).

**Remise en cause si** : un appel du miroir change un état comparé par le hash (écart au tour suivant), déclenche une exécution (`tr_input_leak`, `menu_drift`), ou si les fonctions des pages ne font rien sans entrées enregistrées (`menu_mirror_gave_up`).

## D18 — Installation en un fichier, rôle décidé en jeu, mises à jour par les releases, options (2026-10-22, validé par Killian)

**Constat** : pour jouer, l'ami devait décompresser un zip, lancer un script PowerShell et taper des options de lancement Steam différentes selon le rôle ; chaque nouvelle version demandait de refaire tout ça.

**Décision** :
1. **Pas d'option de lancement.** Sans `-e33role`, le mod démarre en mode **neutre** (« prêt au coop », Steam). Le rôle se décide en jeu : **F7** dans une partie chargée (hors combat) → hôte (lobby créé à ce moment, fenêtre d'invitation dès qu'il est prêt) ; au menu principal, invitation acceptée, `+connect_lobby`, **F7** ou bouton **Rejoindre *ami*** → client (rejoint toujours la partie de l'hôte) ; **F10** au menu → hôte de la copie coop (D15). Une invitation acceptée en pleine partie attend le retour au menu (pas de perte de progression). Fin de session puis retour au menu → neutre.
2. **Initialisation dépendante du rôle différée** : en mode neutre, seuls la copie coop (gardes de sauvegarde), le retour au menu et le bouton Rejoindre sont abonnés ; les hooks du coop (combat, exploration, aperçu) le sont au moment où le rôle est choisi (`HookBus` enregistre aussi les abonnements tardifs). Retour au neutre : état de session, exploration, builds et présence remis à zéro ; hooks gardés (inactifs sans rôle) ; transport Steam gardé.
3. **Options explicites inchangées** : `-e33role` (et toutes les `-e33…`) garde l'ancien fonctionnement, UDP seulement si demandé. En test, seules les `launch_args` du scénario comptent (inchangé). Le test à distance garde son mode client par défaut.
4. **Version unique** : fichier `VERSION`, lu par CMake (mod, installateur, faux pair) ; le workflow `release` refuse un tag différent (docs/RELEASE.md).
5. **Installateur en un fichier** (`Installer E33 Coop.exe`, Win32, sans dépendance) : trouve le jeu par Steam (registre, `libraryfolders.vdf`, `appmanifest_1903340.acf`), télécharge le zip de la dernière release (HTTPS, hôtes GitHub seulement, redirections vérifiées), vérifie le SHA-256 contre `SHA256SUMS` de la même release, installe comme `install.ps1` (sauvegarde de l'ancien contenu, remise en place si échec), garde `settings.ini` et `remotetest.txt`. Rien n'est installé sans empreinte correspondante. Zip local + `SHA256SUMS` à côté de l'exe : hors ligne.
6. **Mises à jour** (`updates = auto | ask | off`, défaut `auto`) : au menu principal, vérification en arrière-plan (une fois par lancement, erreurs réseau ignorées). `auto` : l'installateur du dossier du mod (chemin et arguments fixes : `--stage`) télécharge et vérifie, signale « prêt », puis installe à la fermeture du jeu ; jamais de redémarrage en pleine partie. `ask` : carte « U pour mettre à jour » au menu (`--update` : attend la fermeture, installe, relance par `steam://rungameid/1903340`). Écart de version à la jonction : le côté le plus ancien est prévenu ; client en `auto` avec une release plus récente publiée → mise à jour immédiate, relance et nouvelle jonction de l'ami (SteamID gardé 15 min dans `SaveGames\e33coop\rejoin.txt`) ; pas de mise à jour automatique en boucle si l'autre a une version de développement.
7. **Options** : `ue4ss/Mods/E33Coop/settings.ini` (`lang`, `updates`, `aim_cam`, `turndep`, `duo_parry`, `turn_timer_s`) ; ordre : scénario de test seul → option de lancement → fichier → défaut. Le test à distance reste dans `remotetest.txt` à part : c'est une autorisation (SteamID de confiance, relance, mise à jour), écrite seulement par l'installation sur demande, qu'un réglage de confort ne doit pas pouvoir activer.
8. **Panneau Coop au clavier** : **F9** choisit une ligne (personnages de l'équipe pour l'hôte hors combat, puis options), **Maj+F9** la change (personnage → joueur suivant, même chemin que F6 ; option → valeur suivante, enregistrée). Pas de flèches ni d'Entrée : le panneau n'a jamais le focus, le menu pause du jeu garde sa navigation (**remplacé par D24** : panneau navigable, F9 / Maj+F9 restent des raccourcis). Hors du menu pause, une carte montre la ligne choisie. Options d'hôte en lecture seule chez un client.

**Non vérifié en jeu** : tout le mode neutre (F7 hôte en pleine partie avec hooks accrochés tard, invitation, retour au neutre), la vérification de release et l'installateur lancé depuis le jeu (`CREATE_BREAKAWAY_FROM_JOB`, copie de l'exe dans `%TEMP%`), l'installation à la fermeture du jeu.

**Remise en cause si** : un hook accroché en pleine partie rate un état déjà commencé (combat, cinématique) ; ou si Steam relance le jeu avant la fin de l'installation à la fermeture.

## D19 — Carte du monde : pilote sur Esquie, suiveur « fantôme » (2026-10-22, bug signalé par Killian)

**Constat** (essai à deux PC, sans journal) : l'hôte (pilote) vole sur Esquie ; chez le client, le pion suiveur (à pied) est conduit vers une position en l'air ou au-dessus de l'eau : il tombe, nage, est téléporté (écart vertical > 400), retombe… « comme s'il entrait et sortait de l'eau 20 fois par seconde » (cadence de `PilotState`). Le suiveur n'a aucun moyen de reproduire la monture : `PilotState` ne portait que position, rotation et vitesse.

**Données du jeu (dump)** : `BP_jRPG_GI_Custom_C.IsMountInUse` (aussi dans la save : `BP_SaveGameData`, `BP_SaveGameObject_V0`) ; `BP_jRPG_Character_World_C.bIsWorldMapPawn`, capacités (`E_jRPG_CharacterWorldCapability`, `UpdateCharacterCapabilities`) ; `BP_jRPG_Controller_World_C.OnDismountRequested()` et `LastControlledCharacterReference` (le contrôleur change de pion à la monture) ; `CharacterMovement.MovementMode` (`EMovementMode` : 3 chute, 4 nage, 5 vol, 6 custom). La classe d'Esquie et sa fonction « monter » ne sont pas dans le dump (pris hors carte du monde).

**Décision** :
1. `PilotState` gagne des champs finaux facultatifs : `move` (`EMovementMode`, 255 = inconnu/absent), `custom`, `flags` (monture = `IsMountInUse`, pion de carte du monde, pion attaché) et `pawn` (classe du pion). Anciens enregistrements : décodés avec `move` = 255, suivi inchangé.
2. Suiveur, **sur la carte du monde seulement** (drapeau du pilote, `bIsWorldMapPawn` d'ici ou niveau `*WorldMap*`) : si la monture diffère, si la classe du pion diffère, si le pilote est attaché, ou s'il vole / est en mode custom (0,3 s), le pion est tenu en **fantôme** : caché, sans collision, en mode vol (sans gravité), placé sur la position prédite et lissée du pilote à chaque image. Rendu (posé à la position du pilote, visible, chute puis marche) après 1 s d'état reproductible, ou dès que le suivi s'arrête (prise de main, combat, niveau, fin de session). Pas de nage reproduite à tort, plus de boucle air/eau.
3. Rien n'est écrit dans la save : la copie coop du client (D15) est sautée tant que le pion est fantôme (position en l'air, sans monture ici).
4. Suiveur resté sur Esquie (instantané pris monté) alors que le pilote est à pied au sol : `OnDismountRequested()` du contrôleur, 3 essais au plus (`-e33mountsync=0` pour couper).
5. Journal pour le prochain essai : `coop_pilot_move` (pilote : mode, monture, classe, z), `coop_pilot_pawn_other` (pion possédé hors `BP_jRPG_Character_World_C`), `coop_mount_functions` (fonctions du pion et du contrôleur contenant mount/esquie/fly/swim/land… : de quoi rejouer la monture plus tard), `coop_follow_mode` (pilote vu d'ici et pion d'ici), `coop_follower_ghost` (entrée/sortie, raison, durée), `coop_follower_ghost_mode` (le jeu repasse le pion en nage/chute), `coop_follower_dismount`, `coop_copy_skipped`.

**Limite** : pendant le vol, l'ami ne voit pas l'hôte (pas d'avatar distant) ; sa caméra suit le pion caché. Reproduire la monture (Esquie chez le suiveur aussi) demande le nom de la fonction de montée, à lire dans `coop_mount_functions`.

**Remise en cause si** : le pion de la carte du monde à pied utilise un mode custom (fantôme permanent : `coop_follower_ghost` raison `pilot_move` avec `pilot_mount` faux) ; ou si `IsMountInUse` n'est pas mis à jour à la monture (raison `pawn_class` à la place, à vérifier dans `coop_pilot_move`).

---

## D20 — Cinématiques : le suiveur déclenche la scène du pilote aussitôt (2026-10-23, demande de Killian)

**Constat** (essai à deux PC du 2026-10-22) : scène de Sophie jouée en série (hôte 798→835 s, client 835→874 s). La scène part d'une zone `BP_GameActionTriggerZone_C` au bas de la corde ; le pion suiveur, quelques mètres derrière, était figé par la barrière avant d'y arriver.

**Décision** :
1. Le pilote publie `SceneStart` (nouveau type 53 ; un pair ancien l'ignore) au pré-hook de `BP_CinematicSystem_C:TriggerCinematic` : identifiant (`Cinematic_LevelSequence_ID`, souvent `None`), asset de séquence, cause (zone « au passage » vue par `ExecuteGameActionWrapperFromTriggerZone` pendant `TryExecuteGameActionOnOverlap`, sinon interaction publiée, sinon game action ; fenêtre de 60 s), game action et position du pilote.
2. Le suiveur, s'il n'est pas déjà dans une narration ni dans la même scène (clé = identifiant ou séquence, 120 s) : zone -> `TryExecuteGameActionOnOverlap()` sur sa copie de la zone (le jeu tient l'état de la zone : pas de double lecture quand le pion y passe ensuite) ; zone qui refuse -> son action par `ExecuteGameAction` ; sinon (cause inconnue, pas de zone) téléportation du pion sur la position du pilote au déclenchement. Interaction : rejouée par `WorldInteract`, repli après 3 s. Résultat vérifié 3 s après (`scene_syncs_ok` / `_failed`).
3. `SceneSkip` (type 54) : publié quand le pilote a vraiment passé la scène (`OnSequenceFinished` ≤ 5 s après `SkipCinematic`) ; le suiveur passe la sienne (pause de la cinématique puis passage, réessayé pendant 10 s tant que la séquence joue).
4. La barrière ne sert plus qu'à attendre la fin ; le délai de grâce de 6 s reste le filet (pair ancien, scène non retrouvée).

**Limites** : une scène enchaînée sur un combat (Renoir) part aussi chez le suiveur par sa propre game action : combat déjà engagé ici, accepté par `BattleStart` (« already_engaged »), non testé en scénario. Cinématiques lancées autrement que par `TriggerCinematic` (vidéo d'intro) non couvertes.

**Remise en cause si** : `coop_scene_sync` en méthode 3 (téléportation) fréquente, ou `scene_syncs_failed` ; ou une zone dont l'action dépend du pion qui la traverse.

---

## D21 — Carte du monde : le suiveur monte sur Esquie et le conduit comme le pilote (2026-10-23, demande de Killian)

**Données du jeu (en jeu, save `TEST_LATE`)** : pion à pied `BP_jRPG_Character_WorldMap_pawn_C`, Esquie = pion `BP_WorldMapCharacter_C` (sous-classe de `BP_jRPG_Character_World_C`) possédé à la montée. `BP_WorldMapControllerComponent` du contrôleur : `OnInputMountPressed` -> `OnMountRequested` -> `ProcessMountRequest` (montée), `OnDismountRequested` (descente, refusée en vol), `MountIsInUse`, `IsMountPerformingAnAnimation`. Esquie : `CurrentMovementMode` (0 marche, 1 eau, 2 vol), `TryChangeMovementMode(MovementMode, ZoneType)` (décollage / atterrissage, appelé par l'événement de saut), `ChangeMovementMode` (imposé), `IsInTransition`.

**Décision** :
1. `PilotState` gagne un dernier champ facultatif `wm_mode` (mode d'Esquie, 255 = pas sur Esquie).
2. Suiveur sur la carte du monde, pilote sur Esquie : montée par `OnInputMountPressed` (repli : événement de la touche de monture), 3 essais toutes les 1,5 s, pion laissé sur place pendant ce temps. Puis même mode (`TryChangeMovementMode`, `ChangeMovementMode` après 2 refus), toutes les 0,5 s au plus.
3. Conduite : vol et eau (3000-4000 u/s) : Esquie placé à chaque image sur la position prédite du pilote (cible avancée à sa vitesse puis rapprochée de la position reçue en ~0,1 s) ; marche (350 u/s) : `AddMovementInput` comme le suivi à pied, recalé au-delà de 1500 u. Pendant les animations de montée / transitions : rien.
4. Pilote à pied, suiveur sur Esquie : retour au sol (mode marche) puis `OnDismountRequested`, 8 essais.
5. Le fantôme de D19 reste en repli (montée impossible, ex. pion à la nage) ; `-e33mountsync=0` coupe montée et descente.

**Remise en cause si** : `coop_follower_mount` échoue souvent (`can_mount` faux), ou `coop_follower_wm_mode` `forced` fréquent (mode refusé par le jeu), ou Esquie placé qui saccade visiblement en vol (passer à une poursuite par entrées).

---

## D22 — Aucune attente sans fin en combat ; action automatique seulement sur preuve positive (2026-10-23, exigence de Killian)

**Constat** : duel du prologue (2026-10-22) : client tenu par la barrière, hôte figé 10 min au plus en attendant son `BattleReady`. Audit des attentes : seules celles du thread de jeu (`wait_for`) figent l'image ; toutes sont bornées (tour 15 s, défense et QTE 3 s, tir 2 s, récompenses 5 + 4 s) ; les attentes non figées (démarrage coordonné 600 s, barrière 600 s, tour d'un autre joueur sans minuteur : illimitée) pouvaient tenir la partie pour un joueur dont le jeu ne suit plus.

**Décision** :
1. **Démarrage coordonné** : l'hôte attend toujours un joueur qui charge, regarde une scène, est dans un menu ou se reconnecte (10 min au plus). Un joueur **inactif** (rien de tout ça) qui n'entre pas dans le combat depuis 60 s continues (`-e33readyidle=N`) : le combat commence sans lui (toast), il suivra en arrivant. Un client sain entre en ~6 s sans action du joueur.
2. **Tour d'un autre joueur** : jamais d'action automatique pour un joueur qui réfléchit, fouille ses menus, est en pause, alt-tab ou lit une description, même longtemps. Attaque automatique (`-e33stuckturn=N`, 60 s de temps de jeu, pauses exclues) seulement si le lien est vivant, qu'il ne charge ni ne se reconnecte, **et** qu'aucun signe de ce tour n'est venu de lui : ni `StateHash` du tour (envoyé par son jeu au début de chaque tour), ni `MenuState`, `IntentPreview`, `AimView`, `FreeAimEvent`, `ActionCommand`, `ComboResult`, `DefenseResult` de ce tour. Le minuteur de tour reste une option de l'hôte, coupée.
3. **Joueur bloqué** (tour forcé, ou délai de défense dépassé sur un tour qu'il n'a pas commencé) : ses défenses et QTE ne sont plus attendus (échec imposé, l'état de l'hôte fait foi, D11) jusqu'à son prochain signe de tour.
4. **Client** : les attentes bloquantes s'arrêtent au `BattleEnd` de l'hôte (aucun résultat ne viendra).
5. **Mesure** : chaque attente bloquante est journalisée au-delà de 250 ms avec son origine (`coop_wait_long`).
6. Essai `-e33turnalign=1` (désactivé ; remplacé par D28, actif par défaut) : tours ennemis retenus chez l'hôte (dépendance de début de tour, jeu animé) jusqu'au `StateHash` des clients qui ont un héros vivant, 4 s au plus : l'hôte n'est plus en avance (~1 s après les tours de ses héros, rejoués avec décalage chez le client), donc ne fige plus son image en attendant leurs parades ; le client attend celles de l'hôte un aller-retour.

**Remise en cause si** : `turn_stuck_forced` ou `battle_ready_idle_timeouts` apparaît dans une partie normale à deux PC (faux positif) ; ou `coop_wait_long` site `defense` fréquent au-delà de 300 ms (activer `-e33turnalign`).

## D23 — Suivi du pilote : tampon d'interpolation, commande en vitesse, critères de fluidité (2026-10-23, demande de Killian)

**Constat** (essai à deux PC du 2026-10-22, journaux du client en 0.6.1) : quand l'hôte courait, l'ami voyait des « freeze ». Causes trouvées côté mod : téléportations toutes les ~2,2 s (sprint non suivi, seuil fixe de 800 u) et **deux rechargements d'instantané de 5-6 s en plein sprint** (resynchro d'inventaire demandée sans regarder le mouvement) ; en 0.6.4 encore (`p8_sprint_client`) : sprint du suiveur obtenu seulement par le repli `SetDesiredGait` après 0,6 s, retard moyen de 200 u jamais rattrapé (le suiveur ne peut pas aller plus vite que le pilote), départs/arrêts répétés en marche, et une resynchro envoyée en plein sprint.

**Décision** :
1. `PilotState` gagne un champ final facultatif `t_ms` (horloge du pilote à l'envoi). Ancien message : décodé avec `t_ms` = 0 (instants de réception utilisés).
2. Le suiveur garde les positions du pilote dans un tampon trié par `t_ms` et vise la trajectoire du pilote 100 ms après le transit minimal (120 ms sans horloge) : interpolation d'Hermite (positions + vitesses), prolongée au plus 0,5 s si une position tarde (sans arrêt ni sortie du sprint tant que le retard reste < 1 s), puis pilote tenu arrêté. Un trou > 1 s ou un saut > 400 u n'est pas interpolé (téléportation « gap » permise).
3. Commande en vitesse : vitesse voulue = vitesse du pilote rendu + 2 x écart (1/s), plafonnée à 1,2 x sprint. En course : échelle d'entrée = vitesse voulue / `MaxWalkSpeed`. Sprint quand le pilote rendu dépasse 1,1 x la course (ou retard > 120 u) : allure voulue Sprint par `SetDesiredGait` (plus de `SprintAction`, sans effet sur le pion suiveur), entrée pleine (l'ALS n'accorde le sprint qu'au-delà de 0,9) et vitesse réglée par `CharacterMovement.CurrentMovementSettings.SprintSpeed` = vitesse voulue (de la course + 15 à 1,2 x le sprint) + `bRequestMovementSettingsChange` ; rendue (720) à la sortie. **Mesuré** : écrire `MaxWalkSpeed` directement est sans effet (le pion restait à 720 avec 365 ou 864 écrits). Sortie du sprint dès que pilote et vitesse voulue repassent sous 1,03 x la course, sans délai : le pion ralentit avec le pilote (avant : 720 gardé puis freinage de 662 à 185 u/s en 0,1 s).
4. Téléportation : trou de données (« gap »), écart > 800 u qui ne baisse pas de 50 u en 1 s ou > 1600 u, pion bloqué 2 s, écart vertical > 400 u pendant 0,5 s.
5. Client : une resynchro (rechargement) n'est jamais demandée pendant un mouvement (pilote ou pion local > 50 u/s dans les 1,5 s) : attendue jusqu'à l'arrêt, au plus 120 s.
6. **Critères de fluidité** mesurés par le mod chez le suiveur (compteurs, vérifiés par les scénarios `p8_sprint_*`) : 0 téléportation hors trou de données ; écart au pilote < 250 u en sprint, < 150 u en course, < 120 u en marche, < 60 u à l'arrêt ; 0 « stall » (pion < 30 % de la vitesse du pilote pendant 150 ms, pilote en mouvement), immobilité du pion pilote en mouvement <= 150 ms ; 0 creux (< 60 %, 150 ms) et 0 chute brusque (> 250 u/s en 0,1 s) pendant une course continue ; 0 arrêt du pion pilote en mouvement ; 0 image > 50 ms due au mod pendant le mouvement ; 0 resynchro envoyée pendant le mouvement. Pourquoi ces seuils : à 720 u/s, 0,2-0,3 s de retard (transit + tampon + réaction de l'ALS) font 150-220 u ; 150 ms d'immobilité est le seuil où un arrêt se voit ; à l'arrêt, 60 u = un pas.

**Remise en cause si** : le jeu impose sa vitesse de sprint autrement (rattrapage sans effet : `own_speed_max` jamais au-dessus de 720 dans `coop_follow_stats`) ; ou si l'écart en sprint reste > 250 u à deux PC (tampon trop long pour le réseau réel : réduire `kRenderDelay`).

---

## D24 — Dialogues : même synchro que les cinématiques (2026-10-23, demande de Killian, étend D20)

**Constat** : D20 ne couvrait que `TriggerCinematic`. Un dialogue lancé par une zone ou une game action (Maelle avant le duel du prologue) n'avait lieu que chez le pilote ; le suiveur attendait sous la barrière.

**Décision** :
1. Le pilote publie aussi le début d'un dialogue : pré-hook de `BP_DialogueSystemComponent_C:ActivateDialogue(Dialogue)`, `SceneStart` avec le champ final facultatif `kind` = dialogue (absent = cinématique) et `sequence` = chemin de l'acteur `BP_Dialogue_C`. Même cause (zone, interaction, game action) ; jamais en combat ; même dialogue réactivé dans les 5 s ignoré.
2. Le suiveur suit D20 (zone -> `TryExecuteGameActionOnOverlap`, repli action, puis téléportation), sauf : dialogue d'interaction -> rien (déjà rejoué par `WorldInteract`) ; zone déjà exécutée ici dans les 120 s -> rien (pas de double lecture, ex. dialogue qui suit une cinématique de la même zone, pour les cinématiques aussi).
3. Les réponses restent rejouées par `DialogueChoice` (attente de la question `StartChoice`), vérifié sur ce dialogue.

**Limite** : un dialogue qui lance un combat (duel) le lance aussi chez le suiveur ; le combat est accepté comme déjà engagé, avec 1 PA d'écart aux premiers tours (réparé par la resynchro).

## D25 — Panneau Coop navigable comme le menu du jeu (2026-10-23, demande de Killian)

**Constat** : le panneau « Expédition coop » du menu pause n'avait jamais le focus ; ses lignes se choisissaient avec F9 et se changeaient avec Maj+F9. Killian : « ça serait pas possible par la souris et les flèches du clavier/manette comme le menu natif ? »

**Décision** :
1. Chaque ligne modifiable (personnage de l'équipe chez l'hôte hors combat, option) est un **vrai bouton du jeu** : `WBP_BaseButton_C` créé à l'exécution, style copié de `SettingsButton` du menu pause (comme le bouton Rejoindre de l'écran titre), contenu « libellé …… valeur » dans `ButtonContent`. Focus, survol, sons et clic sont ceux du jeu. Lignes non modifiables (réserve, équipe en combat, client) : texte comme avant.
2. **Valider** (clic, Entrée, A) : hook de `CommonButtonBase:HandleButtonClicked` -> `options::change()` (même chemin que Maj+F9 : `Owners` diffusé pour un personnage, settings.ini pour une option).
3. **Gauche / droite** : bouton en navigation contrainte horizontale (`USFUIButtonBase::SetIsNavigationConstrained` + `SetConstrainOrientation`, mécanisme des lignes de l'écran Paramètres) : le jeu ne déplace pas le focus et diffuse `OnConstrainedNavigationTriggered(Direction)`. Le mod y abonne (`FMulticastDelegateProperty::AddDelegate`) la fonction `SetJustification` d'un TextBlock caché de la ligne (paramètre d'un octet, sans effet visible) et la reçoit par un hook. Gauche = valeur précédente, droite = suivante ; un personnage passe toujours au joueur suivant.
4. **Entrer / sortir** : règle explicite « droite » des boutons du menu du jeu vers le panneau (remise par défaut quand le panneau disparaît) ; haut / bas bouclent dans le panneau. **Retour** (Échap / B) quand une ligne a le focus : le focus revient au dernier bouton du menu, le menu reste ouvert. Porte (`UFunction::Func` remplacée, comme les gardes d'intent.cpp) sur l'événement Retour du menu pause (`BndEvt_…_Back_…_5`) et sur `InpActEvt_IA_UI_Menu_Pause` du contrôleur (exploration) et du personnage de combat ; un même appui vu par deux chemins n'est avalé qu'une fois (0,3 s). Sans focus dans le panneau : appel d'origine.
5. **Focus <-> ligne choisie** : la ligne qui a le focus devient `options::selected()` (plus d'oubli après 20 s tant qu'elle l'a) ; F9 choisit une ligne -> elle prend le focus ; F9 après la dernière -> retour au menu. Surlignage (chevron doré) mis à jour sans reconstruire ; reconstruction seulement quand le contenu change, la ligne qui avait le focus le reprend.
6. Clics et flèches notés dans les hooks, traités au tick suivant (jamais de reconstruction pendant l'appel du jeu). Pointeurs gardés seulement déréférencés si notre racine est encore dans le `PausePanel` du menu courant.
7. **Repli** : bouton impossible (classe, modèle, contenu) -> panneau en texte comme avant (F9 / Maj+F9), pour la session.

**Limites** : l'appui sur Retour peut aussi passer par la gestion native de CommonUI (`NativeOnHandleBackAction`, sans UFunction) : dans ce cas Échap / B ferment le menu comme avant. Le survol ne déplace pas le focus (comme le menu du jeu). Non vérifié avec de vraies entrées (scénario `p6_panelnav_input_host`, à lancer avec l'accord de Killian ; manette à la main).

**Remise en cause si** : `owners_ui_panel_back` absent alors qu'Échap a été pressé avec le focus dans le panneau (chemin Retour non couvert) ; `nav_bind_errors` > 0 ; ou un focus qui saute au mauvais bouton du menu.

---

## D26 — Changements de niveau : le suiveur ne change de niveau que sur MapChange, MapChange gardé si besoin (2026-10-23, demande de Killian, précise D12 point 4)

**Constat** (`p9_zones_*`, tranche réelle de 7 transitions sur `TEST_LATE` : drapeau -> carte du monde, entrée de la carte
du monde, sorties de zone, camp) : le suivi d'une transition simple était déjà fluide (1 chargement, début 33 ms après la
réception, aucune pause ni resynchro). Deux défauts : (1) un MapChange reçu pendant que le suiveur charge un autre niveau
(hôte plus rapide qui repart aussitôt) : `ChangeMapByAssetName` en plein chargement est ignoré par le jeu, le suiveur
restait dans la mauvaise zone (`p9_zones_rapid_client` avant : 2 chargements manquants, 4 transitions jamais finies) ;
(2) MapChange perdu pendant la jonction, et position de l'ancien niveau reçue après le MapChange.

**Décision** :
1. Le suiveur applique un MapChange tout de suite si son monde est prêt ; sinon (chargement d'un autre niveau, combat pas
   fini, jonction en cours) il garde le dernier et l'applique dès que le monde est prêt (`coop_map_change_deferred`).
   Entre deux phases d'un chargement (`IsMapChangeInProgress` retombe un instant), un ChangeMap remplace le chargement
   en cours : il est appliqué aussitôt (transition « remplacée », `map_transitions_superseded`). Le mod compte ses propres
   ChangeMap comme début de chargement (une fin et un début peuvent tomber dans la même image).
2. Il ne change de niveau que sur MapChange : les boîtes de passage (`BP_jRPG_MapTeleportPoint*`, sorties de zone et
   entrées de la carte du monde) sont coupées chez lui (`SetGenerateOverlapEvents(false)`, reprises toutes les 10 s pour
   les parties chargées ensuite), rendues quand il redevient pilote ou à la fin de session.
3. Positions du pilote de l'ancien niveau ignorées après le MapChange (jusqu'à sa première position dans le nouveau, 30 s
   au plus) ; retour par `level_resync` avec le point d'apparition du dernier MapChange vers ce niveau.
4. Niveau du menu principal jamais publié ni suivi (le retour au menu est une fin de session).
5. Mesures chez le suiveur (`coop_map_transition`, horloge murale) et critères des scénarios : 1 chargement par transition,
   début < 500 ms après réception, arrivée < 3 s après le pilote, écart à la reprise < 300 u, rattrapage < 2 s, aucune
   pause, 0 `level_resyncs`, 0 `world_resyncs`. `-e33zonefix=0` coupe 1 à 3 (mesure « avant »).

**Remise en cause si** : un niveau dont l'arrivée dépend du passage du pion dans la boîte (aucun vu) ; ou
`coop_map_change_deferred` fréquent en vrai (l'hôte devrait alors attendre le client avant de repartir).

## D27 — Instantané de resynchro : jamais à la fin d'un dialogue, client gardé dans la partie, chargement borné (2026-10-23, essai à deux PC)

**Constat** (v0.6.6, Killian + TOTOFFF) : instantané envoyé dans la seconde de la fin du dialogue du client -> chargement
resté sur l'écran noir (2 fois) ; resynchro demandée en début de dialogue -> client retiré de la partie, combat lancé
par ce dialogue joué seul de chaque côté ; empreinte d'inventaire toujours différente (tenues / coiffures du compte).

**Décision** :
1. Hôte : instantané seulement au point sûr, sans narration du client, et au moins 3 s après la fin de sa dernière.
2. Hôte : un client entré qui demande une resynchro reste `Joined` (combats compris) jusqu'à l'envoi.
3. Client : resynchro d'exploration reportée pendant dialogue, cinématique, combat (en plus du mouvement, D23).
4. Client : chargement après 2 s sans dialogue ni cinématique (20 s au plus) ; monde pas prêt 45 s après -> rechargement,
   puis `ResyncRequest` ; journal `coop_join_wait_world` toutes les 10 s.
5. Empreinte d'inventaire `inventory2` : noms en minuscules, `item:Skin*` et `item:Face*` exclus.

**Remise en cause si** : `coop_join_load_stuck` en vrai (chercher dans `coop_join_wait_world` la condition qui manque) ;
ou un objet d'édition / DLC autre que tenues et coiffures qui différerait encore.

## D28 — Démarrage synchronisé des tours ennemis (2026-10-24, décision de Killian, précise D22.6)

**Constat** (essai à deux PC, v0.6.9) : mini-gels de ~1 s chez l'hôte à chaque coup reçu par un héros du client
(01:41:36, 01:41:53 ; 3 s à 00:54:09) : le client commence le tour ennemi ~1 s après l'hôte (il rejoue les actions
des héros de l'hôte avec décalage), l'hôte frappe donc avant lui et fige son image (`defense_point`) jusqu'au résultat
du client. Gênant surtout sur une attaque de groupe (le gel tombe dans la fenêtre de parade de l'autre joueur). Sens
inverse possible : après un tour d'un héros du client, le client commençait le tour ennemi avant l'état de l'hôte et
attendait de même les défenses des héros de l'hôte. L'essai `-e33turnalign=1` (D22.6) ne retenait en fait que certains
tours : le jeu n'écoute les dépendances du début de tour (`TurnStartNotifyDependencies`) que si un « auditeur »
(`BattleListeners` : effets, pictos…) s'y inscrit ; `ProcessNextTurnStartListener` remet le compteur à zéro (`Reset`)
pour chaque auditeur, puis n'écoute que s'il y a une dépendance (tours 9 et 16 du Lancelier Alpha jamais écoutés).

**Décision** (par défaut ; `-e33turnalign=0` coupe tout, hôte et client) :
1. Hôte : au début de chaque tour ennemi, juste après le premier `Reset` du compteur (ou à `StartListenToDependencies`
   si le jeu écoute déjà), le mod s'inscrit comme dépendance : le jeu attend en continuant d'animer, jusqu'à ce que chaque
   client qui a un héros vivant ait commencé ce tour (son `StateHash`, ou tout autre signe de ce tour). Puis les deux jeux
   jouent le tour ensemble : l'hôte n'attend plus qu'un aller simple les défenses du client.
2. Bornes (D22) : 4 s de jeu au plus (pauses non comptées), une seule retenue par tour ; pas attendus : joueur bloqué
   (`unresponsive`), parti, pas dans la partie (chargement, menu), sans héros vivant ; rien pendant un contre-attaque
   (`HasActiveCounterAttack`), le démarrage coordonné ou la fin du combat. Aucune action automatique, même résultat (D11).
3. Client : un tour ennemi commencé avant l'état de l'hôte (vérification différée, `-e33turndep`) est retenu de même
   (inscription après le premier `Reset`) jusqu'à cet état (borne existante : 15 s). Tours des héros jamais retenus ici
   (le menu du joueur n'est pas retardé).
4. Journal : `coop_turn_align` (how = reset / listen, joueurs attendus), `coop_turn_aligned` (waited_ms, timeout,
   listening = retenue effective), `coop_turn_dep` how=reset (client) ; compteurs `turn_aligns`, `turn_align_max_ms`,
   `turn_align_timeouts`, `turn_align_not_listening`, `turn_deps_on_reset`, `defense_waits_long` (attentes de défense
   > 250 ms, comme `coop_wait_long` site defense). Diagnostic `coop_turn_flow` (écoute, auditeurs, phase 2).
5. Tests : le faux client `--turn-lag 1.0` commence chaque tour ennemi 1 s après l'hôte et ne résout un coup qu'au même
   moment de son tour (`DefenseRequest.turn_ms`, champ facultatif ajouté) ; le faux hôte `--turn-lag 5.5` commence le
   tour ennemi 5,5 s après l'action du client. Scénarios avant / après : `p8_turnlag_before_host` / `p8_turnlag_host`,
   `p8_turnlag_before_client` / `p8_turnlag_client`.

**Remise en cause si** : `turn_align_timeouts` ou `turn_align_not_listening` en partie réelle ; `coop_wait_long` site
defense encore > 300 ms à deux PC ; ou une action ennemie jouée deux fois (deux `StartCharacterTurn_Phase2` pour un tour
dans `coop_turn_flow`).

## D29 — Miroir des menus : action exécutée dans la même image que la fermeture du ciblage (2026-10-24, bug vu par Killian)

**Constat** : quand un autre joueur lance une compétence, chez celui qui regarde, le menu s'ouvre et se ferme 2-3 fois très vite juste avant l'action. Journal de TOTOFFF (0.6.9) : à chaque action de l'hôte, `menu_mirror_release` appelle `OnCancelTargeting` 3 fois à 0,3 s d'intervalle (le ciblage d'une attaque reste levé après l'annulation), puis ramène la roue à sa page du début du tour, ~0,9 s au total, avant l'exécution qui rouvre l'action.

**Décision** (précise D17) : pour une commande d'action (`ActionCommand`), plus de rembobinage visible. Le ciblage rejoué est refermé (un `OnCancelTargeting`) dans la même image que l'exécution (`SelectAction` / `SelectSecondaryAction` / `RequestActionExecution`) : aucune image n'est affichée entre les deux, et la roue reste sur la page du propriétaire (c'est aussi là qu'il était en confirmant). La visée libre garde l'ancienne fermeture. `-e33mirrorrelease=old` : ancien comportement (scénario « avant »).

**Mesure** : `menu_release_flips` (changements de page / ciblage relevés à chaque image entre le début de la fermeture et l'exécution), `menu_release_slow` (fermeture > 0,1 s), `menu_targeting_after_action` (ciblage encore ouvert 1 s après l'action). Scénarios `p7_mirror_release_old_host` (avant) et `p7_mirror_release_host` (après), sans entrées injectées.

**Remise en cause si** : l'action exécutée depuis la page du propriétaire échoue ou diffère (écart de hash, `menu_targeting_after_action`), ou si la roue réapparaît pendant l'action (captures).

---

## D30 — Tirages de la mise en place d'un combat : même graine dès le lancement (2026-10-24, demande de Killian, amende D10)

**Constat** : D10 ne remplaçait les tirages de gameplay qu'à partir du tour 1. Or des tirages ont lieu **avant** : la
position de départ de la roue de Monoco (`BP_UniqueMechanic_TransformMasks_Component_C`, `RandomIntegerInRange` dans
son `ReceiveBeginPlay` -> `Starting_Position` = `CurrentMeter`), l'équipement de début de combat
(`AC_jRPG_CharacterBattleStats_C:ActivateBuff` depuis `InitBattleGear`), les effets « au début du combat ». Avec la même
graine 1865, la roue partait de 5 ou de 1 / 7 chez l'hôte, de 6 contre 5 chez un client ; l'écart n'était corrigé qu'au
tour 1 par la réparation des champs (`raw_hash_diffs`), sans garantie que l'affichage de la roue suive. La graine,
choisie au premier tour de l'hôte, n'existait pas encore à ce moment-là.

**Décision** :
1. **Graine choisie au lancement** : l'hôte la tire au pré-hook de `AC_jRPG_BattleManager_C:StartBattleNEW` (commun à
   tous les lancements) ou, pour un combat de la carte, dès l'avis précoce (`BattleStart` n° 0, qui la porte désormais) ;
   `host_turn_start` la reprend pour le `BattleStart` (repli : tirée là comme avant, `setup_windows_missing`).
2. **Fenêtre de mise en place** : de `StartBattleNEW` au tour 1 (120 s au plus, fermée à la fin du combat). Un tirage de
   gameplay (même classement que D10, sans le cache des ennemis) y vaut `f(graine, tour 0, flux, rang)`. Les composants
   de mécanique propre (`*Mechanic*`, `StainHolder`) sont aussi des appelants de gameplay, dans les tours comme avant.
3. **Client** : graine prise au lancement de son combat dans le `BattleStart` reçu (combat lancé par le mod) ou dans
   l'avis précoce (pion suiveur qui engage avant) ; inconnue -> tirage laissé au jeu, compté (`setup_draws_natural`),
   réparé au tour 1 comme avant. `setup_seed_mismatches` si le `BattleStart` porte une autre graine.
4. **Vérification** : l'hôte publie les tirages de la mise en place (`Draws`, tour 0) juste après le `BattleStart` ; le
   client les compare à son tour 1 (`draw_mismatches`), et compare la mécanique propre de chaque héros avant toute
   réparation (`start_mech_diffs`, `coop_start_mech` avec la position de départ de la roue des deux côtés). Un hôte ou un
   enregistrement d'avant D30 ne publie rien : pas de comparaison (`coop_setup_draws_unverified`).
5. Hors fenêtre et hors combat (exploration) : aucun tirage touché. `-e33setupdraws=0` = ancien comportement.
6. **Butin** : en cas d'écart de récompenses, le client écrit aussi objets et quantités de l'hôte (même nombre
   d'entrées ; sinon `rewards_loot_unrepaired`).

**Constats (p9_rng_host, 2026-10-24)** : ~100 tirages de mise en place : roue de Monoco (1), `ActivateBuff` de
l'équipement (~90) et `BP_LootSystemComponent_C:RollLootTableSection` (butin tiré dès le début du combat, désormais
identique). Les taches de Lune ne sont pas tirées au hasard (aucun tirage d'un `StainHolder` dans les traces ;
`StainQueue` créée par ElementalTrick, vidée par Mayhem). `raw_mech_diffs` compte les tours où une mécanique propre
diffère avant correction.

**Limites** : `GetRandomCharacterByBuffCritera` et l'attaque automatique forcée (`Array_Random` du gestionnaire) ne
sont toujours pas synchronisés (journalisés, `unsynced_random_targeting`) ; `Array_Random` ne doit jamais être hooké.

**Remise en cause si** : `draw_mismatches` au tour 0 (ordre des tirages de mise en place différent d'une machine à
l'autre) ; `start_mech_diffs` > 0 avec une graine connue ; `setup_draws_natural` > 0 chez l'hôte.

## D32 — Parade appuyée pendant une attente figée (2026-10-24, essai à deux PC)

**Constat** (18:27, attaque de zone du Portier, 0.6.16) : Killian et TOTOFFF parent tous les deux ; chez TOTOFFF, le jeu règle d'abord le coup du héros de Killian (attente bloquante de son résultat, 63 ms, plus un à-coup de 81 ms), puis, dans la même image, celui de Lune : son appui, fait pendant l'attente, n'est lu qu'à l'image suivante (17 ms trop tard) et passe d'abord par le héros de Killian (`coop_stray_defense`). Lune perd des PV alors qu'il a paré à temps.

**Décision** : pendant une attente de résultat de défense (`wait_for` site `defense`), le mod relève lui-même la touche de parade (front montant : E au clavier, RB à la manette via XInput, jeu au premier plan). Le coup suivant d'un héros d'ici, dans les 300 ms, lance sa parade avant le test du jeu (`clear_defense_lock` + `start_parry`, comme l'imposition d'un résultat). Journal `coop_parry_from_wait`, compteurs `parry_presses_during_wait`, `parries_from_wait`. `-e33waitparry=0` : ancien comportement. Esquive et saut non couverts (cas non vu).

**Remise en cause si** : touches de parade remappées (E / RB en dur) ; parade lancée sur un héros que le joueur ne voulait pas parer (appui pendant l'attente d'un coup visant seulement l'autre joueur, puis coup sur le sien dans les 300 ms).

## D33 — Écran de défaite : seul l'hôte choisit, les clients suivent (2026-10-24, bug vu en vrai, demande de Killian)

**Constat** (essai à deux PC, 0.6.14, combat « WM_Portier_Abbest_Volester », 16:49) : sur l'écran de défaite, Killian
(hôte) a choisi « Réessayer » et TOTOFFF « Abandonner » : l'hôte a rechargé sa partie (`MapChange`), le client est resté
sur un écran noir. Le mod ne gérait rien de cet écran. En test (TEST_B) : l'écran ouvre d'abord « Envoyer l'équipe de
réserve ? » (`WBP_ModalPopup_C`, parent = `WBP_jRPG_GameOverScreen_C`, réponse 1 oui / 2 non) ; oui : le combat continue
avec la réserve ; non : « Réessayer / Abandonner » (`PromptBattleRetry`) si le combat est « rejouable », sinon le jeu
répond seul `OnRetryEncounterResponse(false)` et revient à l'exploration sur place, sans chargement. Réessayer
(`RequestRestartCurrentEncounter`) recharge la dernière partie (`GetLastSavedGameData`) puis relance le combat.

**Décision** :
1. Seul l'hôte répond. Chaque réponse (`OnReserveTeamResponse`, `OnRetryEncounterResponse`) part aux clients :
   `DefeatChoice {battle, prompt (0 réessayer/abandonner, 1 réserve), yes}` (nouveau message, n° 68).
2. Client : toute fenêtre de choix de l'écran de défaite est refermée dès qu'elle s'ouvre (`DeactivateWidget`, réduite),
   statut « Défaite : <hôte> choisit… ». La réponse de l'hôte est donnée ici par la même fenêtre (sinon par l'écran),
   donc le jeu d'ici fait la même chose que celui de l'hôte (réserve, réessayer, abandon). Une réponse différente qui
   atteint le gestionnaire de combat est remplacée par celle de l'hôte (`defeat_choices_rewritten`) ; un clic du joueur
   d'ici est compté (`defeat_local_choices`, attendu 0).
3. Filet abandon : encore en combat 20 s après l'abandon suivi (cas de l'écran noir) -> `ResyncRequest`, la partie de
   l'hôte arrive par un instantané (D27). Hôte qui abandonne un combat que le client n'a pas perdu : idem après 30 s.
4. Hôte muet 120 s après l'ouverture d'une fenêtre chez le client : le joueur d'ici choisit (« Réessayer / Abandonner »
   rouverte ; réserve refusée ici). Hôte parti : fin de session habituelle (retour au menu).
5. `-e33defeatsync=0` : ancien comportement (scénario « avant ») ; `-e33defeatabandon=resync` : abandon de l'hôte suivi
   par un instantané, sans abandon local.

**Limites** : « Réessayer » recharge chez chacun **sa** dernière partie (client : instantané de jonction) ; l'écart
éventuel d'exploration est rattrapé par la comparaison d'état / la resynchro habituelle après le combat (non testé).

**Remise en cause si** : `defeat_local_choices` > 0 ou `defeat_modal_open_ms_max` > 500 chez un client ;
`coop_defeat_rescue` fréquent (l'abandon local ne revient pas à l'exploration) ; écart d'exploration après « Réessayer ».

---

## D34 — Combat lancé par le client meneur : l'hôte lance le même (2026-10-25, bug vu en vrai)

**Constat** (24/10, 22:27, journaux des deux PC) : TOTOFFF meneur (F8) frappe un Cruler (`coop_world_battle_early`,
engage 1) ; son jeu lance le combat seul. Le pion suiveur de l'hôte n'atteint l'ennemi que 17 s plus tard : l'hôte lance
alors son propre combat (engage 0), que le client rejoint « déjà engagé ». Ordres des tours et états différents (désyncs).

**Décision** : le client meneur dont le pion lance un combat sans BattleStart de l'hôte envoie aussitôt une **demande** :
`BattleStart` avec `battle_id` 0 (ennemi du monde, engagement, équipe), sans nouveau message ni changement de protocole
(versions du mod identiques imposées). L'hôte, s'il n'est pas en combat ni en chargement et que la demande vient du
meneur, lance le même ennemi (`start_world_battle`, rencontre de l'ennemi, engagement du client). La suite est le chemin
habituel : avis précoce, BattleStart, client « déjà engagé » qui reprend l'ordre et l'état de l'hôte. Écart attendu : un
aller-retour réseau au lieu du temps de trajet du pion suiveur. Le combat local du client n'est pas empêché.

**Journaux** : `coop_battle_request` (client), `coop_battle_request_received` (hôte : started, why). Compteurs
`battle_requests_sent` / `_started` / `_refused`.

**Remise en cause si** : `coop_battle_request_received` started faux (ennemi introuvable chez l'hôte, hôte en menu…),
ou désyncs au tour 1 toujours présents après une demande (il faudrait alors empêcher le combat local du client et
attendre le BattleStart de l'hôte).
