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



### Stage 5 — potwierdzone wdrożenie na komputerze użytkownika (2026-11-22)

- Zatwierdzony kod renderera i test numeryczny: git SHA `f5d90365ea25718daf11847191549f2bf4f243b3`.
- Dwa workflowy GitHub Actions zakończone statusem success.
- `BuildPlugin` (Editor, Game Development, Shipping) UE 5.8.1: `BUILD SUCCESSFUL`, dwa DLL edytora. Log: `Saved/ChatGPT/NiagaraFire/Stage4/Stage5_Final_Build_f5d9036.log`.
- Projekt instalowany z dokładnej rewizji f5d9036, z użyciem już skompilowanego pakietu w katalogu TEMP; przed nadpisaniem wykonano kopie zapasowe wyłącznie źródeł wtyczki, jej DLL oraz własnych assetów `M_AST_FirePixel_Volume` i `BP_AST_FireSource`.
- Installer: `Automation/ASTFire/deploy_stage5_verified_windows.ps1`. To skrypt dla **już zweryfikowanej kompilacji** przechowywanej lokalnie w TEMP, a nie niezależny instalator do dowolnej maszyny.
- Commandlet konfigurujący: exit 0, `VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW`, `errors: []`, jeden komponent ASTFireReactive, dwa starsze Niagara wyłączone, dwa komponenty świateł ustawione. Log: `Saved/ChatGPT/NiagaraFire/Stage5/Stage5_Commandlet_20261122_041338.log`. Brak krytycznych błędów materiału/shaderów w odfiltrowanym logu.
- Backup lokalny: `Saved/ChatGPT/NiagaraFire/Stage5/Backup_20261122_041338`.
- **Jakość wizualna, zachowanie w PIE i multiplayer: NIEPOTWIERDZONE.** Wymagają osobistego testu użytkownika. Nie należy nazywać Stage 5 ukończonym silnikiem spalania.


## Stage 6 — visual end contours + separate contact-igniter variant

The Stage5 screenshot showed an unnaturally FLAT top and bottom despite a moving heat field.
Stage6 multiplies each cell's *temperature coverage* (not only material color) by
a time-varying **root contour**, three independent unequal **tip envelopes**,
and a narrowing moving **side envelope**. All three are world-visual only.
The top is bounded below the grid edge to avoid slicing the visible fire in half;
the bottom has a curved, intermittent contact silhouette. Existing advection,
wind reaction, pixel-grid style, and optional ignition via Blueprint library remain.

Second actor: \`/Game/VFX/Fire/Blueprints/BP_AST_FireSource_Igniter\`.
An independent duplicate of our own \`BP_AST_FireSource\`, plus the spawnable
\`UASTFireIgnitionZoneComponent\` box. Server-authoritative begin-overlap ignites
pawns/objects that provide overlaps, via existing \`IgniteActor\`. Bounds select
1/2/4/6/8 flame points, clamped by MaxDistributedFlames (default 6).
It tracks actors during overlap to avoid repeated impact points. Contact ignition
does **not** add damage, char, fuel, or automatic area spread. If another actor
has no overlap events/collision configured, no contact event is guaranteed;
that limitation needs gameplay testing. Default: no ignition of objects already
overlapping at spawn, to avoid torch igniting the entire static floor.

Unreal Editor setup: \`Automation/ASTFire/setup_stage6_igniter.py\`, after
building and deploying the ASTFireRuntime plugin. It only creates/updates
\`BP_AST_FireSource_Igniter\`, never modifies base source or POIKit.
GitHub Actions checks Python syntax, C++ structure, and dynamic contour metrics.
Real UE5.8.1 build and headless commandlet are required before claiming deployed.
Never claim visual/PIE gameplay validated without user screenshots.


### Stage 6: verified UE5.8.1 build, editor assets (2026-11-22)

- Plugin C++ commit: 41392d01e4a42c2c7e60eeaf0fa42bbfe6cab2d5. BuildPlugin on Win64 succeeded, two editor DLLs deployed. Small-file backup: Saved/ChatGPT/NiagaraFire/Stage6/Backup_20261122_043841.
- setup_stage4_volume.py: UE commandlet exit 0, VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW, no errors.
- Python asset setup fix: ec5843b753c622036ec5f60e16fa2cf04c4a6ebb. setup_stage6_igniter.py: exit 0, IGNITER_CREATED_REQUIRES_CONTACT_TEST, exactly one ASTFireIgnitionZoneComponent and one ASTFireReactiveComponent, errors []. Result: Saved/ChatGPT/NiagaraFire/Stage6/stage6_result.json. Run log: stage6_dedup_retry.log.
- Saved variant: /Game/VFX/Fire/Blueprints/BP_AST_FireSource_Igniter. Original base fire source and unrelated maps unaffected except intended Stage6 material/visual configuration.
- UE CDO crash fixed by moving ALL collision/scene setters out of component constructor into BeginPlay. Unreal SubobjectData handles duplicate object paths, so script deduplicates before validating/adding.
- Important: neither visual quality of the flame's top/bottom nor actual contact gameplay has yet been tested in PIE. No char/damage logic is implemented. For contact to work, the other actor must have compatible collision and generate overlap events. Unreplicated actors are not guaranteed to share ignition state across multiplayer clients.

## Stage 7 — anatomically distributed flames and stronger inertial response

After testing Stage6, the user reported weak reaction to running/moving objects,
jelly-like persistent fire mass, near-static light, and almost identical flame
size for small tables and large crates. Their explicit requirement: **fit flames
to the actual mesh**, not just the actor's bounding sphere/capsule.

- SkeletalMesh: obtain actual bone/socket transforms, distribute localized small
  flame anchors using farthest-point bone sampling so arms, legs and torso can
  ignite independently and follow animation. Optionally project anchor to a
  physics/collision surface for a skin-side offset.
- StaticMesh/Primitive: sample surface points with per-component complex
  line traces. Bounds are used only to construct the rays and as fallback if
  collision is missing. This is NOT a guaranteed exact render-triangle fit
  when the actor has no usable collision. Asset collision configuration matters.
- Auto-fire on a one-point contact also uses mesh-aware bounds mode. A skeletal
  actor receives >=7 bone points automatically; native contact zone defaults
  to up to 8 independent localized patches, chosen from local object size.
- Per-patch flame size comes from local surface area / number of patches
  (instead of actor height). No billowing full-height capsule flame on humans.
- The flame field responds strongly to bone/socket acceleration, actor
  acceleration and moving pawns, with local turbulent gusts and world-space
  inertial lean. Source bands grow and die out of phase, cells erode, detached
  fragments appear and heat-derived visual energy modulates the actual fire
  point light's intensity, attenuation and color.
- All heat fields, fragment lifetimes and light fluctuations remain visual-only
  and unreplicated. The authoritative burn state stays separate.
- Updated Stage7 tests check relative sizes, mesh-aware source structure,
  intensity-of-change of thermal life cycles and wind strength.
- **Unreal build, visual fidelity and actual PIE network/contact testing are
  required before claiming the implementation validated.**


### Stage 7 — potwierdzenie kompilacji i instalacji UE 5.8.1 (2026-11-22)

- Zweryfikowany commit źródłowy: `36d2d1738e155da44b1693f75347c5d661eff818`. Dwa przebiegi GitHub Actions zakończone sukcesem.
- `BuildPlugin` (Win64) na komputerze użytkownika: `STAGE6_BUILD_EXIT=0`. Obie biblioteki DLL wtyczki zostały wdrożone: `STAGE6_DEPLOYED_DLLS=2`.
- Backup: `Saved/ChatGPT/NiagaraFire/Stage6/Backup_20261122_050545`.
- Commandlet materiału: `EXIT=0`, `VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW`.
- Commandlet wariantu podpalającego: `EXIT=0`, `IGNITER_CREATED_REQUIRES_CONTACT_TEST`.
- Cały instalator: `STAGE6_INSTALL_SUCCESS_VISUAL_AND_CONTACT_TEST_PENDING`, exit 0.
- Silnik renderujący dopasowuje płomienie do kości oraz próbuje osadzić punkty na powierzchni fizycznej/kolizyjnej mesha. Bez właściwej kolizji następuje awaryjny fallback na obwiednię, co **nie gwarantuje** zgodności z renderowanymi trójkątami każdego mesha.
- **Brak testów PIE, zweryfikowanej jakości wizualnej, benchmarków 1/10/50 aktorów oraz testów multiplayer.** Kompilacja i commandlety nie potwierdzają, że deformacja, cykle życia i światło spełniają wymagania użytkownika w ruchu.


## Stage 8 — one coupled fire system: exact skinned surface, buoyant tongues, responsive light

Stage 7 screenshots showed red floating blobs and flames left behind by sprinting.
Stage 8 renders the burning ROOT as an original masked material overlay on the
**actual GPU-skinned/rendered triangles** of the actor. The base mesh/material
assets remain untouched; animation automatically deforms the fire mask.
Pre-existing overlays are deliberately preserved. Flame coverage is quantized
in rest-pose local coordinates, while flow, intensity and thermal energy
are runtime parameters. This is one simulation/state: the skinned surface,
small independent buoyant tongues and fire light receive the same energy and
wind. Large bone billboards are scaled down, the hot root can no longer be
erased entirely by random heat decay, and detached fragments are capped at four.

New files: `Automation/ASTFire/setup_stage8_surface.py` generates
`/Game/VFX/Fire/Materials/M_AST_FireSurface` and configures only AST-owned
`BP_AST_FireSource` and `BP_AST_FireSource_Igniter`.
`Automation/ASTFire/deploy_stage8_windows.ps1` verifies exact commit SHA,
builds in TEMP, backs up scoped plugin sources/DLLs and owned fire assets,
and runs Stage4 volume material, Stage6 contact Blueprint and Stage8 surface
commandlets (once each). It refuses to overwrite binaries if UE is open.

**Not exact:** detached rising tongues still use secondary attachment points,
not full skinned-triangle GPU particle sampling. Overlay shape IS exactly the
real render surface. No char/damage. No claims about final looks or movement
until user PIE tests. Tests verify source/structure, not perception.


### Stage 8 — verified UE 5.8.1 deployment (2026-11-22)

- Source revision: `322a5ece2324056d8620472832ea1b1e1065bf01` (both GitHub Actions workflows passed).
- Real Win64 UE 5.8.1 `BuildPlugin`: `STAGE8_BUILD_EXIT=0`, 2 editor DLLs actually deployed.
- Backup: `Saved/ChatGPT/NiagaraFire/Stage8/Backup_20261122_053638`.
- Stage4 volume commandlet: EXIT 0, `VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW`.
- Stage6 contact actor commandlet: EXIT 0, `IGNITER_CREATED_REQUIRES_CONTACT_TEST`.
- Stage8 surface commandlet: EXIT 0, `SURFACE_FIRE_CONFIGURED_REQUIRES_VISUAL_TEST`. Created material `/Game/VFX/Fire/Materials/M_AST_FireSurface` and updated both `BP_AST_FireSource` and `BP_AST_FireSource_Igniter`. Result `stage8_result.json` had `errors: []`.
- Material setup log `setup_stage8_surface.py_20261122_053638.log`: zero critical shader/material/Python errors matching targeted filters.
- Overall installer exit code 0: `STAGE8_INSTALL_SUCCESS_VISUAL_AND_CONTACT_TEST_PENDING`.
- **Not verified:** subjective appearance, actual skinned-surface coverage, sprint attachment, dynamic light perception, in-game performance, PIE contact behavior and multiplayer. These require manual game testing. Do not equate build success to a final fire look.



## Stage 9 — real render-vertex source and genuinely unbounded ember flight

User's Stage8 screenshot still showed flames floating between legs and in front of
the character. Root cause confirmed directly from the C++ renderer:
- A hard-coded 22×30 RECTANGLE of camera-facing quads was still running for
  EVERY bone, independent of the GPU surface material.
- Coverage tips were clamped at Y <= 0.96 and every blob shared a finite
  RealWidth/RealHeight, artificially clipping tongues and fragments.

Stage9 introduces **UASTFireFreeFlameComponent**, a separate visual-only render
path that is enabled by default. It uses an actual skeletal LOD0 vertex selected
from render geometry and evaluates the current CPU-skinned vertex position each
update. The base therefore follows the *deformed mesh*, not a bone center,
collision capsule, or a guessed rectangular proxy. Static meshes use actual
complex LineTraceComponent surface intersections. Without usable collision the
static mesh overlay remains and no fabricated hovering collision anchors are
created. Standalone no-mesh fire sources emit from a compact source ring.

The free emitter draws organically varying buoyant ribbons from these real
source points and advects detached embers in independent WORLD SPACE. It has
no fixed Height/Width field and no baked XY rectangular grid. Roots remain
on the skinned surface, while hot gas may drift away. The material overlay,
free tongue temperature and light share the existing fire component intensity
and visual energy. Old Stage5-8 22×30 heat grid remains solely as explicit
`bUseLegacyHeatGrid` migration/debug toggle (default false).
No gameplay damage or char. Rendering remains lightweight and capped
(18 roots × 7 segment-quads + 12 embers per actor, 25Hz).

Compilation via UE 5.8.1 and PIE visual QA are required; skin vertex data may
be unavailable on some mesh LOD/platforms and then only the exact GPU surface
overlay is shown — do not silently fall back to inaccurate bone flames.
Installer: `Automation/ASTFire/deploy_stage9_windows.ps1` builds exact GitHub
commit in TEMP, backs up all owned source/material/Blueprints and only deploys
if UnrealEditor is closed. It runs all three existing asset configuration
commandlets.


### Stage 9 — sprawdzona instalacja na UE 5.8.1 (2026-11-22)

- Kod renderera z przypiętego commita `b07c05fbc292f4eb6f183c93ddf9a62a6907f365`.
- Dwa workflowy GitHub Actions zakończone `success`; Win64 `BuildPlugin EXIT=0`.
- Wdrożono dwie DLL (`STAGE9_DEPLOYED_DLLS=2`), backup:
  `Saved/ChatGPT/NiagaraFire/Stage9/Backup_20261122_055140`.
- Trzy commandlety zakończone `EXIT=0`:
  - `setup_stage4_volume.py` — `VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW`
  - `setup_stage6_igniter.py` — `IGNITER_CREATED_REQUIRES_CONTACT_TEST`
  - `setup_stage8_surface.py` — `SURFACE_FIRE_CONFIGURED_REQUIRES_VISUAL_TEST`
- Instalator kończy `STAGE9_INSTALL_SUCCESS_VISUAL_AND_CONTACT_TEST_PENDING`, exit 0.
- Krytyczne zachowanie: `bUseLegacyHeatGrid = false` jest domyślne i ustawione w skrypcie BP. W normalnym trybie **nie instancjonujemy** poprzednich prostokątnych `FireVolumes` na kościach. Zamiast nich `UASTFireFreeFlameComponent` próbuje próbkując renderowane, skórkowane wierzchołki (LOD0). Free-space embers mają współrzędne świata i czas życia, a nie granice siatki 22×30.
- Ograniczenia: kilka płaskich segmentów w ramach każdego języka nadal renderuje się jako dwustronne billboarding quady (nie pełny solver CFD). Płonący StaticMesh bez użytecznej kolizji może mieć jedynie materiał powierzchniowy. Brak dostępnego CPU skin vertex buffer w buildzie może zablokować sampling geometryczny; nie należy wtedy sztucznie przenosić płomieni na kości.
- **Niepotwierdzone**: rzeczywisty wygląd, animacja i kolizje w PIE, multiplayer oraz profilowanie GPU/CPU. Testy CI i commandlety nie są dowodem jakości wizualnej.


## Stage 10 — final implementation specification, coupled modules (2026-11-22)

After Stage9 feedback, do not describe Stage10 as another box/grid patch.
**All the following coexist in one runtime path:**

- Surface skin roots sampled from deformed render vertices (or static-mesh
  collision triangle hits); no fixed heat rectangle, detached heads in world.
- Merge hotspots on original animated triangle overlay: up to 8 mesh-local
  ignition points packed into MaterialInstanceDynamic vector parameters.
  Hotspot support expands over time; adjacent hotspots merge through a smooth
  union. Full-body fire is reached progressively (not by individual stickers).
  The hotspot distance is Euclidean in rest pose, not expensive geodesic mesh
  adjacency: this is a documented limitation.
- 3–6 free tongues on characters, 4–5 on small objects, 10–15 on large ones.
  Tongues are one continuous tapered strip, not stacked identical flat cards.
- Each tongue maintains own GrowthSeconds, MaxLength, width, seed, direction
  and birth generation. It grows to maturity, tears its tip away, preserves
  a shorter connected root, and generates a new distinct silhouette.
  Detached tips have world positions and velocities and cool, shrink, speed
  up and fade over their own individual lifetimes. This also powers local light.
- Hottest almost white -> yellow -> orange -> red -> dying black-red, indexed
  by **local heat and age**, not one universal yellow material.
- Shader asset setup:
  `Automation/ASTFire/setup_stage10_lifecycle.py` creates two new AST-owned
  materials `M_AST_FireTongue` and `M_AST_FireSurfaceLife`, updates only
  `BP_AST_FireSource` and `BP_AST_FireSource_Igniter`.
- Installation uses `Automation/ASTFire/deploy_stage10_windows.ps1`;
  pin exact SHA, build in TEMP, back up only own source/DLL/material/Blueprints,
  prevent overwriting while UE editor is open. No modifying AST maps/torches,
  no charring/damage and no per-particle replication.
- Test: `test_stage10_lifecycle.py` checks cycle of tongue birth/adult/split/
  detachment/death, cooling and acceleration, mesh coverage union, size-based
  tongue counts, runtime invariants and vertex/triangle budgets for 1/10/30
  actors. This is not a true PIE FPS/GPU benchmark and cannot validate art.
  Visual and multiplayer test still required before claiming final quality.


### Stage 10 — potwierdzone wykonanie i instalacja (2026-11-22)

- **Przypięty kod:** `2efc24ebcf701d95928468362e2d829740cda299` (zawiera pełny model: mesh-local union, cykle narodziny–wzrost–split–odlot–stygnięcie, osobny zanik końcówki, termiczną paletę i światło). GitHub Actions: oba przebiegi status `success`.
- **Realny UE 5.8.1 BuildPlugin:** `BUILD SUCCESSFUL`, `STAGE10_BUILD_EXIT=0`, runtime/editor DLL faktycznie zainstalowane: `STAGE10_DEPLOYED_DLLS=2`. Build log `Saved/ChatGPT/NiagaraFire/Stage10/BuildPlugin_20261122_063906.log`.
- **Backup przed instalacją:** `Saved/ChatGPT/NiagaraFire/Stage10/Backup_20261122_063906` (poprzednie własne pliki systemu ognia, dwa nowe assety jeśli już istniały oraz DLL; nie nadpisano map AST).
- **Commandlety:** Stage4 volume — exit 0; Stage6 igniter — exit 0; Stage8 existing surface — exit 0; **Stage10** — exit 0, `STAGE10_THERMAL_MATERIALS_CONFIGURED_REQUIRES_VISUAL_TEST`. Instalator kończy `STAGE10_INSTALL_SUCCESS_VISUAL_AND_CONTACT_TEST_PENDING`, exit 0.
- **Raport Stage10:** `Saved/ChatGPT/NiagaraFire/Stage10/stage10_result.json`, `errors: []`. Utworzone i zapisane własne materiały `M_AST_FireTongue.uasset` (7979 bytes) i `M_AST_FireSurfaceLife.uasset` (21234 bytes), oba skrypty BP zaktualizowane. `setup_stage10_lifecycle.py_20261122_063906.log`: 0 pasujących krytycznych błędów materiałów, shaderów lub Pythona.
- **Testy:** `test_stage10_lifecycle.py` potwierdza referencyjny cykl życia i termiczny porządek, łączenie obszarów, zakres liczby języków oraz ograniczenie wielkości pojedynczej proceduranej siatki. To kontrola liczbowa/statyczna; **nie** jest to pomiar FPS GPU ani test wizualny w PIE.
- **Wymagane jeszcze w grze:** wizualne sprawdzenie ognia na animowanej postaci, małym i dużym StaticMesh, krótkiego okresu po zapłonie i łączenia obszarów, oderwania końcówek wraz z kolorem, bardzo szybkiego ruchu, kosztu 1/10/30 aktorów, zachowania multiplayer.
- **Dokładność:** warstwa bazowa jest materiałem faktycznego mesha, ale sąsiedztwo punktów zapłonu na nim aproksymuje dystans euklidesowy w lokalnym rest pose, nie geodezję po topologii trójkątów. Wystające języki pozostają kamerowo orientowanymi, ciągłymi pasami w świecie. Bez użytecznej kolizji StaticMesh emitter może mieć wyłącznie warstwę bazową. Nie należy twierdzić, że to pełny CFD lub ostatecznie zweryfikowany wygląd.


### Stage10 correction — shared burning mantle and visibly bridged separation

After PIE feedback, the code now prioritizes the actual skeletal body mesh over
large attached static props; the GPU surface material forms a brighter, earlier
merging mantle of mesh-bound hotspots instead of isolated tongue roots.
Procedural tongue strips taper to **zero planar width at the source** and emerge
from that common mantle, rather than having separate flat sprite bottoms.
Each tongue now has three states: Growing, Bridging (roughly 0.12–0.18 seconds,
multiple frames at 25Hz), and Retracting (0.16–0.27 seconds). The detached tip
starts at the previous visible head, with a thin hot connecting bridge that
stretches and snaps. Only after the bridge has gone does the parent reduce its
length using smoothstep; it then grows a newly randomized shape from the
actual current short length. The extra bridge geometry has fixed topology and
is invisible except during separation. Upper-bound per actor ~550 vertices,
458 triangles for 15 tongues/16 detached fragments — NOT a measured GPU cost.
Only the AST fire plugin, own material script and tests are changed.

### Critical overlay rendering correction

UE 5.8 documents `UMeshComponent::OverlayMaterial` as a **translucent**
material to blend over the mesh. Earlier Stage8/10 authored it as
`BLEND_MASKED` with `MP_OPACITY_MASK`, a plausible reason why the
actual mesh base could be invisible despite valid asset/Blueprint saves.
Stage10 now authors the **surface-only** material as
`BLEND_TRANSLUCENT`, routes its smooth pixel-boundary coverage into
`MP_OPACITY`, and keeps the rising tongue material Masked. This is
a targeted fix based on Epic's documented overlay API; still requires
real PIE verification. Original mesh slots and base materials remain intact.


### Stage10 connected-source / bridged-split hotfix — verified installation 2026-11-22

- **Installed exact code revision:** `f3581e2b8469ce0e637e6c9fac7965c545ff3ed8`. GitHub Actions both `success`; actual Unreal Engine 5.8.1 BuildPlugin Win64 returned `STAGE10_BUILD_EXIT=0`.
- Both runtime/editor DLLs deployed (`STAGE10_DEPLOYED_DLLS=2`); source assets and DLLs backed up to `Saved/ChatGPT/NiagaraFire/Stage10/Backup_20261122_070735`. The Unreal Editor was not running when files were replaced.
- All four Unreal commandlets completed at exit 0 (Stage4 volume, Stage6 contact, Stage8 old surface, Stage10 final materials). Installer exit 0, `STAGE10_INSTALL_SUCCESS_VISUAL_AND_CONTACT_TEST_PENDING`.
- Final `stage10_result.json`: `errors: []`, two owned fire Blueprints updated, `M_AST_FireTongue.uasset` (9382 B) and `M_AST_FireSurfaceLife.uasset` (34598 B) saved.
- Final material commandlet log `setup_stage10_lifecycle.py_20261122_070735.log`: **0** matching critical Python/material/shader error lines. Material now authored as `BLEND_TRANSLUCENT` using `MP_OPACITY`, as required by Unreal's documented `UMeshComponent::OverlayMaterial` translucent pass; tongue ribbons remain Masked.
- Runtime surface selection prioritizes the actual SkeletalMesh over large attachments, and the source's root region expands/merges on mesh triangles while tongue roots have no planar flat-bottom geometry.
- Tip detachment now includes a short visible neck/bridge and smoothstep parent retraction, so parent length no longer instantaneously snaps from mature to short. New contacts join the same live source without restarting its growth cycles.
- **NOT tested:** subjective quality/silhouette in PIE, exact mesh overlap on every actor, multiplayer gameplay, 1/10/30 actor frame rate. Those remain user's in-editor checks. Never claim visual quality from build/commandlets alone.



### Source-first correction with verified fallback (2026-11-22)

Before editing, preserved both a GitHub branch
`backup/ast-fire-stage10-best-2026-11-22` at
`b163652061fb398b7fbcfefb1e35c2a20b784b22` and a physical local
project backup `Saved/ChatGPT/NiagaraFire/Stage10_BEST_20261122`
containing complete `Plugins/ASTFireRuntime` and `Content/VFX/Fire`.

**Found a real regression in PIE logs**: `M_AST_FireSurfaceLife` previously
failed the PCD3D_SM6 shader compile with
`(Node PreSkinnedPosition) External code identifier 'PreSkinnedPosition' is
not available in the Pixel shader`. It also lacked the skeletal mesh usage
flag. Thus the exact skinned surface visual was substituted with Unreal's
default material, while the independent tongues remained visible. Stage10's
material script now passes PreSkinnedPosition through a
`MaterialExpressionVertexInterpolator` before the pixel shader and sets
`used_with_skeletal_mesh=true`.

**Source-first C++:** `FBurnCluster` represents a mesh-local source.
Nearby world-contact hits merge into an existing cluster; distant hits add a
new independent cluster, capped at 8. Tongues retain their OWN existing
skinned mesh anchors but are associated with one shared cluster via
`ClusterIndex`; multiple tongues may grow from one burning region instead
of implicitly defining one GPU hotspot per tongue. Mesh overlay parameters
are now sourced only from burn clusters, not tongue positions.
Tongues become active as spread reaches their rest-pose skin positions,
without restarting existing growth/split or detached fragments.
This preserves the existing user-approved tongue lifecycle/bridge.

**Still required**: actual material shader compilation with PCD3D_SM6,
PIE validation of coverage on different meshes and performance, and checks
for overlay conflicts. The initial CPU clustering uses rest-pose Euclidean
distance, not a full mesh-topology flood solver.


### Shader validation resource limit / status (2026-11-22)

After the source-first fix, compiled and installed exact SHA
`9f0f00b62a800a25ce4a2111dfc5a68457b6d07d` on UE 5.8.1:
BuildPlugin EXIT 0; deployed 2 DLLs; all four NullRHI asset commandlets
reported Exit 0. Prior successful backup remains at
`Saved/ChatGPT/NiagaraFire/Stage10_BEST_20261122`, the new deployment
made its own backup under `Stage10/Backup_20261122_073139`.
Static GitHub CI passed both checks for that revision.

The new full RHI verification started tens of thousands of unrelated
WorldGrid/Nanite/global shader permutations due to missing warm shader
cache and XGE fallback. The attempt and a narrower CompileShadersTestBed
attempt were safely stopped; **do not claim that GPU shader compile or
appearance passed**. Exact original PIE failure was corrected in script
by passing PreSkinnedPosition through a VertexInterpolator and setting
`used_with_skeletal_mesh=true`; this remains to be confirmed at runtime.
`deploy_stage10_windows.ps1` now uses an opt-in `-ValidateGPU` switch,
avoiding expensive cold DDC compilation on every install.


### Real PIE failure and root-cause shader fix — 2026-11-22

The player reported that Stage10 **still had no visible shared burn
source**, despite successful C++ CI/build and Python material authoring.
Investigation of `Saved/Logs/AST_V1_18.log` after actual PIE exposed
a real PCD3D_SM6 compiler failure for `M_AST_FireSurfaceLife`:

```text
error: vector swizzle 'w' is out of bounds
float radius0=C0.w*grow0; ...
```

The authored `MaterialExpressionVectorParameter` supplied an RGB
`float3` to `MaterialExpressionCustom`, while HLSL expected RGBA
and read the non-existent `.w`. UE therefore substituted Default
Material for the **entire skinned mesh surface overlay**, leaving only
detached ribbons/independent tongues visible. Both previous GitHub CI
and `-nullrhi` commandlets missed the GPU-only error.

**Fix:** The eight `FireCluster%d` parameter vectors carry ONLY XYZ.
Each cluster radius is uploaded separately as
`FireClusterRadius0`–`FireClusterRadius7` scalar parameters, wired
to Custom node inputs `R0`–`R7`. All HLSL now uses
`radius%d=R%d*grow%d`. No changed tongue/bridge logic, which the
player previously approved. Regression tests assert new Custom node
inputs and scalar MID upload. Exact source change:
`5970995267035dd179ed799683cf747f82991d33`.

- GitHub CI after test-only adjustment `942b75574e682e5013d0bf852ccdda380da62b96`: 2/2 success.
- **Real UE 5.8.1 Win64 BuildPlugin log** `BuildPlugin_20261122_075150.log`:
  `BUILD SUCCESSFUL`, AutomationTool ExitCode 0.
- The subsequently executed material authoring commandlet log
  `setup_stage10_lifecycle.py_20261122_075150.log` reports
  `Python script executed successfully`, 0 errors (one unrelated
  deprecation warning). Both logs observed in synced Google Drive.
- Do not claim visual success until the next PIE run: shader success
  on PCD3D_SM6 after this fix, the appearance of contiguous source
  regions on moving SkeletalMesh/static props, and performance remain
  **pending real-time validation**.
- Retain immutable recovery reference:
  `backup/ast-fire-stage10-best-2026-11-22`
  (SHA `b163652061fb398b7fbcfefb1e35c2a20b784b22`),
  and physical `Saved/ChatGPT/NiagaraFire/Stage10_BEST_20261122`.



### Stage11 displaced source cloak — not a recolor (2026-11-22)

User PIE evidence showed that the shader now compiled, but simply painted
the character and cube yellow; existing independent tongue strips remained
visually isolated. **The original material overlay did not have ANY
WorldPositionOffset**, so by construction it could not envelope the mesh.
This is a distinct architectural defect, not another shader-error workaround.

New shared source pass **reuses the actual original mesh triangles and bone
skinning**, using `MaterialExpressionVertexNormalWS` and a mesh-local
cluster-field `MaterialExpressionCustom` in VERTEX stage linked to
`MP_WORLD_POSITION_OFFSET`. Thus no duplicate CPU skinning, no replacement
base materials, and no additional per-actor skeletal mesh required. Cluster
radius/position calculations are shared with the pixel mask. Outward normal
extrusion (10–28 cm nominal depth, sized to mesh bounds) plus upward motion,
hotspot noise and wind makes a single moving silhouette envelope. Tongues
continue from this one connected source, retaining previously approved
bridge split and detached life. The color uses the existing heat palette,
but the surface opacity is now **Fresnel/edge-driven** (thin alpha on face,
stronger alpha at silhouette), rather than coating the entire skin yellow.

Limits: true silhouette detail depends on original mesh topology/vertex
density; hard-edge normals may reveal seams; translucent overlay + WPO
requires GPU in-game verification. Does **not** add new mesh triangles,
geodesic burning simulation, or 3D fluid dynamics.


### Stage14 — shared mantle-embedded tongue necks (2026-11-22)

- Safety: immutable GitHub backup `backup/ast-fire-handoff-2026-11-22`
  at `b53ecb7906823f93f2d8c67941e1e7b1467ffc27`.
  A CI checksum test locks `ConfigureGeneration`, `EmitTip`,
  `AdvanceSimulation` and `AddStrip` against that revision.
- Section 1 now absorbs the lowest 42% of every *living* tongue into
  the same animated source field as the mesh sampled burning mantle.
  3 tapered neck capsules follow the exact same `TongueCenterAt`
  (including wind, inertia, curl, current growth length) that the
  section 0 ribbon follows. The ribbon smoothly appears between
  V=.20 and V=.46, overlapping the mantle field: no independent,
  brightly visible ribbon root.
- Neck geometry extends the bounds and heat field but is deliberately
  excluded from the old Prim MST, avoiding MST bridges between flying tips.
  Neck depth is blended toward the actual ribbon curve while the
  shared root still follows current skinned/static surface samples.
  Fixed 32x44 grid, same palette, no new persistent assets or replication.
- Not solved by this step: full watertight 3D mantle surface and geodesic
  adjacency; the source remains camera-facing and mesh-aware,
  not a full 3D fluid solver. Never infer quality from static checks;
  user PIE inspection and multi-actor profiling are necessary.


### Stage15 — live source = full flame curves (2026-11-22)

User screenshots showed an opaque full-body yellow blanket and independent
ribbons. The previous screen-space MST of dozens of skinned vertices was
used for **shape** as well as depth and was the main cause of the oversized
sphere-like mantle. Stage15 keeps those samples **only for depth** and
computes source coverage exclusively from the complete 0–100% growth path
of each skinned, wind-curved, tapering tongue. The original attached ribbon
section is invisible, so the source and its living tongues are genuinely
one masked image, not two overlaid visual objects. Bridged detachment and
world-space detached geometry are preserved exactly as before.

- Nearby skeletal roots remain in a localized 38–78 cm patch on the actual
  animated mesh, instead of being forcibly distributed over all distant limbs.
- Only short root links join close tongue bases. There is no thermal body
  generated from arbitrary mesh samples or cross-body MST edges.
- Same masked pixel-art tongue shader and fixed 32×44 source grid.
- Updated Stage10 and Stage14/15 tests keep independent generation,
  bridging, retraction, split trajectories and heat palette intact.
- This is still a 2.5D camera-facing style, **not a 3D fluid simulation**.
- Visual appearance and gameplay must be checked by the user in PIE.


### Stage16 — full-body coverage and dancing tongues (2026-11-22)

**Stage15 is the first stable GitHub `main`**, fixed at
`c8ebec3f3ef805bd43c1d88db2337db56b7cceb4`.
Stage16 changes only the work branch:

- Skeleton: replace the previous 38–78cm first-anchor patch restriction,
  which confined fire to the feet, with 10–14 true skinned-vertex roots
  distributed over the entire sampled skeleton (surface-centered seed +
  3D farthest-point coverage). Each root remains attached to the animated
  surface, using the Stage15 shared 2.5D heat silhouette.
- Static meshes: 8–15 roots from a deterministic, six-face 4×4 complex
  collision-ray lattice, selecting farthest uncovered surface points.
  Missing collision does not create invented flying anchors.
- Tongues: broad randomized generation-specific length (19–177cm x scale),
  width (5.5–43.5cm x scale), individual horizontal lean and multi-frequency
  turbulence; their origins stay fixed to their surface roots.
- Detached: length proportional to the **actual lost mother-tongue height**
  (82–107% of the removed segment), corresponding width, individual lateral
  ejection vectors, and precise placement at the fully swaying visual tip.
- Kept unchanged by byte-hash regression: `AdvanceSimulation` state
  transitions/growth/bridging/retraction/detached motion and `AddStrip`.
  New shape/launch parameters are deliberately protected by updated
  tests. The original Stage15 `main` and backup remain untouched.
- Still requires real UE 5.8.1 BuildPlugin and user PIE visual checks before
  claiming success or acceptable performance. No CFD or gameplay physics.


### Stage17 — silhouette and detached-life refinement (2026-11-22)

- **Stage16 remains pinned to `main`** at commit `30318a7b5b96aa02029226df0873654e10152234`.
  Immutable recovery: `backup/ast-fire-stage16-deployed-2026-11-22` and the
  local Stage16 `Backup_20261122_111629`. This stage edits only the work branch.
- Same single projected field: every tongue has independently curling upper
  contour, notched width and zero-to-two **same-life** shorter upper forks.
  No new root render layer, particles, smoke, or additional life state.
- Detached fragments: same `M_AST_FireTongue` shader/palette, similar
  brightness at birth with gradual cooling, undulating/tumbling shapes.
  A post-simulation vortex pass alters real world-space velocities; thus
  trajectories curve, rather than merely moving straight up with a sine sprite.
- `ConfigureGeneration`, `AdvanceSimulation` (including exact split,
  retraction, detached lifetime/age/death, base buoyancy), and `AddStrip`
  remain byte-identical to deployed Stage16; CI verifies hashes. Only
  detached spawn thermal/RHS frame, live rendering and flight vortex are new.
- GPU shader/render appearance and performance require separate validation;
  no automatic gameplay/PIE.


### Stage18 — less tentacular source and shared detached thermals (2026-11-22)

User screenshot after Stage17 showed long, hose-like branches and bright,
almost flat-colored detached pieces. Root cause of palette mismatch: detached
fragments used only 2 transverse mesh vertices and a uniform longitudinal
temperature. The established pixel shader had no data for an inner hot core
and cooler outer rim; raising spawn temperature cannot replace missing
transverse shading samples.

- Keep the entire Stage16/17 growth, bridge, retraction, detached-lifetime
  and vortex behaviors. Three protected C++ function hashes remain unchanged.
- Long, abnormally narrow living tongues get a modest length-proportional
  width floor. Distinct moving bulges, waist constriction and shorter swept
  secondary lobes replace the long thin shaft/horizontal antenna shape.
  They still share the same Stage15/16/17 live field.
- Detached visual strips use **7 columns x 5 rings** instead of the previous
  2-column geometry. Each column computes the same Core/Cover thermal
  envelope as the live source; edge vertices are dark red in the SAME
  `M_AST_FireTongue` masked material, interior cores remain hot yellow/
  white, and temperature cools over the existing lifetime. This is a
  geometric palette correction, not a new color preset/material.
- Vertex/triangle estimate (max 15 tongues, 16 detached): 2435
  vertices / 3914 triangles per 25Hz actor update, not a measured FPS figure.
- Stage16 is stable `main`, Stage17 backup exists. Stage18 stays on
  `work/niagara-fire-stage2` until visual approval. No smoke or sparks yet.


### Stage19 — approved corrective live silhouette taper (2026-11-22)

User PIE screenshots of Stage18 showed over-round, balloon-shaped primary flames.
ONLY `RadiusAt` in `UASTFireFreeFlameComponent::UpdateConnectedSource` is
changed: length-based width floor .098–.115 becomes .058–.068;
two excessive bulges (.46 and .55) become a lower main bulge (.26)
and a weak middle support (.12). The waist becomes .14 and the upper
contour receives a smooth taper down to .55 from V=.42–.82.
Moving rim amplitudes are reduced to .10/.06. Root coverage, all
life simulation/bridges, Stage18's 7-column detached palette, vortex and
world-space movement remain exactly unchanged.

`test_stage19_live_silhouette.py` locks the rest of UpdateConnectedSource,
entire UpdateGeometry (including detached gradient), and AdvanceSimulation
against Stage18 by exact FNV checksums; separately checks the slender
contour numerically. Stage16 remains stable `main`, Stage18 is backed up.
User PIE visual review is still required before promoting Stage19.
