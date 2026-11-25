#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "ASTFireFreeFlameComponent.generated.h"

class USceneComponent;
class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPointLightComponent;

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
    void SetSizeScale(float Value) { SizeScale=FMath::Max(.01f,Value); }
    void SetSource(USceneComponent* InSource, UMaterialInterface* InMaterial, int32 InSeed);
    void SetThermal(float InHeat, const FVector& InWorldWind, float InEnergy);
    void SetSurfaceMaterial(UMaterialInstanceDynamic* InMaterial);
    void SetIgnitionPoints(const TArray<FVector_NetQuantize10>& InPoints);
    int32 GetTongueCount() const { return Tongues.Num(); }
    float GetVisualEnergy() const { return VisualEnergy; }
    void SetFlamesVisible(bool bVisible);

    /** Editor commandlet audit: isolated transient physics world, no map writes. */
    UFUNCTION(BlueprintCallable, Category="AST Fire|Diagnostics")
    static FString RunSmokeAudit(UMaterialInterface* TestMaterial);

    UFUNCTION(BlueprintCallable, Category="AST Fire|Diagnostics")
    static FString RunFireV4Audit(UMaterialInterface* TestMaterial);

    UFUNCTION(BlueprintCallable, Category="AST Fire|Diagnostics")
    static FString RunFireV4Capture(UMaterialInterface* TestMaterial);

    UFUNCTION(BlueprintCallable, Category="AST Fire|Diagnostics")
    static FString RunFireSurfaceAudit(UMaterialInterface* TestMaterial, USkeletalMesh* Mesh);

protected:
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

private:
    float SizeScale=1.f;
    static constexpr int32 MaxTongues = 15;
    static constexpr int32 RingsPerTongue = 9;
    static constexpr int32 MaxDetached = 16;
    static constexpr int32 MaxSparks = 48;
    static constexpr int32 MaxSmoke = 72; // bounded world-space density carriers
    static constexpr int32 SmokeTones = 5;
    static constexpr int32 SmokeGridMaxX = 64;
    static constexpr int32 SmokeGridMaxY = 112;
    static constexpr int32 RingsPerDetached = 5;
    static constexpr int32 BridgeRings = 4;
    static constexpr int32 SurfaceClusters = 8;
    // Shared source silhouette uses a fixed topology and the SAME color
    // material as the approved tongue and split-bridge geometry.
    static constexpr int32 SourceGridX = 32;
    static constexpr int32 SourceGridY = 44;
    static constexpr int32 MaxSourceAnchors = 96;

    enum class EFlameLife : uint8 { Growing, Bridging };
    // One physical source may support several independently-lived tongues.
    // Hotspots nearby are merged here, before the mesh-bound GPU source mask.
    struct FBurnCluster
    {
        FVector RestCenter = FVector::ZeroVector;
        float Radius = 70.f;
        int32 Members = 1;
    };

    struct FSourceAnchor
    {
        int32 SkinnedVertexIndex = INDEX_NONE;
        FVector RestPosition = FVector::ZeroVector;
    };

    struct FTongue
    {
        int32 SkinnedVertexIndex = INDEX_NONE;
        int32 ClusterIndex = INDEX_NONE;
        float ActivationSpread = 0.f;
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
        float PreviousPhase = 0.f;
        float PreviousWidth = 0.f;
        float CurrentLength = 0.f;
        float CurrentWidth = 0.f;
        float RestartLength = 0.f;
        float SplitLength = 0.f;
        float RetractTarget = 0.f;
        float PhaseAge = 0.f;
        float BridgeSeconds = .14f;
        float RetractSeconds = .22f;
        int32 BridgedChild = INDEX_NONE;
        EFlameLife Life = EFlameLife::Growing;
        int32 Generation = 0;
        bool bActive = true;
        FVector DisplayArc[RingsPerTongue]{};
        float DisplayRadius[RingsPerTongue]{};
        float DisplayClock=0.f;
        float DisplayHeat=1.f;
        int32 DisplayGeneration=INDEX_NONE;
        bool bDisplayedPinch=false;
        // World-space radial growth, persistent per source anchor.
        FVector Outward=FVector::ZeroVector;
        float RebirthDelay=0.f;
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
        // The actual severed upper arc, expressed relative to its cut point.
        FVector Arc[RingsPerTongue]{};
        float Radius[RingsPerTongue]{};
        float SourceV[RingsPerTongue]{};
        int32 ArcCount=0;
        int32 ParentGeneration=0;
        float BirthClock=0.f;
        float BirthPhase=0.f;
        float BirthHeat=1.f;
        bool bAlive = false;
    };

    struct FSpark
    {
        FVector Position = FVector::ZeroVector;
        FVector DriftPosition = FVector::ZeroVector;
        FVector WaveSide = FVector::RightVector;
        FVector Velocity = FVector::ZeroVector;
        float WaveAmplitude = 0.f;
        float WaveFrequency = 0.f;
        float WavePhase = 0.f;
        float EmissionHeat = 1.f; // retained after the source cools
        float Age = 0.f;
        float Lifetime = .4f;
        float Size = 2.f;
        float Seed = 0.f;
        bool bAlive = false;
    };

    struct FSmoke
    {
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        float Age = 0.f;
        float Lifetime = 1.4f;
        float Radius = 10.f; // collision-cleared support radius
        float InitialRadius = 10.f;
        float MaxRise = 150.f;
        float RiseSpeed = 40.f;
        float Seed = 0.f;
        float BaseZ = 0.f;
        bool bAlive = false;
    };

    void BuildAnchors();
    void RebuildClusters();
    void BuildSourceAnchors();
    float UpdateConnectedSource(bool bCreate, const FVector& View,
        const FVector& Right);
    FVector SampleRoot(const FTongue& Tongue) const;
    FVector TongueCenterAt(const FTongue& Tongue, const FVector& Base,
        float Fraction, const FVector& Right) const;
    float TongueRadiusAt(const FTongue& Tongue,float V) const;
    void AdvanceSimulation(float Dt);
    void UpdateDetachedVortices(float Dt);
    void UpdateSparks(float Dt);
    void UpdateSmoke(float Dt);
    void UpdateSmokeGeometry();
    void UpdateGeometry(bool bCreate);
    void UpdateSurfaceMap(float Dt);
    int32 EmitTip(FTongue& Tongue, const FVector& Base, float Length,
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
    TObjectPtr<UProceduralMeshComponent> SmokeRender = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> Material = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> SmokeMaterial = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> SparkMaterial = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> SurfaceMID = nullptr;

    TArray<FTongue> Tongues;
    TArray<FBurnCluster> BurnClusters;
    TArray<FSourceAnchor> SourceAnchors;
    // The render-space roots are sampled from the SAME 3D mantle surface
    // as mesh section 1. A tongue must not start on a different depth plane.
    TArray<FVector> ConnectedRoots;
    TArray<FDetached> Detached;
    TArray<FSpark> Sparks;
    float SparkSpawnDebt = 0.f;
    int32 SparkCursor = 0;
    TArray<FSmoke> Smoke;
    float SmokeSpawnDebt = 0.f;
    int32 SmokeCursor = 0;
    float SmokeLightScanDebt = 0.f;
    float SmokeSimDebt = 0.f;
    float SmokeFireSize = 0.f;
    float SmokePixelPitch = 0.f;
    FVector SmokeFireTop = FVector::ZeroVector;
    TArray<TWeakObjectPtr<UPointLightComponent>> NearbySmokeLights;
    TArray<FVector> IgnitionPoints;
    FVector Wind = FVector::ZeroVector;
    float Heat = 1.f;
    float Energy = 0.75f;
    float VisualEnergy = 0.65f;
    float Elapsed = 0.f;
    float Spread = .22f;
    int32 Seed = 1729;
    int32 DetachCursor = 0;
    bool bGenerated = false;
    bool bVisible = true;
    bool bClusterMapUploaded = false;
    // Appended to preserve offsets of existing fields used by game modules.
    float FirePixelPitch=0.f;
    float FireVisualHeight=0.f;
    float FireVisualWidth=0.f;
};
