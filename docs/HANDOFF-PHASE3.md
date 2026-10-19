Reprends le projet de mod coop en ligne pour Clair Obscur: Expedition 33. Les phases 0, 1 et 2 sont finies, et la phase 2 a été validée en jeu par moi le 2026-10-19. Attaque maintenant la phase 3.

Avant toute action, lis dans cet ordre :
1. ~/dev/e33-coop/docs/BRIEF.md : le cahier des charges complet. Les phases 0 à 6, les décisions déjà prises, les règles et les limites s'appliquent toutes.
2. ~/dev/e33-coop/docs/PROGRESS.md : l'état courant et l'historique.
3. ~/dev/e33-coop/docs/DECISIONS.md (D1 à D10 ; D10 = tirages de gameplay dérivés d'une graine de l'hôte, il amende D9) et ~/dev/e33-coop/docs/GAME-INTERNALS.md (la section 15 est ce qui a été vérifié en jeu, avec les pièges récents à la fin).

Où on en est :
- main contient tout jusqu'à la fin de la phase 2. Crée une branche phase-3 depuis main.
- Synchronisé et testé : attaque de base d'un héros distant (ActionCommand → l'hôte l'exécute), défense distante (parade et saut résolus chez le propriétaire, appliqués par l'hôte, 0–16 ms d'attente), hash d'état par tour, tirages de gameplay (IA ennemie, critiques, Luminas, buffs) identiques via la graine (D10), vérification des tirages par tour (Draws).
- Tests de référence (3/3 verts dans chaque sens) :
  - powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_host_vs_fakeclient -Package <dossier e33coop-mod>
  - powershell -NoProfile -File tools/run-scenario/run-scenario.ps1 p2_client_vs_fakehost -Package <dossier e33coop-mod>
  Ils doivent rester verts à chaque étape de la phase 3 (non-régression). Vérifications : desyncs = 0, hashes_compared > 0, gameplay_draws > 0, draw_mismatches = 0, hp_overrides = 0 côté client.
- Save de test : TEST_B (.tools/testsaves/TEST_B.sav, copie locale de ma save NG+ aux Vallons fleuris, jamais commitée). Rencontres : SM_Lancelier_Alpha (court, attaques parables), SM_Eveque*1 (boss + sbires, long). Équipe actuelle : Verso seul. Pour les combats à plusieurs héros, demande-moi de composer une équipe de 3 et de resauvegarder (je bouge, je sauvegarde au drapeau, je quitte : tu recopies).

Environnement :
- Repo GitHub public kipavy/e33-coop, CI Actions gratuite, secret UEPSEUDO_PAT en place. On ne compile jamais en local (pas de toolchain, pas de Python) : uniquement via la CI.
- La CI est incrémentale : environ 2 minutes par build si seul notre code change. Annule un run devenu inutile (gh run cancel) plutôt que de l'attendre.
- Les artefacts e33coop-mod et e33coop-tools se récupèrent avec gh run download <id> -D .tools/artifacts/<id>. Copie fakepeer.exe et test_protocol.exe dans .tools/bin.
- reset-env (lancé par run-scenario) réinstalle UE4SS et le mod, puis remet mes saves en lecture seule après en avoir fait une copie. Ne touche jamais au dossier SaveGames\76561198355797267 en écriture. Le backup d'origine est dans ~/dev/e33-coop-backups. Pour me rendre le jeu : tools/reset-env/vanilla.ps1.
- À la reprise, le jeu est probablement encore moddé et mes saves en lecture seule (état laissé par les derniers tests).

Pièges connus :
- Écris les commandes Lua de télécommande de façon atomique : cmd.tmp, puis mv vers cmd.lua. N'installe pas E33Explore pendant les tests : ses dumps ont fait planter le jeu.
- Captures d'écran uniquement via tools/dev/screenshot.ps1, qui ne capture que la fenêtre du jeu. Option de scénario "shots_every_s" pour des captures périodiques : regarde-les pour vérifier le rendu et les caméras.
- Ne hooke JAMAIS KismetArrayLibrary:Array_Random : ça casse GetRandomAliveHero (l'ennemi attaque dans le vide, caméra sous la carte). Synchronise le ciblage au niveau de FL_TargetingHelpers_C:GetRandomAliveHero / AcquireTargetRandomHero.
- Jamais d'appel au jeu (ProcessEvent) dans un hook très fréquent (fonctions de tirage) : crash au chargement. Ces hooks peuvent aussi être appelés hors du thread de jeu.
- Hooks UE4SS sur fonctions natives : le cadre fourni est celui de la fonction elle-même ; le vrai appelant BP est dans TheStack.PreviousFrame().
- Un crash affiche une boîte « Fatal error » ; run-scenario la ferme, garde le texte et résume le dump (tools/dev/dmp-info.ps1).
- Ne valide pas « 0 désync » seul : le client recopie les PV de l'hôte avant de hasher. Regarde hp_overrides, coop_raw_hash_differs et draw_mismatches. Étends ces garde-fous quand tu synchronises de nouveaux états (PA, buffs, morts…).
- Pour choisir une graine qui provoque un comportement d'IA donné : tools/dev/draw-preview.ps1 (même calcul que le mod, vérifié contre le jeu). Les graines de test se fixent dans les scénarios avec -e33seed=N.
- Outils shell : évite sed/perl avec des apostrophes françaises (un sed partiellement appliqué a déjà dupliqué du code) ; préfère l'outil Edit. Avec powershell -File, un tableau passé en argument arrive comme une chaîne, et un -ExtraArgs vide fait échouer le script.

Phase 3 (critère du brief) : toutes les actions, compétences, objets, IA ennemie, attaques de groupe, mort et réanimation, fin de combat, récompenses. Fini quand une batterie de scénarios couvrant ces cas passe et que les enregistrements rejoués restent verts.
Points déjà identifiés :
- ActionCommand cible toujours l'ennemi 0 et ne porte que l'action « attaque » : il faut les vraies cibles, les compétences et les objets, ainsi que les QTE offensifs résolus chez le propriétaire.
- L'hôte n'impose que les PV (ActionResult) : ajoute PA, bouclier, buffs, morts, et fais une resynchronisation complète sur désync.
- Le ciblage aléatoire des ennemis n'est pas encore synchronisé (sans effet avec un seul héros).
- Combats à plusieurs héros, avec un héros par joueur (owners), et attaques de groupe (barrière de défense par joueur, une seule parade couvre tous ses héros).

Tiens PROGRESS.md à jour et fais des petits commits sur la branche phase-3. Toute fonctionnalité de synchro arrive avec un scénario qui la vérifie.
Sollicite-moi seulement aux jalons de validation (fin de phase 3 : checklist en jeu de moins de 20 minutes avec les commandes exactes), pour tout ce qui touche à mes comptes, pour une save avec une autre équipe, ou si tu es dans une impasse réelle.
