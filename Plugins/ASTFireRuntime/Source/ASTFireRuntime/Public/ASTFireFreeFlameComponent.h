#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "ASTFireFreeFlameComponent.generated.h"

class USceneComponent;
class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/** Stage10: mesh-conforming ignition and connected growing flame tongues.
 * Cosmetic simulation only; burn authority stays with ASTFireReactiveComponent.
 * The base follows rendered skin vertices; only detached tips travel freely.
 */
UCLASS(ClassGroup=(ASTFire))
class ASTFIRERUNTIME_API UASTFireFreeFlameComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UASTFireFreeFlameComponent();
    void SetSource(USceneComponent* InSource, UMaterialInterface* InMaterial, int32 InSeed);
    void SetThermal(float InHeat, const FVector& InWorldWind, float InEnergy);
    void SetSurfaceMaterial(UMaterialInstanceDynamic* InMaterial);
    void SetIgnitionPoints(const TArray<FVector_NetQuantize10>& InPoints);
    float GetVisualEnergy() const { return VisualEnergy; }
    void SetFlamesVisible(bool bVisible);

protected:
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

private:
    static constexpr int32 MaxTongues = 15;
    static constexpr int32 RingsPerTongue = 9;
    static constexpr int32 MaxDetached = 16;
    static constexpr int32 RingsPerDetached = 5;
    static constexpr int32 SurfaceClusters = 8;

    struct FTongue
    {
        int32 SkinnedVertexIndex = INDEX_NONE;
        FVector RestPosition = FVector::ZeroVector;
        FVector WorldPrevious = FVector::ZeroVector;
        FVector WorldVelocity = FVector::ZeroVector;
        float Seed = 0.f;
        float Scale = 1.f;
        float Age = 0.f;
        float GrowSeconds = 0.7f;
        float MaxLength = 75.f;
        float BaseWidth = 12.f;
        float RemainingFraction = 0.36f;
        float Phase = 0.f;
        int32 Generation = 0;
        bool bActive = true;
    };
    struct FDetached
    {
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        FVector Right = FVector::RightVector;
        float Age = 0.f;
        float Lifetime = 0.8f;
        float Width = 10.f;
        float Length = 22.f;
        float Temperature = 0.65f;
        float Seed = 0.f;
        bool bAlive = false;
    };

    void BuildAnchors();
    FVector SampleRoot(const FTongue& Tongue) const;
    void AdvanceSimulation(float Dt);
    void UpdateGeometry(bool bCreate);
    void UpdateSurfaceMap(float Dt);
    void EmitTip(FTongue& Tongue, const FVector& Base, float Length,
        const FVector& Flow);
    void ConfigureGeneration(FTongue& Tongue);
    float Uniform(int32 Salt) const;
    void AddStrip(const TArray<FVector>& Centers, const TArray<float>& Widths,
        const TArray<float>& Temperatures, float Seed, float LifeFade,
        const FVector& Right, TArray<FVector>& Verts,
        TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors) const;

    UPROPERTY(Transient)
    TObjectPtr<USceneComponent> Source = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<UProceduralMeshComponent> Render = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> Material = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> SurfaceMID = nullptr;

    TArray<FTongue> Tongues;
    TArray<FDetached> Detached;
    TArray<FVector> IgnitionPoints;
    FVector Wind = FVector::ZeroVector;
    float Heat = 1.f;
    float Energy = 0.75f;
    float VisualEnergy = 0.65f;
    float Elapsed = 0.f;
    float Spread = 0.0f;
    int32 Seed = 1729;
    int32 DetachCursor = 0;
    bool bGenerated = false;
    bool bVisible = true;
    bool bClusterMapUploaded = false;
};
