# AST Fire — uniwersalny ogień UE 5.8.1

## Użycie na dowolnym obiekcie

Dodaj komponent **AST Fire Reactive** (`UASTFireReactiveComponent`) do dowolnego Actor Blueprint. Nie wymaga on klasy bazowej, zmiany materiałów obiektu, istniejącego światła ani niestandardowego mesha.

W ustawieniach komponentu:

| Obiekt | Coverage | Flame Points | Uwagi |
|---|---|---:|---|
| Pochodnia | Point | 1 | `PointOffset` ustawia podstawę płomienia |
| Drzewo | Bounds | 4–6 | Płomienie rozłożone po wymiarach największego widocznego mesha |
| Postać | Sockets | 3–6 | Ustaw **rzeczywiste** nazwy kości/socketów szkieletu |
| Meble/skrzyńka | Bounds | 2–4 | Zmniejsz `FlameSize` dla małych przedmiotów |
| Lecący pocisk | Point | 1 | Komponent podąża za ruchem i przechyla płomienie |

`PreferredMeshComponentName` pozwala wskazać konkretny komponent mesha. Jeśli jest puste, system wybiera największy widoczny mesh. Działa z `StaticMeshComponent` i `SkeletalMeshComponent`.

## Sterowanie przez Blueprint

W komponencie:

- `Ignite(Strength)` — zapal.
- `Extinguish()` — zgaś.
- `SetFireIntensity(Value)` — ustaw moc w zakresie 0–1.
- `SetFireCoverage(Mode, FlamePoints)` — zmień rozkład.
- `IgniteAtWorldPosition(HitLocation)` — dodaj punkt ognia tam, gdzie obiekt został trafiony; efekt podąża za obiektem.

Nowe funkcje `AST Fire Blueprint Library`:

- `Ignite Any Actor(Target, HitWorldPosition, Strength, DesiredFlamePoints)` — jeśli Actor nie ma komponentu ognia, funkcja utworzy go automatycznie, a następnie zapali obiekt.
- `Extinguish Actor(Target)`.
- `Is Actor Burning(Target)`.
- `Find Fire Component(Target)`.

Przykład: po trafieniu drzewa przez płonącą strzałę wywołaj `Ignite Any Actor` z `Hit Actor` i `Impact Point`. Aby płonęło więcej obszaru drzewa, użyj `DesiredFlamePoints = 5`.

## Renderowanie i oświetlenie

Komponent tworzy autorskie, skrzyżowane płaszczyzny płomienia z proceduralnym animowanym materiałem `M_AST_FirePixel_Core`. To niezależna warstwa wizualna zapewniająca widoczny płomień także wtedy, gdy system cząsteczek Niagara nie renderuje się poprawnie.

Dodatkowo używa `NS_Fire_Master` (płomień, żar, dym) i **jednego własnego źródła światła na Actor**, a nie oryginalnych lamp/placeholderów. Oświetlenie pulsuje w runtime. Komponent reaguje na ruch aktora i przechodzące postacie.

`bEditorPreview` pozwala widzieć proceduralne płomienie w viewport edytora. Podgląd renderu nie jest pełnym testem animacji zachowania w grze.

## Ograniczenia i walidacja

- To **uniwersalny system wizualny ognia**. Nie zawiera jeszcze obrażeń, rozprzestrzeniania po otoczeniu, spalania geometrii, synchronizacji sieciowej ani automatycznego osmalania tekstur.
- `Bounds` rozmieszcza ogień względem granic mesha, a nie dokładnej powierzchni trójkątów. Dla skomplikowanych obiektów użyj socketów lub miejsc trafień.
- Wiele efektów Niagara na jednym obiekcie może być kosztowne. Zacznij od 3–6 punktów i profiluj większe sceny.
- **Ręczne sprawdzenie:** zrestartuj edytor po zmianie biblioteki DLL, umieść `BP_AST_FireSource`, zobacz płomień, potem uruchom PIE samodzielnie i sprawdź migotanie światła. Następnie dodaj komponent do drzewa i postaci.
- Nie uruchamiać testów rozgrywki za pomocą automatyzacji GUI ani nie zmieniać autorskich map użytkownika.

Projektowe źródła są na GitHubie; `.uasset` pozostają w zsynchronizowanym projekcie. Bez Git LFS.
