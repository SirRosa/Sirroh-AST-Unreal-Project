#include "ASTFireReactiveComponent.h"

#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "UObject/ConstructorHelpers.h"

namespace ASTFire
{
    static const FName FireLightTag(TEXT("ASTFireOwnedLight"));
    static const FName FireFXTag(TEXT("ASTFireOwnedFX"));
    static bool IsDedicatedFireActor(const AActor* Actor)
    {
        return Actor && Actor->GetClass()->GetName().StartsWith(TEXT("BP_AST_FireSource"));
    }
}

UASTFireReactiveComponent::UASTFireReactiveComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.04f; // 25 Hz max, volume updates LOD-throttled
    SetIsReplicatedByDefault(true);
    FireSystem = TSoftObjectPtr<UNiagaraSystem>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Systems/NS_Fire_Master.NS_Fire_Master")));
    VolumeMaterial = TSoftObjectPtr<UMaterialInterface>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Materials/M_AST_FirePixel_Volume.M_AST_FirePixel_Volume")));
    FlameMaterial = TSoftObjectPtr<UMaterialInterface>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Materials/M_AST_FirePixel_Core.M_AST_FirePixel_Core")));
}

void UASTFireReactiveComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UASTFireReactiveComponent, Intensity);
    DOREPLIFETIME(UASTFireReactiveComponent, bStartLit);
    DOREPLIFETIME(UASTFireReactiveComponent, Coverage);
    DOREPLIFETIME(UASTFireReactiveComponent, FlamePoints);
    DOREPLIFETIME(UASTFireReactiveComponent, ExtraLocalPoints);
    DOREPLIFETIME(UASTFireReactiveComponent, SimulationSeed);
}

void UASTFireReactiveComponent::OnRep_Burning()
{
    if (!bInitialized) InitializeFire();
    UpdateVisibility();
}

void UASTFireReactiveComponent::OnRep_BurnAnchors()
{
    if (bInitialized) RefreshFireVisuals();
}

void UASTFireReactiveComponent::OnRegister()
{
    Super::OnRegister();
    if (IsTemplate() || !GetOwner()) return;
    const UWorld* World = GetWorld();
    if (World && bEditorPreview &&
        (World->WorldType == EWorldType::Editor ||
         World->WorldType == EWorldType::EditorPreview))
    {
        InitializeFire(); // transient render components, not persisted in the map
    }
}

void UASTFireReactiveComponent::OnUnregister()
{
    RemoveGeneratedVisuals();
    Super::OnUnregister();
}

void UASTFireReactiveComponent::BeginPlay()
{
    Super::BeginPlay();
    bPlaying = true;
    bStartLit = bStartLit && Intensity > 0.005f;
    InitializeFire();
    if (!bStartLit) SmoothIntensity = 0.0f;
    else SmoothIntensity = Intensity;

    if (Light) Light->SetIntensity(LightPower * SmoothIntensity);
    UpdateVisibility();

    AActor* Owner = GetOwner();
    if (!Owner) return;
    LastOwnerPosition = Owner->GetActorLocation();
    LastOwnerRotation = Owner->GetActorQuat();
    LastOwnerVelocity = Owner->GetVelocity();

    InfluenceSphere = NewObject<USphereComponent>(
        Owner, NAME_None, RF_Transient);
    if (InfluenceSphere)
    {
        if (USceneComponent* Parent = FindTargetComponent())
            InfluenceSphere->SetupAttachment(Parent);
        InfluenceSphere->SetSphereRadius(InfluenceRadius);
        InfluenceSphere->SetMobility(EComponentMobility::Movable);
        InfluenceSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        InfluenceSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
        InfluenceSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
        InfluenceSphere->SetGenerateOverlapEvents(true);
        InfluenceSphere->OnComponentBeginOverlap.AddDynamic(
            this, &UASTFireReactiveComponent::OnInfluenceBegin);
        InfluenceSphere->OnComponentEndOverlap.AddDynamic(
            this, &UASTFireReactiveComponent::OnInfluenceEnd);
        InfluenceSphere->RegisterComponent();
        InfluenceSphere->SetHiddenInGame(true);
    }
}

void UASTFireReactiveComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bPlaying = false;
    NearbyPawns.Reset();
    if (InfluenceSphere)
    {
        InfluenceSphere->DestroyComponent();
        InfluenceSphere = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

USceneComponent* UASTFireReactiveComponent::FindTargetComponent() const
{
    AActor* Owner = GetOwner();
    if (!Owner) return nullptr;

    TArray<UMeshComponent*> Meshes;
    Owner->GetComponents<UMeshComponent>(Meshes);
    UMeshComponent* Best = nullptr;
    float BestRadius = -1.0f;

    for (UMeshComponent* Mesh : Meshes)
    {
        if (!IsValid(Mesh) || Mesh->IsEditorOnly() || !Mesh->IsVisible()) continue;
        if (!PreferredMeshComponentName.IsNone() &&
            Mesh->GetFName() == PreferredMeshComponentName)
            return Mesh;
        const float Radius = Mesh->Bounds.SphereRadius;
        // Prefer the largest visible mesh; works for static and skeletal geometry.
        if (Radius > BestRadius)
        {
            BestRadius = Radius;
            Best = Mesh;
        }
    }
    return Best ? static_cast<USceneComponent*>(Best) : Owner->GetRootComponent();
}

UPointLightComponent* UASTFireReactiveComponent::FindOwnedLight() const
{
    AActor* Owner = GetOwner();
    if (!Owner) return nullptr;

    TArray<UPointLightComponent*> Lights;
    Owner->GetComponents<UPointLightComponent>(Lights);
    for (UPointLightComponent* Candidate : Lights)
    {
        if (IsValid(Candidate) && Candidate->ComponentHasTag(ASTFire::FireLightTag))
            return Candidate;
    }
    if (ASTFire::IsDedicatedFireActor(Owner))
    {
        // This Blueprint's light was created exclusively for AST Fire, not the
        // original POIKit torch. Do not borrow lights from other actor types.
        for (UPointLightComponent* Candidate : Lights)
            if (IsValid(Candidate)) return Candidate;
    }
    return nullptr;
}

UNiagaraComponent* UASTFireReactiveComponent::FindExistingFireFX() const
{
    AActor* Owner = GetOwner();
    if (!Owner) return nullptr;

    TArray<UNiagaraComponent*> Candidates;
    Owner->GetComponents<UNiagaraComponent>(Candidates);
    for (UNiagaraComponent* Candidate : Candidates)
    {
        if (IsValid(Candidate) && Candidate->ComponentHasTag(ASTFire::FireFXTag))
            return Candidate;
    }
    if (ASTFire::IsDedicatedFireActor(Owner))
    {
        for (UNiagaraComponent* Candidate : Candidates)
            if (IsValid(Candidate)) return Candidate;
    }
    return nullptr;
}

void UASTFireReactiveComponent::AddFlamePoint(
    USceneComponent* Parent, FName Socket, FVector LocalPosition,
    float CardScale, UNiagaraSystem* System, UStaticMesh* PlaneMesh,
    UMaterialInterface* Material, UMaterialInterface* ProceduralMaterial,
    bool bUseExistingFX)
{
    AActor* Owner = GetOwner();
    if (!Owner || !Parent) return;
    const bool bLit = bStartLit && Intensity > 0.005f;

    if (bEnableProceduralFlames && ProceduralMaterial)
    {
        AddVolume(Parent, Socket, LocalPosition, CardScale, ProceduralMaterial);
    }

    if (bEnableFlameCards && !bEnableProceduralFlames && PlaneMesh && Material)
    {
        for (int32 Cross = 0; Cross != 2; ++Cross)
        {
            UStaticMeshComponent* Card = NewObject<UStaticMeshComponent>(
                Owner, NAME_None, RF_Transient);
            if (!Card) continue;
            Card->SetMobility(EComponentMobility::Movable);
            Card->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Card->SetGenerateOverlapEvents(false);
            Card->SetCastShadow(false);
            Card->SetStaticMesh(PlaneMesh);
            Card->SetMaterial(0, Material);
            Card->SetupAttachment(Parent, Socket);
            Card->SetRelativeLocation(LocalPosition + FVector(0, 0, 41.0f * CardScale));
            // Engine plane is XY, rotate the Y axis into vertical Z.
            Card->SetRelativeRotation(FRotator(0.0f, Cross == 0 ? 0.0f : 90.0f, 90.0f));
            Card->SetRelativeScale3D(
                FVector(0.56f, 0.90f, 1.0f) * FMath::Max(0.08f, CardScale * FlameSize));
            Card->SetVisibility(bLit);
            Card->RegisterComponent();
            FireCards.Add(Card);
        }
    }

    if (bEnableNiagara && System)
    {
        UNiagaraComponent* FX = bUseExistingFX ? FindExistingFireFX() : nullptr;
        if (!FX)
        {
            FX = NewObject<UNiagaraComponent>(Owner, NAME_None, RF_Transient);
            if (!FX) return;
            FX->ComponentTags.AddUnique(ASTFire::FireFXTag);
            FX->SetMobility(EComponentMobility::Movable);
            FX->SetAutoActivate(bLit);
            FX->SetupAttachment(Parent, Socket);
            GeneratedFX.Add(FX);
        }
        FX->SetAsset(System);
        if (bUseExistingFX)
        {
            FX->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Socket);
        }
        FX->SetRelativeLocation(LocalPosition);
        FX->SetRelativeScale3D(FVector(FMath::Max(0.10f, CardScale * FlameSize)));
        if (!FX->IsRegistered()) FX->RegisterComponent();
        if (bLit) FX->Activate(true);
        else FX->Deactivate();
        FireFX.AddUnique(FX);
    }
}


// Stage 4 - lightweight 3D procedural flame geometry. One CPU mesh section per
// burn anchor, no per-spark ticks, particle transforms or simulation-grid readbacks.
void UASTFireReactiveComponent::AddVolume(
    USceneComponent* Parent, FName Socket, FVector LocalPosition,
    float Scale, UMaterialInterface* Material)
{
    AActor* Owner = GetOwner();
    if (!Owner || !Parent || !Material) return;

    UProceduralMeshComponent* Volume = NewObject<UProceduralMeshComponent>(
        Owner, NAME_None, RF_Transient);
    if (!Volume) return;
    Volume->SetMobility(EComponentMobility::Movable);
    Volume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Volume->SetGenerateOverlapEvents(false);
    Volume->SetCastShadow(false);
    Volume->SetupAttachment(Parent, Socket);
    Volume->SetRelativeLocation(LocalPosition);
    Volume->SetMaterial(0, Material);
    Volume->RegisterComponent();
    FireVolumes.Add(Volume);
    FireVolumeScales.Add(FMath::Max(0.12f, Scale * FlameSize));
    UpdateVolume(Volume, FireVolumes.Num() - 1, true);
    Volume->SetVisibility(bStartLit && Intensity > 0.005f);
}

void UASTFireReactiveComponent::UpdateVolume(
    UProceduralMeshComponent* Volume, int32 Index, bool bCreate)
{
    if (!IsValid(Volume) || !FireVolumeScales.IsValidIndex(Index)) return;

    // Vertex budget: 5 connected and branching tongues x 8 rings x 7 sides.
    // Topology never changes, so a single mesh section is updated in-place.
    constexpr int32 Tongues = 5;
    constexpr int32 Rings = 8;
    constexpr int32 Sides = 7;
    TArray<FVector> Vertices;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    Vertices.Reserve(Tongues * Rings * Sides);
    UVs.Reserve(Tongues * Rings * Sides);
    Colors.Reserve(Tongues * Rings * Sides);
    if (bCreate) Triangles.Reserve(Tongues * (Rings - 1) * Sides * 6);

    const FTransform Transform = Volume->GetComponentTransform();
    const FVector Up = Transform.InverseTransformVectorNoScale(FVector::UpVector).GetSafeNormal();
    const FVector Reference = FMath::Abs(Up.Z) > 0.95f ? FVector::ForwardVector : FVector::UpVector;
    const FVector Right = FVector::CrossProduct(Reference, Up).GetSafeNormal();
    const FVector Forward = FVector::CrossProduct(Up, Right).GetSafeNormal();
    const FVector OwnerPos = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
    const FVector TangentialVelocity = -FVector::CrossProduct(
        SmoothedAngularVelocity, Volume->GetComponentLocation() - OwnerPos);
    const FVector Flow = Transform.InverseTransformVectorNoScale(
        (SmoothedFlow + TangentialVelocity * 0.5f).GetClampedToMaxSize(800.0f));
    const float T = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    const float Scale = FireVolumeScales[Index];
    const float Heat = FMath::Clamp(bPlaying ? SmoothIntensity : Intensity, 0.0f, 1.0f);
    const float BurnScale = 0.28f + 0.72f * Heat;

    for (int32 Tongue = 0; Tongue < Tongues; ++Tongue)
    {
        const float Phase = (float(SimulationSeed % 997) * 0.017f) +
            float(Index * 7 + Tongue) * 2.39996323f;
        const float BranchAngle = Phase + 0.16f * FMath::Sin(T * 1.9f + Phase);
        const FVector Branch = Tongue == 0 ? FVector::ZeroVector :
            (Right * FMath::Cos(BranchAngle) + Forward * FMath::Sin(BranchAngle)) *
            (12.0f + float(Tongue % 2) * 6.0f) * Scale;
        const float HeightVariation = 0.93f + 0.07f * FMath::Sin(
            T * (2.1f + 0.29f * Tongue) + Phase);
        const float Height = (Tongue == 0 ? 145.0f :
            86.0f + float((Tongue * 13 + Index * 3) % 5) * 14.0f) *
            Scale * BurnScale * HeightVariation;
        const float Radius = (Tongue == 0 ? 26.0f : 14.0f) * Scale *
            FMath::Sqrt(BurnScale);

        for (int32 Ring = 0; Ring < Rings; ++Ring)
        {
            const float H = float(Ring) / float(Rings - 1);
            const float Sway = H * H;
            const float Bend = 7.0f * Scale * Sway;
            const FVector Eddy = Bend * (
                Right * FMath::Sin(Phase + T * 3.1f + H * 7.0f) +
                Forward * FMath::Cos(Phase * 0.73f + T * 2.4f + H * 9.0f));
            const FVector Center = Branch * (1.0f - 0.28f * H) +
                Up * Height * H + Flow * (0.22f * Sway) + Eddy;
            const float Taper = FMath::Max(0.025f,
                FMath::Pow(1.0f - H, 0.82f) * (0.72f + 0.28f * FMath::Sin(PI * H)));
            const float RingRadius = Radius * Taper *
                (0.94f + 0.06f * FMath::Sin(T * 5.2f + Phase + H * 14.0f));

            for (int32 Side = 0; Side < Sides; ++Side)
            {
                const float A = 2.0f * PI * float(Side) / float(Sides);
                const FVector Radial = Right * FMath::Cos(A) + Forward * FMath::Sin(A);
                const float Jagged = 1.0f + 0.075f *
                    FMath::Sin(A * 3.0f + Phase * 2.0f + H * 11.0f + T * 4.0f);
                Vertices.Add(Center + Radial * RingRadius * Jagged);
                UVs.Add(FVector2D(float(Side) / float(Sides), H));
                const float Temperature = FMath::Clamp(
                    0.96f - H * 0.51f - float(Tongue > 0) * 0.11f +
                    0.11f * FMath::Sin(Phase + T * 3.4f + H * 12.0f), 0.0f, 1.0f);
                Colors.Add(FLinearColor(Temperature, H, float(Tongue) / Tongues, 1.0f));

                if (bCreate && Ring < Rings - 1)
                {
                    const int32 Here = (Tongue * Rings + Ring) * Sides + Side;
                    const int32 NextSide = (Tongue * Rings + Ring) * Sides +
                        (Side + 1) % Sides;
                    const int32 Above = Here + Sides;
                    const int32 AboveNext = NextSide + Sides;
                    Triangles.Add(Here); Triangles.Add(NextSide); Triangles.Add(Above);
                    Triangles.Add(NextSide); Triangles.Add(AboveNext); Triangles.Add(Above);
                }
            }
        }
    }

    if (bCreate)
    {
        Volume->CreateMeshSection_LinearColor(
            0, Vertices, Triangles, Normals, UVs, Colors, Tangents, false);
    }
    else
    {
        Volume->UpdateMeshSection_LinearColor(
            0, Vertices, Normals, UVs, Colors, Tangents);
    }
}

void UASTFireReactiveComponent::InitializeFire()
{
    if (bInitialized || IsTemplate() || !GetOwner()) return;
    AActor* Owner = GetOwner();
    // Dedicated servers own combustion state but never allocate visual mesh/light/FX.
    if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
    {
        bInitialized = true;
        return;
    }
    USceneComponent* Parent = FindTargetComponent();
    if (!Parent) return;

    // No dependence on user-provided torch/character materials or project mesh assets.
    UStaticMesh* PlaneMesh = bEnableFlameCards && !bEnableProceduralFlames
        ? LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"))
        : nullptr;
    UMaterialInterface* Material = bEnableFlameCards && !bEnableProceduralFlames
        ? FlameMaterial.LoadSynchronous() : nullptr;
    UMaterialInterface* ProceduralMaterial = bEnableProceduralFlames
        ? VolumeMaterial.LoadSynchronous() : nullptr;
    if (bEnableProceduralFlames && !ProceduralMaterial)
        UE_LOG(LogTemp, Error, TEXT("AST Fire: M_AST_FirePixel_Volume missing; run setup_stage4_volume.py"));
    UNiagaraSystem* System = bEnableNiagara
        ? FireSystem.LoadSynchronous() : nullptr;

    if (bEnableFlameCards && !bEnableProceduralFlames && (!PlaneMesh || !Material))
    {
        UE_LOG(LogTemp, Error, TEXT("AST Fire: original flame material/engine plane unavailable for %s"),
            *Owner->GetName());
    }
    if (bEnableNiagara && !System)
    {
        UE_LOG(LogTemp, Error, TEXT("AST Fire: NS_Fire_Master unavailable for %s"),
            *Owner->GetName());
    }

    Light = FindOwnedLight();
    if (!Light)
    {
        GeneratedLight = NewObject<UPointLightComponent>(
            Owner, NAME_None, RF_Transient);
        if (GeneratedLight)
        {
            GeneratedLight->ComponentTags.AddUnique(ASTFire::FireLightTag);
            GeneratedLight->SetMobility(EComponentMobility::Movable);
            GeneratedLight->SetCastShadows(false);
            GeneratedLight->SetupAttachment(Parent);
            GeneratedLight->RegisterComponent();
            Light = GeneratedLight;
        }
    }
    if (Light)
    {
        Light->SetMobility(EComponentMobility::Movable);
        Light->SetAttenuationRadius(LightRadius);
        Light->SetLightColor(FLinearColor(1.0f, 0.42f, 0.21f));
    }

    struct FFirePoint { FName Socket; FVector Offset; float Size; };
    TArray<FFirePoint> Points;
    const int32 Count = FMath::Clamp(FlamePoints, 1, 16);

    if (Coverage == EASTFireCoverage::Sockets && FlameSockets.Num())
    {
        for (FName Socket : FlameSockets)
        {
            if (Points.Num() >= Count) break;
            if (Parent->DoesSocketExist(Socket))
                Points.Add({Socket, FVector::ZeroVector, 0.8f});
        }
    }

    if (Coverage == EASTFireCoverage::Bounds && Points.IsEmpty())
    {
        UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Parent);
        const FBoxSphereBounds LocalBounds = Primitive
            ? Primitive->CalcBounds(FTransform::Identity)
            : FBoxSphereBounds(FVector::ZeroVector, FVector(45.0f), 80.0f);
        const FVector Extent = LocalBounds.BoxExtent;
        const float Height = FMath::Max(30.0f, Extent.Z * 2.0f);
        const float Size = FMath::Clamp(Height / (Count > 2 ? 300.0f : 190.0f), 0.45f, 3.0f);
        for (int32 i = 0; i < Count; ++i)
        {
            const float Fraction = (float(i) + 0.5f) / float(Count);
            const float Angle = i * 2.39996323f; // golden angle, deterministic placement
            const FVector Offset = LocalBounds.Origin + FVector(
                FMath::Cos(Angle) * Extent.X * 0.67f,
                FMath::Sin(Angle) * Extent.Y * 0.67f,
                (Fraction * 1.45f - 0.65f) * Extent.Z);
            Points.Add({NAME_None, Offset, Size});
        }
    }
    if (Points.IsEmpty())
        Points.Add({NAME_None, PointOffset, 1.0f});

    for (const FVector_NetQuantize10& HitOffset : ExtraLocalPoints)
    {
        if (Points.Num() >= 16) break;
        Points.Add({NAME_None, HitOffset, 0.72f});
    }
    for (int32 i = 0; i < Points.Num(); ++i)
    {
        AddFlamePoint(Parent, Points[i].Socket, Points[i].Offset,
            Points[i].Size, System, PlaneMesh, Material, ProceduralMaterial, i == 0);
    }

    bInitialized = true;
    UpdateVisibility();
}

void UASTFireReactiveComponent::RemoveGeneratedVisuals()
{
    for (UNiagaraComponent* FX : GeneratedFX)
        if (IsValid(FX)) FX->DestroyComponent();
    GeneratedFX.Empty();
    FireFX.Empty();

    for (UStaticMeshComponent* Card : FireCards)
        if (IsValid(Card)) Card->DestroyComponent();
    FireCards.Empty();

    for (UProceduralMeshComponent* Volume : FireVolumes)
        if (IsValid(Volume)) Volume->DestroyComponent();
    FireVolumes.Empty();
    FireVolumeScales.Empty();
    VolumeUpdateDebt = 0.0f;

    if (IsValid(GeneratedLight))
        GeneratedLight->DestroyComponent();
    GeneratedLight = nullptr;
    Light = nullptr;
    bInitialized = false;
}

void UASTFireReactiveComponent::RefreshFireVisuals()
{
    RemoveGeneratedVisuals();
    InitializeFire();
}

void UASTFireReactiveComponent::SetFireIntensity(float NewIntensity)
{
    if (bPlaying && GetOwner() && GetOwner()->GetNetMode() == NM_Client)
        return; // Fire gameplay state is server authoritative.
    Intensity = FMath::Clamp(NewIntensity, 0.0f, 1.0f);
    bStartLit = Intensity > 0.005f;
    UpdateVisibility();
}

void UASTFireReactiveComponent::Ignite(float NewIntensity)
{
    SetFireIntensity(FMath::Max(0.05f, NewIntensity));
    if (!bInitialized) InitializeFire();
}

void UASTFireReactiveComponent::Extinguish()
{
    SetFireIntensity(0.0f);
}

void UASTFireReactiveComponent::SetFireCoverage(
    EASTFireCoverage NewCoverage, int32 NewFlamePoints)
{
    if (bPlaying && GetOwner() && GetOwner()->GetNetMode() == NM_Client)
        return;
    Coverage = NewCoverage;
    FlamePoints = FMath::Clamp(NewFlamePoints, 1, 16);
    RefreshFireVisuals();
}

void UASTFireReactiveComponent::IgniteAtWorldPosition(FVector WorldPosition)
{
    if (bPlaying && GetOwner() && GetOwner()->GetNetMode() == NM_Client)
        return;
    if (USceneComponent* Parent = FindTargetComponent())
    {
        if (ExtraLocalPoints.Num() >= 15) ExtraLocalPoints.RemoveAt(0);
        ExtraLocalPoints.Add(Parent->GetComponentTransform()
            .InverseTransformPosition(WorldPosition));
        if (!bStartLit) Ignite(1.0f);
        RefreshFireVisuals();
    }
}

void UASTFireReactiveComponent::UpdateVisibility()
{
    const bool bLit = bStartLit && Intensity > 0.005f;
    for (UStaticMeshComponent* Card : FireCards)
        if (IsValid(Card)) Card->SetVisibility(bLit);
    for (UProceduralMeshComponent* Volume : FireVolumes)
        if (IsValid(Volume)) Volume->SetVisibility(bLit);
    for (UNiagaraComponent* FX : FireFX)
    {
        if (!IsValid(FX)) continue;
        FX->SetVisibility(bLit);
        if (bLit && !FX->IsActive()) FX->Activate(true);
        if (!bLit && FX->IsActive()) FX->Deactivate();
    }
    if (IsValid(Light))
    {
        Light->SetVisibility(bLit);
        if (!bLit) Light->SetIntensity(0.0f);
        else if (!bPlaying) Light->SetIntensity(LightPower * Intensity);
    }
}

void UASTFireReactiveComponent::OnInfluenceBegin(
    UPrimitiveComponent* Overlapped, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
    bool bFromSweep, const FHitResult& SweepResult)
{
    if (APawn* Pawn = Cast<APawn>(OtherActor))
        NearbyPawns.Add(Pawn);
}

void UASTFireReactiveComponent::OnInfluenceEnd(
    UPrimitiveComponent* Overlapped, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
    if (APawn* Pawn = Cast<APawn>(OtherActor))
        NearbyPawns.Remove(Pawn);
}

void UASTFireReactiveComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bPlaying || !GetOwner() || !GetWorld() || DeltaTime <= KINDA_SMALL_NUMBER)
        return;
    if (!bInitialized) InitializeFire();

    const FVector Position = GetOwner()->GetActorLocation();
    const FVector Velocity = (Position - LastOwnerPosition) / DeltaTime;
    LastOwnerPosition = Position;
    const FVector Acceleration = (Velocity - LastOwnerVelocity) / DeltaTime;
    LastOwnerVelocity = Velocity;

    const FQuat CurrentRotation = GetOwner()->GetActorQuat();
    const FQuat RotationDelta = CurrentRotation * LastOwnerRotation.Inverse();
    LastOwnerRotation = CurrentRotation;
    FVector RotationAxis = FVector::UpVector;
    float RotationAngle = 0.0f;
    RotationDelta.ToAxisAndAngle(RotationAxis, RotationAngle);
    if (RotationAngle > PI) RotationAngle -= 2.0f * PI;
    const FVector AngularVelocity = RotationAxis.GetSafeNormal() *
        FMath::Clamp(RotationAngle / DeltaTime, -12.0f, 12.0f);

    float LocalDisturbance = 0.0f;
    FVector LocalWind = FVector::ZeroVector;
    for (auto It = NearbyPawns.CreateIterator(); It; ++It)
    {
        APawn* Pawn = It->Get();
        if (!IsValid(Pawn))
        {
            It.RemoveCurrent();
            continue;
        }
        const float Distance = FVector::Dist(Position, Pawn->GetActorLocation());
        if (Distance >= InfluenceRadius) continue;
        const FVector PawnVelocity = Pawn->GetVelocity();
        const float Strength =
            (1.0f - Distance / FMath::Max(InfluenceRadius, 1.0f)) *
            FMath::Clamp(PawnVelocity.Size() / 650.0f, 0.0f, 1.0f);
        if (Strength > LocalDisturbance)
        {
            LocalDisturbance = Strength;
            LocalWind = PawnVelocity;
        }
    }

    Disturbance = FMath::FInterpTo(
        Disturbance, LocalDisturbance, DeltaTime,
        LocalDisturbance > Disturbance ? 10.0f : 3.0f);
    const float TargetIntensity = bStartLit ? Intensity : 0.0f;
    SmoothIntensity = FMath::FInterpTo(
        SmoothIntensity, TargetIntensity, DeltaTime, 6.0f);

    const float Time = GetWorld()->GetTimeSeconds();
    const float Pulse = 0.85f + 0.09f * FMath::Sin(Time * 4.4f)
        + 0.045f * FMath::Sin(Time * 12.8f)
        + 0.025f * FMath::Sin(Time * 18.9f);
    const float Movement = FMath::Clamp(Velocity.Size() / 600.0f, 0.0f, 1.0f);
    const float Response = FMath::Max(Movement, Disturbance);
    const FVector Wind = -Velocity * 0.48f - Acceleration.GetClampedToMaxSize(5000.0f) * 0.025f
        + LocalWind * 0.18f + AmbientWind;
    const float ResponseRate = Response > 0.1f ? 5.0f : 2.5f;
    SmoothedFlow = FMath::VInterpTo(SmoothedFlow, Wind.GetClampedToMaxSize(800.0f),
        DeltaTime, ResponseRate);
    SmoothedAngularVelocity = FMath::VInterpTo(
        SmoothedAngularVelocity, AngularVelocity, DeltaTime, 4.0f);
    const FVector LocalWindVec = GetOwner()->GetActorTransform()
        .InverseTransformVectorNoScale(Wind);

    if (IsValid(Light))
    {
        Light->SetIntensity(LightPower *
            FMath::Max(0.0f, SmoothIntensity * (Pulse + Response * 0.14f)));
        Light->SetAttenuationRadius(LightRadius * (0.92f + Pulse * 0.10f));
        Light->SetLightColor(FLinearColor(1.0f, 0.41f + Pulse * 0.07f, 0.20f));
    }

    const float Lean = FMath::Clamp(
        LocalWindVec.Size2D() / 600.0f, 0.0f, 1.0f) * MaxLeanDegrees;
    for (UNiagaraComponent* FX : FireFX)
    {
        if (!IsValid(FX)) continue;
        const FRotator Wanted(
            -FMath::Clamp(LocalWindVec.X / 600.0f, -1.0f, 1.0f) * Lean,
            0.0f,
            FMath::Clamp(LocalWindVec.Y / 600.0f, -1.0f, 1.0f) * Lean);
        FX->SetRelativeRotation(FMath::RInterpTo(
            FX->GetRelativeRotation(), Wanted, DeltaTime, 7.0f));
        FX->SetVariableFloat(FName(TEXT("User.FireIntensity")), SmoothIntensity);
        FX->SetVariableFloat(FName(TEXT("User.DisturbanceStrength")), Response);
        FX->SetVariableFloat(FName(TEXT("User.FlamePulse")), Pulse);
        FX->SetVariableVec3(FName(TEXT("User.WindVector")), SmoothedFlow);
    }

    // Volume has its own deformable geometry; these parameters affect actual vertices,
    // not merely the rotation of a billboard or an unconnected Niagara User variable.
    if (FireVolumes.Num() && SmoothIntensity > 0.004f)
    {
        float DistanceSquared = 0.0f;
        if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
            if (const APlayerCameraManager* Camera = PC->PlayerCameraManager)
                DistanceSquared = FVector::DistSquared(Position, Camera->GetCameraLocation());
        const float UpdateInterval = DistanceSquared > FMath::Square(9000.0f) ? 0.40f :
            (DistanceSquared > FMath::Square(3500.0f) ? 0.16f : 0.04f);
        VolumeUpdateDebt += DeltaTime;
        if (VolumeUpdateDebt >= UpdateInterval)
        {
            VolumeUpdateDebt = 0.0f;
            for (int32 i = 0; i < FireVolumes.Num(); ++i)
                if (IsValid(FireVolumes[i])) UpdateVolume(FireVolumes[i], i, false);
        }
    }
}
