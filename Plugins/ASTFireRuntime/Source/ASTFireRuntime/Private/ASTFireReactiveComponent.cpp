#include "ASTFireReactiveComponent.h"
#include "ASTFireFreeFlameComponent.h"

#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInstanceDynamic.h"
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
    // Bounds are used to generate candidate rays, not as the flame's final
    // surface. A triangle/physics collision hit puts the anchor on the mesh.
    // When a mesh has no collision there is an explicit bounds fallback.
    bool ProjectToSurface(UPrimitiveComponent* Mesh,
        const FVector& OutsideWorld, const FVector& InsideWorld,
        FVector& SurfaceWorld)
    {
        if (!IsValid(Mesh) || Mesh->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
            return false;
        FHitResult Hit;
        FCollisionQueryParams Params;
        Params.bTraceComplex = true;
        Params.bReturnPhysicalMaterial = false;
        if (!Mesh->LineTraceComponent(Hit, OutsideWorld, InsideWorld, Params))
            return false;
        const FVector Normal = FVector(
            Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z);
        SurfaceWorld = FVector(
            Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z) +
            Normal.GetSafeNormal() * 2.0;
        return true;
    }

    bool ValidFlameBone(const FName Name)
    {
        if (Name.IsNone()) return false;
        const FString Key = Name.ToString().ToLower();
        return !Key.Contains(TEXT("ik_")) && !Key.Contains(TEXT("twist"))
            && !Key.Contains(TEXT("weapon")) && !Key.Contains(TEXT("socket"))
            && !Key.Contains(TEXT("virtual")) && !Key.Contains(TEXT("cloth"))
            && !Key.Contains(TEXT("vb_")) && !Key.Contains(TEXT("_end"))
            && !Key.Equals(TEXT("root"));
    }
    static bool IsDedicatedFireActor(const AActor* Actor)
    {
        return Actor && Actor->GetClass()->GetName().StartsWith(TEXT("BP_AST_FireSource"));
    }
}

UASTFireReactiveComponent::UASTFireReactiveComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.04f; // 25 Hz max, volume updates LOD-throttled
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork; // sample bones after skeletal animation
    SetIsReplicatedByDefault(true);
    FireSystem = TSoftObjectPtr<UNiagaraSystem>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Systems/NS_Fire_Master.NS_Fire_Master")));
    VolumeMaterial = TSoftObjectPtr<UMaterialInterface>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Materials/M_AST_FireTongue.M_AST_FireTongue")));
    SurfaceMaterial = TSoftObjectPtr<UMaterialInterface>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Materials/M_AST_FireSurfaceLife.M_AST_FireSurfaceLife")));
    CharMaterial = TSoftObjectPtr<UMaterialInterface>(
        FSoftObjectPath(TEXT("/Game/VFX/Fire/Materials/M_AST_FireChar.M_AST_FireChar")));
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
    DOREPLIFETIME(UASTFireReactiveComponent, CharProgress);
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
    SmoothIntensity = bStartLit ? Intensity : 0.0f;
    BurnElapsedSeconds = 0.0f;
    BurnDurationSeconds = ComputeBurnDuration();
    InitialBurnIntensity = Intensity;
    InitializeFire();

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
    LastNearbyPawnPositions.Reset();
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
    USkeletalMeshComponent* BestSkeleton = nullptr;
    float SkeletonRadius = -1.f;

    for (UMeshComponent* Mesh : Meshes)
    {
        if (!IsValid(Mesh) || Mesh->IsEditorOnly() || !Mesh->IsVisible()) continue;
        if (!PreferredMeshComponentName.IsNone() &&
            Mesh->GetFName() == PreferredMeshComponentName)
            return Mesh;
        const float Radius = Mesh->Bounds.SphereRadius;
        // The torso's skinned mesh is the correct burnable surface. A backpack
        // or attached static weapon may have larger bounds than the character.
        if (USkeletalMeshComponent* Skeleton=Cast<USkeletalMeshComponent>(Mesh))
        {
            if (Radius > SkeletonRadius)
            {
                BestSkeleton=Skeleton;
                SkeletonRadius=Radius;
            }
        }
        if (Radius > BestRadius)
        {
            BestRadius = Radius;
            Best = Mesh;
        }
    }
    return BestSkeleton ? static_cast<USceneComponent*>(BestSkeleton)
        : (Best ? static_cast<USceneComponent*>(Best) : Owner->GetRootComponent());
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

    if (bEnableProceduralFlames && bUseLegacyHeatGrid && ProceduralMaterial)
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



// Stage 5: advected 2D heat field projected onto a 3D, spatially anchored mesh.
// Nothing here simulates combustion for gameplay. All temperatures and fragments
// are expendable local visual data; network authority remains in burning state.
namespace ASTHeat
{
    constexpr int32 Width = 22;
    constexpr int32 Height = 30;
    constexpr int32 FragmentCount = 4;
    constexpr int32 Cells = Width * Height;
    constexpr float Threshold = 0.21f;

    float HashWave(float X)
    {
        return FMath::Sin(X * 1.932f) * 0.55f +
            FMath::Sin(X * 3.117f + 1.4f) * 0.30f;
    }

    float Read(const TArray<float>& Data, float X, float Y)
    {
        if (X < 0.0f || Y < 0.0f ||
            X >= float(Width - 1) || Y >= float(Height - 1))
            return 0.0f;
        const int32 X0 = FMath::FloorToInt(X), Y0 = FMath::FloorToInt(Y);
        const float AX = X - float(X0), AY = Y - float(Y0);
        const int32 I = Y0 * Width + X0;
        return FMath::Lerp(
            FMath::Lerp(Data[I], Data[I + 1], AX),
            FMath::Lerp(Data[I + Width], Data[I + Width + 1], AX), AY);
    }

    float Source(float X, float Time, float Seed)
    {
        const float U = (X + 0.5f) / float(Width) * 2.0f - 1.0f;
        const float Center = 0.09f * FMath::Sin(Time * 2.3f + Seed);
        const float Left = -0.36f + 0.12f * FMath::Sin(Time * 3.1f + Seed * 1.37f);
        const float Right = 0.32f + 0.14f * FMath::Sin(Time * 2.7f + Seed * 0.79f);
        const float Body = FMath::Exp(-FMath::Square((U - Center) / 0.49f));
        const float LobeL = 0.82f * FMath::Exp(-FMath::Square((U - Left) / 0.22f));
        const float LobeR = 0.73f * FMath::Exp(-FMath::Square((U - Right) / 0.23f));
        return FMath::Clamp(FMath::Max3(Body, LobeL, LobeR), 0.0f, 1.0f);
    }

    // Fill the editor preview immediately: a broad base, several unequal peaks
    // and small detached pockets. Runtime then evolves the SAME field by advection.
    void Prewarm(FASTLocalFlameField& Field, int32 Seed)
    {
        Field.Temperature.SetNumZeroed(Cells);
        Field.Work.SetNumZeroed(Cells);
        Field.Fragments.SetNum(FragmentCount);
        const float Phase = float(Seed % 1009) * 0.0137f;
        for (int32 Y = 0; Y < Height; ++Y)
        {
            const float V = (float(Y) + 0.5f) / float(Height);
            for (int32 X = 0; X < Width; ++X)
            {
                const float U = (float(X) + 0.5f) / float(Width) * 2.0f - 1.0f;
                const float Drift = 0.105f * FMath::Sin(V * 11.0f + Phase) +
                    0.048f * FMath::Sin(V * 26.0f - Phase);
                const float BaseWidth = 0.76f *
                    FMath::Pow(FMath::Max(0.0f, 1.0f - V / 0.77f), 0.55f);
                const float Base = FMath::Clamp(
                    (BaseWidth - FMath::Abs(U - Drift)) * 7.0f, 0.0f, 1.0f) *
                    (0.94f - V * 0.49f);
                float Tips = 0.0f;
                for (int32 B = 0; B < 3; ++B)
                {
                    const float MaxHeight = B == 0 ? 0.96f : (B == 1 ? 0.74f : 0.86f);
                    const float Center = (B == 0 ? -0.09f : (B == 1 ? -0.36f : 0.33f)) +
                        0.15f * FMath::Sin(V * (9.0f + float(B)) +
                            Phase + float(B) * 1.8f);
                    const float HalfWidth = 0.19f * (1.0f - 0.68f * V);
                    const float Edge = FMath::Clamp(
                        (HalfWidth - FMath::Abs(U - Center)) * 11.0f, 0.0f, 1.0f);
                    const float Cap = FMath::Clamp(
                        (MaxHeight - V) * 11.0f, 0.0f, 1.0f);
                    Tips = FMath::Max(Tips, Edge * Cap * (0.72f - V * 0.31f));
                }
                Field.Temperature[Y * Width + X] = FMath::Max(Base, Tips);
            }
        }
        for (int32 K = 0; K < 3; ++K)
        {
            FASTFlameFragment& Part = Field.Fragments[K];
            Part.X = float(Width) * (0.28f + K * 0.19f);
            Part.Y = float(Height) * (0.77f + K * 0.075f);
            Part.VX = float(K - 1) * 2.5f;
            Part.VY = 7.0f + K * 1.4f;
            Part.Life = 0.32f + K * 0.11f;
            Part.Heat = 0.55f;
        }
        Field.bReady = true;
    }

    // This envelope affects COVERAGE, not just color. Without it the last
    // rows of the heat grid are hard-clipped into horizontal top/bottom edges.
    // Unequal drifting peaks form a finite but irregular upper silhouette.
    float ShapedCoverage(int32 X, int32 Y, float Time, float Seed)
    {
        const float U = (float(X) + 0.5f) / float(Width) * 2.0f - 1.0f;
        const float V = (float(Y) + 0.5f) / float(Height);
        const float PeakCenter = 0.07f + 0.115f * FMath::Sin(Time * 1.75f + Seed);
        const float PeakLeft = -0.38f +
            0.12f * FMath::Sin(Time * 2.03f + Seed * 0.71f);
        const float PeakRight = 0.34f +
            0.13f * FMath::Cos(Time * 1.51f + Seed * 1.21f);
        const float CentralTip = (0.64f + 0.27f *
            FMath::Exp(-FMath::Square((U - PeakCenter) / 0.18f)));
        const float LeftTip = 0.56f + 0.25f *
            FMath::Exp(-FMath::Square((U - PeakLeft) / 0.15f));
        const float RightTip = 0.58f + 0.26f *
            FMath::Exp(-FMath::Square((U - PeakRight) / 0.16f));
        const float TipNoise = 0.025f * FMath::Sin(U * 19.0f + Time * 3.1f + Seed)
            + 0.014f * FMath::Sin(U * 31.0f - Time * 2.2f + Seed * 0.43f);
        const float TipY = FMath::Clamp(
            FMath::Max3(CentralTip, LeftTip, RightTip) + TipNoise, 0.52f, 0.96f);

        // Bottom is lowest in the hot centre and retreats around irregular
        // side tongues. Unlike a rectangular tile strip it has no uniform row.
        const float RootNoise = 0.019f *
            FMath::Sin(U * 13.0f + Time * 2.5f + Seed) +
            0.013f * FMath::Sin(U * 23.0f - Time * 1.3f + Seed * 0.8f);
        const float RootY = FMath::Clamp(
            0.014f + 0.082f * FMath::Pow(FMath::Abs(U), 1.4f) + RootNoise,
            0.0f, 0.15f);

        const float Bottom = FMath::Clamp(
            (V - RootY) * float(Height) * 1.8f, 0.0f, 1.0f);
        const float Top = FMath::Clamp(
            (TipY - V) * float(Height) * 0.64f, 0.0f, 1.0f);
        // Narrow higher levels, with moving asymmetry. Keep a broad, hot root.
        const float CenterDrift = 0.085f * FMath::Sin(Time * 1.9f + V * 6.0f + Seed);
        const float EdgeWidth = 0.82f - 0.39f * V +
            0.035f * FMath::Sin(Time * 2.3f + V * 17.0f);
        const float Side = FMath::Clamp(
            (EdgeWidth - FMath::Abs(U - CenterDrift)) * 11.0f, 0.0f, 1.0f);
        return Bottom * Top * Side;
    }

    void AddQuad(
        TArray<FVector>& Vertices, TArray<int32>& Triangles,
        TArray<FVector2D>& UV, TArray<FLinearColor>& Colors, bool bCreate,
        const FTransform& Transform, const FVector& Center,
        const FVector& Right, const FVector& Up,
        float HalfX, float HalfY, float Heat, float LocalHeight)
    {
        const int32 Start = Vertices.Num();
        const FVector A = Center - Right * HalfX - Up * HalfY;
        const FVector B = Center + Right * HalfX - Up * HalfY;
        const FVector C = Center + Right * HalfX + Up * HalfY;
        const FVector D = Center - Right * HalfX + Up * HalfY;
        Vertices.Add(Transform.InverseTransformPosition(A));
        Vertices.Add(Transform.InverseTransformPosition(B));
        Vertices.Add(Transform.InverseTransformPosition(C));
        Vertices.Add(Transform.InverseTransformPosition(D));
        for (int32 K = 0; K < 4; ++K)
        {
            Colors.Add(FLinearColor(Heat, LocalHeight, 0.0f, 1.0f));
        }
        UV.Add(FVector2D(0.0f, 1.0f));
        UV.Add(FVector2D(1.0f, 1.0f));
        UV.Add(FVector2D(1.0f, 0.0f));
        UV.Add(FVector2D(0.0f, 0.0f));
        if (bCreate)
        {
            Triangles.Add(Start);     Triangles.Add(Start + 1); Triangles.Add(Start + 2);
            Triangles.Add(Start);     Triangles.Add(Start + 2); Triangles.Add(Start + 3);
        }
    }
}

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
    // The exact animated surface provides the hot root. The buoyant shapes
    // extend it, but may never replace it with a character-sized billboard.
    FireVolumeScales.Add(FMath::Clamp(Scale * FlameSize *
        (IsValid(FireSurfaceMID) ? 0.46f : 0.72f), 0.09f, 1.10f));
    const int32 Index = FireVolumes.Num() - 1;
    FASTLocalFlameField& Field = LocalFields.AddDefaulted_GetRef();
    ASTHeat::Prewarm(Field, SimulationSeed + Index * 5381);
    Field.LastAnchorLocation = Volume->GetComponentLocation();
    UpdateVolume(Volume, Index, true);
    Volume->SetVisibility(bStartLit && Intensity > 0.005f);
}

void UASTFireReactiveComponent::UpdateVolume(
    UProceduralMeshComponent* Volume, int32 Index, bool bCreate, float StepSeconds)
{
    if (!IsValid(Volume) || !FireVolumeScales.IsValidIndex(Index) ||
        !LocalFields.IsValidIndex(Index))
        return;
    FASTLocalFlameField& Field = LocalFields[Index];
    if (!Field.bReady) return;

    const float Scale = FireVolumeScales[Index];
    const float Heat = FMath::Clamp(
        bPlaying ? SmoothIntensity : Intensity, 0.0f, 1.0f);
    const float Size = 0.42f + 0.58f * Heat;
    const float RealWidth = 67.0f * Scale * Size;
    const float RealHeight = 118.0f * Scale * Size;
    const FVector Origin = Volume->GetComponentLocation();
    const FTransform Transform = Volume->GetComponentTransform();
    const FVector WorldUp = FVector::UpVector;

    // Camera-facing sampling plane, but PHYSICAL deformation happens in the
    // scalar field and world-space wind before the projection.
    FVector ViewDirection = GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
    if (const UWorld* World = GetWorld())
    {
        if (const APlayerController* PC = World->GetFirstPlayerController())
            if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
                ViewDirection = Cam->GetCameraLocation() - Origin;
    }
    ViewDirection.Z = 0.0f;
    ViewDirection = ViewDirection.GetSafeNormal();
    if (ViewDirection.IsNearlyZero()) ViewDirection = FVector::ForwardVector;
    const FVector Right = FVector::CrossProduct(WorldUp, ViewDirection).GetSafeNormal();

    if (!bCreate && StepSeconds > 0.0f)
    {
        const float Dt = FMath::Clamp(StepSeconds, 0.005f, 0.4f);
        Field.Time += Dt;
        const float Seed = float((SimulationSeed + Index * 5381) % 1009) * 0.0137f;

        // React to motion of the attachment/socket, including skeletal motion.
        const FVector AnchorVelocity = (Origin - Field.LastAnchorLocation) / Dt;
        const FVector AnchorAcceleration = (AnchorVelocity - Field.LastAnchorVelocity) / Dt;
        Field.LastAnchorVelocity = AnchorVelocity;
        Field.LastAnchorLocation = Origin;
        const FVector LocalFlow = (SmoothedFlow - AnchorVelocity * 1.05f
            - AnchorAcceleration.GetClampedToMaxSize(6500.0f) * 0.035f)
            .GetClampedToMaxSize(1800.0f);
        const float Wind = FMath::Clamp(
            FVector::DotProduct(LocalFlow, Right) / 265.0f, -3.25f, 3.25f);
        const float DepthGust = FMath::Clamp(
            FMath::Abs(FVector::DotProduct(LocalFlow, ViewDirection)) / 550.0f,
            0.0f, 2.4f);
        const float Gust = FMath::Clamp(LocalFlow.Size() / 850.0f, 0.0f, 2.0f);
        Field.CurrentGust = Gust;
        const float Breath = FMath::Clamp(
            0.58f + 0.30f * FMath::Sin(Field.Time * 5.6f + Seed)
            + 0.24f * FMath::Sin(Field.Time * 9.9f + Seed * 1.71f)
            + 0.12f * FMath::Sin(Field.Time * 2.3f + Seed * 0.37f),
            0.08f, 1.24f);
        Field.Breath = FMath::FInterpTo(Field.Breath, Breath, Dt, 10.0f);

        // Semi-Lagrangian advection of heat: upward buoyancy + moving eddies.
        // Simulates temperature distribution rather than deforming fixed tubes.
        for (int32 Y = 0; Y < ASTHeat::Height; ++Y)
        {
            const float V = (float(Y) + 0.5f) / float(ASTHeat::Height);
            const float Cooling = FMath::Exp(-Dt * (0.21f + 0.36f * V));
            for (int32 X = 0; X < ASTHeat::Width; ++X)
            {
                const int32 Cell = Y * ASTHeat::Width + X;
                const float OldHeat = Field.Temperature[Cell];
                const float U = (float(X) + 0.5f) / float(ASTHeat::Width) * 2.0f - 1.0f;
                const float Eddies =
                    6.2f * FMath::Sin(U * 7.0f + V * 14.0f + Field.Time * 3.2f + Seed) +
                    4.1f * FMath::Sin(U * 13.0f - V * 8.0f - Field.Time * 2.7f + Seed * 0.6f);
                const float Rise = (10.0f + 3.8f * OldHeat + Heat * 2.3f +
                    Gust * 1.6f) * Dt;
                const float Shift = (Wind * 19.5f + Eddies *
                    (1.0f + 0.37f * Gust + 0.19f * DepthGust)) * Dt;
                const float Sample = ASTHeat::Read(
                    Field.Temperature, float(X) - Shift, float(Y) - Rise);
                float NewHeat = Sample * Cooling;

                // Three asynchronously fluctuating burners grow one broad base.
                if (Y < 5)
                {
                    const float Falloff = FMath::Clamp(
                        1.0f - float(Y) / 5.0f, 0.0f, 1.0f);
                    // Separate flame tongues are born and die out of phase.
                    const float LocalLife = FMath::Clamp(
                        0.48f + 0.43f * FMath::Sin(
                            Field.Time * (4.8f + float(X % 5) * 0.27f) +
                            U * 5.7f + Seed) +
                        0.17f * FMath::Sin(Field.Time * 11.1f -
                            float(X) * 0.83f + Seed * 1.6f), 0.035f, 1.05f);
                    // Maintain a HOT rooted mass. Tongue lifetimes still vary,
                    // but random erosion cannot extinguish the whole source.
                    const float Source = ASTHeat::Source(float(X), Field.Time, Seed) *
                        (0.83f + 0.17f * ASTHeat::HashWave(
                            Field.Time * 2.6f + U * 3.0f + Seed)) *
                        (0.75f + 0.25f * LocalLife) *
                        (0.76f + 0.24f * Field.Breath);
                    NewHeat = FMath::Max(NewHeat, Source * Falloff * Heat);
                }
                // Local shear tears weak edges while keeping the shared hot base.
                const float Shear = FMath::Max(0.0f,
                    FMath::Sin(U * 15.0f - V * 23.0f + Field.Time * 4.6f + Seed) +
                    0.45f * FMath::Cos(U * 9.0f + V * 17.0f - Field.Time * 3.0f));
                NewHeat *= FMath::Exp(-Dt * (1.4f * V * Shear +
                    0.42f + 0.48f * V + Gust * 0.32f));

                // Turbulent pockets create short-lived detached islands.
                if (Y > 14 && Y < ASTHeat::Height - 2)
                {
                    const float Pocket = 0.11f * FMath::Max(0.0f,
                        FMath::Sin(Field.Time * 9.7f + float(X * 13 + Y * 7) + Seed));
                    NewHeat += Pocket * Dt * Heat;
                }
                Field.Work[Cell] = FMath::Clamp(NewHeat, 0.0f, 1.0f);
            }
        }
        Swap(Field.Temperature, Field.Work);

        // The aggregate temperature directly supplies light lifetime and
        // growth information. Keep this visual-only; not replicated.
        double Sum = 0.0;
        for (int32 Y = 0; Y < ASTHeat::Height; ++Y)
            for (int32 X = 0; X < ASTHeat::Width; ++X)
                Sum += Field.Temperature[Y * ASTHeat::Width + X] *
                    (0.65 + 0.35 * double(Y) / double(ASTHeat::Height));
        const float Mass = FMath::Clamp(
            float(Sum / double(ASTHeat::Cells)) * 3.9f, 0.04f, 1.25f);
        const float WantedEnergy = FMath::Clamp(
            Mass * (0.42f + 0.65f * Field.Breath), 0.04f, 1.20f);
        Field.VisualEnergy = FMath::FInterpTo(
            Field.VisualEnergy, WantedEnergy, Dt,
            WantedEnergy > Field.VisualEnergy ? 11.0f : 5.0f);

        // Discrete torn-off fire islands; fixed budget, no per-fragment components.
        for (FASTFlameFragment& Fragment : Field.Fragments)
        {
            if (Fragment.Life <= 0.0f) continue;
            Fragment.X += (Fragment.VX + Wind * 8.0f) * Dt;
            Fragment.Y += Fragment.VY * Dt;
            Fragment.VX += 1.3f * FMath::Sin(Field.Time * 5.0f + Fragment.Y) * Dt;
            Fragment.Life = FMath::Max(0.0f, Fragment.Life - Dt);
        }
        Field.SpawnDebt = FMath::Min(3.0f, Field.SpawnDebt +
            Dt * Heat * (1.6f + 2.0f * Field.Breath + Gust));
        for (FASTFlameFragment& Fragment : Field.Fragments)
        {
            if (Field.SpawnDebt < 1.0f) break;
            if (Fragment.Life > 0.0f) continue;
            Field.SpawnDebt -= 1.0f;
            Fragment.X = float(ASTHeat::Width) * (0.15f + 0.70f *
                FMath::Abs(FMath::Sin(Field.Time * 2.7f + Seed)));
            Fragment.Y = float(ASTHeat::Height) * (0.72f + 0.15f *
                FMath::Abs(FMath::Cos(Field.Time * 4.1f + Seed)));
            Fragment.VX = 3.5f * FMath::Sin(Field.Time * 3.7f + Seed);
            Fragment.VY = 8.0f + 3.0f * FMath::Abs(FMath::Sin(Field.Time * 5.1f));
            Fragment.Life = 0.38f + 0.21f *
                FMath::Abs(FMath::Sin(Field.Time * 1.9f + Seed));
            Fragment.Heat = 0.76f;
        }
    }

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector2D> UV;
    TArray<FLinearColor> Colors;
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    const int32 QuadCount = ASTHeat::Cells + ASTHeat::FragmentCount;
    Vertices.Reserve(QuadCount * 4);
    UV.Reserve(QuadCount * 4);
    Colors.Reserve(QuadCount * 4);
    if (bCreate) Triangles.Reserve(QuadCount * 6);

    const float CellW = RealWidth / float(ASTHeat::Width);
    const float CellH = RealHeight / float(ASTHeat::Height);
    const FVector MotionFlow = SmoothedFlow - Field.LastAnchorVelocity * 0.75f;
    const float FlowSide = FMath::Clamp(
        FVector::DotProduct(MotionFlow, Right) * 0.34f,
        -RealWidth * 0.46f, RealWidth * 0.46f);
    const float HeightCycle = FMath::Clamp(
        0.60f + 0.44f * Field.Breath - 0.10f * Field.CurrentGust,
        0.42f, 1.18f);
    for (int32 Y = 0; Y < ASTHeat::Height; ++Y)
    {
        const float V = (float(Y) + 0.5f) / float(ASTHeat::Height);
        // Upper cells lag behind the anchor. No global sprite rotation.
        const float Lag = FlowSide * V * V;
        for (int32 X = 0; X < ASTHeat::Width; ++X)
        {
            const float U = (float(X) + 0.5f) / float(ASTHeat::Width) - 0.5f;
            const FVector Center = Origin + Right * (U * RealWidth + Lag) +
                WorldUp * (V * RealHeight * HeightCycle);
            // Cells flicker and detach independently; contour alone is jelly.
            const float Seed = float((SimulationSeed + Index * 5381) % 1009) * 0.0137f;
            const float LocalAge = FMath::Clamp(
                0.55f + 0.33f * FMath::Sin(Field.Time * 6.7f +
                    float(X) * 0.63f - float(Y) * 0.22f + Seed) +
                0.18f * FMath::Sin(Field.Time * 12.3f -
                    float(Y) * 0.41f + float(X) * 0.17f + Seed),
                0.03f, 1.12f);
            const float RootSupport = FMath::Clamp(1.0f - V * 1.4f, 0.0f, 1.0f);
            const float Hot = FMath::Pow(FMath::Clamp(
                Field.Temperature[Y * ASTHeat::Width + X] *
                ASTHeat::ShapedCoverage(X, Y, Field.Time, Seed),
                0.0f, 1.0f), 0.68f);
            const float T = Hot * FMath::Lerp(
                FMath::Clamp(0.66f + 0.34f * LocalAge, 0.30f, 1.15f),
                1.0f, RootSupport);
            ASTHeat::AddQuad(Vertices, Triangles, UV, Colors, bCreate,
                Transform, Center, Right, WorldUp,
                CellW * 0.503f, CellH * 0.503f, T, V);
        }
    }
    for (const FASTFlameFragment& Fragment : Field.Fragments)
    {
        const float V = Fragment.Y / float(ASTHeat::Height);
        const float U = Fragment.X / float(ASTHeat::Width) - 0.5f;
        const FVector Center = Origin + Right * (U * RealWidth +
            FlowSide * V * V) + WorldUp * (V * RealHeight * HeightCycle);
        const float Bright = Fragment.Life > 0.0f
            ? Fragment.Heat * FMath::Min(1.0f, Fragment.Life * 3.2f) : 0.0f;
        ASTHeat::AddQuad(Vertices, Triangles, UV, Colors, bCreate,
            Transform, Center, Right, WorldUp,
            CellW * 0.80f, CellH * 1.12f, Bright, V);
    }

    if (bCreate)
        Volume->CreateMeshSection_LinearColor(
            0, Vertices, Triangles, Normals, UV, Colors, Tangents, false);
    else
        Volume->UpdateMeshSection_LinearColor(
            0, Vertices, Normals, UV, Colors, Tangents);
}


// Primary burning layer on the actual rendered mesh triangles. GPU skinning
// makes it stick to limbs and the torso throughout sprint/animation frames.
void UASTFireReactiveComponent::SetupSurfaceFire(USceneComponent* Parent)
{
    // Stage22 uses a translucent UV-anchored soot/cracks layer over the
    // EXISTING mesh material: no WPO or changes to authored material slots.
    // The old Stage11 WPO cloak remains disabled for procedural fire.
    const bool bCharOverlay = bEnableProceduralFlames && bEnableCharring;
    if ((!bCharOverlay && !bEnableSurfaceFire) || !GetOwner()) return;
    UMeshComponent* Mesh = Cast<UMeshComponent>(Parent);
    if (!IsValid(Mesh) || !Mesh->IsVisible()) return;
    UMaterialInterface* Surface = bCharOverlay
        ? CharMaterial.LoadSynchronous() : SurfaceMaterial.LoadSynchronous();
    if (!Surface)
    {
        UE_LOG(LogTemp, Warning, TEXT("AST Fire: surface material missing for %s"),
            *GetOwner()->GetName());
        return;
    }
    UMaterialInterface* Existing = Mesh->GetOverlayMaterial(false, 0);
    // Never steal another actor's overlay: damage, highlighting, stealth etc.
    if (Existing)
    {
        UE_LOG(LogTemp, Warning, TEXT("AST Fire: existing overlay on %s, left untouched."),
            *GetOwner()->GetName());
        return;
    }
    FireSurfaceMID = UMaterialInstanceDynamic::Create(Surface, this);
    if (!FireSurfaceMID) return;
    FireSurfaceMesh = Mesh;
    PreviousFireSurfaceOverlay = Existing;
    if (bCharOverlay)
    {
        const float MeshSize=Cast<UPrimitiveComponent>(Mesh)
            ? float(Mesh->Bounds.BoxExtent.GetMax())*2.f : 180.f;
        FireSurfaceMID->SetScalarParameterValue(FName(TEXT("CharSeed")),
            float((SimulationSeed + GetOwner()->GetUniqueID()) % 10007));
        FireSurfaceMID->SetScalarParameterValue(FName(TEXT("CharProgress")),
            CharProgress);
        FireSurfaceMID->SetScalarParameterValue(FName(TEXT("CharGlow")), 0.f);
        FireSurfaceMID->SetScalarParameterValue(FName(TEXT("CharTiling")),
            FMath::Clamp(34.f*MeshSize/180.f,18.f,85.f));
        UpdateSurfaceVisibility();
        return;
    }
    FireSurfaceMID->SetScalarParameterValue(FName(TEXT("FireSeed")),
        float((SimulationSeed + GetOwner()->GetUniqueID()) % 10007));
    FireSurfaceMID->SetScalarParameterValue(FName(TEXT("FireIntensity")),
        FMath::Clamp(Intensity, 0.0f, 1.0f));
    FireSurfaceMID->SetScalarParameterValue(FName(TEXT("FireEnergy")), 0.9f);
    // A single displaced overlay pass forms an animated shell outside the
    // entire original skinned/static silhouette. This is not a base-color
    // change; the original material and mesh are left untouched.
    float LongestSide = 180.f;
    if (const UPrimitiveComponent* Primitive=Cast<UPrimitiveComponent>(Mesh))
        LongestSide = Primitive->CalcBounds(FTransform::Identity)
            .BoxExtent.GetAbs().GetMax()*2.f;
    const float CloakDepth = FMath::Clamp(
        9.f + LongestSide*.025f, 10.f, 28.f);
    FireSurfaceMID->SetScalarParameterValue(
        FName(TEXT("FireCloakDepth")), CloakDepth);
    UE_LOG(LogTemp, Log,
        TEXT("AST Fire: connected displaced cloak %s depth=%.1f cm"),
        *GetOwner()->GetName(),CloakDepth);
    UpdateSurfaceVisibility();
}

void UASTFireReactiveComponent::UpdateSurfaceVisibility()
{
    if (!IsValid(FireSurfaceMesh) || !IsValid(FireSurfaceMID)) return;
    UMaterialInterface* Current = FireSurfaceMesh->GetOverlayMaterial(false, 0);
    const bool bShowChar = bEnableProceduralFlames && bEnableCharring &&
        (CharProgress > .001f || (bStartLit && Intensity > .005f)
            || SmoothIntensity > .02f);
    if (bShowChar || (bStartLit && Intensity > 0.005f))
    {
        // Do not steal overlays installed by unrelated gameplay systems.
        if (!Current || Current == PreviousFireSurfaceOverlay)
            FireSurfaceMesh->SetOverlayMaterial(FireSurfaceMID);
    }
    else if (Current == FireSurfaceMID)
        FireSurfaceMesh->SetOverlayMaterial(PreviousFireSurfaceOverlay);
}

void UASTFireReactiveComponent::ReleaseSurfaceFire()
{
    if (IsValid(FireSurfaceMesh) && IsValid(FireSurfaceMID) &&
        FireSurfaceMesh->GetOverlayMaterial(false, 0) == FireSurfaceMID)
        FireSurfaceMesh->SetOverlayMaterial(PreviousFireSurfaceOverlay);
    FireSurfaceMesh = nullptr;
    FireSurfaceMID = nullptr;
    PreviousFireSurfaceOverlay = nullptr;
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
    SetupSurfaceFire(Parent);

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

    if (bEnableProceduralFlames && !bUseLegacyHeatGrid && ProceduralMaterial)
    {
        FreeFlames = NewObject<UASTFireFreeFlameComponent>(Owner,
            NAME_None, RF_Transient);
        if (IsValid(FreeFlames))
        {
            FreeFlames->SetSource(Parent, ProceduralMaterial, SimulationSeed);
            FreeFlames->SetIgnitionPoints(ExtraLocalPoints);
            FreeFlames->SetSurfaceMaterial(FireSurfaceMID);
            FreeFlames->SetThermal(Intensity, AmbientWind, 0.75f);
            FreeFlames->RegisterComponent();
        }
    }

    // Do not instantiate ANY bone-bound square sprites/volumes in Stage9.
    // The free flame component samples the true skinned mesh vertices each tick.
    if (IsValid(FreeFlames) && !bEnableFlameCards && !bEnableNiagara)
    {
        bInitialized = true;
        UpdateVisibility();
        return;
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

    // Mesh-aware auto coverage: no actor-sized glowing blob. Animated
    // characters get independent flame attachments on distributed bones;
    // props get local surface hits. A missing collision mesh uses bounds
    // only as an explicit fallback, never as our preferred placement.
    if (Coverage == EASTFireCoverage::Bounds && Points.IsEmpty())
    {
        if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Parent))
        {
            struct FBoneCandidate { FName Name; FVector World; float Scale; };
            TArray<FBoneCandidate> Candidates;
            Candidates.Reserve(Skeletal->GetNumBones());
            for (int32 BoneIndex = 0; BoneIndex < Skeletal->GetNumBones(); ++BoneIndex)
            {
                const FName Name = Skeletal->GetBoneName(BoneIndex);
                if (!ASTFire::ValidFlameBone(Name)) continue;
                const FVector P = Skeletal->GetBoneLocation(Name, EBoneSpaces::WorldSpace);
                if (P.ContainsNaN()) continue;
                const FName ParentName = Skeletal->GetParentBone(Name);
                const float SegmentLength = ParentName.IsNone() ? 38.0f :
                    FVector::Dist(P, Skeletal->GetBoneLocation(ParentName, EBoneSpaces::WorldSpace));
                Candidates.Add({Name, P, FMath::Clamp(SegmentLength / 120.0f, 0.22f, 0.62f)});
            }

            // Farthest-point bone sampling avoids selecting six spine bones
            // while leaving arms, legs or head completely unlit.
            TArray<int32> Chosen;
            for (int32 Slot = 0; Slot < Count && Chosen.Num() < Candidates.Num(); ++Slot)
            {
                int32 Best = INDEX_NONE;
                double BestDistance = -TNumericLimits<double>::Max();
                for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
                {
                    if (Chosen.Contains(CandidateIndex)) continue;
                    double Distance = Chosen.Num() == 0 ?
                        -FVector::DistSquared(Candidates[CandidateIndex].World,
                            Skeletal->Bounds.Origin) : TNumericLimits<double>::Max();
                    for (int32 Existing : Chosen)
                        Distance = FMath::Min(Distance, FVector::DistSquared(
                            Candidates[CandidateIndex].World, Candidates[Existing].World));
                    if (Distance > BestDistance || Best == INDEX_NONE)
                    {
                        Best = CandidateIndex;
                        BestDistance = Distance;
                    }
                }
                if (Best == INDEX_NONE) break;
                Chosen.Add(Best);
                const FBoneCandidate& Part = Candidates[Best];
                FVector SurfaceWorld = Part.World;
                FVector Radial = (Part.World - Skeletal->Bounds.Origin).GetSafeNormal();
                if (Radial.IsNearlyZero()) Radial = FVector::ForwardVector;
                const FVector Outer = Part.World + Radial * 58.0f;
                const FVector Inner = Part.World - Radial * 15.0f;
                ASTFire::ProjectToSurface(Skeletal, Outer, Inner, SurfaceWorld);
                const FTransform BoneTransform = Skeletal->GetSocketTransform(Part.Name, RTS_World);
                const FVector BoneOffset = BoneTransform.InverseTransformPosition(SurfaceWorld);
                Points.Add({Part.Name, BoneOffset, Part.Scale});
            }
            UE_LOG(LogTemp, Verbose,
                TEXT("AST Fire: %s generated %d skeletal surface/bone anchors"),
                *Owner->GetName(), Points.Num());
        }

        if (Points.IsEmpty())
        {
            UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Parent);
            const FBoxSphereBounds LocalBounds = Primitive
                ? Primitive->CalcBounds(FTransform::Identity)
                : FBoxSphereBounds(FVector::ZeroVector, FVector(45.0f), 80.0f);
            const FVector Extent = LocalBounds.BoxExtent.GetAbs();
            const FVector Centre = LocalBounds.Origin;
            const FTransform ComponentTransform = Parent->GetComponentTransform();

            // Patch size is determined from LOCAL surface area divided over
            // anchors; total actor height alone must never set flame radius.
            const double LargestFace = FMath::Max3(Extent.X * Extent.Y,
                Extent.X * Extent.Z, Extent.Y * Extent.Z);
            const float PatchScale = FMath::Clamp(
                FMath::Sqrt(FMath::Max(1.0, LargestFace * 4.0 / double(Count))) /
                    200.0, 0.18, 1.35);

            for (int32 i = 0; i < Count; ++i)
            {
                const float A = (FMath::Frac(0.618034f * i + 0.17f) * 1.7f - 0.85f);
                const float B = (FMath::Frac(0.754877f * i + 0.39f) * 1.7f - 0.85f);
                FVector Face(0, 0, 1);
                if (i % 5 == 1) Face = FVector(1, 0, 0);
                if (i % 5 == 2) Face = FVector(-1, 0, 0);
                if (i % 5 == 3) Face = FVector(0, 1, 0);
                if (i % 5 == 4) Face = FVector(0, -1, 0);
                FVector Offset = Centre;
                if (FMath::Abs(Face.Z) > 0.5f)
                    Offset += FVector(A * Extent.X, B * Extent.Y, Extent.Z);
                else if (FMath::Abs(Face.X) > 0.5f)
                    Offset += FVector(Face.X * Extent.X, A * Extent.Y, B * Extent.Z);
                else
                    Offset += FVector(A * Extent.X, Face.Y * Extent.Y, B * Extent.Z);

                FVector Outer = Offset + Face * 24.0f;
                FVector Inner = Offset - Face * FMath::Max(
                    30.0f, FMath::Max3(Extent.X, Extent.Y, Extent.Z) * 2.1f);
                FVector HitWorld;
                if (ASTFire::ProjectToSurface(Primitive,
                    ComponentTransform.TransformPosition(Outer),
                    ComponentTransform.TransformPosition(Inner), HitWorld))
                {
                    Offset = ComponentTransform.InverseTransformPosition(HitWorld);
                }
                Points.Add({NAME_None, Offset, PatchScale});
            }
            UE_LOG(LogTemp, Verbose,
                TEXT("AST Fire: %s generated %d static surface anchors"),
                *Owner->GetName(), Points.Num());
        }
    }
    if (Points.IsEmpty())
        Points.Add({NAME_None, PointOffset, 1.0f});

    for (const FVector_NetQuantize10& HitOffset : ExtraLocalPoints)
    {
        if (Points.Num() >= 16) break;
        const FVector LocalHit(HitOffset.X, HitOffset.Y, HitOffset.Z);
        if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Parent))
        {
            const FVector WorldHit = Skeletal->GetComponentTransform().TransformPosition(LocalHit);
            FName NearestBone = NAME_None;
            double BestDistance = TNumericLimits<double>::Max();
            for (int32 BoneIndex = 0; BoneIndex < Skeletal->GetNumBones(); ++BoneIndex)
            {
                const FName Name = Skeletal->GetBoneName(BoneIndex);
                if (!ASTFire::ValidFlameBone(Name)) continue;
                const double D = FVector::DistSquared(WorldHit,
                    Skeletal->GetBoneLocation(Name, EBoneSpaces::WorldSpace));
                if (D < BestDistance) { BestDistance = D; NearestBone = Name; }
            }
            if (!NearestBone.IsNone())
            {
                const FTransform Bone = Skeletal->GetSocketTransform(NearestBone, RTS_World);
                Points.Add({NearestBone, Bone.InverseTransformPosition(WorldHit), 0.25f});
                continue;
            }
        }
        // Static impact is localized and never bigger than the nearby clusters.
        Points.Add({NAME_None, LocalHit, Points.Num() ?
            FMath::Clamp(Points[0].Size * 0.6f, 0.18f, 0.55f) : 0.32f});
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
    if (IsValid(FreeFlames))
        FreeFlames->DestroyComponent();
    FreeFlames = nullptr;
    ReleaseSurfaceFire();
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
    LocalFields.Empty();
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

float UASTFireReactiveComponent::ComputeBurnDuration() const
{
    float SizeCm = FMath::Max(10.f, ReferenceSizeCm);
    if (const UPrimitiveComponent* Primitive =
        Cast<UPrimitiveComponent>(FindTargetComponent()))
    {
        // World-space mesh bounds work for StaticMesh and animated SkeletalMesh;
        // use the largest dimension, not unbounded cubic volume scaling.
        SizeCm = FMath::Max(10.f, float(
            Primitive->Bounds.BoxExtent.GetMax()) * 2.f);
    }
    const float RelativeSize = SizeCm / FMath::Max(10.f, ReferenceSizeCm);
    const float Factor = FMath::Clamp(FMath::Pow(RelativeSize, .42f),
        .7f, FMath::Max(1.f, MaxBurnSizeMultiplier));
    return FMath::Max(.1f, BaseBurnSeconds) * Factor;
}

void UASTFireReactiveComponent::SetFireIntensity(float NewIntensity)
{
    if (bPlaying && GetOwner() && GetOwner()->GetNetMode() == NM_Client)
        return; // Fire gameplay state is server authoritative.
    const bool bWasLit = bStartLit && Intensity > 0.005f;
    Intensity = FMath::Clamp(NewIntensity, 0.0f, 1.0f);
    bStartLit = Intensity > 0.005f;
    if (bStartLit && !bWasLit)
    {
        // Explicit re-ignition starts a fresh fuel clock.
        BurnElapsedSeconds = 0.f;
        BurnDurationSeconds = ComputeBurnDuration();
        InitialBurnIntensity = Intensity;
    }
    else if (bStartLit && bAutoExtinguish)
    {
        // An external intensity adjustment changes fuel heat, not elapsed age.
        InitialBurnIntensity = FMath::Max(InitialBurnIntensity, Intensity);
    }
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
        if (IsValid(FreeFlames))
            FreeFlames->SetIgnitionPoints(ExtraLocalPoints);
        else
            RefreshFireVisuals();
    }
}

void UASTFireReactiveComponent::UpdateVisibility()
{
    const bool bLit = bStartLit && Intensity > 0.005f;
    // Never cut the living mesh/lights abruptly at the authoritative timer.
    // Its smooth thermal tail and already-detached children finish naturally.
    const bool bAfterglow = bPlaying && SmoothIntensity > .012f;
    const bool bShowFire = bLit || bAfterglow;
    UpdateSurfaceVisibility();
    if (IsValid(FreeFlames)) FreeFlames->SetFlamesVisible(bShowFire);
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
        Light->SetVisibility(bShowFire);
        if (!bShowFire) Light->SetIntensity(0.0f);
        else if (!bPlaying) Light->SetIntensity(LightPower * Intensity);
    }
}

void UASTFireReactiveComponent::OnInfluenceBegin(
    UPrimitiveComponent* Overlapped, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
    bool bFromSweep, const FHitResult& SweepResult)
{
    if (APawn* Pawn = Cast<APawn>(OtherActor))
        if (Pawn != GetOwner())
        {
            NearbyPawns.Add(Pawn);
            LastNearbyPawnPositions.Add(TWeakObjectPtr<APawn>(Pawn), Pawn->GetActorLocation());
        }
}

void UASTFireReactiveComponent::OnInfluenceEnd(
    UPrimitiveComponent* Overlapped, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
    if (APawn* Pawn = Cast<APawn>(OtherActor))
    {
        NearbyPawns.Remove(Pawn);
        LastNearbyPawnPositions.Remove(TWeakObjectPtr<APawn>(Pawn));
    }
}

void UASTFireReactiveComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bPlaying || !GetOwner() || !GetWorld() || DeltaTime <= KINDA_SMALL_NUMBER)
        return;
    if (!bInitialized) InitializeFire();

    // Only the authoritative instance consumes fuel and changes replicated
    // burning state; clients use the replicated Intensity and local smoothing.
    if (GetOwner()->HasAuthority() && bAutoExtinguish && bStartLit)
    {
        if (BurnDurationSeconds <= 0.f)
        {
            BurnDurationSeconds = ComputeBurnDuration();
            InitialBurnIntensity = Intensity;
        }
        BurnElapsedSeconds += DeltaTime;
        const float FadeStart = FMath::Max(0.f,
            BurnDurationSeconds - FMath::Max(.1f, BurnFadeSeconds));
        if (BurnElapsedSeconds >= FadeStart)
        {
            const float Fade = FMath::Clamp(
                (BurnDurationSeconds - BurnElapsedSeconds) /
                FMath::Max(.01f, BurnDurationSeconds - FadeStart), 0.f, 1.f);
            Intensity = InitialBurnIntensity * FMath::Pow(Fade, 1.2f);
            if (BurnElapsedSeconds >= BurnDurationSeconds)
            {
                Intensity = 0.f;
                bStartLit = false;
                UpdateVisibility();
            }
        }
    }

    // Char accumulates on the server and survives extinguishing.
    // Replication makes the amount deterministic for late-joining clients.
    if (GetOwner()->HasAuthority() && bEnableCharring && bStartLit)
        CharProgress=FMath::Clamp(CharProgress+
            DeltaTime*FMath::Max(0.f,Intensity)*
            FMath::Max(.1f,CharringRate)/FMath::Max(.25f,BurnDurationSeconds),
            0.f,1.f);

    const FVector Position = GetOwner()->GetActorLocation();
    const FVector Velocity = (Position - LastOwnerPosition) / DeltaTime;
    LastOwnerPosition = Position;
    const FVector Acceleration = (Velocity - LastOwnerVelocity) / DeltaTime;
    LastOwnerVelocity = Velocity;

    const FQuat CurrentRotation = GetOwner()->GetActorQuat();
    const FQuat RotationDelta = CurrentRotation * LastOwnerRotation.Inverse();
    LastOwnerRotation = CurrentRotation;
    FVector RotationAxis = FVector::UpVector;
    double RotationAngle = 0.0;
    RotationDelta.ToAxisAndAngle(RotationAxis, RotationAngle);
    if (RotationAngle > PI) RotationAngle -= 2.0f * PI;
    const FVector AngularVelocity = RotationAxis.GetSafeNormal() *
        FMath::Clamp(RotationAngle / DeltaTime, -12.0, 12.0);

    float LocalDisturbance = 0.0f;
    FVector LocalWind = FVector::ZeroVector;

    // Include pawns that were already in the volume at flame startup.
    if (IsValid(InfluenceSphere))
    {
        TArray<AActor*> InRange;
        InfluenceSphere->GetOverlappingActors(InRange, APawn::StaticClass());
        for (AActor* Candidate : InRange)
            if (APawn* Pawn = Cast<APawn>(Candidate))
                if (Pawn != GetOwner()) NearbyPawns.Add(Pawn);
    }
    for (auto It = NearbyPawns.CreateIterator(); It; ++It)
    {
        APawn* Pawn = It->Get();
        if (!IsValid(Pawn))
        {
            LastNearbyPawnPositions.Remove(*It);
            It.RemoveCurrent();
            continue;
        }
        if (Pawn == GetOwner()) continue;
        const FVector PawnPosition = Pawn->GetActorLocation();
        const FVector* Previous = LastNearbyPawnPositions.Find(*It);
        const FVector MeasuredVelocity = Previous && DeltaTime > 0.001f
            ? (PawnPosition - *Previous) / DeltaTime : Pawn->GetVelocity();
        LastNearbyPawnPositions.Add(*It, PawnPosition);
        const FVector PawnVelocity = Pawn->GetVelocity().SizeSquared() >
            MeasuredVelocity.SizeSquared() ? Pawn->GetVelocity() : MeasuredVelocity;
        float Distance = FVector::Dist(Position, PawnPosition);
        for (const UProceduralMeshComponent* Volume : FireVolumes)
            if (IsValid(Volume))
                Distance = FMath::Min(Distance,
                    FVector::Dist(Volume->GetComponentLocation(), PawnPosition));
        if (Distance >= InfluenceRadius) continue;
        const float Proximity = FMath::Square(1.0f -
            Distance / FMath::Max(InfluenceRadius, 1.0f));
        const float Strength = Proximity *
            FMath::Clamp(PawnVelocity.Size() / 280.0f, 0.0f, 1.8f);
        if (Strength > LocalDisturbance)
        {
            LocalDisturbance = Strength;
            LocalWind = PawnVelocity;
        }
    }

    Disturbance = FMath::FInterpTo(
        Disturbance, LocalDisturbance, DeltaTime,
        LocalDisturbance > Disturbance ? 17.0f : 4.0f);
    const float TargetIntensity = bStartLit ? Intensity : 0.0f;
    SmoothIntensity = FMath::FInterpTo(
        SmoothIntensity, TargetIntensity, DeltaTime, 8.0f);
    if (!bStartLit && SmoothIntensity < .012f)
        UpdateVisibility(); // Hide only once the residual flame has faded.

    const float Time = GetWorld()->GetTimeSeconds();
    const float Pulse = 0.55f + 0.22f * FMath::Sin(Time * 5.8f)
        + 0.17f * FMath::Sin(Time * 9.7f + 1.2f)
        + 0.12f * FMath::Sin(Time * 2.1f + 2.4f);
    const float Movement = FMath::Clamp(Velocity.Size() / 600.0f, 0.0f, 1.0f);
    const float Response = FMath::Max(Movement, Disturbance);
    const FVector Wind = -Velocity * 1.05f
        - Acceleration.GetClampedToMaxSize(6500.0f) * 0.065f
        + LocalWind * 1.35f + AmbientWind;
    const float ResponseRate = Wind.SizeSquared() >
        SmoothedFlow.SizeSquared() ? 15.0f : 6.5f;
    SmoothedFlow = FMath::VInterpTo(SmoothedFlow, Wind.GetClampedToMaxSize(1600.0f),
        DeltaTime, ResponseRate);
    SmoothedAngularVelocity = FMath::VInterpTo(
        SmoothedAngularVelocity, AngularVelocity, DeltaTime, 9.0f);
    const FVector LocalWindVec = GetOwner()->GetActorTransform()
        .InverseTransformVectorNoScale(Wind);
    if (IsValid(FireSurfaceMID))
    {
        if (bEnableProceduralFlames && bEnableCharring)
        {
            FireSurfaceMID->SetScalarParameterValue(
                FName(TEXT("CharProgress")),CharProgress);
            FireSurfaceMID->SetScalarParameterValue(
                FName(TEXT("CharGlow")),
                FMath::Clamp(SmoothIntensity*LightEnvelope,0.f,1.2f));
            UpdateSurfaceVisibility(); // Keep cooled soot visible on the mesh.
        }
        // Root texture, extending tongues and light share the same heat energy.
        FireSurfaceMID->SetScalarParameterValue(FName(TEXT("FireIntensity")),
            SmoothIntensity);
        FireSurfaceMID->SetScalarParameterValue(FName(TEXT("FireEnergy")),
            LightEnvelope);
        const FVector Flow = IsValid(FireSurfaceMesh) ?
            FireSurfaceMesh->GetComponentTransform().InverseTransformVectorNoScale(Wind)
            : LocalWindVec;
        FireSurfaceMID->SetVectorParameterValue(FName(TEXT("FireMotion")),
            FLinearColor(
                FMath::Clamp(Flow.X / 450.0f, -2.0f, 2.0f),
                FMath::Clamp(Flow.Y / 450.0f, -2.0f, 2.0f),
                FMath::Clamp(Flow.Z / 450.0f, -2.0f, 2.0f), 0.0f));
    }

    if (IsValid(FreeFlames))
        FreeFlames->SetThermal(SmoothIntensity, SmoothedFlow, LightEnvelope);

    // Light follows the same actual living flame energy.
    float VisualEnergy = IsValid(FreeFlames) ? FreeFlames->GetVisualEnergy() : 0.0f;
    for (const FASTLocalFlameField& Field : LocalFields)
        VisualEnergy += Field.VisualEnergy;
    if (LocalFields.Num() > 0)
        VisualEnergy /= float(LocalFields.Num());
    else if (!IsValid(FreeFlames))
        VisualEnergy = FMath::Clamp(Pulse, 0.05f, 1.0f);
    const float WantedLight = FMath::Clamp(
        0.09f + 0.91f * VisualEnergy + 0.04f * FMath::Max(Pulse, 0.0f),
        0.10f, 1.22f);
    LightEnvelope = FMath::FInterpTo(LightEnvelope, WantedLight, DeltaTime,
        WantedLight > LightEnvelope ? 12.0f : 6.0f);
    if (IsValid(Light))
    {
        const float FireLight = SmoothIntensity * LightEnvelope;
        Light->SetIntensity(LightPower * FireLight);
        Light->SetAttenuationRadius(LightRadius *
            FMath::Clamp(0.54f + 0.62f * LightEnvelope, 0.52f, 1.27f));
        Light->SetLightColor(FLinearColor(
            1.0f, 0.24f + 0.26f * LightEnvelope, 0.075f + 0.18f * LightEnvelope));
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
            const float Elapsed = VolumeUpdateDebt;
            VolumeUpdateDebt = 0.0f;
            for (int32 i = 0; i < FireVolumes.Num(); ++i)
                if (IsValid(FireVolumes[i])) UpdateVolume(FireVolumes[i], i, false, Elapsed);
        }
    }
}
