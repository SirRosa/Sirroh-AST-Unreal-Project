#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ASTFireEditorLibrary.generated.h"

class UNiagaraSystem;
class UNiagaraEmitter;
class UMaterialInterface;

/** Editor-only tooling: assembles an AST fire system from native Niagara emitter templates. */
UCLASS()
class ASTFIREEDITOR_API UASTFireEditorLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="AST Fire")
    static FString BuildSystem(
        UNiagaraSystem* System,
        UNiagaraEmitter* Core,
        UNiagaraEmitter* Ember,
        UNiagaraEmitter* Smoke,
        UMaterialInterface* CMat,
        UMaterialInterface* EMat,
        UMaterialInterface* SMat);
};
