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
class UProceduralMeshComponent;
struct FLifetimeProperty;

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

    /** Procedural 3D flame geometry; replaces intersecting flat cards. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    bool bEnableProceduralFlames = true;

    /** Authored material generated under /Game/VFX/Fire/Materials by Stage 4 commandlet. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Visuals")
    TSoftObjectPtr<UMaterialInterface> VolumeMaterial;

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
    float LightPower = 1300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Light", meta=(ClampMin="0"))
    float LightRadius = 525.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Response", meta=(ClampMin="10"))
    float InfluenceRadius = 225.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AST Fire|Response", meta=(ClampMin="0", ClampMax="45"))
    float MaxLeanDegrees = 16.0f;

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
    USceneComponent* FindTargetComponent() const;
    UPointLightComponent* FindOwnedLight() const;
    UNiagaraComponent* FindExistingFireFX() const;
    void AddFlamePoint(USceneComponent* Parent, FName Socket, FVector LocalPosition,
        float CardScale, UNiagaraSystem* System, UStaticMesh* PlaneMesh,
        UMaterialInterface* Material, UMaterialInterface* ProceduralMaterial,
        bool bUseExistingFX);
    void AddVolume(USceneComponent* Parent, FName Socket,
        FVector LocalPosition, float Scale, UMaterialInterface* Material);
    void UpdateVolume(UProceduralMeshComponent* Volume, int32 Index, bool bCreate);
    void UpdateVisibility();

    UPROPERTY(Transient)
    TArray<TObjectPtr<UNiagaraComponent>> FireFX;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UNiagaraComponent>> GeneratedFX;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UStaticMeshComponent>> FireCards;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UProceduralMeshComponent>> FireVolumes;

    TArray<float> FireVolumeScales;

    UPROPERTY(Transient)
    TObjectPtr<UPointLightComponent> Light = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UPointLightComponent> GeneratedLight = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USphereComponent> InfluenceSphere = nullptr;

    UPROPERTY(ReplicatedUsing=OnRep_BurnAnchors)
    TArray<FVector_NetQuantize10> ExtraLocalPoints;

    TSet<TWeakObjectPtr<APawn>> NearbyPawns;
    FVector LastOwnerPosition = FVector::ZeroVector;
    FVector LastOwnerVelocity = FVector::ZeroVector;
    FQuat LastOwnerRotation = FQuat::Identity;
    FVector SmoothedFlow = FVector::ZeroVector;
    FVector SmoothedAngularVelocity = FVector::ZeroVector;
    float VolumeUpdateDebt = 0.0f;
    float Disturbance = 0.0f;
    float SmoothIntensity = 0.0f;
    bool bInitialized = false;
    bool bPlaying = false;
};
