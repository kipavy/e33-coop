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
