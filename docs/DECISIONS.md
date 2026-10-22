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
5. **Cinématiques et dialogues** : joués localement. Chaque instance annonce leur début et leur fin (`NarrativeState`). Celle qui termine avant les autres reste en pause de jeu jusqu'à ce que tout le monde ait fini (`BarrierRelease` de l'hôte, ou délai maximal en cas de pair muet).
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
8. **Panneau Coop au clavier** : **F9** choisit une ligne (personnages de l'équipe pour l'hôte hors combat, puis options), **Maj+F9** la change (personnage → joueur suivant, même chemin que F6 ; option → valeur suivante, enregistrée). Pas de flèches ni d'Entrée : le panneau n'a jamais le focus, le menu pause du jeu garde sa navigation. Hors du menu pause, une carte montre la ligne choisie. Options d'hôte en lecture seule chez un client.

**Non vérifié en jeu** : tout le mode neutre (F7 hôte en pleine partie avec hooks accrochés tard, invitation, retour au neutre), la vérification de release et l'installateur lancé depuis le jeu (`CREATE_BREAKAWAY_FROM_JOB`, copie de l'exe dans `%TEMP%`), l'installation à la fermeture du jeu.

**Remise en cause si** : un hook accroché en pleine partie rate un état déjà commencé (combat, cinématique) ; ou si Steam relance le jeu avant la fin de l'installation à la fermeture.
