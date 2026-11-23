#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/SoftObjectPtr.h"
#include "Engine/NetSerialization.h"
#include "ASTFireReactiveComponent.generated.h"

class AActor;
class APawn;
class UNiagaraSystem;
class UNiagaraComponent;
class UPointLightComponent;
class USphereComponent;
class UPrimitiveComponent;
class USceneComponent;
class UMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UASTFireFreeFlameComponent;
class FLifetimeProperty;


/** Purely local visual state. Never replicated: gameplay cannot depend on pixels. */
struct FASTFlameFragment
{
    float X = 0.0f;
    float Y = 0.0f;
    float VX = 0.0f;
    float VY = 0.0f;
    float Life = 0.0f;
    float Heat = 0.0f;
};

struct FASTLocalFlameField
{
    TArray<float> Temperature;
    TArray<float> Work;
    TArray<FASTFlameFragment> Fragments;
    FVector LastAnchorLocation = FVector::ZeroVector;
    float Time = 0.0f;
    float SpawnDebt = 0.0f;
    FVector LastAnchorVelocity = FVector::ZeroVector;
    float Breath = 0.78f;
    float VisualEnergy = 0.55f;
    float CurrentGust = 0.0f;
    bool bReady = false;
};

/** Placement options shared by torches, characters, trees, furniture and physics actors. */
UENUM(BlueprintType)
enum class EASTFireCoverage : uint8
{
    Point UMETA(DisplayName="One flame"),
    Bounds UMETA(DisplayName="Distributed over object bounds"),
    Sockets UMETA(DisplayName="Sockets and bones")
};

/**
 * Fire that can be added to ANY Actor, without changing its mesh, material or light.
 * Creates own flame cards, Niagara particles and one fire-owned point light.
 * Blueprint API: Ignite, Extinguish, SetFireIntensity, SetFireCoverage, IgniteAtWorldPosition.
 * Attach target auto-selects the largest mesh unless PreferredMeshComponentName is specified.
 */
UCLASS(ClassGroup=(ASTFire), meta=(BlueprintSpawnableComponent))
class ASTFIRERUNTIME_API UASTFireReactiveComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UASTFireReactiveComponent();

    /** Intensity represents active fire, not temperature or damage. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing=OnRep_Burning, Category="AST Fire", meta=(ClampMin="0", ClampMax="1"))
    float Intensity = 1.0f;

    /** Disable to have an actor begin unlit; call Ignite in Blueprint on demand. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing=OnRep_Burning, Category="AST Fire")
    bool bStartLit = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    bool bEnableFlameCards = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    bool bEnableNiagara = false;

    /** Procedural camera-facing 2.5D heat-field flame, following spatial burn anchors. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    bool bEnableProceduralFlames = true;

    /** Only for migration/debug. Default renderer uses UNBOUNDED surface emission,
        not the Stage5-8 fixed rectangle per bone. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    bool bUseLegacyHeatGrid = false;

    /** Authored material generated under /Game/VFX/Fire/Materials by Stage 4 commandlet. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    TSoftObjectPtr<UMaterialInterface> VolumeMaterial;

    /** Animated masked flame on EXACT GPU-skinned/rendered mesh triangles. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    bool bEnableSurfaceFire = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    TSoftObjectPtr<UMaterialInterface> SurfaceMaterial;

    /** Independent translucent char overlay (never replaces mesh material slots). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Charring")
    bool bEnableCharring = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Charring")
    TSoftObjectPtr<UMaterialInterface> CharMaterial;

    /** Persistent burn marks on objects/NPCs. Player soot recovers after extinguishing. */
    UPROPERTY(ReplicatedUsing=OnRep_Burning, VisibleAnywhere, BlueprintReadOnly,
        Category="AST Fire|Charring")
    float CharProgress = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Charring",
        meta=(ClampMin="0.1", ClampMax="5"))
    float CharringRate = 1.5f;

    /** Only player-controlled Pawns recover their original appearance.
        Recovery begins after extinguishing, and lasts 10 seconds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Charring",
        meta=(ClampMin="0.1", ClampMax="60"))
    float PlayerCharRecoverySeconds = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Response")
    FVector AmbientWind = FVector::ZeroVector;

    /** Deterministic seed for visual shape variation, not gameplay authority. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="AST Fire|Visuals")
    int32 SimulationSeed = 1729;

    /** Original project fire system; defaults to /Game/VFX/Fire/Systems/NS_Fire_Master. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    TSoftObjectPtr<UNiagaraSystem> FireSystem;

    /** Original procedural fire material, not any existing project placeholder. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    TSoftObjectPtr<UMaterialInterface> FlameMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals", meta=(ClampMin="0.05", ClampMax="20"))
    float FlameSize = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    bool bEditorPreview = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing=OnRep_BurnAnchors, Category="AST Fire|Coverage")
    EASTFireCoverage Coverage = EASTFireCoverage::Point;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing=OnRep_BurnAnchors, Category="AST Fire|Coverage", meta=(ClampMin="1", ClampMax="16"))
    int32 FlamePoints = 1;

    /** Optional precise target (e.g. SkeletalMeshComponent or trunk). Otherwise auto-select. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Coverage")
    FName PreferredMeshComponentName;

    /** Explicit bones/sockets for animated characters and rigged objects. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Coverage")
    TArray<FName> FlameSockets;

    /** A relative offset from the attachment target for a single-point flame. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Coverage")
    FVector PointOffset = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Light", meta=(ClampMin="0"))
    float LightPower = 450.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Light", meta=(ClampMin="0"))
    float LightRadius = 525.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Response", meta=(ClampMin="10"))
    float InfluenceRadius = 375.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Response", meta=(ClampMin="0", ClampMax="45"))
    float MaxLeanDegrees = 16.0f;

    /** Server-authoritative fire lifetime. Standard 180 cm ignited object lasts ~6 s. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Burn", meta=(ClampMin="0"))
    bool bAutoExtinguish = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Burn", meta=(ClampMin="0.1", ClampMax="30"))
    float BaseBurnSeconds = 6.0f;

    /** Terminal temperature ramp; the mesh remains visible during afterglow. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Burn", meta=(ClampMin="0.1", ClampMax="5"))
    float BurnFadeSeconds = 0.8f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Burn", meta=(ClampMin="10"))
    float ReferenceSizeCm = 180.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Burn", meta=(ClampMin="1", ClampMax="6"))
    float MaxBurnSizeMultiplier = 3.5f;

    /** A unified API for objects that can burn; gameplay damage/spread is separate. */
    UFUNCTION(BlueprintCallable, Category="AST Fire")
    void Ignite(float NewIntensity = 1.0f);

    UFUNCTION(BlueprintCallable, Category="AST Fire")
    void Extinguish();

    UFUNCTION(BlueprintCallable, Category="AST Fire")
    void SetFireIntensity(float NewIntensity);

    UFUNCTION(BlueprintCallable, Category="AST Fire")
    void SetFireCoverage(EASTFireCoverage NewCoverage, int32 NewFlamePoints = 1);

    /** Add a flame where an attack/projectile hit the object; follows the parent mesh. */
    UFUNCTION(BlueprintCallable, Category="AST Fire")
    void IgniteAtWorldPosition(FVector WorldPosition);

    /** Rebuild generated visuals (useful after changing coverage settings). */
    UFUNCTION(BlueprintCallable, Category="AST Fire")
    void RefreshFireVisuals();

protected:
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION()
    void OnRep_Burning();

    UFUNCTION()
    void OnRep_BurnAnchors();

    UFUNCTION()
    void OnInfluenceBegin(UPrimitiveComponent* Overlapped, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
        bool bFromSweep, const FHitResult& SweepResult);

    UFUNCTION()
    void OnInfluenceEnd(UPrimitiveComponent* Overlapped, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

private:
    void InitializeFire();
    void RemoveGeneratedVisuals();
    void SetupSurfaceFire(USceneComponent* Parent);
    void ReleaseSurfaceFire();
    void UpdateSurfaceVisibility();
    USceneComponent* FindTargetComponent() const;
    UPointLightComponent* FindOwnedLight() const;
    UNiagaraComponent* FindExistingFireFX() const;
    void AddFlamePoint(USceneComponent* Parent, FName Socket, FVector LocalPosition,
        float CardScale, UNiagaraSystem* System, UStaticMesh* PlaneMesh,
        UMaterialInterface* Material, UMaterialInterface* ProceduralMaterial,
        bool bUseExistingFX);
    void AddVolume(USceneComponent* Parent, FName Socket,
        FVector LocalPosition, float Scale, UMaterialInterface* Material);
    void UpdateVolume(UProceduralMeshComponent* Volume, int32 Index, bool bCreate, float StepSeconds = 0.0f);
    void UpdateVisibility();
    float ComputeBurnDuration() const;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UNiagaraComponent>> FireFX;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UNiagaraComponent>> GeneratedFX;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UStaticMeshComponent>> FireCards;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UProceduralMeshComponent>> FireVolumes;

    TArray<float> FireVolumeScales;
    TArray<FASTLocalFlameField> LocalFields;

    UPROPERTY(Transient)
    TObjectPtr<UMeshComponent> FireSurfaceMesh = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> FireSurfaceMID = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> PreviousFireSurfaceOverlay = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UPointLightComponent> Light = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UASTFireFreeFlameComponent> FreeFlames = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UPointLightComponent> GeneratedLight = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USphereComponent> InfluenceSphere = nullptr;

    UPROPERTY(ReplicatedUsing=OnRep_BurnAnchors)
    TArray<FVector_NetQuantize10> ExtraLocalPoints;

    TSet<TWeakObjectPtr<APawn>> NearbyPawns;
    TMap<TWeakObjectPtr<APawn>, FVector> LastNearbyPawnPositions;
    FVector LastOwnerPosition = FVector::ZeroVector;
    FVector LastOwnerVelocity = FVector::ZeroVector;
    FQuat LastOwnerRotation = FQuat::Identity;
    FVector SmoothedFlow = FVector::ZeroVector;
    FVector SmoothedAngularVelocity = FVector::ZeroVector;
    float VolumeUpdateDebt = 0.0f;
    float Disturbance = 0.0f;
    float LightEnvelope = 0.65f;
    float SmoothIntensity = 0.0f;
    float BurnElapsedSeconds = 0.0f;
    float BurnDurationSeconds = 0.0f;
    float InitialBurnIntensity = 1.0f;
    float CharSmolderRemaining = 0.f;
    float PreviousReplicatedCharProgress = 0.f;
    float PlayerCharFadeElapsedSeconds = 0.f;
    float PlayerCharFadeStartProgress = 0.f;
    bool bPreviousReplicatedLit = false;
    bool bInitialized = false;
    bool bPlaying = false;
};
