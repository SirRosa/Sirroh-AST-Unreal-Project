#include "ASTFireReactiveComponent.h"

#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
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
    PrimaryComponentTick.TickInterval = 0.04f; // 25 Hz regardless of flame count
    FireSystem = TSoftObjectPtr<UNiagaraSystem>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Systems/NS_Fire_Master.NS_Fire_Master")));
    FlameMaterial = TSoftObjectPtr<UMaterialInterface>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Materials/M_AST_FirePixel_Core.M_AST_FirePixel_Core")));
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
    UMaterialInterface* Material, bool bUseExistingFX)
{
    AActor* Owner = GetOwner();
    if (!Owner || !Parent) return;
    const bool bLit = bStartLit && Intensity > 0.005f;

    if (bEnableFlameCards && PlaneMesh && Material)
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

void UASTFireReactiveComponent::InitializeFire()
{
    if (bInitialized || IsTemplate() || !GetOwner()) return;
    AActor* Owner = GetOwner();
    USceneComponent* Parent = FindTargetComponent();
    if (!Parent) return;

    // No dependence on user-provided torch/character materials or project mesh assets.
    UStaticMesh* PlaneMesh = bEnableFlameCards
        ? LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"))
        : nullptr;
    UMaterialInterface* Material = bEnableFlameCards
        ? FlameMaterial.LoadSynchronous() : nullptr;
    UNiagaraSystem* System = bEnableNiagara
        ? FireSystem.LoadSynchronous() : nullptr;

    if (bEnableFlameCards && (!PlaneMesh || !Material))
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

    for (const FVector& HitOffset : ExtraLocalPoints)
    {
        if (Points.Num() >= 16) break;
        Points.Add({NAME_None, HitOffset, 0.72f});
    }
    for (int32 i = 0; i < Points.Num(); ++i)
    {
        AddFlamePoint(Parent, Points[i].Socket, Points[i].Offset,
            Points[i].Size, System, PlaneMesh, Material, i == 0);
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
    Coverage = NewCoverage;
    FlamePoints = FMath::Clamp(NewFlamePoints, 1, 16);
    RefreshFireVisuals();
}

void UASTFireReactiveComponent::IgniteAtWorldPosition(FVector WorldPosition)
{
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
    const FVector Wind = -Velocity * 0.4f + LocalWind * 0.18f;
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
        FX->SetVariableVec3(FName(TEXT("User.WindVector")), Wind);
    }
}
