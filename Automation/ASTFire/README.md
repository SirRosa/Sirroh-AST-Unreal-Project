# AST Fire — Stage 3 / UE 5.8.1

## Potwierdzone wykonanie

- **BuildPlugin SUCCEEDED** dla UnrealEditor Development, UnrealGame Development i UnrealGame Shipping (UE 5.8.1).
- Skompilowane moduły `ASTFireRuntime` i `ASTFireEditor`, w tym `UASTFireReactiveComponent` oraz `UASTFireBlueprintLibrary`.
- Bazowy `NS_Fire_Master` zapisany z 3 emitterami; bounds powiększone z ±100 do ±1200 UU.
- `BP_AST_FireSource`: skompilowany i zapisany, Niagara Auto Activate włączony, materiały oryginalne, własne światło.
- Ręcznie sprawdzony wynik commandletu Stage 3: `CONFIGURED_EDITOR_VISUAL_REQUIRES_USER_VIEW`, bez błędów (poprawka `start_lit`).
- Dodatkowa wizualizacja płomienia wykorzystuje dwie skrzyżowane płaszczyzny z proceduralnym materiałem `M_AST_FirePixel_Core`; nie wymaga działającego emitera Niagara, ale nadal wymaga oceny użytkownika w viewport.
- Oryginalny `BP_Torch_U`, jego światło oraz autorskie mapy pozostają nietknięte.

## Uniwersalny ogień

`UASTFireReactiveComponent` można dodać do dowolnego Actor. Obsługuje rozmieszczenie `Point`, `Bounds`, `Sockets`, punkty trafień, własne światło, Niagara i płomienie proceduralne. Dla aktorów bez wstępnej konfiguracji istnieje `AST Fire Blueprint Library` z `Ignite Any Actor`, `Extinguish Actor`, `Is Actor Burning`. Jest to uniwersalny **system wizualny**, nie gotowy system obrażeń czy rozprzestrzeniania fizycznego po otoczeniu.

Zobacz [USAGE.md](USAGE.md), aby podłączyć ogień do pochodni, drzewa i człowieka.

## Instalacja

`Automation/ASTFire/deploy_stage3_windows.ps1` pobiera niewielkie źródła z przypiętego commita, zapisuje backup, buduje wtyczkę poza zsynchronizowanym katalogiem Drive, kopiuje DLL i uruchamia commandlety UE. Wyniki:

- `Saved/ChatGPT/NiagaraFire/Stage3/stage3_result.json`
- `Saved/ChatGPT/NiagaraFire/Stage2/BuildPlugin_*.log`
- `Saved/ChatGPT/NiagaraFire/Stage2/Stage3_VisibleFire_*.log`

Nie uruchamiać instalatora przy otwartym edytorze z niezapisanymi zmianami.

## Granica weryfikacji

Nie wykonywano renderowania, automatycznego PIE ani symulacji rozgrywki. **Sukces kompilacji i zapisania assetów nie jest dowodem, że płomienie są wizualnie poprawne.**

Prośba o ręczny test po ponownym uruchomieniu UE:
1. Umieść `BP_AST_FireSource` w pustym miejscu testowym.
2. Upewnij się, że podgląd jest widoczny; włącz Realtime w viewport.
3. Uruchom PIE ręcznie, sprawdź animację, pulsowanie światła i reakcję na ruch.
4. Wywołaj `Ignite Any Actor` dla drzewa / SkeletalMesh Actor, korzystając z `Impact Point` i liczby płomieni 3–6.
5. Zgłoś zrzut ekranu, jeśli warstwa płomienia nadal jest niewidoczna.

Pełny projekt i `.uasset` przechowujemy na Google Drive, GitHub na źródła, bez Git LFS.
