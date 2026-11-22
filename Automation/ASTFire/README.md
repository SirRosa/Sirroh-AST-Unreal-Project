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


## Stage 4 — proceduralny płomień 3D (2026-11-22)

**Weryfikacja techniczna:**

- GitHub Actions (2/2) zielone dla zmian źródłowych 76510289112fe5a7adb0a0f5f72b47ca47c702a5.
- BuildPlugin na rzeczywistym komputerze użytkownika z UE 5.8.1 zakończony sukcesem dla kodu z 09b55973182878c139a3373a7f75e61596dd7dc5; biblioteki runtime/editor wdrożone do projektu.
- Następny headless commandlet z aktualnym setup_stage4_volume.py zakończony kodem 0; raport: VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW, errors: [], modified_components: 1, disabled_legacy_niagara: 2.
- Logi i wynik: Saved/ChatGPT/NiagaraFire/Stage4/BuildPlugin_20261122_035020.log, MaterialSetup_retry.log, stage4_result.json.

**Zmiany:**

- Renderer runtime zastępuje statyczne karty ogniowe dynamicznym UProceduralMeshComponent z pięcioma wzajemnie nakładającymi się językami na punkt spalania (bez kolizji i cieni).
- Deformacja wierzchołków uwzględnia prędkość, przyspieszenie, rotację, podmuchy od pobliskich postaci, wiatr, bezwładność i turbulencję.
- Geometryczne aktualizacje są rzadsze w dalszych zakresach; stan spalania nie zależy od liczby cząsteczek.
- RepNotify synchronizuje podstawowy stan ognia, pokrycie i punkty zapłonu; **nie przeprowadzono jeszcze testów sieciowych**.
- Materiał unlit /Game/VFX/Fire/Materials/M_AST_FirePixel_Volume prezentuje dyskretną paletę temperatury na rzeczywistej geometrii 3D.
- W BP_AST_FireSource wyłączono automatyczny start dwóch wcześniejszych samodzielnych NiagaraComponent, aby nie zasłaniały nowej geometrii. Oryginalny BP_Torch_U i mapy AST pozostają nietknięte.

**Granice obecnej wersji:** Nie jest to pełny solver CFD ani gotowa symulacja paliwa, dymu, rozprzestrzeniania pożaru czy powierzchni każdego trójkąta mesha. Skalowanie 1/10/50/100 płonących aktorów nie zostało jeszcze zmierzone. Kompilacja i commandlet nie potwierdzają jakości wizualnej.

**Ręczna ocena przez użytkownika:** uruchomić ponownie edytor, umieścić BP_AST_FireSource, włączyć Realtime/PIE, obejrzeć nieruchomy płomień, szybki ruch/rotację, zmianę intensywności i ogień na StaticMesh/SkeletalMesh. Po testach wizualnych iterować w stronę spójnego korpusu, rozrywania płomieni i dymu. Nie uruchamiać automatycznych testów PIE.

**Instalator dla następnego wdrożenia:** Automation/ASTFire/deploy_stage4_windows.ps1 (pobiera z jednej przypiętej rewizji, backup małych źródeł, BuildPlugin w katalogu TEMP i headless commandlet).


## Stage 5 — kierunek po negatywnej ocenie wizualnej

Użytkownik zdecydowanie odrzucił Stage 4: macki/stożki zamiast ognia, za jasny i niemal jednolity żółty kolor, brak podziału na języki i odrywające się fragmenty. Nie uważać kompilacji za dowód jakości wizualnej.

W Stage 5 zmieniono renderer na **lokalne 22×30 pole temperatury na punkt spalania**, którego obraz jest prezentowany w orientacji kamery jako stylizowane 2.5D w świecie 3D (punkty spalania pozostają przestrzenne i podążają za socketami). Adwekcja w górę, wiry, schładzanie, trzy zmienne źródła zasilające wspólny korpus i 9 odrywanych fragmentów na klaster. Stan spalania pozostaje oddzielny od stanu wizualnego; weryfikacja multiplayer nadal wymagana. Stała topologia 22×30 quads + 9 fragments umożliwia UpdateMeshSection zamiast rekonstrukcji powierzchni w każdej klatce. Kolory są ograniczone do pięciu ciemniejszej czerwieni → pomarańczy → żółci, bez 2.5-krotnego wzmocnienia HDR, z maską niskiej temperatury.

**Przed oceną:** wymagane zielone GitHub Actions, BuildPlugin UE 5.8.1, ponowne wykonanie commandletu setup_stage4_volume.py (materiał modyfikowany w swoim folderze) oraz ręczny test wizualny użytkownika. Nie modyfikować oryginalnych map, Advanced Parkour ani pochodni.

