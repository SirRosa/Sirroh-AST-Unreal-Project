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
