# AST Fire — Etap 2 / Unreal Engine 5.8.1

## Wynik (22 listopada 2026)

**STATUS: ASSETS_ASSEMBLED_REQUIRES_VISUAL_VALIDATION**

Potwierdzone na komputerze projektu przy użyciu rzeczywistego UnrealBuildTool i UnrealEditor-Cmd:

- `BuildPlugin` zakończył się kodem 0; zbudowane i wdrożone `UnrealEditor-ASTFireRuntime.dll` i `UnrealEditor-ASTFireEditor.dll`.
- Trzy autorskie materiały `M_AST_FirePixel_{Core,Ember,Smoke}` zapisane; **6/6** połączeń grafu materiału na każdy.
- Trzy emitery `NE_AST_Fire_{Core,Ember,Smoke}_V2` zduplikowane z silnikowego szablonu Niagara `Fountain`.
- `NS_Fire_Master` — dokładnie 3 emitery (`Flame`, `Embers`, `Smoke`), skompilowane skrypty Niagara i zapisany system.
- `BP_AST_FireSource` — `NiagaraComponent` przypisany do systemu, `PointLightComponent` należy do źródła ognia, dodano `ASTFireReactiveComponent`.
- Po końcowym wywołaniu `UnrealEditor-Cmd.exe`: **0 błędów, 1 nieblokujące ostrzeżenie** (deprecacja `SubobjectDataBlueprintFunctionLibrary.get_object`).
- Raport z rzeczywistego uruchomienia: `Saved/ChatGPT/NiagaraFire/Stage2/stage2_complete_result.json`.
- Mapy, pierwotny `BP_Torch_U` i jego placeholderowe światło pozostawiono bez zmian.

## Architektura

- `Plugins/ASTFireRuntime` — dwa moduły C++: sterownik oświetlenia i reakcji w runtime oraz pomocnik składania Niagara w edytorze.
- `Content/VFX/Fire/Materials` — autorski proceduralny shader płomienia, żaru i dymu.
- `Content/VFX/Fire/Emitters` — trzy emitery Niagara, niezależne od istniejących placeholderów.
- `Content/VFX/Fire/Systems/NS_Fire_Master` — wspólny system.
- `Content/VFX/Fire/Blueprints/BP_AST_FireSource` — przenośne źródło płomienia z własnym światłem.
- Flicker i kierunek reakcji na ruch są obliczane przez komponent `ASTFireReactiveComponent`. Bliskość postaci obsługuje lokalny `SphereComponent` i zdarzenia overlap, bez przeszukiwania wszystkich graczy.

## Granica weryfikacji

**Nie wykonywano żadnego renderu, PIE ani automatycznego testu gameplay.** Kompilacja i zapis assetów nie dowodzą, że wizualnie przypominają Rain World ani że zachowanie i wydajność są satysfakcjonujące.

Pozostałe zadania: dostrojenie ruchu i liczby cząstek w stosach emitera `Fountain`; podłączenie faktycznych parametrów Niagara User do grafów; profile dla pochodni, ogniska, płonącej strzały, zaklęć i podłoża; oraz integracja z docelowym miejscem pochodni po walidacji.

## Ręczna kontrola w Unreal Editor

1. Otwórz **Content Browser** i znajdź `/Game/VFX/Fire/Blueprints/BP_AST_FireSource`.
2. Umieść **nową instancję** w pustym miejscu testowym; nie usuwaj obecnej pochodni.
3. Sprawdź, czy widać proceduralne płomienie i żar, czy dym jest widoczny i czy sam obiekt ognia emituje światło (nie polega na `TorchLight`).
4. Przejdź oraz przebiegnij blisko źródła; zweryfikuj, czy płomień reaguje na prędkość postaci.
5. Porusz aktorem ognia i sprawdź reakcję na jego prędkość; skontroluj wygląd, flicker i koszt renderowania.
6. Nie uznawaj etapów wizualnych za ukończone bez tej kontroli.

## Ponowne wdrożenie

`Automation/ASTFire/deploy_stage2_windows.ps1` pobiera tylko 11 małych plików źródłowych, tworzy backup, buduje wtyczkę **poza katalogiem synchronizacji Google Drive** i uruchamia finalizator.

Nie używamy Git LFS; projekt i binarne assety pozostają na Google Drive. GitHub służy do kodu, skryptów i historii commitów. Nie uruchamiać instalatora podczas pracy z niezapisanym projektem Unreal.

## Ograniczenia projektu

Nigdy nie uruchamiać automatycznego gameplay/PIE, renderowania czy symulowanego interfejsu edytora. Testy wizualne wykonuje właściciel projektu. Nie modyfikować `BP_Torch_U` ani jego placeholderów bez uzgodnienia sposobu integracji.
