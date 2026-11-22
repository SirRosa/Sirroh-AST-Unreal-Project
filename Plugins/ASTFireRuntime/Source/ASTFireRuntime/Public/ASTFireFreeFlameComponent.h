#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ASTFireFreeFlameComponent.generated.h"

class USceneComponent;
class UProceduralMeshComponent;
class UMaterialInterface;

/**
 * Stage9: free world-space flame ribbons emitted from actual SKINNED VERTICES
 * or static-mesh collision surfaces. NO heat rectangle, height box or bones.
 * The root stays on the source; only heated gas/embers are advected away.
 * Entirely cosmetic and never replicated.
 */
UCLASS(ClassGroup=(ASTFire))
class ASTFIRERUNTIME_API UASTFireFreeFlameComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UASTFireFreeFlameComponent();
    void SetSource(USceneComponent* InSource, UMaterialInterface* InMaterial, int32 InSeed);
    void SetThermal(float InHeat, const FVector& InWorldWind, float InEnergy);
    float GetVisualEnergy() const { return VisualEnergy; }
    void SetFlamesVisible(bool bVisible);

protected:
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

private:
    static constexpr int32 MaxRoots = 18;
    static constexpr int32 Segments = 7;
    static constexpr int32 MaxEmbers = 12;

    struct FRoot
    {
        int32 SkinnedVertexIndex = INDEX_NONE;
        FVector LocalPosition = FVector::ZeroVector;
        FVector LastPosition = FVector::ZeroVector;
        FVector LastVelocity = FVector::ZeroVector;
        float Seed = 0.f;
        float Scale = 1.f;
    };
    struct FEmber
    {
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        float Life = 0.f;
        float MaximumLife = 1.f;
        float Temperature = 0.f;
    };

    void BuildAnchors();
    FVector SampleRoot(const FRoot& Root) const;
    void UpdateGeometry(float DeltaTime, bool bCreate);
    void AddBillboard(const FVector& Center, const FVector& Right,
        const FVector& Up, float HalfWidth, float HalfHeight,
        float Heat, float HeightFraction, TArray<FVector>& Verts,
        TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors) const;

    UPROPERTY(Transient)
    TObjectPtr<USceneComponent> Source = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UProceduralMeshComponent> Render = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> Material = nullptr;

    TArray<FRoot> Roots;
    TArray<FEmber> Embers;
    FVector Wind = FVector::ZeroVector;
    float Heat = 1.f;
    float Energy = 0.75f;
    float VisualEnergy = 0.65f;
    float Time = 0.f;
    float EmberDebt = 0.f;
    int32 Seed = 1729;
    bool bGenerated = false;
};
