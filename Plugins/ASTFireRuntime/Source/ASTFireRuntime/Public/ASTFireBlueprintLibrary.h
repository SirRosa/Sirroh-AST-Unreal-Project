#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ASTFireBlueprintLibrary.generated.h"

class AActor;
class UASTFireReactiveComponent;

/**
 * Generic fire API for any Actor: no special parent class, interface or custom
 * Blueprint required. Call IgniteActor from collision, projectile or gameplay.
 */
UCLASS()
class ASTFIRERUNTIME_API UASTFireBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="AST Fire",
        meta=(DisplayName="Ignite Any Actor"))
    static UASTFireReactiveComponent* IgniteActor(
        AActor* Target, FVector HitWorldPosition,
        float Strength = 1.0f, int32 DesiredFlamePoints = 1);

    UFUNCTION(BlueprintCallable, Category="AST Fire",
        meta=(DisplayName="Extinguish Actor"))
    static void ExtinguishActor(AActor* Target);

    UFUNCTION(BlueprintPure, Category="AST Fire",
        meta=(DisplayName="Is Actor Burning"))
    static bool IsActorBurning(const AActor* Target);

    UFUNCTION(BlueprintPure, Category="AST Fire")
    static UASTFireReactiveComponent* FindFireComponent(const AActor* Target);
};
