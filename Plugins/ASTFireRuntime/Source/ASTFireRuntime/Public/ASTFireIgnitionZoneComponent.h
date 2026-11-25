#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "TimerManager.h"
#include "ASTFireIgnitionZoneComponent.generated.h"

class AActor;
class AStaticMeshActor;
class UStaticMesh;
class UPrimitiveComponent;

/**
 * Standalone contact-ignition zone for BP_AST_FireSource_Igniter.
 * Server-authoritative continuous contact heating and fuel replenishment.
 */
UCLASS(ClassGroup=(ASTFire), meta=(BlueprintSpawnableComponent))
class ASTFIRERUNTIME_API UASTFireIgnitionZoneComponent : public UBoxComponent
{
    GENERATED_BODY()

public:
    UASTFireIgnitionZoneComponent();

    UFUNCTION(BlueprintCallable, Category="AST Fire|Audit")
    static FString RunContactAudit(UClass* BoxClass,UClass* BarrelClass);

    UFUNCTION(BlueprintCallable, Category="AST Fire|Audit")
    static FString RunWoodAudit(UStaticMesh* Beam,UStaticMesh* Fence,UStaticMesh* Rock);

    // Editable zone size/offset; applied on BeginPlay, NEVER on the CDO.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Contact")
    FVector TriggerHalfExtent = FVector(75.0f, 75.0f, 84.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Contact")
    FVector TriggerOffset = FVector(0.0f, 0.0f, 68.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Contact",
        meta=(ClampMin="0.05", ClampMax="1.0"))
    float IgnitionStrength = 0.9f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Contact",
        meta=(ClampMin="1", ClampMax="12"))
    int32 MaxDistributedFlames = 8;

    /** Opt-in for initial native-only overlaps (e.g. static level geometry). Movable props and pawns heat normally. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Contact")
    bool bIgniteInitialOverlaps = false;

    /** Do not react to another dedicated fire source by default. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Contact")
    bool bIgniteOtherFireSources = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Contact")
    bool bRequireSourceBurning = true;

    bool IsSustaining(const AActor* Target) const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION()
    void HandleBeginOverlap(UPrimitiveComponent* OverlappedComponent,
        AActor* OtherActor, UPrimitiveComponent* OtherComponent,
        int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

    UFUNCTION()
    void HandleEndOverlap(UPrimitiveComponent* OverlappedComponent,
        AActor* OtherActor, UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex);

private:
    void IgniteTarget(AActor* Target, const FVector& ContactPosition);
    void PollContactOverlaps();
    void PollContactOverlapsAt(double Now);
    bool IsHeatActive() const;
    void ClearContacts();
    static float ContactDelay(float SizeCm);
    FTimerHandle ContactPollTimer;
    bool bPrimingContacts = false;
    TSet<TWeakObjectPtr<AActor>> IgnitedDuringOverlap;
    TMap<TWeakObjectPtr<AActor>,double> ContactStartedAt;
    TSet<TWeakObjectPtr<AActor>> SuppressedInitialContacts;
    TArray<TWeakObjectPtr<AStaticMeshActor>> NearbyWood;
    double WoodScanAt=-1.;
    FVector WoodScanCenter=FVector::ZeroVector;
};
