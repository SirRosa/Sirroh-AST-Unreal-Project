# AST Fire — Etap 2 (Unreal Engine 5.8.1)

## Zakres

- Autorskie materiały `M_AST_FirePixel_Core`, `M_AST_FirePixel_Ember`, `M_AST_FirePixel_Smoke`.
- Oddzielne emitery `NE_AST_Fire_{Core,Ember,Smoke}_V2`, duplikowane z silnikowego szablonu Niagara `Fountain`.
- System `NS_Fire_Master` zawierający docelowo trzy emitery.
- `BP_AST_FireSource` z własnym `NiagaraComponent`, własnym `PointLightComponent` i `ASTFireReactiveComponent`.
- Własne oświetlenie ognia (bez wykorzystywania `TorchLight` z `BP_Torch_U`).
- Reakcja płomienia na prędkość źródła i blisko przechodzących postaci (detekcja zdarzeniami overlap, bez przeszukiwania świata).

## Bieżący stan weryfikacji

- Poprzednia wersja wtyczki ASTFireRuntime / ASTFireEditor kompilowała się niezależnie w BuildPlugin (UE 5.8.1).
- Wcześniejszy skrypt finalizacyjny kończył się błędem `Custom -> ComponentMask`.
- **Nowe źródła C++ i nowy finalizator zostały zapisane na GitHubie, ale NIE były jeszcze zbudowane/uruchomione w UE.**
- GitHub Actions sprawdza składnię Pythona i zakres zmian. Nie kompiluje Unreal C++.
- Ogień nie jest jeszcze uznany za gotowy; wyświetlanie i właściwości wizualne wymagają testów użytkownika.

## Kolejność wdrożenia na komputerze z UE

1. Połączyć pliki z `Plugins/ASTFireRuntime` z lokalnym pluginem AST i zbudować wtyczkę dla UE 5.8.1. Nie nadpisywać map ani oryginalnego `BP_Torch_U`.
2. Upewnić się, że plugin `ASTFireRuntime` jest włączony w `.uproject`.
3. Uruchomić `Automation/ASTFire/finalize_stage2.py` przez `UnrealEditor-Cmd.exe -run=pythonscript -script=<pełna_ścieżka>` (plik `fix_stage2_materials.py` musi być w tym samym katalogu).
4. Sprawdzić `Saved/ChatGPT/NiagaraFire/Stage2/stage2_complete_result.json`.
5. Jeśli wynik to `ASSETS_ASSEMBLED_REQUIRES_VISUAL_VALIDATION`, ręcznie sprawdzić efekt w edytorze, bez automatycznego playtestu.

## Co jeszcze nie jest gotowe

- Artystyczne strojenie emitera Fountain dla płomienia / żaru / dymu.
- Parametry Niagara User używane wewnątrz grafów emitterów (runtime obecnie dodatkowo używa zmiany skali i nachylenia komponentu).
- Presety dla pochodni, ogniska, płonących strzał, zaklęć i podłogi.
- Finalna integracja z `BP_Torch_U`.

## Organizacja

Projekt i zasoby 16 GB pozostają na Google Drive. GitHub służy do wersjonowania niewielkich plików źródłowych i skryptów. **Nie wymagamy Git LFS ani importowania do GitHub całej zawartości projektu.** Nie zakładamy, że commit automatycznie aktualizuje kopię lokalną projektu.

Ograniczenia projektu: nie uruchamiać automatycznych playtestów, symulowanego interfejsu edytora, renderowania lub testów ruchu; użytkownik wykonuje walidację wizualną i gameplay.
