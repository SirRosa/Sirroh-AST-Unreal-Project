#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ASTFireReactiveComponent.generated.h"

class APawn;
class UNiagaraComponent;
class UPointLightComponent;
class USphereComponent;
class UPrimitiveComponent;

/** Fire-owned light and visual response. No dependency on an existing torch light. */
UCLASS(ClassGroup=(ASTFire), meta=(BlueprintSpawnableComponent))
class ASTFIRERUNTIME_API UASTFireReactiveComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UASTFireReactiveComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire", meta=(ClampMin="0", ClampMax="1"))
    float Intensity = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Light", meta=(ClampMin="0"))
    float LightPower = 1300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Light", meta=(ClampMin="0"))
    float LightRadius = 525.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Response", meta=(ClampMin="10"))
    float InfluenceRadius = 225.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Response", meta=(ClampMin="0", ClampMax="50"))
    float MaxLeanDegrees = 16.0f;

    UFUNCTION(BlueprintCallable, Category="AST Fire")
    void SetFireIntensity(float NewIntensity);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION()
    void OnInfluenceBegin(UPrimitiveComponent* Overlapped, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
        bool bFromSweep, const FHitResult& SweepResult);

    UFUNCTION()
    void OnInfluenceEnd(UPrimitiveComponent* Overlapped, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

private:
    UPROPERTY(Transient)
    TObjectPtr<UNiagaraComponent> FX = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UPointLightComponent> Light = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USphereComponent> InfluenceSphere = nullptr;

    TSet<TWeakObjectPtr<APawn>> NearbyPawns;
    FVector LastOwnerPosition = FVector::ZeroVector;
    FRotator InitialFXRotation = FRotator::ZeroRotator;
    FVector InitialFXScale = FVector::OneVector;
    float Disturbance = 0.0f;
    float SmoothIntensity = 0.0f;
};
