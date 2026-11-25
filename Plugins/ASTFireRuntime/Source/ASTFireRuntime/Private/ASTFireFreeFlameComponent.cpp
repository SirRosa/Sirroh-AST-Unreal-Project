#include "ASTFireFreeFlameComponent.h"
#include "ASTFireBlueprintLibrary.h"
#include "ASTFireReactiveComponent.h"

#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "EngineUtils.h"
#include "Components/SpotLightComponent.h"
#include "UObject/UObjectIterator.h"
#include "CollisionQueryParams.h"
#include "Components/BoxComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"

namespace ASTFireLife
{
    // Deterministic per-flame irregularity: no copies of a shared sine sprite.
    float Hash(float V)
    {
        return FMath::Frac(FMath::Sin(V * 78.233f + 3.17f) * 43758.5453f);
    }

    // Reference-calibrated seven hard colour bands, stored as linear scalar.
    int32 FireTone(float H)
    {
        return int32(H>=.10f)+int32(H>=.25f)+int32(H>=.40f)+
            int32(H>=.54f)+int32(H>=.68f)+int32(H>=.82f);
    }

    float ToneCode(float H)
    {
        // The material decodes discrete indices, not a gradient or noise.
        return (FireTone(H)+.5f)/7.f;
    }

    struct FFireProjection
    {
        FVector Cam,Forward,Right,Up;
        static constexpr float Reference=600.f;
        FFireProjection(const FVector& Origin,const UWorld* World)
        {
            Cam=Origin+FVector::ForwardVector*1000.f;
            Forward=-FVector::ForwardVector;Right=-FVector::RightVector;
            if(World) if(const APlayerController* PC=World->GetFirstPlayerController())
                if(const APlayerCameraManager* C=PC->PlayerCameraManager)
                {
                    Cam=C->GetCameraLocation();
                    Forward=C->GetCameraRotation().RotateVector(FVector::ForwardVector);
                    Right=C->GetCameraRotation().RotateVector(FVector::RightVector);
                }
            Up=FVector::CrossProduct(Forward,Right).GetSafeNormal();
        }
        float Depth(const FVector& W) const
        { return FMath::Max(10.f,float(FVector::DotProduct(W-Cam,Forward))); }
        FVector2D Project(const FVector& W) const
        {
            const FVector D=W-Cam;
            const float S=Reference/Depth(W);
            return FVector2D(FVector::DotProduct(D,Right)*S,FVector::DotProduct(D,Up)*S);
        }
        FVector Unproject(const FVector2D& P,float Depth) const
        {return Cam+Forward*Depth+(Right*P.X+Up*P.Y)*(Depth/Reference);}
    };

    // Hidden torch wicks are explicit attachment points, not burnable surfaces.
    bool IsPointSource(const USceneComponent* Source, bool bHasContact)
    {
        const UMeshComponent* Mesh=Cast<UMeshComponent>(Source);
        if(!Mesh)return true;
        return bHasContact && !Mesh->IsVisible() &&
            Mesh->GetCollisionEnabled()==ECollisionEnabled::NoCollision;
    }

    // Both halves evaluate the same thermal field, parameterized along the
    // original arc. Its clock is carried across the split, not randomized.
    float ArcHeatBase(float V,float Seed,float Clock)
    {
        const float Flow=.64f*FMath::Sin(V*13.f-Clock*8.8f+Seed)+
            .36f*FMath::Sin(V*24.f-Clock*12.1f+Seed*.7f);
        return .23f-.30f*V+.12f*Flow;
    }
    // One lookup table per swept segment, shared by the live and severed
    // arc. Avoid two trigonometric calls per covered pixel. With a .125
    // segment interval and 12 subdivisions the max interpolation error
    // is below a palette quantization step.
    struct FArcHeatSegment
    {
        float Values[13];
        FArcHeatSegment(float V0,float V1,float Seed,float Clock)
        {
            for(int32 I=0;I<=12;++I)
                Values[I]=ArcHeatBase(FMath::Lerp(V0,V1,float(I)/12.f),Seed,Clock);
        }
        float Evaluate(float Along,float Core) const
        {
            const float U=FMath::Clamp(Along,0.f,1.f)*12.f;
            const int32 I=FMath::Min(11,FMath::FloorToInt(U));
            return FMath::Clamp(FMath::Lerp(Values[I],Values[I+1],U-I)+.73f*Core,0.f,1.f);
        }
    };
    float ArcFade(float Age,float Life)
    {return FMath::Clamp(1.f-Age/FMath::Max(.01f,Life),0.f,1.f);}
    float SmoothStep(float V)
    {
        const float T = FMath::Clamp(V, 0.f, 1.f);
        return T * T * (3.f - 2.f * T);
    }
}

UASTFireFreeFlameComponent::UASTFireFreeFlameComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = .04f; // 25 Hz, visual only
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
    Detached.SetNum(MaxDetached);
    Sparks.SetNum(MaxSparks);
    Smoke.SetNum(MaxSmoke);
}

void UASTFireFreeFlameComponent::SetSource(
    USceneComponent* InSource, UMaterialInterface* InMaterial, int32 InSeed)
{
    Source = InSource;
    Material = InMaterial;
    Seed = InSeed;
}

void UASTFireFreeFlameComponent::SetThermal(
    float InHeat, const FVector& InWorldWind, float InEnergy)
{
    const float NewHeat=FMath::Clamp(InHeat,0.f,1.f);
    if(bGenerated && Heat>.10f && NewHeat<=.10f && SmokeFireSize>8.f)SmokeAfterglowSeconds=3.f;
    if(NewHeat>.10f)SmokeAfterglowSeconds=0.f;
    Heat=NewHeat;
    Wind = InWorldWind.GetClampedToMaxSize(1800.f);
    Energy = FMath::Clamp(InEnergy, .02f, 1.25f);
}

void UASTFireFreeFlameComponent::SetSurfaceMaterial(UMaterialInstanceDynamic* InMaterial)
{
    SurfaceMID = InMaterial;
    bClusterMapUploaded = false;
}

void UASTFireFreeFlameComponent::SetIgnitionPoints(
    const TArray<FVector_NetQuantize10>& InPoints)
{
    IgnitionPoints.Reset();
    for (const FVector_NetQuantize10& Point : InPoints)
        IgnitionPoints.Add(FVector(Point.X, Point.Y, Point.Z));
    // Recluster new contacts against the existing source. Do not restart
    // the existing tongues, split bridges, or detached child lifetimes.
    if (bGenerated)
    {
        RebuildClusters();
        UpdateSurfaceMap(0.f);
    }
}

void UASTFireFreeFlameComponent::SetFlamesVisible(bool bInVisible)
{
    bVisible = bInVisible;
    // The tick owns visibility so released embers remain visible until dead.
    if (bVisible && IsValid(Render)) Render->SetVisibility(true);
}

float UASTFireFreeFlameComponent::Uniform(int32 Salt) const
{
    return ASTFireLife::Hash(float(Seed % 7919) + float(Salt) * 1.2317f);
}

void UASTFireFreeFlameComponent::OnRegister()
{
    Super::OnRegister();
    if (IsTemplate() || !IsValid(Source) || !IsValid(Material) || !GetOwner()) return;
    const UWorld* World = GetWorld();
    if (World && World->GetNetMode() == NM_DedicatedServer) return;

    Render = NewObject<UProceduralMeshComponent>(GetOwner(), NAME_None, RF_Transient);
    if (!Render) return;
    Render->SetMobility(EComponentMobility::Movable);
    Render->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Render->SetGenerateOverlapEvents(false);
    Render->SetCastShadow(false);
    Render->SetAbsolute(false,true,true);
    Render->SetupAttachment(GetOwner()->GetRootComponent());
    Render->SetMaterial(0, Material);
    Render->SetMaterial(1, Material); // unified mesh-bound source, same pixels
    SparkMaterial = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/VFX/Fire/Materials/M_AST_FireSpark.M_AST_FireSpark"));
    if (IsValid(SparkMaterial))
        Render->SetMaterial(2, SparkMaterial);
    else
    {
        Render->SetMaterial(2, Material); // conservative fallback
        UE_LOG(LogTemp, Warning,TEXT(
            "AST Fire: missing M_AST_FireSpark on %s"),*GetOwner()->GetName());
    }
    SmokeMaterial = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/VFX/Fire/Materials/M_AST_FireSmoke.M_AST_FireSmoke"));
    Render->RegisterComponent();
    Render->SetWorldRotation(FRotator::ZeroRotator);
    Render->SetWorldScale3D(FVector::OneVector);
    // Separate absolute-transform component: world smoke must not move
    // with the burning actor or alter the approved fire mesh sections.
    if (IsValid(SmokeMaterial))
    {
        SmokeRender=NewObject<UProceduralMeshComponent>(
            GetOwner(),NAME_None,RF_Transient);
        if (IsValid(SmokeRender))
        {
            SmokeRender->SetMobility(EComponentMobility::Movable);
            SmokeRender->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            SmokeRender->SetGenerateOverlapEvents(false);
            SmokeRender->SetCastShadow(false);
            SmokeRender->SetupAttachment(GetOwner()->GetRootComponent());
            SmokeRender->SetAbsolute(true,true,true);
            SmokeRender->RegisterComponent();
            SmokeRender->SetWorldLocation(GetOwner()->GetActorLocation());
            SmokeRender->SetWorldRotation(FRotator::ZeroRotator);
            SmokeRender->SetWorldScale3D(FVector::OneVector);
            SmokeRender->SetMaterial(0,SmokeMaterial);
            SmokeRender->SetVisibility(false);
        }
    }
    else
        UE_LOG(LogTemp,Error,TEXT("AST Fire: missing smoke material on %s"),
            *GetOwner()->GetName());

    Sparks.SetNum(MaxSparks);
    Smoke.SetNum(MaxSmoke);
    BuildAnchors();
    BuildSourceAnchors();
    if (Tongues.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("AST Fire: no valid skin/surface samples for %s; using mesh overlay only"),
            *GetOwner()->GetName());
    }
    UpdateGeometry(true);
    UpdateSurfaceMap(.0f);
    bGenerated = true;
}

void UASTFireFreeFlameComponent::OnUnregister()
{
    if (IsValid(Render)) Render->DestroyComponent();
    if (IsValid(SmokeRender)) SmokeRender->DestroyComponent();
    SmokeRender=nullptr;
    Render = nullptr;
    Tongues.Reset();
    BurnClusters.Reset();
    SourceAnchors.Reset();
    ConnectedRoots.Reset();
    Sparks.Reset();
    SparkSpawnDebt = 0.f;
    Smoke.Reset();
    SmokeSpawnDebt = 0.f;
    SmokeAfterglowSeconds=0.f;
    SmokeLightScanDebt = 0.f;
    SmokeSimDebt = 0.f;
    SmokeFireSize = 0.f;
    SmokePixelPitch = 0.f;
    FirePixelPitch = FireVisualHeight = FireVisualWidth = 0.f;
    PointField.Reset();FieldFragments.Reset();FieldClock=-1.f;
    MeshPrevious.Reset();MeshFieldClock=-1.f;MeshCuts=MeshTransferred=0;
    NearbySmokeLights.Reset();
    SmokeMaterial = nullptr;
    SparkMaterial = nullptr;
    bGenerated = false;
    Super::OnUnregister();
}

void UASTFireFreeFlameComponent::ConfigureGeneration(FTongue& Tongue)
{
    ++Tongue.Generation;
    const int32 Salt = Tongue.Generation * 113 + int32(Tongue.Seed * 917.f);
    // Each generation has a different birth-to-peak duration and reach.
    const float Large=FMath::Clamp((SizeScale-.18f)/.82f,0.f,1.f);
    Tongue.GrowSeconds = FMath::Lerp(.40f,.78f,Large) + FMath::Lerp(.46f,.75f,Large)*Uniform(Salt + 1);
    Tongue.RebirthDelay=FMath::Lerp(.055f,.17f,Large)+.09f*Uniform(Salt+11);
    Tongue.MaxLength = (55.f + 100.f *
        FMath::Pow(Uniform(Salt + 2),1.22f)) * Tongue.Scale * SizeScale;
    // Distributed surface combustion needs shorter flames per root, not
    // character-height torches at every vertex. Small wicks retain their size.
    const bool bPoint=ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty());
    const float Compactness=bPoint?FMath::Lerp(1.f,.72f,Large):
        (Cast<USkinnedMeshComponent>(Source)?.43f:.62f);
    Tongue.MaxLength*=Compactness;
    // At normal size a lobe is broad and short, not a tall thin ribbon.
    Tongue.MaxLength*=bPoint?FMath::Lerp(1.f,.74f,Large):.76f;
    Tongue.BaseWidth = (15.f + 24.f *
        FMath::Pow(Uniform(Salt + 3),1.15f)) * Tongue.Scale * SizeScale;
    Tongue.BaseWidth*=bPoint?FMath::Lerp(1.f,1.19f,Large):1.06f;
    Tongue.BaseWidth=FMath::Max(Tongue.BaseWidth,Tongue.MaxLength*(bPoint?.34f:.39f));
    // Exact ring cut; no independently spawned replacement sprite.
    Tongue.RemainingFraction = Large>.35f?(Uniform(Salt + 4)<.60f?.25f:.375f):.25f;
    Tongue.Phase = 6.283185f * Uniform(Salt + 5);
}

void UASTFireFreeFlameComponent::RebuildClusters()
{
    BurnClusters.Reset();
    if (!IsValid(Source)) return;

    FVector Extents(55.f,55.f,55.f);
    if (const UPrimitiveComponent* Primitive=Cast<UPrimitiveComponent>(Source))
        Extents=Primitive->CalcBounds(FTransform::Identity).BoxExtent.GetAbs();
    const float MaxSize=FMath::Max(40.f,2.f*Extents.GetMax());
    const float MergeDistance=FMath::Clamp(MaxSize*.36f,22.f,145.f);

    // The original flame patch survives additional impacts. It is not rebuilt
    // from every tongue: a cluster is ONE source with MANY tongues.
    if (!Tongues.IsEmpty())
    {
        FBurnCluster Main;
        Main.RestCenter=Tongues[0].RestPosition; // actual sampled mesh vertex
        Main.Radius=FMath::Clamp(MaxSize*.69f,35.f,415.f);
        BurnClusters.Add(Main);
    }

    // Additional contacts either extend an existing patch or create a new
    // separate burn region. A new patch starts on a real surface hit and may
    // merge with its neighbors as their radii grow.
    for (const FVector& Hit:IgnitionPoints)
    {
        int32 Nearest=INDEX_NONE;
        float DistanceSquared=TNumericLimits<float>::Max();
        for (int32 I=0;I<BurnClusters.Num();++I)
        {
            const float D=FVector::DistSquared(Hit,BurnClusters[I].RestCenter);
            if (D<DistanceSquared)
            {
                DistanceSquared=D;
                Nearest=I;
            }
        }
        if (Nearest!=INDEX_NONE &&
            (DistanceSquared<FMath::Square(MergeDistance) ||
             BurnClusters.Num()>=SurfaceClusters))
        {
            // Retain an ON-SURFACE representative instead of averaging into
            // the hollow interior of a limb or a box.
            FBurnCluster& C=BurnClusters[Nearest];
            ++C.Members;
            C.Radius=FMath::Clamp(FMath::Max(C.Radius,
                FMath::Sqrt(DistanceSquared)+MaxSize*.22f),22.f,415.f);
        }
        else if (BurnClusters.Num()<SurfaceClusters)
        {
            FBurnCluster C;
            C.RestCenter=Hit;
            C.Radius=FMath::Clamp(MaxSize*.42f,24.f,260.f);
            BurnClusters.Add(C);
        }
    }

    // Multiple tongues choose actual surface vertices within a region's
    // future supporting area. The cluster is the source; tongues are children.
    for (FTongue& Tongue:Tongues)
    {
        int32 Nearest=INDEX_NONE;
        float DistanceSquared=TNumericLimits<float>::Max();
        for (int32 I=0;I<BurnClusters.Num();++I)
        {
            const float D=FVector::DistSquared(
                Tongue.RestPosition,BurnClusters[I].RestCenter);
            if (D<DistanceSquared)
            {
                Nearest=I;
                DistanceSquared=D;
            }
        }
        Tongue.ClusterIndex=Nearest;
        if (Nearest!=INDEX_NONE)
        {
            const FBurnCluster& C=BurnClusters[Nearest];
            const float Relative=FMath::Sqrt(DistanceSquared)/
                FMath::Max(20.f,C.Radius);
            Tongue.ActivationSpread=FMath::Clamp(
                .06f+Relative*.24f,.06f,.78f);
        }
        // Existing active tongues must not be extinguished or reset when
        // another collision adds a nearby burn cluster.
    }

    bClusterMapUploaded=false;
    UE_LOG(LogTemp,Log,TEXT("AST Fire source %s: %d joined mesh clusters / %d independent tongues"),
        *GetOwner()->GetName(),BurnClusters.Num(),Tongues.Num());
}

void UASTFireFreeFlameComponent::BuildAnchors()
{
    Tongues.Reset();
    if (!IsValid(Source)) return;
    // One narrow wick; broad fire has multiple independent outward axes.
    int32 DesiredCount = SizeScale<.22f?2:SizeScale<.55f?3:SizeScale<1.4f?5:6;
    const bool bPoint=ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty());
    if (!bPoint && Cast<USkinnedMeshComponent>(Source))
    {
        // A whole character needs head/torso/arms/legs, not 3 foot roots.
        DesiredCount = 6 + FMath::RoundToInt(Uniform(731) * 2.f);
    }
    else if (const UStaticMeshComponent* Static = bPoint?nullptr:Cast<UStaticMeshComponent>(Source))
    {
        const FVector Size = Static->Bounds.BoxExtent * 2.f;
        const float Area = 2.f * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
        const float MaxSide = Size.GetMax();
        DesiredCount = MaxSide < 140.f ? 8 + FMath::RoundToInt(Uniform(732)*3.f)
            : MaxSide > 340.f ? 12 + FMath::RoundToInt(Uniform(733)*3.f)
            : FMath::Clamp(FMath::RoundToInt(Area / 14500.f) + 7, 9, 14);
    }
    DesiredCount = FMath::Clamp(DesiredCount, 2, MaxTongues);

    if (const USkinnedMeshComponent* Skeletal = bPoint?nullptr:Cast<USkinnedMeshComponent>(Source))
    {
        USkeletalMesh* Asset = Cast<USkeletalMesh>(Skeletal->GetSkinnedAsset());
        FSkeletalMeshRenderData* Data = Asset ? Asset->GetResourceForRendering() : nullptr;
        if (Data && Data->LODRenderData.Num() > 0)
        {
            const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[0];
            const int32 N = int32(LOD.GetNumVertices());
            TArray<int32> Candidates;
            const int32 Cap = FMath::Min(320, N);
            if (Cap > 0)
            {
                for (int32 I = 0; I < Cap; ++I)
                    Candidates.Add(FMath::Clamp(
                        (I * N + N / 3) / Cap, 0, N-1));
            }
            // Surface-centered first ignition, then farthest-point sampling
            // over the ENTIRE rest mesh. Never confine later roots to the
            // initial 38-78cm patch (which isolated only the character feet).
            FBox CandidateBounds(ForceInit);
            for (const int32 Id:Candidates)
                CandidateBounds+=FVector(
                    LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Id));
            const FVector SurfaceCenter=CandidateBounds.GetCenter();
            for (int32 Slot=0; Slot<DesiredCount && Candidates.Num()>0; ++Slot)
            {
                int32 Best = INDEX_NONE;
                double BestScore = -TNumericLimits<double>::Max();
                for (int32 Id : Candidates)
                {
                    const FVector P(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Id));
                    double Distance = Tongues.IsEmpty()
                        ? -FVector::DistSquared(P, SurfaceCenter)
                        : TNumericLimits<double>::Max();
                    for (const FTongue& Existing : Tongues)
                        Distance = FMath::Min(Distance,
                            FVector::DistSquared(P, Existing.RestPosition));
                    // Contact points bias the first selected flame, not every
                    // root; growth thereafter merges nearby source regions.
                    if (Slot < IgnitionPoints.Num())
                        Distance = -FVector::DistSquared(P, IgnitionPoints[Slot]);
                    if (Distance > BestScore)
                    {
                        Best = Id;
                        BestScore = Distance;
                    }
                }
                if (Best == INDEX_NONE) break;
                FTongue Root;
                Root.SkinnedVertexIndex = Best;
                Root.RestPosition = FVector(
                    LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Best));
                Root.Seed = Uniform(Slot + 180) * 103.f;
                Root.Scale = .65f + .57f * Uniform(Slot + 270);
                Tongues.Add(Root);
                Candidates.Remove(Best);
            }
        }
    }
    else if (UStaticMeshComponent* Static = bPoint?nullptr:Cast<UStaticMeshComponent>(Source))
    {
        const FBoxSphereBounds Bounds = Static->CalcBounds(FTransform::Identity);
        const FVector Half = Bounds.BoxExtent.GetAbs();

        // Explicit contact hits are already surface-local positions. Retain
        // every distinct contact before distributing additional fire patches.
        for (int32 I=0; I<IgnitionPoints.Num() && Tongues.Num()<DesiredCount; ++I)
        {
            const FVector Contact = IgnitionPoints[I];
            bool bDuplicate=false;
            for (const FTongue& Existing:Tongues)
                if (FVector::DistSquared(Contact,Existing.RestPosition)<100.f)
                    {bDuplicate=true;break;}
            if (bDuplicate) continue;
            FTongue Root;
            Root.RestPosition=Contact;
            Root.Seed=Uniform(I+680)*103.f;
            Root.Scale=FMath::Clamp(Half.GetMax()/190.f,.45f,1.25f);
            Tongues.Add(Root);
        }

        // Deterministic 6-face surface lattice (96 once-only complex traces),
        // then farthest-point selection. Avoid pseudo-random hot specks on
        // just two cube faces while preserving precise real hit positions.
        TArray<FVector> SurfaceCandidates;
        SurfaceCandidates.Reserve(96);
        const FTransform Transform=Static->GetComponentTransform();
        const float Far=2.6f*Half.GetMax()+12.f;
        FCollisionQueryParams Query;
        Query.bTraceComplex=true;
        for(int32 Face=0;Face<6;++Face)
            for(int32 A=0;A<4;++A)
                for(int32 B=0;B<4;++B)
                {
                    const float U=(float(A)/3.f)*1.72f-.86f;
                    const float V=(float(B)/3.f)*1.72f-.86f;
                    FVector Normal=FVector::UpVector,Point=Bounds.Origin;
                    switch(Face)
                    {
                    case 0: Point+=FVector(U*Half.X,V*Half.Y,Half.Z);break;
                    case 1: Normal=FVector(1,0,0);
                        Point+=FVector(Half.X,U*Half.Y,V*Half.Z);break;
                    case 2: Normal=FVector(-1,0,0);
                        Point+=FVector(-Half.X,U*Half.Y,V*Half.Z);break;
                    case 3: Normal=FVector(0,1,0);
                        Point+=FVector(U*Half.X,Half.Y,V*Half.Z);break;
                    case 4: Normal=FVector(0,-1,0);
                        Point+=FVector(U*Half.X,-Half.Y,V*Half.Z);break;
                    default: Normal=FVector(0,0,-1);
                        Point+=FVector(U*Half.X,V*Half.Y,-Half.Z);break;
                    }
                    FHitResult Hit;
                    const FVector Start=Transform.TransformPosition(Point+Normal*18.f);
                    const FVector End=Transform.TransformPosition(Point-Normal*Far);
                    if(!Static->LineTraceComponent(Hit,Start,End,Query))
                    {
                        FCollisionQueryParams Simple=Query;Simple.bTraceComplex=false;
                        if(!Static->LineTraceComponent(Hit,Start,End,Simple))continue;
                    }
                    const FVector Rest=Transform.InverseTransformPosition(
                        FVector(Hit.ImpactPoint));
                    bool bDuplicate=false;
                    for(const FVector& Existing:SurfaceCandidates)
                        if(FVector::DistSquared(Existing,Rest)<FMath::Square(8.f))
                        {bDuplicate=true;break;}
                    if(!bDuplicate) SurfaceCandidates.Add(Rest);
                }
        // A carried prop disables collision, so surface traces can all fail.
        // Sample the same actual static-mesh render vertices used by the mantle.
        if(SurfaceCandidates.Num()<DesiredCount)
            if(UStaticMesh* Asset=Static->GetStaticMesh())
            if(Asset->bAllowCPUAccess || GIsEditor)
            if(const FStaticMeshRenderData* Data=Asset->GetRenderData())
            if(Data->LODResources.Num()>0)
            {
                const auto& Positions=Data->LODResources[0].VertexBuffers.PositionVertexBuffer;
                const int32 Count=int32(Positions.GetNumVertices());
                const int32 Samples=FMath::Min(Count,192);
                if(Positions.GetVertexData()!=nullptr)
                    for(int32 I=0;I<Samples;++I)
                        SurfaceCandidates.Add(FVector(Positions.VertexPosition((I*Count+Count/2)/Samples)));
            }
        // Cooked assets may strip CPU buffers too. Last-resort samples remain
        // on the local mesh bounds, not an unrelated pickup collision proxy.
        if(SurfaceCandidates.IsEmpty())
            for(int32 Axis=0;Axis<3;++Axis)
                for(int32 Sign:{-1,1})
                {
                    FVector P=Bounds.Origin;P[Axis]+=Half[Axis]*Sign;
                    SurfaceCandidates.Add(P);
                }
        while(Tongues.Num()<DesiredCount && !SurfaceCandidates.IsEmpty())
        {
            int32 Best=INDEX_NONE;
            double BestScore=-TNumericLimits<double>::Max();
            for(int32 I=0;I<SurfaceCandidates.Num();++I)
            {
                const FVector& P=SurfaceCandidates[I];
                double Score=Tongues.IsEmpty()
                    ? -FVector::DistSquared(P,Bounds.Origin)
                    : TNumericLimits<double>::Max();
                for(const FTongue& Existing:Tongues)
                    Score=FMath::Min(Score,
                        FVector::DistSquared(P,Existing.RestPosition));
                if(Score>BestScore){BestScore=Score;Best=I;}
            }
            if(Best==INDEX_NONE)break;
            FTongue Root;
            Root.RestPosition=SurfaceCandidates[Best];
            Root.Seed=Uniform(Tongues.Num()+380)*103.f;
            Root.Scale=FMath::Clamp(Half.GetMax()/190.f,.45f,1.25f);
            Tongues.Add(Root);
            SurfaceCandidates.RemoveAtSwap(Best);
        }
    }
    if (Tongues.IsEmpty() && bPoint)
    {
        // Free-standing source uses compact flame base, never a rectangular field.
        for (int32 I=0; I<DesiredCount; ++I)
        {
            FTongue Root;
            const float Angle=2.f*PI*float(I)/FMath::Max(1,DesiredCount)+Uniform(980)*2.f*PI;
            const FVector Radial(FMath::Cos(Angle),FMath::Sin(Angle),0.f);
            const float SpreadRadius=FMath::Lerp(2.f,8.5f,FMath::Clamp(SizeScale,0.f,1.f));
            Root.Outward=Radial;
            const FVector Offset=IgnitionPoints.IsEmpty()?FVector::ZeroVector:IgnitionPoints[0];
            // The explicit wick position is component-local; keep root spacing in world cm.
            Root.RestPosition=Offset+Source->GetComponentTransform().InverseTransformVector(
                Radial*SpreadRadius*FMath::Min(1.5f,SizeScale));
            Root.Seed = Uniform(I+501)*103.f;
            Root.Scale = .68f+.34f*Uniform(I+777);
            Tongues.Add(Root);
        }
    }
    RebuildClusters();
    for (int32 I=0; I<Tongues.Num(); ++I)
    {
        FTongue& Root=Tongues[I];
        ConfigureGeneration(Root);
        Root.Age = -(.04f + Uniform(I+611)*.70f);
        Root.CurrentLength = Root.MaxLength * .13f;
        Root.RestartLength = Root.CurrentLength;
        Root.CurrentWidth = Root.BaseWidth*.58f;
        Root.PreviousWidth = Root.CurrentWidth;
        Root.PreviousPhase = Root.Phase;
        Root.bActive = Spread >= Root.ActivationSpread;
        Root.WorldPrevious = SampleRoot(Root);
    }
    UE_LOG(LogTemp, Log,TEXT("AST Fire Stage10: %s -> %d unique tongues on mesh surface"),
        *GetOwner()->GetName(),Tongues.Num());
}

FVector UASTFireFreeFlameComponent::SampleRoot(const FTongue& Root) const
{
    if (!IsValid(Source)) return FVector::ZeroVector;
    if (Root.SkinnedVertexIndex!=INDEX_NONE)
    {
        if (USkinnedMeshComponent* Skeletal=Cast<USkinnedMeshComponent>(Source))
        {
            USkeletalMesh* Asset=Cast<USkeletalMesh>(Skeletal->GetSkinnedAsset());
            FSkeletalMeshRenderData* Data=Asset?Asset->GetResourceForRendering():nullptr;
            if (Data && Data->LODRenderData.Num()>0)
            {
                const FSkeletalMeshLODRenderData& LOD=Data->LODRenderData[0];
                if (Root.SkinnedVertexIndex<int32(LOD.GetNumVertices()))
                {
                    FSkinWeightVertexBuffer* Weights =
                        const_cast<FSkeletalMeshLODRenderData&>(LOD).GetSkinWeightVertexBuffer();
                    if (Weights)
                    {
                        const FVector3f Skinned=USkinnedMeshComponent::GetSkinnedVertexPosition(
                            Skeletal,Root.SkinnedVertexIndex,LOD,*Weights);
                        return Skeletal->GetComponentTransform().TransformPosition(FVector(Skinned));
                    }
                }
            }
        }
    }
    return Source->GetComponentTransform().TransformPosition(Root.RestPosition);
}

FVector UASTFireFreeFlameComponent::TongueCenterAt(
    const FTongue& Tongue, const FVector& Base, float V,
    const FVector& Right) const
{
    // The same curve feeds the visible tongue AND its first mantle samples.
    const float Fraction=FMath::Clamp(
        Tongue.Age/FMath::Max(.1f,Tongue.GrowSeconds),0.f,1.f);
    const float Growth=ASTFireLife::SmoothStep(Fraction);
    const float Phase=FMath::Lerp(
        Tongue.PreviousPhase,Tongue.Phase,Growth);
    const FVector Flow=(Wind-Tongue.WorldVelocity*.64f)
        .GetClampedToMaxSize(1600.f);
    const float Curl=FMath::Sin(V*8.1f+Elapsed*8.3f+Phase)*
        V*V*(1.2f+5.f*Tongue.Scale);
    const float Curl2=FMath::Sin(V*13.3f-Elapsed*5.1f+Tongue.Seed)*
        V*(.9f+2.8f*Tongue.Scale);
    // Per-tongue buoyant oscillation and a persistent sideways inclination.
    // Root stays fixed (V=0); tips dance independently in wind and inertia.
    const float SwaySpeed=1.7f+3.6f*ASTFireLife::Hash(Tongue.Seed*.31f);
    const float SwayAmplitude=(7.f+23.f*
        ASTFireLife::Hash(Tongue.Seed*.47f))*Tongue.Scale;
    const float Lean=(ASTFireLife::Hash(Tongue.Seed*.79f)*2.f-1.f)*
        FMath::Min(Tongue.CurrentLength*.34f,42.f);
    const float Dance=(FMath::Sin(Elapsed*SwaySpeed+Phase)+
        .37f*FMath::Sin(Elapsed*SwaySpeed*1.81f+Tongue.Seed))*
        SwayAmplitude*FMath::Pow(V,1.35f);
    // Only the last half bends into an asymmetric hooked tip; the shared
    // root and approved wind response are not shifted. The curl is tied
    // to this tongue's own seed rather than an identical sine for all.
    const float Upper=ASTFireLife::SmoothStep((V-.54f)/.43f);
    const float TipHook=Upper*(7.f+13.f*
        ASTFireLife::Hash(Tongue.Seed*.63f))*Tongue.Scale*
        (FMath::Sin(Elapsed*(2.1f+
            ASTFireLife::Hash(Tongue.Seed*.29f)*2.5f)+Tongue.Seed*.37f)
         +.34f*FMath::Sin(V*14.7f-Elapsed*4.6f+Phase));
    // Shorter tongues do not acquire the same 40 cm sideways travel as
    // long ones. The physical wind still drives the visible and emitted tip.
    const float L=FMath::Max(.1f,Tongue.CurrentLength);
    const float S=FMath::Min(1.f,SizeScale);
    const bool bPoint=ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty());
    // Keep low and middle sections close to the shared source; bend only
    // the short upper lobes. Wind and carrier velocity remain physical.
    const float Shape=(Curl*.55f+Curl2*.48f+Dance*.52f+TipHook*.28f)*S+Lean*V*.64f;
    const float Large=FMath::Clamp((SizeScale-.18f)/.82f,0.f,1.f);
    const float MaxSide=FMath::Lerp(.24f,.20f,Large);
    const float Side=FMath::Clamp(Shape*(1.f-.24f*Large),-L*MaxSide*V,L*MaxSide*V);
    const FVector Drift=(Flow*V*V*.095f*S).GetClampedToMaxSize(L*.45f*V*V);
    const FVector Radial=Tongue.Outward.IsNearlyZero()?FVector(FMath::Cos(Tongue.Seed),FMath::Sin(Tongue.Seed),0.f):Tongue.Outward;
    const float Fan=bPoint?FMath::Lerp(.10f,.24f,Large):FMath::Lerp(.08f,.14f,Large);
    const float Lift=1.f-.09f*Large*FMath::Abs(float(FVector::DotProduct(Radial,Right)));
    // Distinct 3D trajectories, not camera-facing convergent tips.
    const FVector Out=Radial*(L*Fan*FMath::Pow(V,1.28f)*(.78f+.22f*FMath::Sin(Phase+Elapsed*2.f+V*4.f)));
    return Base+FVector::UpVector*L*V*Lift+Drift+Right*Side+Out;
}

float UASTFireFreeFlameComponent::TongueRadiusAt(const FTongue& T,float V) const
{
    // Rounded root, fuller lower/central gas, then an asymmetric taper.
    // Radius is one field; the pinch acts on exactly the ring that is cut.
    // Reference silhouette: rounded broad shoulders, full middle, short
    // irregular tips. The radius does not taper along the entire length.
    constexpr float ProfileSamples[9]={.58f,.91f,1.f,.96f,.84f,.70f,.50f,.28f,0.f};
    const float U=FMath::Clamp(V,0.f,1.f)*8.f;
    const int32 Index=FMath::Min(7,FMath::FloorToInt(U));
    const float Profile=FMath::Lerp(ProfileSamples[Index],ProfileSamples[Index+1],U-Index);
    const float Pulse=.93f+.07f*FMath::Sin(V*10.f-T.Age*8.f+T.Phase);
    float Radius=FMath::Min(T.CurrentWidth,T.CurrentLength*.68f)*Profile*Pulse;
    if(T.Life==EFlameLife::Bridging)
    {
        const float Pinch=ASTFireLife::SmoothStep(T.PhaseAge/FMath::Max(.01f,T.BridgeSeconds));
        const float Near=FMath::Exp(-FMath::Square((V-T.RemainingFraction)/.10f));
        Radius*=1.f-Pinch*Near;
    }
    return FMath::Max(0.f,Radius);
}

int32 UASTFireFreeFlameComponent::EmitTip(
    FTongue& Tongue, const FVector& Base, float Length, const FVector& Flow)
{
    // Transfer the EXISTING upper arc. No new shape is generated at the tip.
    int32 Slot=INDEX_NONE;
    for(int32 I=0;I<MaxDetached;++I)
    {
        const int32 Candidate=(DetachCursor+I)%MaxDetached;
        if(!Detached[Candidate].bAlive){Slot=Candidate;break;}
    }
    if(Slot==INDEX_NONE)return INDEX_NONE; // keep pinched arc until capacity frees
    FDetached& Child=Detached[Slot];
    ++DetachCursor;
    const int32 Salt=DetachCursor*137+Tongue.Generation*91;
    FVector View=FVector::ForwardVector;
    if(const UWorld* World=GetWorld())
        if(const APlayerController* PC=World->GetFirstPlayerController())
            if(const APlayerCameraManager* Cam=PC->PlayerCameraManager)
                View=(Cam->GetCameraLocation()-GetOwner()->GetActorLocation()).GetSafeNormal();
    FVector Right=FVector::CrossProduct(FVector::UpVector,View).GetSafeNormal();
    if(Right.IsNearlyZero())Right=FVector::RightVector;
    const int32 Cut=FMath::Clamp(FMath::RoundToInt(Tongue.RemainingFraction*(RingsPerTongue-1)),1,RingsPerTongue-2);
    const bool bSnapshot=Tongue.bDisplayedPinch && Tongue.DisplayGeneration==Tongue.Generation;
    Child.Position=bSnapshot?Tongue.DisplayArc[Cut]:TongueCenterAt(Tongue,Base,float(Cut)/(RingsPerTongue-1),Right);
    Child.ArcCount=RingsPerTongue-Cut;
    Child.Width=0.f;
    for(int32 K=0;K<Child.ArcCount;++K)
    {
        const float V=float(Cut+K)/(RingsPerTongue-1);
        Child.SourceV[K]=V;
        Child.Arc[K]=(bSnapshot?Tongue.DisplayArc[Cut+K]:TongueCenterAt(Tongue,Base,V,Right))-Child.Position;
        Child.Radius[K]=bSnapshot?Tongue.DisplayRadius[Cut+K]:TongueRadiusAt(Tongue,V);
        Child.Width=FMath::Max(Child.Width,Child.Radius[K]);
    }
    Child.Length=Length*(1.f-Tongue.RemainingFraction);
    Child.ParentGeneration=Tongue.Generation;
    Child.BirthClock=bSnapshot?Tongue.DisplayClock:Elapsed;
    Child.BirthPhase=Tongue.Seed+Tongue.Phase;
    Child.BirthHeat=bSnapshot?Tongue.DisplayHeat:Heat;
    Child.Age=0.f;
    const float Large=FMath::Clamp((SizeScale-.18f)/.82f,0.f,1.f);
    Child.Lifetime=FMath::Lerp(.52f,.71f,Large)+FMath::Lerp(.25f,.25f,Large)*Uniform(Salt+3);
    Child.Temperature=1.f;
    Child.Seed=Uniform(Salt+9)*102.f;
    Child.Right=Right;
    const float Travel=FMath::Clamp(Length/90.f,.08f,1.25f);
    const FVector Radial=Tongue.Outward.IsNearlyZero()?FVector(FMath::Cos(Tongue.Seed),FMath::Sin(Tongue.Seed),0.f):Tongue.Outward;
    // The captured geometry is unchanged at birth. Velocity then carries it
    // away from the parent and other roots in world space, not toward camera.
    Child.Velocity=FVector::UpVector*(43.f+30.f*Uniform(Salt+7))*Travel+
        Radial*(FMath::Lerp(32.f,76.f,Large)*Travel)+
        Flow*.18f-Tongue.WorldVelocity*.08f;
    Child.Velocity.Z=FMath::Max(12.f*Travel,float(Child.Velocity.Z));
    Child.bAlive=true;
    return Slot;
}

void UASTFireFreeFlameComponent::AdvanceSimulation(float Dt)
{
    Elapsed += Dt;
    Spread = FMath::Clamp(Spread + Dt * (Heat > .01f ? .23f : 0.f), 0.f, 1.f);
    float SumEnergy = 0.f;
    // Advance older detached arcs BEFORE any new split. At birth, the
    // exported upper geometry is still exactly the parent's last arc.
    for(FDetached& D:Detached)
    {
        if(!D.bAlive)continue;
        D.Age+=Dt;
        if(D.Age>=D.Lifetime){D.bAlive=false;continue;}
        const float Fade=ASTFireLife::ArcFade(D.Age,D.Lifetime);
        const float Travel=FMath::Clamp(D.Length/65.f,.08f,1.20f);
        D.Velocity+=FVector::UpVector*(112.f+110.f*(1.f-Fade))*Travel*Dt+
            FVector(Wind.X,Wind.Y,0.f)*(.07f*Dt);
        D.Position+=D.Velocity*Dt;
        D.Temperature=FMath::Pow(Fade,.88f);
        SumEnergy+=D.BirthHeat*D.Temperature*.15f;
    }
    for(int32 TongueIndex=0;TongueIndex<Tongues.Num();++TongueIndex)
    {
        FTongue& T=Tongues[TongueIndex];
        const FVector Base=SampleRoot(T);
        const FVector Velocity=(Base-T.WorldPrevious)/FMath::Max(.004f,Dt);
        T.WorldPrevious=Base;
        T.WorldVelocity=FMath::VInterpTo(T.WorldVelocity,Velocity,Dt,13.f);
        if(!T.bActive)
        {
            if(Spread<T.ActivationSpread)continue;
            T.bActive=true;T.Age=-.06f;
        }
        if(T.Age<0.f){T.Age+=Dt;continue;}
        if(Heat<.015f)continue;
        if(T.Life==EFlameLife::Growing)
        {
            T.Age+=Dt;
            const float Growth=ASTFireLife::SmoothStep(FMath::Max(0.f,T.Age-T.RebirthDelay)/FMath::Max(.1f,T.GrowSeconds));
            T.CurrentLength=FMath::Lerp(T.RestartLength,T.MaxLength,Growth);
            T.CurrentWidth=FMath::Lerp(T.PreviousWidth,T.BaseWidth,Growth);
            if(T.Age>=T.GrowSeconds+T.RebirthDelay)
            {
                T.SplitLength=T.CurrentLength=T.MaxLength;
                T.BridgeSeconds=.11f+.10f*Uniform(T.Generation*83+int32(T.Seed)*9);
                T.PhaseAge=0.f;
                T.Life = EFlameLife::Bridging;
            }
        }
        else
        {
            // Pinch only. The upper arc does not retract toward the root.
            const bool bCutReady=T.bDisplayedPinch && T.DisplayGeneration==T.Generation;
            T.PhaseAge=FMath::Min(T.BridgeSeconds,T.PhaseAge+Dt);
            if(T.PhaseAge>=T.BridgeSeconds && bCutReady)
            {
                const FVector Flow=(Wind-T.WorldVelocity*.65f).GetClampedToMaxSize(1600.f);
                // Correct previous render root for the carrier's movement.
                const FVector VisualBase=ConnectedRoots.IsValidIndex(TongueIndex)?
                    ConnectedRoots[TongueIndex]+Velocity*Dt:Base;
                const float Remaining=T.SplitLength*T.RemainingFraction;
                const int32 Child=EmitTip(T,VisualBase,T.SplitLength,Flow);
                if(Child!=INDEX_NONE)
                {
                    // Atomic split: the upper arc is now world-space gas and
                    // the lower part is the seed of an independent generation.
                    T.PreviousPhase=T.Phase;
                    T.Age=0.f;T.PhaseAge=0.f;
                    ConfigureGeneration(T);
                    const float ResetCap=T.SplitLength*FMath::Lerp(.20f,.15f,FMath::Clamp(SizeScale,0.f,1.f));
                    T.RestartLength=FMath::Min(FMath::Min(Remaining,ResetCap),T.MaxLength*.18f);
                    T.CurrentLength=T.RestartLength;
                    T.PreviousWidth=T.CurrentWidth=FMath::Min(T.CurrentWidth,T.BaseWidth*.48f);
                    T.BridgedChild=INDEX_NONE;
                    T.bDisplayedPinch=false;
                    T.Life = EFlameLife::Growing;
                }
            }
        }
        SumEnergy+=Heat*(.40f+.60f*FMath::Clamp(T.CurrentLength/FMath::Max(1.f,T.MaxLength),0.f,1.2f));
    }
    VisualEnergy=FMath::FInterpTo(VisualEnergy,Tongues.IsEmpty()?0.f:
        FMath::Clamp(SumEnergy/FMath::Max(1,Tongues.Num())*.81f,.05f,1.2f),Dt,8.f);
    UpdateSurfaceMap(Dt);
}

void UASTFireFreeFlameComponent::UpdateSurfaceMap(float Dt)
{
    if (!IsValid(SurfaceMID)) return;
    SurfaceMID->SetScalarParameterValue(FName(TEXT("FireSpread")),Spread);
    // Cluster positions are in rest-pose mesh-local space: upload once.
    // The GPU grows their radii from FireSpread, merging adjacent patches.
    if (bClusterMapUploaded) return;
    FVector LocalSize=FVector(110,110,110);
    if (const UPrimitiveComponent* Primitive=Cast<UPrimitiveComponent>(Source))
        LocalSize=Primitive->CalcBounds(FTransform::Identity).BoxExtent*2.f;
    const float Radius=FMath::Clamp(LocalSize.GetMax()*.63f,12.f,420.f);
    for (int32 I=0; I<SurfaceClusters; ++I)
    {
        FLinearColor P(0,0,0,-1);
        if (BurnClusters.IsValidIndex(I))
        {
            const FBurnCluster& C=BurnClusters[I];
            P=FLinearColor(C.RestCenter.X,C.RestCenter.Y,C.RestCenter.Z,
                FMath::Min(Radius,C.Radius));
        }
        // Material VectorParameter's main output is RGB, not RGBA.
        // The old .w used by the custom HLSL never compiled on PCD3D_SM6.
        SurfaceMID->SetVectorParameterValue(
            FName(*FString::Printf(TEXT("FireCluster%d"),I)),P);
        SurfaceMID->SetScalarParameterValue(
            FName(*FString::Printf(TEXT("FireClusterRadius%d"),I)),
            BurnClusters.IsValidIndex(I) ? P.A : 0.f);
    }
    bClusterMapUploaded = true;
}

// Stage23: independent short-lived ember pool. Does not modify tongue/bridge
// cycles or the approved detached vortices. Fixed topology and 25 Hz updates.
// Stage32: surface-perimeter sparks, not vertically-launched projectiles.
// The actual position is the advected point PLUS a perpendicular harmonic
// oscillation; tangential directions include left/right/diagonals and top.
void UASTFireFreeFlameComponent::UpdateSparks(float Dt)
{
    for (FSpark& S:Sparks)
    {
        if (!S.bAlive) continue;
        S.Age+=Dt;
        if (S.Age>=S.Lifetime) { S.bAlive=false; continue; }
        // The base flight may arc upward slowly. It is NOT forced onto Z.
        S.Velocity+=(FVector::UpVector*12.f+
            (Wind*.035f).GetClampedToMaxSize(25.f)-
            S.Velocity*.48f)*Dt;
        S.Velocity=S.Velocity.GetClampedToMaxSize(135.f);
        S.DriftPosition+=S.Velocity*Dt;
        const float Offset=FMath::Sin(
            S.WavePhase+S.Age*S.WaveFrequency)-
            FMath::Sin(S.WavePhase);
        const float Second=.28f*FMath::Sin(
            S.WavePhase*.71f+S.Age*S.WaveFrequency*.49f)-
            .28f*FMath::Sin(S.WavePhase*.71f);
        S.Position=S.DriftPosition+
            S.WaveSide*(S.WaveAmplitude*(Offset+Second));
    }
    if (!bVisible || Heat<.15f || Tongues.IsEmpty()) return;

    FVector CameraScreenRight=FVector::RightVector;
    FVector CameraForward=FVector::ForwardVector;
    if (const UWorld* World=GetWorld())
        if (const APlayerController* PC=World->GetFirstPlayerController())
            if (const APlayerCameraManager* Cam=PC->PlayerCameraManager)
            {
                CameraScreenRight=Cam->GetCameraRotation()
                    .RotateVector(FVector::RightVector);
                CameraForward=Cam->GetCameraRotation()
                    .RotateVector(FVector::ForwardVector);
            }
    CameraScreenRight.Z=0.f;
    CameraScreenRight=CameraScreenRight.GetSafeNormal();
    if (CameraScreenRight.IsNearlyZero())
        CameraScreenRight=FVector::RightVector;

    SparkSpawnDebt=FMath::Min(4.f,SparkSpawnDebt+
        Dt*(4.f+9.f*Heat*FMath::Clamp(Energy,.2f,1.2f)));
    const int32 Born=FMath::Min(3,FMath::FloorToInt(SparkSpawnDebt));
    SparkSpawnDebt-=Born;
    for (int32 I=0;I<Born;++I)
    {
        const int32 Salt=SparkCursor++;
        const int32 Start=Salt%Tongues.Num();
        const FTongue* Chosen=nullptr;
        int32 Index=INDEX_NONE;
        for (int32 J=0;J<Tongues.Num();++J)
        {
            const int32 Candidate=(Start+J)%Tongues.Num();
            if (Tongues[Candidate].bActive && Tongues[Candidate].Age>=0.f)
            {
                Chosen=&Tongues[Candidate];
                Index=Candidate;
                break;
            }
        }
        if (!Chosen) continue;
        const FTongue& T=*Chosen;
        FSpark& S=Sparks[Salt%MaxSparks];
        const float Random=ASTFireLife::Hash(float(Salt)+T.Seed*.91f);
        const float Other=ASTFireLife::Hash(float(Salt)*.71f+T.Seed);
        const float Angle=2.f*PI*ASTFireLife::Hash(
            float(Salt)*.419f+T.Seed*.31f);
        // Circle in the camera plane: both sides, upward diagonals, and
        // occasional downward/outward embers. Depth is independently varied.
        const FVector Outward=(CameraScreenRight*FMath::Cos(Angle)+
            FVector::UpVector*FMath::Sin(Angle)+
            CameraForward*((Other-.5f)*.27f)).GetSafeNormal();
        const FVector Root=ConnectedRoots.IsValidIndex(Index)
            ? ConnectedRoots[Index] : SampleRoot(T);
        const float HeightFraction=.08f+.86f*Random;
        S.Seed=float(Salt)*.731f+T.Seed*.38f;
        S.DriftPosition=Root+
            FVector::UpVector*(T.CurrentLength*HeightFraction)+
            Outward*(T.CurrentWidth*(.45f+.40f*Other));
        S.Position=S.DriftPosition;
        S.Velocity=Outward*(45.f+55.f*FMath::Abs(Other))+
            FVector::UpVector*8.f+
            (T.WorldVelocity*.045f+Wind*.025f).GetClampedToMaxSize(20.f);
        S.Velocity=S.Velocity.GetClampedToMaxSize(125.f);
        S.WaveSide=FVector::CrossProduct(
            Outward,CameraForward).GetSafeNormal();
        if (S.WaveSide.IsNearlyZero())
            S.WaveSide=CameraScreenRight;
        S.WaveAmplitude=5.f+9.f*FMath::Abs(Random);
        S.WaveFrequency=3.0f+2.3f*FMath::Abs(Other);
        S.WavePhase=2.f*PI*ASTFireLife::Hash(
            float(Salt)*.57f+T.Seed*.29f);
        S.Size=2.8f+2.9f*Other;
        S.Lifetime=.68f+.70f*Random;
        S.EmissionHeat=FMath::Clamp(Heat*(.86f+.14f*Other),.35f,1.f);
        S.Age=0.f;
        S.bAlive=true;
    }
}

// Stage33: slow, scale-dependent smoke. Invisible carriers transport mass;
// the visible field is reconstructed separately. No centreline links.
void UASTFireFreeFlameComponent::UpdateSmoke(float Dt)
{
    if (!IsValid(SmokeRender) || !GetWorld() || !GetOwner()) return;
    SmokeSimDebt+=Dt;
    if (SmokeSimDebt<.079f) return;
    Dt=FMath::Min(SmokeSimDebt,.12f);
    SmokeSimDebt=0.f;
    UWorld* World=GetWorld();
    // Use a blocking trace, not object-type overlaps: ignition zones
    // deliberately overlap WorldDynamic and must not act like walls.
    constexpr ECollisionChannel SmokeChannel=ECC_Visibility;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(ASTSmoke),false,GetOwner());
    Query.bFindInitialOverlaps=true;

    // Evaluate the actual live flame envelope, not the entire actor bounds.
    // Smooth dimensions only: old smoke must stay in world space.
    FBox FlameBounds(ForceInit);
    FVector Highest=GetOwner()->GetActorLocation();
    FVector HighestRoot=Highest;
    FBox RootBounds(ForceInit);
    float Peak=-BIG_NUMBER,WidthSum=0.f,LengthSum=0.f,ActiveArea=0.f;
    int32 Active=0;
    FVector TipRight=FVector::RightVector;
    if (const APlayerController* PC=World->GetFirstPlayerController())
        if (const APlayerCameraManager* Cam=PC->PlayerCameraManager)
        {
            const FVector V=(Cam->GetCameraLocation()-
                GetOwner()->GetActorLocation()).GetSafeNormal();
            const FVector R=FVector::CrossProduct(FVector::UpVector,V)
                .GetSafeNormal();
            if (!R.IsNearlyZero()) TipRight=R;
        }
    for (const FTongue& T:Tongues)
    {
        if (!T.bActive || T.Age<0.f || T.CurrentLength<3.f) continue;
        const FVector Root=SampleRoot(T);
        const FVector Tip=TongueCenterAt(T,Root,1.f,TipRight);
        FlameBounds+=Root;
        FlameBounds+=Tip;
        RootBounds+=Root;
        if (Tip.Z>Peak) { Peak=Tip.Z; Highest=Tip; HighestRoot=Root; }
        WidthSum+=FMath::Max(1.f,T.CurrentWidth);
        LengthSum+=T.CurrentLength;
        ActiveArea+=T.CurrentWidth*T.CurrentLength;
        ++Active;
    }
    if (Active>0)
    {
        const FVector Size=FlameBounds.GetSize();
        const float Width=WidthSum/Active;
        const float MeanLength=LengthSum/Active;
        const FVector RootSize=RootBounds.GetSize();
        const float Footprint=FMath::Max(RootSize.X,RootSize.Y);
        // Sum the active burning area across roots. The former average
        // assigned almost the same smoke to a candle and a burning body.
        const float BaseSize=Width*1.4f+Footprint*.30f+MeanLength*.25f;
        const float AreaSize=.84f*FMath::Sqrt(FMath::Max(0.f,ActiveArea))+
            Footprint*.32f;
        const float Measured=FMath::Clamp(FMath::Max(BaseSize,AreaSize)*
            FMath::Sqrt(FMath::Clamp(Heat,.02f,1.f)),8.f,280.f);
        SmokeFireSize=SmokeFireSize>0.f?
            FMath::FInterpTo(SmokeFireSize,Measured,Dt,1.8f):Measured;
        SmokeFireTop=Highest;
    }
    const float Scale=FMath::Clamp((SmokeFireSize-16.f)/220.f,0.f,1.f);
    const float Abundance=ASTFireLife::SmoothStep((SmokeFireSize-24.f)/90.f);

    // Light cache: actual point/spot components, including BP lights.
    // Cache only the strongest nearby candidates, not a per-pixel world scan.
    SmokeLightScanDebt-=Dt;
    if (SmokeLightScanDebt<=0.f)
    {
        SmokeLightScanDebt=2.f+.4f*FMath::Abs(Uniform(772));
        NearbySmokeLights.Reset();
        struct FLightCandidate { UPointLightComponent* Light; float Score; };
        TArray<FLightCandidate> Candidates;
        for (TObjectIterator<UPointLightComponent> It;It;++It)
        {
            UPointLightComponent* L=*It;
            if (!IsValid(L) || L->GetWorld()!=World || !L->IsRegistered() ||
                !L->IsVisible() || L->Intensity<=0.f) continue;
            const float R=FMath::Max(50.f,L->AttenuationRadius);
            const float D=FVector::Dist(L->GetComponentLocation(),SmokeFireTop);
            if (D>R+1000.f) continue;
            Candidates.Add({L, (R+100.f)/(D+100.f)*
                (L->Intensity/(L->Intensity+100.f))});
        }
        Candidates.Sort([](const FLightCandidate& A,const FLightCandidate& B)
            { return A.Score>B.Score; });
        for (int32 I=0;I<FMath::Min(8,Candidates.Num());++I)
            NearbySmokeLights.Add(Candidates[I].Light);
    }

    // Collision volume is also the exact support of visible density.
    // A growing carrier can shrink under a ceiling or next to a wall.
    const auto IsClear=[&](const FVector& P,float Radius)
    {
        return !World->OverlapBlockingTestByChannel(P,FQuat::Identity,
            SmokeChannel,FCollisionShape::MakeSphere(Radius+1.5f),Query);
    };
    for (FSmoke& S:Smoke)
    {
        if (!S.bAlive) continue;
        S.Age+=Dt;
        if (S.Age>=S.Lifetime) { S.bAlive=false; continue; }
        const float Age=FMath::Clamp(S.Age/S.Lifetime,0.f,1.f);
        const float Height=FMath::Max(0.f,float(S.Position.Z-S.BaseZ));
        // Cap the complete visible carrier (not merely the centre).
        const float Headroom=S.MaxRise-Height-2.f;
        if (Headroom<3.f) { S.bAlive=false; continue; }
        const float Growth=1.f+1.5f*ASTFireLife::SmoothStep(Age/.68f);
        const float Desired=FMath::Min(Headroom,S.InitialRadius*Growth*
            (.91f+.09f*FMath::Sin(S.Seed+S.Age*.8f)));
        // Never shrink a cloud just because the next growth step touches
        // a roof. Keep its last clear radius; shrink only on actual intrusion.
        float Radius=FMath::Min(Headroom,S.Radius);
        for(int32 Try=0;Try<6 && Radius>=3.f && !IsClear(S.Position,Radius);++Try)
            Radius*=.78f;
        if(Radius<3.f || !IsClear(S.Position,Radius))
        { S.bAlive=false; continue; }
        const float Candidate=FMath::Min(Desired,Radius+Dt*8.f);
        if(Candidate>=3.f && IsClear(S.Position,Candidate)) Radius=Candidate;
        S.Radius=Radius;
        const float Phase=float(S.Position.Z-S.BaseZ)*.013f+
            Elapsed*.43f+S.Seed*.20f;
        const FVector Eddy(
            FMath::Sin(Phase)*(.14f*S.RiseSpeed)+
                FMath::Sin(Phase*1.7f)*3.f,
            FMath::Cos(Phase*.83f+1.f)*(.12f*S.RiseSpeed),
            S.RiseSpeed*(.91f+.08f*FMath::Sin(Phase*.55f)));
        const FVector Target=Eddy+
            FVector(Wind.X,Wind.Y,0.f).GetClampedToMaxSize(450.f)*.035f;
        S.Velocity=FMath::VInterpTo(S.Velocity,Target,Dt,1.25f);
        FVector Delta=S.Velocity*Dt;
        Delta.Z=FMath::Min(float(Delta.Z),FMath::Max(0.f,Headroom-Radius));
        // Match the overlap clearance exactly. Maintain a last-known-safe
        // position for BOTH the approach and slide legs. A grazing sweep
        // reporting initial penetration is a blocked movement, not a death.
        const FCollisionShape Shape=FCollisionShape::MakeSphere(Radius+1.5f);
        const auto MoveClear=[&](const FVector& Start,const FVector& Motion,FHitResult& Out)->FVector
        {
            if(Motion.IsNearlyZero()) return Start;
            const bool bHit=World->SweepSingleByChannel(Out,Start,Start+Motion,
                FQuat::Identity,SmokeChannel,Shape,Query);
            FVector End=bHit ? (Out.bStartPenetrating?Start:Out.Location+Out.Normal*2.f)
                            : Start+Motion;
            if(IsClear(End,Radius)) return End;
            // Numerical contact tolerances can disagree between sweep and
            // overlap. Find a safe point along the already-swept segment.
            float Lo=0.f,Hi=bHit?FMath::Clamp(Out.Time,0.f,1.f):1.f;
            for(int32 K=0;K<6;++K)
            {
                const float Mid=(Lo+Hi)*.5f;
                if(IsClear(Start+Motion*Mid,Radius)) Lo=Mid;
                else Hi=Mid;
            }
            const FVector Safe=Start+Motion*Lo;
            return IsClear(Safe,Radius)?Safe:Start;
        };
        const FVector Previous=S.Position;
        FHitResult Hit;
        S.Position=MoveClear(Previous,Delta,Hit);
        if(Hit.bBlockingHit)
        {
            FVector Normal=Hit.Normal.GetSafeNormal();
            if(Normal.IsNearlyZero()) Normal=Hit.ImpactNormal.GetSafeNormal();
            if(Normal.IsNearlyZero()) Normal=-Delta.GetSafeNormal();
            FVector Slide=FVector::VectorPlaneProject(
                Delta*(1.f-FMath::Clamp(Hit.Time,0.f,1.f)),Normal);
            S.Velocity=FVector::VectorPlaneProject(S.Velocity,Normal);
            if(Normal.Z<-.45f)
            {
                // Persistent tangent momentum keeps the mass sliding under
                // a roof; gravity/buoyancy can lift it again past the edge.
                const FVector Side(FMath::Cos(S.Seed),FMath::Sin(S.Seed),0.f);
                const FVector Tangent=FVector::VectorPlaneProject(Side,Normal).GetSafeNormal();
                Slide+=Tangent*S.RiseSpeed*.55f*Dt;
                S.Velocity+=Tangent*S.RiseSpeed*.24f;
                S.Velocity=S.Velocity.GetClampedToMaxSize(S.RiseSpeed*1.15f);
            }
            FHitResult Second;
            S.Position=MoveClear(S.Position,Slide,Second);
        }
        if(!IsClear(S.Position,Radius)) S.Position=Previous;
        // A valid carrier remains alive under an obstruction until its
        // ordinary lifespan runs out. The ceiling itself cannot delete it.
        const float Remaining=S.MaxRise-FMath::Max(0.f,float(S.Position.Z-S.BaseZ))-2.f;
        S.Radius=FMath::Min(Radius,Remaining);
        if(S.Radius<3.f) { S.Position=Previous; S.Radius=Radius; }
    }

    const bool bSmolder=SmokeAfterglowSeconds>0.f;
    const float Tail=FMath::Clamp(SmokeAfterglowSeconds/3.f,0.f,1.f);
    SmokeAfterglowSeconds=FMath::Max(0.f,SmokeAfterglowSeconds-Dt);
    if ((bVisible && Heat>.10f && Active>0) || bSmolder)
    {
        // Small flames produce modest smoke. 10m is a cap, not a default.
        const float Rate=FMath::Lerp(4.2f,8.5f,Scale)*
            FMath::Lerp(1.f,1.48f,Abundance)*FMath::Lerp(.72f,1.f,Heat)*(bSmolder?.35f*Tail:1.f);
        SmokeSpawnDebt=FMath::Min(3.f,SmokeSpawnDebt+Dt*Rate);
        const int32 Born=FMath::FloorToInt(SmokeSpawnDebt);
        SmokeSpawnDebt-=Born;
        for (int32 I=0;I<Born;++I)
        {
            const int32 Salt=SmokeCursor++;
            FSmoke* Slot=nullptr;
            for (int32 J=0;J<Smoke.Num();++J)
            {
                FSmoke& Candidate=Smoke[(Salt+J)%Smoke.Num()];
                if (!Candidate.bAlive) { Slot=&Candidate; break; }
            }
            if (!Slot) break; // never pop a living cloud to make room
            const float R=FMath::Abs(Uniform(Salt*7+441));
            const float J=FMath::Abs(Uniform(Salt*11+617));
            FSmoke& S=*Slot;
            S.InitialRadius=FMath::Lerp(10.f,46.f,Scale)*(.90f+.20f*R)*
                FMath::Lerp(1.f,1.38f,Abundance);
            S.Radius=S.InitialRadius;
            S.MaxRise=FMath::Lerp(125.f,1000.f,FMath::Pow(Scale,1.25f))*
                (.83f+.17f*J);
            S.RiseSpeed=FMath::Lerp(35.f,85.f,Scale)*(.94f+.12f*R);
            S.Lifetime=FMath::Min(FMath::Min(14.f,float(MaxSmoke-2)/FMath::Max(.01f,Rate)),(S.MaxRise-S.Radius)/
                S.RiseSpeed*(1.04f+.08f*J));
            S.Seed=Salt*.713f+Seed*.031f;
            // Bottom of carrier starts ABOVE the highest active tongue tip.
            FVector Spawn=SmokeFireTop+FVector::UpVector*(S.Radius+6.f);
            // A low ceiling can prevent the desired spawn; use a smaller,
            // clear lateral pocket or reject, never teleport through roof.
            const float Angle=S.Seed*2.39996f;
            const FVector Side(FMath::Cos(Angle),FMath::Sin(Angle),0.f);
            bool bFound=false;
            // Check the route with a thin ray; a full-radius sweep starting
            // at the flame tip would reject otherwise valid nearby pockets.
            FHitResult Roof;
            const FVector ProbeEnd=SmokeFireTop+FVector::UpVector*
                (S.InitialRadius+12.f);
            const bool bRoof=World->LineTraceSingleByChannel(
                Roof,HighestRoot,ProbeEnd,SmokeChannel,Query);
            for (int32 Attempt=0;Attempt<4;++Attempt)
            {
                const float TryRadius=FMath::Max(4.f,S.InitialRadius*
                    (1.f-.22f*Attempt));
                FVector P=SmokeFireTop+FVector::UpVector*
                    (TryRadius+6.f)+Side*(Attempt*TryRadius*.65f);
                if (bRoof)
                {
                    // Under a roof, start on its near side and fan sideways.
                    // Never seed a carrier on the far side of the obstacle.
                    const FVector Tangent=FVector::VectorPlaneProject(
                        Side,Roof.ImpactNormal).GetSafeNormal();
                    const FVector Near=Roof.ImpactPoint+
                        Roof.ImpactNormal*(TryRadius+3.f);
                    P=Near+Tangent*(TryRadius*(1.f+.65f*Attempt));
                    FHitResult SideHit;
                    if (World->SweepSingleByChannel(SideHit,Near,P,
                        FQuat::Identity,SmokeChannel,
                        FCollisionShape::MakeSphere(TryRadius+1.f),Query))
                        continue;
                }
                else
                {
                    FHitResult SpawnHit;
                    if (World->LineTraceSingleByChannel(SpawnHit,
                        SmokeFireTop,P,SmokeChannel,Query)) continue;
                }
                if (IsClear(P,TryRadius))
                { Spawn=P; S.Radius=S.InitialRadius=TryRadius; bFound=true; break; }
            }
            if (!bFound) continue;
            S.Position=Spawn;
            S.BaseZ=SmokeFireTop.Z;
            S.Velocity=FVector::UpVector*S.RiseSpeed*.75f+
                Side*S.RiseSpeed*.08f;
            S.Age=0.f;
            S.bAlive=true;
        }
    }
    UpdateSmokeGeometry();
}

// Perspective-correct density raster, with per-cell collision-safe depth.
// Support is bounded by the swept sphere, including the square's corners.
// Overlapping supports produce one field, not individual circles or ribbons.
void UASTFireFreeFlameComponent::UpdateSmokeGeometry()
{
    if (!IsValid(SmokeRender) || !IsValid(SmokeMaterial)) return;
    FVector Cam=GetOwner()->GetActorLocation()+FVector::ForwardVector*700.f;
    FVector Forward=(GetOwner()->GetActorLocation()-Cam).GetSafeNormal();
    FVector Right=FVector::RightVector;
    if (const APlayerController* PC=GetWorld()->GetFirstPlayerController())
        if (const APlayerCameraManager* C=PC->PlayerCameraManager)
        {
            Cam=C->GetCameraLocation();
            Forward=C->GetCameraRotation().RotateVector(FVector::ForwardVector);
            Right=C->GetCameraRotation().RotateVector(FVector::RightVector);
        }
    Right=(Right-Forward*FVector::DotProduct(Right,Forward)).GetSafeNormal();
    if (Right.IsNearlyZero()) return;
    const FVector Up=FVector::CrossProduct(Forward,Right).GetSafeNormal();
    constexpr float Reference=600.f; // fixed projection plane, stable pixel pitch
    struct FProjected
    {
        FVector2D Center;
        float Radius,Depth,Fade,Height,Light;
        FVector2D LightDir;
    };
    TArray<FProjected> Supports;
    FVector2D Min( BIG_NUMBER, BIG_NUMBER),Max(-BIG_NUMBER,-BIG_NUMBER);
    for (const FSmoke& S:Smoke)
    {
        if (!S.bAlive) continue;
        const FVector Rel=S.Position-Cam;
        const float Depth=FVector::DotProduct(Rel,Forward);
        if (Depth<S.Radius+10.f) continue;
        const float Projection=Reference/Depth;
        const FVector2D P(FVector::DotProduct(Rel,Right)*Projection,
            FVector::DotProduct(Rel,Up)*Projection);
        const float Radius=S.Radius*Projection;
        const float Fade=FMath::Clamp((S.Lifetime-S.Age)/
            FMath::Max(.1f,S.Lifetime*.24f),0.f,1.f);
        const float Height=FMath::Clamp(
            float(S.Position.Z-S.BaseZ)/S.MaxRise,0.f,1.f);
        FVector ToLight=(SmokeFireTop-S.Position).GetSafeNormal();
        float Light=Heat*.65f*FMath::Square(FMath::Max(0.f,1.f-Height));
        FVector LightVector=ToLight*Light;
        for (const auto& Weak:NearbySmokeLights)
        {
            const UPointLightComponent* L=Weak.Get();
            if (!IsValid(L) || !L->IsVisible() || L->Intensity<=0.f) continue;
            const FVector Delta=L->GetComponentLocation()-S.Position;
            const float D=Delta.Size();
            const float R=FMath::Max(50.f,L->AttenuationRadius);
            if (D>=R || D<.1f) continue;
            const FVector Dir=Delta/D;
            if (const USpotLightComponent* Spot=Cast<USpotLightComponent>(L))
                if (FVector::DotProduct(-Dir,Spot->GetForwardVector())<
                    FMath::Cos(FMath::DegreesToRadians(Spot->OuterConeAngle)))
                    continue;
            const float Amount=.65f*FMath::Square(1.f-D/R)*
                (L->Intensity/(L->Intensity+100.f));
            Light+=Amount;
            LightVector+=Dir*Amount;
        }
        const FVector2D LightDir(FVector::DotProduct(LightVector,Right),
            FVector::DotProduct(LightVector,Up));
        Supports.Add({P,Radius,Depth,Fade,Height,
            FMath::Clamp(Light,0.f,1.f),LightDir.GetSafeNormal()});
        Min.X=FMath::Min(Min.X,P.X-Radius);Min.Y=FMath::Min(Min.Y,P.Y-Radius);
        Max.X=FMath::Max(Max.X,P.X+Radius);Max.Y=FMath::Max(Max.Y,P.Y+Radius);
    }
    if (Supports.IsEmpty())
    {
        SmokeRender->SetVisibility(false);
        return;
    }
    // Hard pixel edges. Adapt pitch to the footprint, not a fixed 10m grid.
    const float Required=FMath::Max(.75f,float(FMath::Max(
        (Max.X-Min.X)/(SmokeGridMaxX-2),(Max.Y-Min.Y)/(SmokeGridMaxY-2))));
    // Quantized pitch with hysteresis: no continuously changing raster.
    if (SmokePixelPitch<=0.f || Required>SmokePixelPitch ||
        Required<SmokePixelPitch*.43f)
        SmokePixelPitch=.75f*FMath::Pow(2.f,FMath::CeilToFloat(
            FMath::Log2(Required/.75f)));
    const float Pitch=SmokePixelPitch;
    Min.X=FMath::FloorToDouble(Min.X/Pitch)*Pitch;
    Min.Y=FMath::FloorToDouble(Min.Y/Pitch)*Pitch;
    const int32 NX=FMath::Clamp(FMath::CeilToInt((Max.X-Min.X)/Pitch)+1,1,SmokeGridMaxX);
    const int32 NY=FMath::Clamp(FMath::CeilToInt((Max.Y-Min.Y)/Pitch)+1,1,SmokeGridMaxY);
    struct FCell { float Density=0.f,Best=0.f,Alpha=0.f; int32 Carrier=INDEX_NONE; };
    TArray<FCell> Grid;
    Grid.SetNum(NX*NY);
    for (int32 I=0;I<Supports.Num();++I)
    {
        const FProjected& S=Supports[I];
        // Every corner fits inside the support sphere at this depth.
        const float SafeRadius=S.Radius-Pitch*.72f;
        if (SafeRadius<=0.f) continue;
        const int32 X0=FMath::Clamp(FMath::FloorToInt((S.Center.X-SafeRadius-Min.X)/Pitch),0,NX-1);
        const int32 X1=FMath::Clamp(FMath::CeilToInt((S.Center.X+SafeRadius-Min.X)/Pitch),0,NX-1);
        const int32 Y0=FMath::Clamp(FMath::FloorToInt((S.Center.Y-SafeRadius-Min.Y)/Pitch),0,NY-1);
        const int32 Y1=FMath::Clamp(FMath::CeilToInt((S.Center.Y+SafeRadius-Min.Y)/Pitch),0,NY-1);
        for (int32 Y=Y0;Y<=Y1;++Y) for (int32 X=X0;X<=X1;++X)
        {
            const FVector2D P=Min+FVector2D((X+.5f)*Pitch,(Y+.5f)*Pitch);
            const float Q=(P-S.Center).SizeSquared()/FMath::Square(SafeRadius);
            if (Q>=1.f) continue;
            const float Mass=FMath::Square(1.f-Q)*FMath::Lerp(.58f,1.f,S.Fade);
            FCell& Cell=Grid[Y*NX+X];
            Cell.Density+=Mass;
            Cell.Alpha=FMath::Max(Cell.Alpha,S.Fade);
            if (Mass>Cell.Best)
            { Cell.Best=Mass; Cell.Carrier=I; }
        }
    }
    const float Palette[SmokeTones]={.018f,.065f,.17f,.36f,.64f};
    TArray<FVector> V,N;
    TArray<FVector2D> UV;
    TArray<FLinearColor> C;
    TArray<int32> Tris;
    TArray<FProcMeshTangent> Tangents;
    const FTransform Transform=SmokeRender->GetComponentTransform();
    const auto Density=[&](int32 X,int32 Y)->float
    {
        return X>=0 && X<NX && Y>=0 && Y<NY?Grid[Y*NX+X].Density:0.f;
    };
    for (int32 Y=0;Y<NY;++Y) for (int32 X=0;X<NX;++X)
    {
        const FCell& Cell=Grid[Y*NX+X];
        if (Cell.Carrier==INDEX_NONE || Cell.Density<.19f || Cell.Alpha<.015f) continue;
        const FProjected& S=Supports[Cell.Carrier];
        // Density gradient gives lobe contours, depth and self-shading.
        // Five hard grey bands; lighting direction is evaluated in plane.
        const FVector2D Gradient(Density(X-1,Y)-Density(X+1,Y),
            Density(X,Y-1)-Density(X,Y+1));
        const float Facing=FMath::Max(0.f,float(FVector2D::DotProduct(
            Gradient.GetSafeNormal(),S.LightDir)));
        const float Core=FMath::Clamp((Cell.Density-.35f)/1.5f,0.f,1.f);
        // Facing is directional even in a single carrier; dense interiors
        // and distant regions fall into dark tones, not a flat middle grey.
        const float Shade=FMath::Clamp(.39f-.32f*S.Height+
            .43f*S.Light+.42f*Facing-.32f*Core,0.f,1.f);
        const int32 Tone=FMath::Clamp(FMath::FloorToInt(Shade*5.f),0,SmokeTones-1);
        const float Grey=Palette[Tone];
        const FLinearColor Color(Grey,Grey,Grey,Cell.Alpha);
        const int32 First=V.Num();
        for (int32 K=0;K<4;++K)
        {
            const FVector2D P=Min+FVector2D(
                (X+(K%2))*Pitch,(Y+(K/2))*Pitch);
            const FVector WorldP=Cam+Forward*S.Depth+
                (Right*P.X+Up*P.Y)*(S.Depth/Reference);
            V.Add(Transform.InverseTransformPosition(WorldP));
            N.Add(Transform.InverseTransformVectorNoScale(-Forward));
            UV.Add(FVector2D(K%2,K/2));C.Add(Color);
            Tangents.Add(FProcMeshTangent(1,0,0));
        }
        Tris.Add(First);Tris.Add(First+2);Tris.Add(First+1);
        Tris.Add(First+1);Tris.Add(First+2);Tris.Add(First+3);
    }
    if (V.IsEmpty()) { SmokeRender->SetVisibility(false); return; }
    SmokeRender->CreateMeshSection_LinearColor(0,V,Tris,N,UV,C,Tangents,false);
    SmokeRender->SetMaterial(0,SmokeMaterial);
    SmokeRender->SetVisibility(true);
}


FString UASTFireFreeFlameComponent::RunSmokeAudit(UMaterialInterface* TestMaterial)
{
#if WITH_EDITOR
    if (!IsValid(TestMaterial)) return TEXT("FAIL:NO_MATERIAL");
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if (!World) return TEXT("FAIL:NO_WORLD");
    FString Report=TEXT("FAIL:UNINITIALIZED");
    AActor* Host=World->SpawnActor<AActor>();
    AActor* RoofActor=World->SpawnActor<AActor>();
    AActor* TriggerActor=World->SpawnActor<AActor>();
    if (Host && RoofActor && TriggerActor)
    {
        // Real engine collision components in an isolated, unsaved world.
        UBoxComponent* Roof=NewObject<UBoxComponent>(RoofActor);
        RoofActor->SetRootComponent(Roof);
        Roof->SetBoxExtent(FVector(65,65,8));
        Roof->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Roof->SetCollisionObjectType(ECC_WorldStatic);
        Roof->SetCollisionResponseToAllChannels(ECR_Block);
        Roof->RegisterComponent();
        Roof->SetWorldLocation(FVector(0,0,128));
        UBoxComponent* Trigger=NewObject<UBoxComponent>(TriggerActor);
        TriggerActor->SetRootComponent(Trigger);
        Trigger->SetBoxExtent(FVector(100,100,100));
        Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Trigger->SetCollisionObjectType(ECC_WorldDynamic);
        Trigger->SetCollisionResponseToAllChannels(ECR_Overlap);
        Trigger->RegisterComponent();
        Trigger->SetWorldLocation(FVector(0,0,70));
        FHitResult Check;
        const bool bRoof=World->LineTraceSingleByChannel(Check,
            FVector(0,0,60),FVector(0,0,170),ECC_Visibility);
        if (!bRoof || Check.GetActor()!=RoofActor)
            Report=TEXT("FAIL:FIXTURE_COLLISION");
        else
        {
            UASTFireFreeFlameComponent* Fire=NewObject<UASTFireFreeFlameComponent>(Host);
            Fire->RegisterComponent();
            Fire->SmokeRender=NewObject<UProceduralMeshComponent>(Host);
            Fire->SmokeRender->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Fire->SmokeRender->RegisterComponent();
            Fire->SmokeMaterial=TestMaterial;
            Fire->SmokeFireTop=FVector(0,0,24);
            Fire->Heat=0.f;
            Fire->bVisible=false;
            Fire->Smoke.SetNum(MaxSmoke);
            FSmoke& Blocked=Fire->Smoke[0];
            Blocked.Position=FVector(0,0,64);
            Blocked.BaseZ=24.f;
            Blocked.InitialRadius=Blocked.Radius=12.f;
            Blocked.MaxRise=400.f;
            Blocked.RiseSpeed=40.f;
            Blocked.Lifetime=12.f;
            Blocked.Seed=1.f;
            Blocked.bAlive=true;
            FSmoke& Free=Fire->Smoke[1];
            Free=Blocked;
            Free.Position.X=220.f;
            Free.Seed=2.f;
            bool bClip=false,bMesh=false,bMeshClip=false,bStayedAlive=true;
            float MaxSide=0.f;
            FCollisionQueryParams Q;
            Q.AddIgnoredActor(Host);
            for (int32 Frame=0;Frame<60;++Frame)
            {
                Fire->Elapsed+=.08f;
                Fire->UpdateSmoke(.08f);
                bStayedAlive &= Blocked.bAlive && Free.bAlive;
                if (Blocked.bAlive)
                {
                    MaxSide=FMath::Max(MaxSide,float(
                        FVector2D(Blocked.Position.X,Blocked.Position.Y).Size()));
                    if (World->OverlapBlockingTestByChannel(Blocked.Position,
                        FQuat::Identity,ECC_Visibility,
                        FCollisionShape::MakeSphere(Blocked.Radius),Q))
                        bClip=true;
                }
                if (const FProcMeshSection* Section=Fire->SmokeRender->GetProcMeshSection(0))
                {
                    bMesh |= Section->ProcVertexBuffer.Num()>0;
                    for (const FProcMeshVertex& Vertex:Section->ProcVertexBuffer)
                    {
                        const FVector P=Fire->SmokeRender->GetComponentTransform()
                            .TransformPosition(Vertex.Position);
                        if (FMath::Abs(P.X)<64.f && FMath::Abs(P.Y)<64.f &&
                            P.Z>120.2f && P.Z<135.8f)
                            bMeshClip=true;
                    }
                }
            }
            const bool bRise=Free.bAlive && Free.Position.Z>190.f;
            const bool bSlide=MaxSide>8.f;
            Report=FString::Printf(TEXT("%s:roof=%d;alive=%d;slide=%.1f;freeZ=%.1f;mesh=%d;carrierClip=%d;meshClip=%d"),
                bStayedAlive && bSlide && bRise && bMesh && !bClip && !bMeshClip?
                    TEXT("PASS"):TEXT("FAIL"),
                int32(bRoof),int32(bStayedAlive),MaxSide,
                float(Free.Position.Z),int32(bMesh),int32(bClip),int32(bMeshClip));
            Fire->DestroyComponent();
        }
    }
    // Native emission test: identical controlled sources, different burning
    // area. Measure actual live carriers and volume proxy, not only formulas.
    float SmokeMass[2]={0.f,0.f};
    int32 CarrierCount[2]={0,0};
    for(int32 Case=0;Case<2;++Case)
    {
        AActor* Emitter=World->SpawnActor<AActor>();
        if(!Emitter)continue;
        USceneComponent* Root=NewObject<USceneComponent>(Emitter);
        Emitter->SetRootComponent(Root);Root->RegisterComponent();
        Root->SetWorldLocation(FVector(1400,Case*500,0));
        auto* Fire=NewObject<UASTFireFreeFlameComponent>(Emitter);
        Fire->SetSizeScale(Case==0?.15f:1.5f);
        Fire->SetSource(Root,TestMaterial,317);
        Fire->RegisterComponent();
        Fire->SetFlamesVisible(true);
        Fire->SetThermal(1.f,FVector::ZeroVector,1.f);
        // Controlled active source envelope, independent of ignition delay.
        for(FTongue& T:Fire->Tongues)
        {
            T.bActive=true;T.Age=1.f;T.CurrentLength=T.MaxLength;
            T.CurrentWidth=T.BaseWidth;
        }
        for(int32 Frame=0;Frame<50;++Frame)
        {
            Fire->Elapsed+=.08f;
            Fire->UpdateSmoke(.08f);
        }
        for(const FSmoke& Cloud:Fire->Smoke)
            if(Cloud.bAlive)
            {
                ++CarrierCount[Case];
                SmokeMass[Case]+=Cloud.Radius*Cloud.Radius*Cloud.Radius;
            }
        Fire->DestroyComponent();Emitter->Destroy();
    }
    const float MassRatio=SmokeMass[1]/FMath::Max(1.f,SmokeMass[0]);
    if(CarrierCount[0]==0 || CarrierCount[1]<CarrierCount[0] || MassRatio<2.f)
        Report=TEXT("FAIL:EMISSION_SCALE;")+Report;
    Report+=FString::Printf(TEXT(";smallCarriers=%d;largeCarriers=%d;massRatio=%.2f"),
        CarrierCount[0],CarrierCount[1],MassRatio);
    World->DestroyWorld(false);
    return Report;
#else
    return TEXT("EDITOR_ONLY");
#endif
}

// Applied AFTER the approved life simulation. The age, cooling, death,
// buoyancy and state machine above remain byte-for-byte unchanged.
FString UASTFireFreeFlameComponent::RunFireV4Audit(UMaterialInterface* TestMaterial)
{
#if WITH_EDITOR
    if(!IsValid(TestMaterial)) return TEXT("FAIL:NO_MATERIAL");
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if(!World) return TEXT("FAIL:NO_WORLD");
    bool bPass=true,bFragment=false,bCool=true,bSmall=false,bMesh=false,bMoving=false,bWick=false;
    int32 MaxCells=0,MaxTones=0,FrameCount=0,StableFrames=0,GoodShape=0,Connected=0;
    int32 HotPixels=0,AllPixels=0,FragmentPixels=0,PitchChanges=0,MeshPitchChanges=0;
    int32 ChangedColors=0,CommonColors=0;
    int32 DetachedCells=0,ExposedCells=0,MultiAxisFrames=0,AxisFrames=0,SparkSamples=0;
    bool bSparkGrid=true,bSparkFade=true;
    double AspectSum=0.,FillSum=0.,Cost=0.;
    TArray<double> Costs;
    FString Raster;
    const FString Folder=FPaths::ProjectSavedDir()/TEXT("ChatGPT/NiagaraFire/FireV4");
    IFileManager::Get().MakeDirectory(*Folder,true);
    const auto NewFire=[&](float Scale,int32 Salt,bool bOnMesh,bool bHiddenWick=false)
    {
        AActor* Host=World->SpawnActor<AActor>();
        if(!Host)return static_cast<UASTFireFreeFlameComponent*>(nullptr);
        USceneComponent* Root=nullptr;
        if(bOnMesh || bHiddenWick)
        {
            UStaticMeshComponent* Mesh=NewObject<UStaticMeshComponent>(Host);
            Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
            Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
            Mesh->SetCollisionResponseToAllChannels(ECR_Block);
            if(bHiddenWick){Mesh->SetVisibility(false);Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);Mesh->SetRelativeScale3D(FVector(.12));}
            Root=Mesh;
        }
        else Root=NewObject<USceneComponent>(Host);
        Host->SetRootComponent(Root);Root->RegisterComponent();
        UASTFireFreeFlameComponent* Fire=NewObject<UASTFireFreeFlameComponent>(Host);
        Fire->SetSizeScale(Scale);Fire->SetSource(Root,TestMaterial,Salt);
        // Exact cube surface coordinates; the transient audit world may not expose complex collision.
        TArray<FVector_NetQuantize10> Contacts;
        if(bHiddenWick)Contacts.Add(FVector_NetQuantize10(FVector(0,0,45)));
        else if(bOnMesh)for(const FVector& P:{FVector(50,-30,-20),FVector(50,30,20),FVector(0,0,50),FVector(-50,-25,20)})Contacts.Add(FVector_NetQuantize10(P));
        Fire->SetIgnitionPoints(Contacts);
        Fire->RegisterComponent();
        return Fire;
    };
    const auto Step=[&](UASTFireFreeFlameComponent* Fire,const FVector& Wind)
    {
        Fire->SetThermal(1.f,Wind,.75f);
        Fire->AdvanceSimulation(.04f);
        Fire->UpdateDetachedVortices(.04f);
        Fire->UpdateSparks(.04f);
        Fire->UpdateGeometry(false);
    };
    for(int32 Case=0;Case<8;++Case)
    {
        const bool bOnMesh=Case==6;
        const bool bHiddenWick=Case==7;
        const float Scale=(Case==4 || bHiddenWick)?.15f:Case==5?1.8f:1.f;
        UASTFireFreeFlameComponent* Fire=NewFire(Scale,1729+Case*911,bOnMesh,bHiddenWick);
        if(!Fire){bPass=false;continue;}
        AActor* Host=Fire->GetOwner();
        bool bSeen=false;
        float LastPitch=0.f;
        TMap<FIntPoint,int32> Previous;
        for(int32 I=0;I<120;++I)
        {
            // First 2 seconds are still air. Last 2 seconds move the source
            // and apply wind, rather than testing a static preview only.
            if(I>=72) Host->SetActorLocation(FVector(0,(I-72)*1.2f,0));
            const double Start=FPlatformTime::Seconds();
            Step(Fire,I>60?FVector(0,90,0):FVector::ZeroVector);
            const double Ms=(FPlatformTime::Seconds()-Start)*1000.;
            Cost+=Ms;Costs.Add(Ms);++FrameCount;
            if(!IsValid(Fire->Render)){bPass=false;break;}
            const float Pitch=Fire->FirePixelPitch;
            if(Pitch<=0.f)continue;
            if(I>25 && LastPitch>0.f && Pitch!=LastPitch)
            {
                ++PitchChanges;
                if(bOnMesh && I<72)++MeshPitchChanges;
            }
            LastPitch=Pitch;
            const ASTFireLife::FFireProjection P(Fire->Source->GetComponentLocation(),World);
            const FTransform T=Fire->Render->GetComponentTransform();
            TMap<FIntPoint,int32> Main,DetachedPixels;
            FIntPoint Lo(100000,100000),Hi(-100000,-100000);
            for(int32 Section=0;Section<2;++Section)
            {
                const FProcMeshSection* S=Fire->Render->GetProcMeshSection(Section);
                if(!S)continue;
                const int32 Count=S->ProcVertexBuffer.Num();
                bPass &= Count%4==0 && S->ProcIndexBuffer.Num()==Count/4*6;
                for(int32 J=0;J+3<Count;J+=4)
                {
                    const auto& V=S->ProcVertexBuffer;
                    const int32 Tone=FMath::Clamp(int32(V[J].Color.R*7.f/255.f),0,6);
                    bPass &= V[J].Color==V[J+1].Color && V[J].Color==V[J+2].Color &&
                        V[J].Color==V[J+3].Color && V[J].Color.A==255;
                    const FVector2D A=P.Project(T.TransformPosition(V[J].Position));
                    const FVector2D B=P.Project(T.TransformPosition(V[J+3].Position));
                    const FIntPoint Key(FMath::FloorToInt((A.X+B.X)*.5f/Pitch),
                        FMath::FloorToInt((A.Y+B.Y)*.5f/Pitch));
                    // Real projected corners must remain a square cell,
                    // including depth varying on mesh flames.
                    bPass &= FMath::Abs(FMath::Abs(float(B.X-A.X))-Pitch)<.03f*Pitch+.003f &&
                        FMath::Abs(FMath::Abs(float(B.Y-A.Y))-Pitch)<.03f*Pitch+.003f;
                    if(Section==1)
                    {
                        Main.Add(Key,Tone);
                        Lo.X=FMath::Min(Lo.X,Key.X);Lo.Y=FMath::Min(Lo.Y,Key.Y);
                        Hi.X=FMath::Max(Hi.X,Key.X);Hi.Y=FMath::Max(Hi.Y,Key.Y);
                    }
                    else DetachedPixels.Add(Key,Tone);
                }
            }
            if(Case<4 && I>25 && I<60)
            {
                for(const auto& Pair:DetachedPixels){++DetachedCells;ExposedCells+=!Main.Contains(Pair.Key);}
                // Measure the actual point mask: multiple distinct ridges
                // above the shared bed, rather than invisible legacy axes.
                int32 Top[FieldZones]={};
                for(int32 Z=0;Z<FieldZones;++Z)
                {
                    const int32 CX=FMath::RoundToInt(19.5f+(-.80f+.40f*Z)*13.f);
                    for(int32 Y=0;Y<FieldY;++Y)
                        for(int32 X=FMath::Max(0,CX-1);X<=FMath::Min(FieldX-1,CX+1);++X)
                            if(Fire->PointField.IsValidIndex(Y*FieldX+X)&&Fire->PointField[Y*FieldX+X].bLit)Top[Z]=Y;
                }
                ++AxisFrames;
                int32 MinTop=Top[0],MaxTop=Top[0];
                for(int32 Z=1;Z<FieldZones;++Z){MinTop=FMath::Min(MinTop,Top[Z]);MaxTop=FMath::Max(MaxTop,Top[Z]);}
                MultiAxisFrames+=MaxTop-MinTop>=5;
            }
            if(const FProcMeshSection* SparkSection=Fire->Render->GetProcMeshSection(2))
            {
                const auto& V=SparkSection->ProcVertexBuffer;
                for(int32 J=0;J+3<V.Num() && J/4<Fire->Sparks.Num();J+=4)
                {
                    const FSpark& Spark=Fire->Sparks[J/4];
                    if(!Spark.bAlive)continue;
                    const float Expected=FMath::Pow(FMath::Clamp(1.f-Spark.Age/FMath::Max(.05f,Spark.Lifetime),0.f,1.f),1.35f);
                    const FVector2D A=P.Project(T.TransformPosition(V[J].Position));
                    const FVector2D B=P.Project(T.TransformPosition(V[J+3].Position));
                    bSparkGrid&=FMath::Abs(float(B.X-A.X)-Pitch)<.03f*Pitch+.003f && FMath::Abs(float(B.Y-A.Y)-Pitch)<.03f*Pitch+.003f;
                    bSparkFade&=FMath::Abs(float(V[J].Color.A)/255.f-Expected)<.012f && V[J].Color.A==V[J+3].Color.A;
                    ++SparkSamples;
                }
            }
            if(Main.IsEmpty())continue;
            bSeen=true;
            MaxCells=FMath::Max(MaxCells,Main.Num());
            bFragment|=!DetachedPixels.IsEmpty();
            if(bOnMesh)bMesh|=Fire->Tongues.Num()>=4 && Fire->SourceAnchors.Num()>=30 && Main.Num()>40 && Fire->MeshCuts>=1 && Fire->MeshTransferred>=2;
            if(bHiddenWick)bWick|=Fire->Tongues.Num()==2 && Main.Num()>5 &&
                FMath::Abs(float(Fire->Tongues[0].RestPosition.Z)-45.f)<.01f;
            if(I>=80)bMoving|=Host->GetActorLocation().Y>5.f && Main.Num()>10;
            TSet<int32> Tones;
            for(const auto& Pair:Main)Tones.Add(Pair.Value);
            MaxTones=FMath::Max(MaxTones,Tones.Num());
            if(I>25 && I<60 && Case<4)
            {
                const int32 Width=Hi.X-Lo.X+1,Height=Hi.Y-Lo.Y+1;
                const float Aspect=float(Width)/Height,Fill=float(Main.Num())/(Width*Height);
                ++StableFrames;AspectSum+=Aspect;FillSum+=Fill;
                GoodShape+=Aspect>=.50f && Aspect<=1.70f && Fill>=.42f && Fill<=.90f;
                TSet<FIntPoint> Visited;
                int32 Largest=0;
                const FIntPoint Neighbors[4]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
                for(const auto& Pair:Main)
                {
                    if(Visited.Contains(Pair.Key))continue;
                    TArray<FIntPoint> Queue;Queue.Add(Pair.Key);Visited.Add(Pair.Key);
                    for(int32 J=0;J<Queue.Num();++J)
                        for(const FIntPoint& N:Neighbors)
                        {
                            const FIntPoint Q=Queue[J]+N;
                            if(Main.Contains(Q) && !Visited.Contains(Q)){Visited.Add(Q);Queue.Add(Q);}
                        }
                    Largest=FMath::Max(Largest,Queue.Num());
                }
                Connected+=Largest>=Main.Num()*.85f;
                for(const auto& Pair:Main)
                {
                    ++AllPixels;HotPixels+=Pair.Value==6;
                    if(const int32* Old=Previous.Find(Pair.Key))
                    {++CommonColors;ChangedColors+=*Old!=Pair.Value;}
                }
                FragmentPixels+=DetachedPixels.Num();
            }
            Previous=Main;
            // Actual procedural mesh data, not a reimplementation of the
            // shape. Store sampled frames to inspect reference likeness.
            if(I>=28 && I<=88 && I%10==8)
            {
                FIntPoint L=Lo,U=Hi;
                for(const auto& Pair:DetachedPixels)
                {
                    L.X=FMath::Min(L.X,Pair.Key.X);L.Y=FMath::Min(L.Y,Pair.Key.Y);
                    U.X=FMath::Max(U.X,Pair.Key.X);U.Y=FMath::Max(U.Y,Pair.Key.Y);
                }
                Raster+=FString::Printf(TEXT("FRAME %d %d %.4f %d %d %d %d\n"),
                    Case,I,Pitch,L.X,L.Y,U.X,U.Y);
                for(int32 Y=U.Y;Y>=L.Y;--Y)
                {
                    for(int32 X=L.X;X<=U.X;++X)
                    {
                        const FIntPoint Q(X,Y);
                        if(const int32* C=Main.Find(Q)) Raster.AppendChar(TCHAR('0'+*C));
                        else if(const int32* FragmentTone=DetachedPixels.Find(Q))Raster.AppendChar(TCHAR('a'+*FragmentTone));
                        else Raster.AppendChar(TCHAR('.'));
                    }
                    Raster.AppendChar(TCHAR('\n'));
                }
            }
        }
        bPass &= bSeen;
        if(Case==4)bSmall=bSeen;
        Fire->SetThermal(0.f,FVector::ZeroVector,0.f);
        for(int32 J=0;J<36;++J)Fire->AdvanceSimulation(.04f);
        Fire->UpdateGeometry(false);
        for(int32 Section=0;Section<2;++Section)
        {
            const FProcMeshSection* Cooled=Fire->Render->GetProcMeshSection(Section);
            bCool&=!Cooled || Cooled->ProcVertexBuffer.IsEmpty();
        }
        Fire->DestroyComponent();Host->Destroy();
    }
    bool bCycle=true;
    int32 Splits=0,GrowingFrames=0,CoolingFrames=0;
    float MaxTransferError=0.f,MaxResetRatio=0.f;
    int32 FieldCuts=0,FieldCellsTransferred=0,FieldCoolingFrames=0;
    bool bFieldContinuity=true;
    if(auto* Single=NewFire(1.f,1729,false))
    {
        Single->Tongues.SetNum(1);
        Single->Tongues[0].bActive=true;Single->Tongues[0].Age=0.f;
        for(int32 I=0;I<260;++I)
        {
            const FTongue Old=Single->Tongues[0];
            const TArray<FDetached> Before=Single->Detached;
            const TArray<FFieldCell> OldField=Single->PointField;
            const TArray<FFieldFragment> OldFragments=Single->FieldFragments;
            Step(Single,FVector::ZeroVector);
            for(const auto& F:Single->FieldFragments)
            {
                if(F.Age==0.f)
                {
                    for(const auto& C:F.Cells)
                    {
                        const int32 J=C.SourceIndex;
                        const ASTFireLife::FFireProjection FieldProj(Single->Source->GetComponentLocation(),World);
                        const FVector Restored=F.Center+FieldProj.Right*C.Offset.X+FieldProj.Up*C.Offset.Y;
                        bFieldContinuity&=OldField.IsValidIndex(J) && Single->PointField.IsValidIndex(J) &&
                            OldField[J].bLit && !Single->PointField[J].bLit &&
                            FMath::IsNearlyEqual(OldField[J].Temperature,C.Temperature,.0001f) &&
                            FVector::DistSquared(OldField[J].World,Restored)<.0001f;
                    }
                }
                else for(const auto& Prev:OldFragments)
                    if(F.Serial==Prev.Serial && F.Age>Prev.Age)
                    {++FieldCoolingFrames;bFieldContinuity&=F.Center.Z>Prev.Center.Z && F.Velocity.Z>Prev.Velocity.Z;break;}
            }
            const FTongue& New=Single->Tongues[0];
            if(New.Generation==Old.Generation && Old.Life==EFlameLife::Growing)
            {
                ++GrowingFrames;bCycle&=New.CurrentLength+.001f>=Old.CurrentLength;
            }
            if(New.Generation!=Old.Generation)
            {
                ++Splits;
                MaxResetRatio=FMath::Max(MaxResetRatio,New.CurrentLength/FMath::Max(.01f,Old.SplitLength));
                bool bFound=false;
                bCycle&=Old.bDisplayedPinch && Old.DisplayGeneration==Old.Generation;
                for(const FDetached& D:Single->Detached)
                    if(D.bAlive && D.Age==0.f && D.ParentGeneration==Old.Generation)
                    {
                        bFound=true;
                        for(int32 K=0;K<D.ArcCount;++K)
                        {
                            const int32 Ring=FMath::RoundToInt(D.SourceV[K]*(RingsPerTongue-1));
                            const FVector Expected=Old.DisplayArc[Ring];
                            MaxTransferError=FMath::Max(MaxTransferError,float(FVector::Dist(Expected,D.Position+D.Arc[K])));
                            bCycle&=FMath::Abs(D.Radius[K]-Old.DisplayRadius[Ring])<.01f &&
                                D.BirthClock==Old.DisplayClock && D.BirthHeat==Old.DisplayHeat;
                        }
                        bCycle&=D.SourceV[0]==Old.RemainingFraction && D.SourceV[D.ArcCount-1]==1.f;
                    }
                bCycle&=bFound;
            }
            for(int32 K=0;K<Before.Num();++K)
            {
                const auto& A=Before[K];const auto& B=Single->Detached[K];
                if(A.bAlive && B.bAlive && A.ParentGeneration==B.ParentGeneration && B.Age>A.Age)
                {
                    ++CoolingFrames;
                    bCycle&=B.Position.Z>A.Position.Z && B.Velocity.Z>A.Velocity.Z &&
                        B.Temperature<A.Temperature &&
                        ASTFireLife::ArcFade(B.Age,B.Lifetime)<ASTFireLife::ArcFade(A.Age,A.Lifetime);
                }
            }
            // Export actual single-tongue silhouette each 40 ms: sections 0
            // and 1 are reconstructed from their real procedural mesh cells.
            if(I<80)
            {
                const float Pitch=Single->FirePixelPitch;
                if(Pitch<=0.f)continue;
                const ASTFireLife::FFireProjection Proj(Single->Source->GetComponentLocation(),World);
                const FTransform Tr=Single->Render->GetComponentTransform();
                TMap<FIntPoint,TCHAR> Pixels;
                FIntPoint L(100000,100000),U(-100000,-100000);
                for(int32 Sec=0;Sec<2;++Sec)
                    if(const FProcMeshSection* Mesh=Single->Render->GetProcMeshSection(Sec))
                        for(int32 J=0;J+3<Mesh->ProcVertexBuffer.Num();J+=4)
                        {
                            const auto& V=Mesh->ProcVertexBuffer;
                            const FVector2D A=Proj.Project(Tr.TransformPosition(V[J].Position));
                            const FVector2D B=Proj.Project(Tr.TransformPosition(V[J+3].Position));
                            const FIntPoint Q(FMath::FloorToInt((A.X+B.X)*.5f/Pitch),FMath::FloorToInt((A.Y+B.Y)*.5f/Pitch));
                            const int32 Tone=FMath::Clamp(int32(V[J].Color.R*7.f/255.f),0,6);
                            Pixels.Add(Q,TCHAR((Sec==0?'a':'0')+Tone));
                            L.X=FMath::Min(L.X,Q.X);L.Y=FMath::Min(L.Y,Q.Y);
                            U.X=FMath::Max(U.X,Q.X);U.Y=FMath::Max(U.Y,Q.Y);
                        }
                if(!Pixels.IsEmpty())
                {
                    Raster+=FString::Printf(TEXT("FRAME 8 %d %.4f %d %d %d %d\n"),I,Pitch,L.X,L.Y,U.X,U.Y);
                    for(int32 Y=U.Y;Y>=L.Y;--Y)
                    {
                        for(int32 X=L.X;X<=U.X;++X)
                        {
                            const TCHAR* Value=Pixels.Find(FIntPoint(X,Y));
                            Raster.AppendChar(Value?*Value:TCHAR('.'));
                        }
                        Raster.AppendChar(TCHAR('\n'));
                    }
                }
            }
        }
        FieldCuts=Single->FieldSplitCount;FieldCellsTransferred=Single->FieldTransferredCells;
        AActor* Actor=Single->GetOwner();Single->DestroyComponent();Actor->Destroy();
    }
    else bCycle=false;
    bCycle&=Splits>=3 && GrowingFrames>=30 && CoolingFrames>=12 && MaxTransferError<.05f && MaxResetRatio<=.151f;
    bCycle&=bFieldContinuity && FieldCuts>=5 && FieldCellsTransferred>=15 && FieldCoolingFrames>=20;
    const bool bRaster=FFileHelper::SaveStringToFile(Raster,*(Folder/TEXT("actual_raster.txt")));
    TArray<UASTFireFreeFlameComponent*> Crowd;
    for(int32 I=0;I<20;++I)
    {
        if(UASTFireFreeFlameComponent* F=NewFire(1.f,811+I*97,false))
            Crowd.Add(F);
    }
    double CrowdMs=0.;TArray<double> CrowdCosts;
    for(int32 I=0;I<60;++I)
    {
        const double Start=FPlatformTime::Seconds();
        for(auto* F:Crowd)Step(F,FVector::ZeroVector);
        const double Ms=(FPlatformTime::Seconds()-Start)*1000.;
        if(I>=25){CrowdMs+=Ms;CrowdCosts.Add(Ms);}
    }
    for(auto* F:Crowd){AActor* A=F->GetOwner();F->DestroyComponent();A->Destroy();}
    World->DestroyWorld(false);
    Costs.Sort();CrowdCosts.Sort();
    const float ShapeRate=float(GoodShape)/FMath::Max(1,StableFrames);
    const float Connectivity=float(Connected)/FMath::Max(1,StableFrames);
    const float HotRate=float(HotPixels)/FMath::Max(1,AllPixels);
    const float ChangeRate=float(ChangedColors)/FMath::Max(1,CommonColors);
    const float FragmentRate=float(FragmentPixels)/FMath::Max(1,AllPixels);
    const float Exposure=float(ExposedCells)/FMath::Max(1,DetachedCells);
    const float FanRate=float(MultiAxisFrames)/FMath::Max(1,AxisFrames);
    bPass &= bSparkGrid && bSparkFade && SparkSamples>50 && Exposure>=.50f && FanRate>=.60f && AxisFrames>20;
    bPass &= bCycle && bFragment && bCool && bSmall && bMesh && bWick && bMoving && bRaster &&
        MaxTones>=6 && ShapeRate>=.55f && Connectivity>=.65f &&
        HotRate<=.30f && ChangeRate>.02f && FragmentRate<.85f &&
        Crowd.Num()==20 && MeshPitchChanges<=4;
    return FString::Printf(TEXT("%s:small=%d;fragment=%d;cool=%d;mesh=%d;wick=%d;moving=%d;raster=%d;cells=%d;tones=%d;shape=%.3f;connected=%.3f;aspect=%.3f;fill=%.3f;hot=%.3f;thermalChange=%.3f;detachedRatio=%.3f;pitchChanges=%d;meshPitchChanges=%d;cpuMs=%.3f;p95=%.3f;crowd20Ms=%.3f;crowdP95=%.3f;cycle=%d;splits=%d;growing=%d;cooling=%d;transferError=%.5f;resetRatio=%.3f;exposure=%.3f;fan=%.3f;sparkGrid=%d;sparkFade=%d;sparkSamples=%d;fieldCuts=%d;fieldCells=%d;fieldCooling=%d;fieldContinuity=%d"),
        bPass?TEXT("PASS"):TEXT("FAIL"),int32(bSmall),int32(bFragment),int32(bCool),int32(bMesh),int32(bWick),int32(bMoving),int32(bRaster),
        MaxCells,MaxTones,ShapeRate,Connectivity,AspectSum/FMath::Max(1,StableFrames),
        FillSum/FMath::Max(1,StableFrames),HotRate,ChangeRate,FragmentRate,PitchChanges,MeshPitchChanges,
        Cost/FMath::Max(1,FrameCount),Costs.IsEmpty()?0.:Costs[FMath::Clamp(FMath::FloorToInt(Costs.Num()*.95f),0,Costs.Num()-1)],
        CrowdMs/FMath::Max(1,CrowdCosts.Num()),CrowdCosts.IsEmpty()?0.:CrowdCosts[FMath::Clamp(FMath::FloorToInt(CrowdCosts.Num()*.95f),0,CrowdCosts.Num()-1)],int32(bCycle),Splits,GrowingFrames,CoolingFrames,MaxTransferError,MaxResetRatio,Exposure,FanRate,int32(bSparkGrid),int32(bSparkFade),SparkSamples,FieldCuts,FieldCellsTransferred,FieldCoolingFrames,int32(bFieldContinuity));
#else
    return TEXT("EDITOR_ONLY");
#endif
}

FString UASTFireFreeFlameComponent::RunFireSurfaceAudit(UMaterialInterface* TestMaterial,USkeletalMesh* Mesh,bool bCapture)
{
#if WITH_EDITOR
    if(!IsValid(TestMaterial)||!IsValid(Mesh))return TEXT("FAIL:NO_SKELETAL_ASSET");
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if(!World)return TEXT("FAIL:NO_WORLD");
    AActor* Host=World->SpawnActor<AActor>();
    if(!Host){World->DestroyWorld(false);return TEXT("FAIL:NO_HOST");}
    USkeletalMeshComponent* Skin=NewObject<USkeletalMeshComponent>(Host);
    Host->SetRootComponent(Skin);Skin->SetSkeletalMesh(Mesh);Skin->RegisterComponent();
    Skin->RefreshBoneTransforms();
    auto* Fire=NewObject<UASTFireFreeFlameComponent>(Host);
    Fire->SetSource(Skin,TestMaterial,4811);Fire->RegisterComponent();
    int32 Samples=0,Covered=0,ValidFrames=0,Saved=0;
    USceneCaptureComponent2D* SkinCamera=nullptr;
    UTextureRenderTarget2D* Target=nullptr;
    const FString SkinFolder=FPaths::ProjectSavedDir()/TEXT("ChatGPT/NiagaraFire/FireV4/GPU");
    if(bCapture)
    {
        AActor* ViewActor=World->SpawnActor<AActor>();
        if(!ViewActor){World->DestroyWorld(false);return TEXT("FAIL:CAPTURE_ACTOR");}
        SkinCamera=NewObject<USceneCaptureComponent2D>(ViewActor);
        ViewActor->SetRootComponent(SkinCamera);
        SkinCamera->bCaptureEveryFrame=false;SkinCamera->bCaptureOnMovement=false;
        SkinCamera->ProjectionType=ECameraProjectionMode::Perspective;
        SkinCamera->FOVAngle=30.f;SkinCamera->CaptureSource=SCS_FinalColorLDR;
        SkinCamera->ShowFlags.SetBloom(false);SkinCamera->ShowFlags.SetTemporalAA(false);
        SkinCamera->ShowFlags.SetMotionBlur(false);SkinCamera->ShowFlags.SetEyeAdaptation(false);
        Target=NewObject<UTextureRenderTarget2D>(SkinCamera);
        Target->RenderTargetFormat=RTF_RGBA8_SRGB;Target->ClearColor=FLinearColor(.007f,.009f,.014f,1.f);
        Target->InitAutoFormat(512,512);Target->UpdateResourceImmediate(true);
        SkinCamera->TextureTarget=Target;SkinCamera->RegisterComponent();
        SkinCamera->SetWorldLocation(FVector(1000,0,0));SkinCamera->SetWorldRotation(FRotator(0,180,0));
        IFileManager::Get().MakeDirectory(*SkinFolder,true);
    }
    for(int32 I=0;I<100;++I)
    {
        if(I>45)Host->SetActorRotation(FRotator(0,(I-45)*2.f,0));
        Fire->SetThermal(1.f,FVector::ZeroVector,.75f);
        Fire->AdvanceSimulation(.04f);Fire->UpdateDetachedVortices(.04f);
        Fire->UpdateGeometry(false);
        if(bCapture && SkinCamera && Target && I>=58 && I%10==8)
        {
            World->SendAllEndOfFrameUpdates();SkinCamera->CaptureScene();FlushRenderingCommands();
            const FString Name=SkinFolder/FString::Printf(TEXT("Skin_%03d.png"),I);
            TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*Name));
            if(Ar && FImageUtils::ExportRenderTarget2DAsPNG(Target,*Ar))++Saved;
        }
        if(I<55 || Fire->FirePixelPitch<=0.f || !Fire->Render)continue;
        const FProcMeshSection* Section=Fire->Render->GetProcMeshSection(1);
        if(!Section)continue;
        const ASTFireLife::FFireProjection P(Skin->GetComponentLocation(),World);
        const FTransform Tr=Fire->Render->GetComponentTransform();
        const float Pitch=Fire->FirePixelPitch;
        TSet<FIntPoint> Occupied;
        for(int32 J=0;J+3<Section->ProcVertexBuffer.Num();J+=4)
        {
            const auto& V=Section->ProcVertexBuffer;
            const FVector2D A=P.Project(Tr.TransformPosition(V[J].Position));
            const FVector2D B=P.Project(Tr.TransformPosition(V[J+3].Position));
            Occupied.Add(FIntPoint(FMath::FloorToInt((A.X+B.X)*.5f/Pitch),FMath::FloorToInt((A.Y+B.Y)*.5f/Pitch)));
        }
        if(Occupied.IsEmpty())continue;
        ++ValidFrames;
        for(const FSourceAnchor& Anchor:Fire->SourceAnchors)
        {
            if(Anchor.SkinnedVertexIndex<0)continue;
            FTongue Probe;Probe.RestPosition=Anchor.RestPosition;Probe.SkinnedVertexIndex=Anchor.SkinnedVertexIndex;
            bool bNear=false;
            for(const FTongue& T:Fire->Tongues)
                if(T.bActive && T.Age>=0.f && FVector::DistSquared(T.RestPosition,Anchor.RestPosition)<FMath::Square(32.f))
                {bNear=true;break;}
            if(!bNear)continue;
            const FVector2D Q=P.Project(Fire->SampleRoot(Probe));
            const FIntPoint Key(FMath::FloorToInt(Q.X/Pitch),FMath::FloorToInt(Q.Y/Pitch));
            ++Samples;Covered+=Occupied.Contains(Key);
        }
    }
    const float Coverage=float(Covered)/FMath::Max(1,Samples);
    const int32 Roots=Fire->Tongues.Num(),Anchors=Fire->SourceAnchors.Num();
    const int32 MeshCuts=Fire->MeshCuts,MeshTransferred=Fire->MeshTransferred;
    Fire->DestroyComponent();Host->Destroy();World->DestroyWorld(false);
    const bool bPass=Roots>=4 && Anchors>=30 && Samples>=100 && ValidFrames>=30 && Coverage>=.85f && MeshCuts>=2 && MeshTransferred>=4 && (!bCapture || Saved>=4);
    return FString::Printf(TEXT("%s:mesh=%s;roots=%d;anchors=%d;frames=%d;coverage=%.3f;samples=%d;captures=%d;fieldCuts=%d;fieldCells=%d"),
        bPass?TEXT("PASS"):TEXT("FAIL"),*Mesh->GetName(),Roots,Anchors,ValidFrames,Coverage,Samples,Saved,MeshCuts,MeshTransferred);
#else
    return TEXT("EDITOR_ONLY");
#endif
}

FString UASTFireFreeFlameComponent::RunCarryVisualAudit(UClass* BoxClass,UClass* BarrelClass)
{
#if WITH_EDITOR
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if(!World||!BoxClass||!BarrelClass){if(World)World->DestroyWorld(false);return TEXT("FAIL:assets");}
    bool bPass=true;
    FString Detail;
    for(UClass* Class:{BoxClass,BarrelClass})
    {
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Prop=World->SpawnActor<AActor>(Class,FVector(0,0,0),FRotator(23,40,17),Spawn);
        auto* Body=Prop?Cast<UStaticMeshComponent>(Prop->GetRootComponent()):nullptr;
        if(!Body||!Body->GetStaticMesh()){bPass=false;continue;}
        Body->SetGenerateOverlapEvents(false);
        Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        auto* Fire=UASTFireBlueprintLibrary::IgniteActor(Prop,Body->Bounds.Origin,.9f,6);
        auto* FX=Prop->FindComponentByClass<UASTFireFreeFlameComponent>();
        if(!Fire||!FX){bPass=false;Detail+=TEXT(";no_visual_component");Prop->Destroy();continue;}
        int32 Peak=0,Low=0;
        for(int32 Frame=0;Frame<90;++Frame)
        {
            FX->SetThermal(.9f,FVector::ZeroVector,.75f);
            FX->AdvanceSimulation(.04f);FX->UpdateDetachedVortices(.04f);FX->UpdateSmoke(.04f);FX->UpdateGeometry(false);
            if(const auto* Section=FX->Render->GetProcMeshSection(1))
                Peak=FMath::Max(Peak,Section->ProcVertexBuffer.Num()/4);
        }
        for(int32 Frame=0;Frame<35;++Frame)
        {
            FX->SetThermal(.08f,FVector::ZeroVector,.25f);
            FX->AdvanceSimulation(.04f);FX->UpdateGeometry(false);FX->UpdateSmoke(.04f);
            if(const auto* Section=FX->Render->GetProcMeshSection(1))
                Low=FMath::Max(Low,Section->ProcVertexBuffer.Num()/4);
        }
        bool bSmoke=false;
        for(const auto& Cloud:FX->Smoke)bSmoke|=Cloud.bAlive;
        FX->SetThermal(0.f,FVector::ZeroVector,0.f);
        FX->UpdateGeometry(false);
        const auto* Section=FX->Render->GetProcMeshSection(1);
        const bool bOff=!Section||Section->ProcVertexBuffer.IsEmpty();
        const bool bTail=FX->SmokeAfterglowSeconds>0.f || bSmoke;
        const bool bOK=FX->Tongues.Num()>=2 && Peak>=12 && Low<Peak && bOff && bTail;
        bPass&=bOK;
        Detail+=FString::Printf(TEXT(";%s:roots=%d;peak=%d;cooling=%d;off=%d;smoke_tail=%d"),
            *Class->GetName(),FX->Tongues.Num(),Peak,Low,int32(bOff),int32(bTail));
        Prop->Destroy();
    }
    World->DestroyWorld(false);
    return FString(bPass?TEXT("PASS:"):TEXT("FAIL:"))+Detail;
#else
    return TEXT("EDITOR_ONLY");
#endif
}

FString UASTFireFreeFlameComponent::RunFireV4Capture(UMaterialInterface* TestMaterial)
{
#if WITH_EDITOR
    if(!IsValid(TestMaterial))return TEXT("FAIL:NO_MATERIAL");
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if(!World)return TEXT("FAIL:NO_WORLD");
    AActor* Host=World->SpawnActor<AActor>();
    AActor* Camera=World->SpawnActor<AActor>();
    if(!Host || !Camera){World->DestroyWorld(false);return TEXT("FAIL:ACTORS");}
    USceneComponent* Root=NewObject<USceneComponent>(Host);
    Host->SetRootComponent(Root);Root->RegisterComponent();
    UASTFireFreeFlameComponent* Fire=NewObject<UASTFireFreeFlameComponent>(Host);
    Fire->SetSource(Root,TestMaterial,1729);Fire->RegisterComponent();
    USceneCaptureComponent2D* Capture=NewObject<USceneCaptureComponent2D>(Camera);
    Camera->SetRootComponent(Capture);
    Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;
    Capture->ProjectionType=ECameraProjectionMode::Perspective;
    Capture->FOVAngle=18.f;
    Capture->CaptureSource=SCS_FinalColorLDR;
    Capture->ShowFlags.SetBloom(false);
    Capture->ShowFlags.SetMotionBlur(false);
    Capture->ShowFlags.SetTemporalAA(false);
    Capture->ShowFlags.SetEyeAdaptation(false);
    UTextureRenderTarget2D* RT=NewObject<UTextureRenderTarget2D>(Capture);
    RT->RenderTargetFormat=RTF_RGBA8_SRGB;
    RT->ClearColor=FLinearColor(.007f,.009f,.014f,1.f);
    RT->InitAutoFormat(512,512);
    RT->UpdateResourceImmediate(true);
    Capture->TextureTarget=RT;
    Capture->RegisterComponent();
    Capture->SetWorldLocation(FVector(1000,0,0));
    Capture->SetWorldRotation(FRotator(0,180,0));
    const FString Folder=FPaths::ProjectSavedDir()/TEXT("ChatGPT/NiagaraFire/FireV4/GPU");
    IFileManager::Get().MakeDirectory(*Folder,true);
    int32 Saved=0;
    for(int32 I=0;I<100;++I)
    {
        Fire->SetThermal(1.f,I>60?FVector(0,80,0):FVector::ZeroVector,.75f);
        Fire->AdvanceSimulation(.04f);Fire->UpdateDetachedVortices(.04f);
        Fire->UpdateSparks(.04f);Fire->UpdateGeometry(false);
        if(I>=28 && I%10==8)
        {
            World->SendAllEndOfFrameUpdates();
            Capture->CaptureScene();
            FlushRenderingCommands();
            const FString Name=Folder/FString::Printf(TEXT("Frame_%03d.png"),I);
            TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*Name));
            if(Ar && FImageUtils::ExportRenderTarget2DAsPNG(RT,*Ar))++Saved;
        }
    }
    Fire->DestroyComponent();Host->Destroy();
    AActor* CrateActor=World->SpawnActor<AActor>();
    int32 CrateSaved=0,CrateAnchors=0,CrateCuts=0;
    if(CrateActor)
    {
        UStaticMeshComponent* Crate=NewObject<UStaticMeshComponent>(CrateActor);
        Crate->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
        CrateActor->SetRootComponent(Crate);Crate->RegisterComponent();
        auto* CrateFire=NewObject<UASTFireFreeFlameComponent>(CrateActor);
        CrateFire->SetSource(Crate,TestMaterial,1831);
        TArray<FVector_NetQuantize10> Contacts;
        for(const FVector& Q:{FVector(50,-30,-20),FVector(50,30,20),FVector(0,0,50),FVector(-50,-25,20)})
            Contacts.Add(FVector_NetQuantize10(Q));
        CrateFire->SetIgnitionPoints(Contacts);CrateFire->RegisterComponent();
        Capture->FOVAngle=22.f;
        Capture->ShowFlags.SetLighting(false);
        for(int32 I=0;I<100;++I)
        {
            if(I>=50)CrateActor->SetActorRotation(FRotator(0,(I-50)*1.2f,0));
            CrateFire->SetThermal(1.f,FVector::ZeroVector,.75f);
            CrateFire->AdvanceSimulation(.04f);CrateFire->UpdateDetachedVortices(.04f);
            CrateFire->UpdateGeometry(false);
            if(I>=48 && I%10==8)
            {
                World->SendAllEndOfFrameUpdates();Capture->CaptureScene();FlushRenderingCommands();
                const FString Name=Folder/FString::Printf(TEXT("Crate_%03d.png"),I);
                TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*Name));
                if(Ar && FImageUtils::ExportRenderTarget2DAsPNG(RT,*Ar))++CrateSaved;
            }
        }
        CrateAnchors=CrateFire->SourceAnchors.Num();CrateCuts=CrateFire->MeshCuts;
        CrateFire->DestroyComponent();CrateActor->Destroy();
    }
    Capture->DestroyComponent();World->DestroyWorld(false);
    return FString::Printf(TEXT("%s:frames=%d;crateFrames=%d;crateAnchors=%d;crateCuts=%d"),
        Saved>=6 && CrateSaved>=4 && CrateAnchors>=30 && CrateCuts>=2?TEXT("PASS"):TEXT("FAIL"),Saved,CrateSaved,CrateAnchors,CrateCuts);
#else
    return TEXT("EDITOR_ONLY");
#endif
}

void UASTFireFreeFlameComponent::UpdateDetachedVortices(float Dt)
{
    for(FDetached& D:Detached)
    {
        if(!D.bAlive || D.Age<=0.f)continue;
        const float Scale=FMath::Clamp(D.Length/65.f,.08f,1.8f);
        const float Phase=D.Age*(5.f+3.f*ASTFireLife::Hash(D.Seed*.41f))+D.Seed*.19f;
        const FVector Side=D.Right.GetSafeNormal();
        const FVector Depth=FVector::CrossProduct(FVector::UpVector,Side).GetSafeNormal();
        // Curved buoyant paths; lateral movement cannot cancel acceleration upward.
        D.Velocity+=(Side*FMath::Sin(Phase)*100.f+
            Depth*FMath::Cos(Phase*.77f+D.Seed*.31f)*64.f)*(Scale*Dt);
    }
}

void UASTFireFreeFlameComponent::AddStrip(
    const TArray<FVector>& Centers,const TArray<float>& Widths,
    const TArray<float>& Temperatures,float ShapeSeed,float LifeFade,
    const FVector& Right,TArray<FVector>& Verts,TArray<FVector2D>& UVs,
    TArray<FLinearColor>& Colors) const
{
    const FTransform Transform=IsValid(Render)?
        Render->GetComponentTransform():FTransform::Identity;
    const int32 N=Centers.Num();
    for (int32 I=0; I<N; ++I)
    {
        const float V=float(I)/FMath::Max(1,N-1);
        const FVector World=Centers[I];
        const float W=FMath::Max(0.f,Widths[I]);
        const float Thermal=FMath::Clamp(Temperatures[I]*LifeFade*Heat,0.f,1.f);
        const FVector Left=Transform.InverseTransformPosition(World-Right*W);
        const FVector RightPos=Transform.InverseTransformPosition(World+Right*W);
        Verts.Add(Left);
        Verts.Add(RightPos);
        UVs.Add(FVector2D(0.f,V));
        UVs.Add(FVector2D(1.f,V));
        Colors.Add(FLinearColor(Thermal,V,FMath::Frac(ShapeSeed*.017f),Thermal>.075f?1.f:0.f));
        Colors.Add(FLinearColor(Thermal,V,FMath::Frac(ShapeSeed*.017f),Thermal>.075f?1.f:0.f));
    }
}

void UASTFireFreeFlameComponent::UpdateGeometry(bool bCreate)
{
    if (!IsValid(Render)) return;
    TArray<FVector> Verts;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;
    const int32 Vertices=(Tongues.Num()*(RingsPerTongue+BridgeRings)+
        MaxDetached*RingsPerDetached)*2 +
        MaxDetached*RingsPerDetached*7;
    Verts.Reserve(Vertices);
    UVs.Reserve(Vertices);
    Colors.Reserve(Vertices);

    FVector View=FVector::ForwardVector;
    // Source remains actor-facing. Detached fragments use their OWN
    // camera ray and the camera's screen-right vector below.
    FVector CameraLocation=GetOwner()->GetActorLocation()+
        FVector::ForwardVector*1000.f;
    FVector CameraRight=FVector::RightVector;
    if (const UWorld* World=GetWorld())
        if (const APlayerController* PC=World->GetFirstPlayerController())
            if (const APlayerCameraManager* Cam=PC->PlayerCameraManager)
            {
                CameraLocation=Cam->GetCameraLocation();
                CameraRight=Cam->GetCameraRotation().RotateVector(
                    FVector::RightVector);
                View=(CameraLocation-GetOwner()->GetActorLocation()).GetSafeNormal();
            }
    FVector Right=FVector::CrossProduct(FVector::UpVector,View).GetSafeNormal();
    if (Right.IsNearlyZero()) Right=FVector::RightVector;

    // All flame sections use a single perspective-projected square lattice.
    // Unlike world-up billboards, pixel aspect remains 1:1 when the camera
    // pitches. Fragment spin changes the shape inside this lattice.
    const bool bPointField=ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty());
    const float PixelPitch=bPointField?UpdatePointField(bCreate,Right):UpdateConnectedSource(bCreate, View, Right);
    const ASTFireLife::FFireProjection Projection(
        IsValid(Source)?Source->GetComponentLocation():GetOwner()->GetActorLocation(),GetWorld());
    const FTransform RenderTransform=Render->GetComponentTransform();
    TArray<int32> Triangles;
    const float Pitch=FMath::Max(.1875f,PixelPitch);
    RenderFieldFragments(Pitch,Verts,UVs,Colors,Triangles);
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    Normals.Init(-Projection.Forward,Verts.Num());
    Tangents.Init(FProcMeshTangent(1,0,0),Verts.Num());
    const FProcMeshSection* OldFragments=Render->GetProcMeshSection(0);
    if(Verts.IsEmpty()){if(OldFragments && !OldFragments->ProcVertexBuffer.IsEmpty())Render->ClearMeshSection(0);}
    else if(OldFragments && OldFragments->ProcVertexBuffer.Num()==Verts.Num())
        Render->UpdateMeshSection_LinearColor(0,Verts,Normals,UVs,Colors,Tangents,false);
    else Render->CreateMeshSection_LinearColor(0,Verts,Triangles,Normals,UVs,Colors,Tangents,false,false);
    Render->SetMaterial(0,Material);

    // Stage30: distinct high-contrast hot pixels, now exact shared-grid
    // cells. Alpha alone fades the square; no tapered subpixel triangles.
    TArray<FVector> SV;
    TArray<FVector2D> SU;
    TArray<FLinearColor> SC;
    TArray<int32> ST;
    SV.Reserve(MaxSparks*4);SU.Reserve(MaxSparks*4);
    SC.Reserve(MaxSparks*4);
    if (bCreate) ST.Reserve(MaxSparks*6);
    for (int32 I=0;I<MaxSparks;++I)
    {
        const FSpark& S=Sparks[I];
        const float Fade=S.bAlive?FMath::Pow(FMath::Clamp(
            1.f-S.Age/FMath::Max(.05f,S.Lifetime),0.f,1.f),1.35f):0.f;
        const bool bInFront=S.bAlive && FVector::DotProduct(S.Position-Projection.Cam,Projection.Forward)>10.f;
        const float Depth=Projection.Depth(S.Position);
        const FVector2D Center=Projection.Project(S.Position);
        const FVector2D Cell(FMath::FloorToDouble(Center.X/Pitch)*Pitch,
            FMath::FloorToDouble(Center.Y/Pitch)*Pitch);
        const float Temperature=S.EmissionHeat*Fade;
        const FLinearColor SparkColor=Temperature>.63f
            ? FLinearColor(1.f,.84f,.28f,1.f)
            : Temperature>.29f
                ? FLinearColor(1.f,.40f,.055f,1.f)
                : FLinearColor(.80f,.115f,.019f,1.f);
        for (int32 J=0;J<4;++J)
        {
            const FVector2D P=Cell+FVector2D((J%2)*Pitch,(J/2)*Pitch);
            const FVector World=bInFront?Projection.Unproject(P,Depth):S.Position;
            SV.Add(RenderTransform.InverseTransformPosition(World));
            SU.Add(FVector2D(J%2,J/2));
            SC.Add(FLinearColor(SparkColor.R,SparkColor.G,SparkColor.B,bInFront?Fade:0.f));
        }
        if(bCreate)
        {
            const int32 K=I*4;
            ST.Add(K);ST.Add(K+1);ST.Add(K+2);
            ST.Add(K+1);ST.Add(K+3);ST.Add(K+2);
        }
    }
    TArray<FVector> SN;
    TArray<FProcMeshTangent> SX;
    SN.Init(FVector::ForwardVector,SV.Num());
    SX.Init(FProcMeshTangent(1,0,0),SV.Num());
    if (bCreate)
    {
        Render->CreateMeshSection_LinearColor(
            2,SV,ST,SN,SU,SC,SX,false);
        if (IsValid(SparkMaterial)) Render->SetMaterial(2,SparkMaterial);
    }
    else
        Render->UpdateMeshSection_LinearColor(
            2,SV,SN,SU,SC,SX,false);

    // Section 1 was authored before this section, it contains the common
    // flame field AND the narrowing split bridges.
}


void UASTFireFreeFlameComponent::BuildSourceAnchors()
{
    SourceAnchors.Reset();
    if (!IsValid(Source)) return;
    const int32 Limit=Cast<UStaticMeshComponent>(Source) && !ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty())?192:MaxSourceAnchors;
    auto Add = [this,Limit](const FVector& Rest, int32 VertexIndex)
    {
        if (SourceAnchors.Num() >= Limit) return;
        for (const FSourceAnchor& Existing:SourceAnchors)
            if ((VertexIndex!=INDEX_NONE &&
                 Existing.SkinnedVertexIndex==VertexIndex) ||
                (VertexIndex==INDEX_NONE &&
                 Existing.SkinnedVertexIndex==INDEX_NONE &&
                 FVector::DistSquared(Existing.RestPosition,Rest)<36.f))
                return;
        FSourceAnchor A;
        A.RestPosition=Rest;
        A.SkinnedVertexIndex=VertexIndex;
        SourceAnchors.Add(A);
    };

    // Always include existing true surface ignition roots. Adding a source
    // must never change a tongue's length/split/birth state.
    for (const FTongue& T:Tongues)
        Add(T.RestPosition,T.SkinnedVertexIndex);

    if (USkinnedMeshComponent* Skel=Cast<USkinnedMeshComponent>(Source))
    {
        USkeletalMesh* Asset=Cast<USkeletalMesh>(Skel->GetSkinnedAsset());
        FSkeletalMeshRenderData* Data=Asset?Asset->GetResourceForRendering():nullptr;
        if (Data && Data->LODRenderData.Num()>0)
        {
            const FSkeletalMeshLODRenderData& Lod=Data->LODRenderData[0];
            const int32 Count=int32(Lod.GetNumVertices());
            const int32 Requested=FMath::Min(84,Count);
            for (int32 i=0;i<Requested;++i)
            {
                const int32 Index=FMath::Clamp(
                    (i*Count+Count/2)/Requested,0,Count-1);
                Add(FVector(Lod.StaticVertexBuffers.PositionVertexBuffer
                    .VertexPosition(Index)),Index);
            }
        }
    }
    else if (UStaticMeshComponent* Static=Cast<UStaticMeshComponent>(Source))
    {
        // Prefer actual triangle surfaces when the mesh retains CPU data.
        // Area-weighted stratification avoids concentrating all fuel on the
        // four initial collision contacts of a crate or an irregular mesh.
        if(!ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty()))
        if(UStaticMesh* Asset=Static->GetStaticMesh())
        if(Asset->bAllowCPUAccess || GIsEditor)
        if(const FStaticMeshRenderData* Data=Asset->GetRenderData())
        if(Data->LODResources.Num()>0)
        {
            const FStaticMeshLODResources& L=Data->LODResources[0];
            const FPositionVertexBuffer& Positions=L.VertexBuffers.PositionVertexBuffer;
            if(Positions.GetNumVertices()>0 && Positions.GetVertexData()!=nullptr)
            {
                const FIndexArrayView Indices=L.IndexBuffer.GetArrayView();
                const int32 Count=Indices.Num()/3;
                const int32 Stride=FMath::Max(1,FMath::DivideAndRoundUp(Count,4096));
                struct FTriangle { FVector A,B,C;float End; };
                TArray<FTriangle> Triangles;Triangles.Reserve(FMath::Min(Count,4096));
                float Area=0.f;
                for(int32 I=0;I<Count;I+=Stride)
                {
                    const uint32 IA=Indices[I*3],IB=Indices[I*3+1],IC=Indices[I*3+2];
                    if(IA>=Positions.GetNumVertices() || IB>=Positions.GetNumVertices() || IC>=Positions.GetNumVertices())continue;
                    const FVector A(Positions.VertexPosition(IA)),B(Positions.VertexPosition(IB)),C(Positions.VertexPosition(IC));
                    const float W=float(FVector::CrossProduct(B-A,C-A).Size());
                    if(W<=KINDA_SMALL_NUMBER)continue;
                    Area+=W;Triangles.Add({A,B,C,Area});
                }
                if(Area>0.f)
                {
                    int32 Tri=0;
                    // Low-discrepancy barycentric coordinates and an area
                    // CDF cover small and large triangles uniformly.
                    for(int32 I=0;I<Limit*2 && SourceAnchors.Num()<Limit;++I)
                    {
                        if(I==Limit)Tri=0;
                        const float Target=(float(I%Limit)+(I<Limit?.35f:.75f))*Area/Limit;
                        while(Tri+1<Triangles.Num() && Triangles[Tri].End<Target)++Tri;
                        const auto& T=Triangles[Tri];
                        const float U=FMath::Sqrt(FMath::Frac((I+1)*.754877666f));
                        const float V=FMath::Frac((I+1)*.569840296f);
                        Add(T.A*(1.f-U)+T.B*(U*(1.f-V))+T.C*(U*V),INDEX_NONE);
                    }
                }
            }
        }
        // Cooked meshes may strip CPU buffers: retain a bounded collision
        // sampling path, interleaving faces rather than filling one side.
        const FBoxSphereBounds Box=Static->CalcBounds(FTransform::Identity);
        const FVector H=Box.BoxExtent.GetAbs();
        const FTransform Transform=Static->GetComponentTransform();
        const float Far=H.GetMax()*2.8f+20.f;
        FCollisionQueryParams Query;Query.bTraceComplex=true;
        const int32 Grid=Limit>MaxSourceAnchors?6:4;
        for(int32 A=0;A<Grid && SourceAnchors.Num()<Limit;++A)
            for(int32 B=0;B<Grid && SourceAnchors.Num()<Limit;++B)
                for(int32 Face=0;Face<6 && SourceAnchors.Num()<Limit;++Face)
                {
                    const float U=(float(A)/(Grid-1))*1.76f-.88f;
                    const float V=(float(B)/(Grid-1))*1.76f-.88f;
                    FVector Point=Box.Origin;
                    FVector Normal=FVector::UpVector;
                    switch(Face)
                    {
                    case 0: Normal=FVector(1,0,0);Point+=FVector(H.X,U*H.Y,V*H.Z);break;
                    case 1: Normal=FVector(-1,0,0);Point+=FVector(-H.X,U*H.Y,V*H.Z);break;
                    case 2: Normal=FVector(0,1,0);Point+=FVector(U*H.X,H.Y,V*H.Z);break;
                    case 3: Normal=FVector(0,-1,0);Point+=FVector(U*H.X,-H.Y,V*H.Z);break;
                    case 4: Point+=FVector(U*H.X,V*H.Y,H.Z);break;
                    default: Normal=FVector(0,0,-1);Point+=FVector(U*H.X,V*H.Y,-H.Z);break;
                    }
                    FHitResult Hit;
                    if(Static->LineTraceComponent(Hit,
                        Transform.TransformPosition(Point+Normal*14.f),
                        Transform.TransformPosition(Point-Normal*Far),Query))
                        Add(Transform.InverseTransformPosition(FVector(Hit.ImpactPoint)),INDEX_NONE);
                }
    }

    if(SourceAnchors.IsEmpty())
    {
        for(int32 i=0;i<7;++i)
        {
            const float A=2.f*PI*float(i)/7.f;
            Add(FVector(FMath::Cos(A)*16.f,FMath::Sin(A)*16.f,0.f),
                INDEX_NONE);
        }
    }
    UE_LOG(LogTemp,Log,
        TEXT("AST Fire: source-first mantle %s uses %d skin/surface anchors"),
        *GetOwner()->GetName(),SourceAnchors.Num());
}

// CPU-updated screen-facing silhouette layer. No rectangular mask:
// outer grid cells are completely masked and discarded by the SAME
// M_AST_FireTongue palette as the existing tongue/bridge geometry.
// Mesh projection is sampled from CURRENT skinned vertices, so running,
// sprinting, and limb movement deform the single shared source.
float UASTFireFreeFlameComponent::UpdatePointField(bool bCreate,const FVector& Right)
{
    if(!IsValid(Render)||!IsValid(Source))return 1.f;
    const FVector Origin=IgnitionPoints.IsEmpty()?Source->GetComponentLocation():
        Source->GetComponentTransform().TransformPosition(IgnitionPoints[0]);
    const ASTFireLife::FFireProjection P(Source->GetComponentLocation(),GetWorld());
    const float Depth=FMath::Max(10.f,P.Depth(Origin)-FMath::Clamp(SizeScale*2.f,.2f,3.f));
    const FTransform Tr=Render->GetComponentTransform();
    const float Desired=FMath::Max(.1875f,2.55f*SizeScale*ASTFireLife::FFireProjection::Reference/Depth);
    // The logical field has a fixed number of cells. Its physical width
    // must not double when the camera crosses a power-of-two pitch boundary.
    FirePixelPitch=Desired;
    const float Pitch=FirePixelPitch;
    const FVector2D Projected=P.Project(Origin);
    const FVector2D GridOrigin(FMath::FloorToDouble(Projected.X/Pitch)*Pitch,
        FMath::FloorToDouble(Projected.Y/Pitch)*Pitch);
    const auto Random=[this](int32 Salt){return FMath::Abs(Uniform(Salt));};
    const bool bFirst=PointField.Num()!=FieldX*FieldY || FieldClock<0.f || Elapsed<FieldClock;
    if(bFirst)
    {
        PointField.Reset();PointField.SetNum(FieldX*FieldY);FieldFragments.Reset();
        FieldClock=Elapsed;FieldSplitCount=FieldTransferredCells=0;
        for(int32 Z=0;Z<FieldZones;++Z)
        {FieldGeneration[Z]=0;FieldAge[Z]=Random(915+Z*71)*.9f;}
    }
    const float Dt=FMath::Max(0.f,Elapsed-FieldClock);
    FieldClock=Elapsed;
    // Detached material is already in world space: source motion cannot
    // drag it back onto the next flame. Age-zero uses the exact old cells.
    for(int32 I=FieldFragments.Num()-1;I>=0;--I)
    {
        auto& F=FieldFragments[I];
        F.Age+=Dt;
        if(F.Age>=F.Life){FieldFragments.RemoveAtSwap(I);continue;}
        const float S=FMath::Clamp(SizeScale,.08f,2.f);
        F.Velocity+=(FVector::UpVector*(62.f+65.f*F.Age/F.Life)+
            F.Side*(32.f*FMath::Sin(F.Age*8.f+F.Serial*1.71f)))*S*Dt+Wind*.035f*Dt;
        F.Center+=F.Velocity*Dt;
    }
    if(Heat<.015f)
    {
        for(auto& C:PointField)C.bLit=false;
        Render->ClearMeshSection(1);
        return Pitch;
    }
    // Each zone modulates the height of the SAME continuous combustion
    // front. It owns no mesh, radius, centerline, capsule or sprite.
    // Neighbouring height peaks share a low irregular fuel bed; the upper
    // contour is a corrugated flame front with sharp asymmetrical valleys.
    float Heights[FieldZones],Centers[FieldZones],Widths[FieldZones];
    bool Cut[FieldZones]={};
    for(int32 Z=0;Z<FieldZones;++Z)
    {
        const int32 Salt=Z*213+FieldGeneration[Z]*997;
        const float Grow=.52f+.62f*Random(Salt+123);
        const float Rest=.12f+.14f*Random(Salt+125);
        FieldAge[Z]+=Dt;
        if(FieldAge[Z]>=Grow+Rest)
        {
            Cut[Z]=true;FieldAge[Z]=0.f;++FieldGeneration[Z];
        }
        const int32 NextSalt=Z*213+FieldGeneration[Z]*997;
        const float G=.52f+.62f*Random(NextSalt+123);
        const float R=.12f+.14f*Random(NextSalt+125);
        const float Progress=ASTFireLife::SmoothStep((FieldAge[Z]-R)/G);
        Centers[Z]=- .80f+.40f*Z+.07f*FMath::Sin(Elapsed*3.7f+Z*2.9f+Seed*.01f);
        Widths[Z]=.24f+.15f*Random(NextSalt+222);
        Heights[Z]=(9.f+11.f*Random(NextSalt+121))*Progress;
    }
    const float WindSide=FMath::Clamp(float(FVector::DotProduct(Wind,P.Right))/180.f,-1.f,1.f);
    const float Phase=Seed*.017f;
    TArray<FFieldCell> Next;Next.SetNum(FieldX*FieldY);
    for(int32 Y=0;Y<FieldY;++Y)for(int32 X=0;X<FieldX;++X)
    {
        const float H=float(Y)-3.f;
        const float Z=FMath::Clamp(H/30.f,0.f,1.f);
        const float Warp=Z*(.18f*FMath::Sin(H*.43f-Elapsed*6.f+Phase)+
            .10f*FMath::Sin(H*.87f-Elapsed*9.f+Phase*1.7f)+WindSide*.32f);
        const float U=(float(X)-19.5f)/13.f-Warp;
        const float A=FMath::Abs(U);
        // Lower edge is uneven and comparatively flat, not a disk.
        const float Bottom=-2.5f+6.0f*FMath::Pow(A,2.4f)+
            .8f*FMath::Sin(U*13.f+Elapsed*4.f+Phase);
        const float BaseHeight=(10.f+1.6f*FMath::Sin(U*10.f-Elapsed*5.f+Phase))*
            FMath::Clamp((1.14f-A)/.36f,0.f,1.f);
        float Ridge=0.f;
        for(int32 I=0;I<FieldZones;++I)
        {
            const float D=U-Centers[I]-.13f*Z*FMath::Sin(Elapsed*4.7f+I*2.5f+Phase+H*.21f);
            const float W=Widths[I]*(D>0.f?.83f:1.18f);
            // Pointed, irregular advancing fronts, NOT radial distance
            // from a rounded tongue. A broad valley separates peaks.
            const float Shape=FMath::Pow(FMath::Max(0.f,1.f-FMath::Abs(D)/W),.83f);
            Ridge=FMath::Max(Ridge,Heights[I]*Shape);
        }
        const float Detail=1.2f*FMath::Sin(U*29.f+H*.55f-Elapsed*8.f+Phase)+
            .75f*FMath::Sin(U*51.f-H*.92f+Elapsed*11.f);
        const float Dying=FMath::SmoothStep(0.f,.30f,Heat);
        const float Top=(BaseHeight+Ridge+Detail)*Dying;
        const float Edge=1.04f-.04f*Z+.10f*FMath::Sin(H*.53f-Elapsed*6.f+Phase)+
            .045f*FMath::Sin(H*1.31f+Elapsed*8.f);
        if(H<Bottom || H>Top || A>Edge*FMath::Lerp(.22f,1.f,Dying))continue;
        auto& C=Next[Y*FieldX+X];C.bLit=true;
        C.World=P.Unproject(GridOrigin+FVector2D((X-20+.5f)*Pitch,(Y-3+.5f)*Pitch),Depth);
        // Advect heat independently of the advancing contour. It is not
        // a distance-to-center ring; irregular hot islands travel upwards.
        const float Flow=.62f*FMath::Sin(U*6.2f+H*.34f-Elapsed*7.7f+Phase)+
            .38f*FMath::Sin(U*14.1f-H*.63f+Elapsed*10.8f+Phase);
        const float Interior=FMath::Clamp((Edge-A)*3.2f,0.f,1.f);
        const float Rim=FMath::Clamp((Top-H)*.25f,0.f,1.f);
        C.Temperature=Heat*FMath::Clamp(.16f+.59f*Interior*Rim*
            FMath::Clamp((H-Bottom)*.36f,0.f,1.f)+.16f*Flow-.19f*Z+
            .10f*FMath::Exp(-FMath::Square((H-5.f)/5.f)),0.f,1.f);
    }
    // Transfer exactly the cells that were visible in the preceding field
    // but have been lost by a local cut. No separately generated shape.
    for(int32 Z=0;Z<FieldZones;++Z)if(Cut[Z])
    {
        TArray<int32> Removed;FVector Center=FVector::ZeroVector;
        for(int32 Y=9;Y<FieldY;++Y)for(int32 X=0;X<FieldX;++X)
        {
            const int32 I=Y*FieldX+X;
            if(!PointField[I].bLit || Next[I].bLit)continue;
            const float U=(float(X)-19.5f)/13.f;
            int32 Owner=0;float Dist=BIG_NUMBER;
            for(int32 K=0;K<FieldZones;++K)
            {
                const float D=FMath::Abs(U-Centers[K]);
                if(D<Dist){Dist=D;Owner=K;}
            }
            if(Owner!=Z)continue;
            Removed.Add(I);Center+=PointField[I].World;
        }
        if(Removed.Num()<2)continue;
        if(FieldFragments.Num()>=12)FieldFragments.RemoveAt(0);
        auto& F=FieldFragments.AddDefaulted_GetRef();
        F.Center=Center/Removed.Num();F.Serial=FieldSplitCount+1;F.Life=.66f+.31f*Random(Z*37+FieldGeneration[Z]*811);
        const float S=FMath::Clamp(SizeScale,.08f,2.f);
        F.Side=P.Right;
        F.Velocity=FVector::UpVector*(32.f+Random(Z*91+FieldGeneration[Z]*77)*19.f)*S+
            F.Side*(Centers[Z]*36.f*S)+Wind*.10f;
        for(int32 I:Removed)
        {
            const FVector Offset=PointField[I].World-F.Center;
            F.Cells.Add({FVector2D(FVector::DotProduct(Offset,P.Right),FVector::DotProduct(Offset,P.Up)),PointField[I].Temperature,I});
        }
        ++FieldSplitCount;FieldTransferredCells+=Removed.Num();
    }
    PointField=MoveTemp(Next);
    // Keep the old non-rendered root bookkeeping for smoke, burn contact
    // and spark emission. No arc contributes to the point flame silhouette.
    ConnectedRoots.Reset();
    for(FTongue& T:Tongues)
    {
        const FVector Root=SampleRoot(T);ConnectedRoots.Add(Root);
        T.DisplayClock=Elapsed;T.DisplayHeat=Heat;T.DisplayGeneration=T.Generation;
        T.bDisplayedPinch=T.Life==EFlameLife::Bridging && T.PhaseAge>=T.BridgeSeconds;
        for(int32 K=0;K<RingsPerTongue;++K)
        {
            const float V=float(K)/(RingsPerTongue-1);
            T.DisplayArc[K]=TongueCenterAt(T,Root,V,Right);
            T.DisplayRadius[K]=TongueRadiusAt(T,V);
        }
    }
    TArray<FVector> Verts,Normals;
    TArray<FVector2D> UVs;TArray<FLinearColor> Colors;
    TArray<FProcMeshTangent> Tangents;TArray<int32> Tris;
    for(const auto& Cell:PointField)if(Cell.bLit)
    {
        const FVector2D Pixel=P.Project(Cell.World);
        const FVector2D Lo(FMath::FloorToDouble(Pixel.X/Pitch)*Pitch,FMath::FloorToDouble(Pixel.Y/Pitch)*Pitch);
        const int32 First=Verts.Num();
        for(int32 K=0;K<4;++K)
        {
            const FVector2D Q=Lo+FVector2D((K%2)*Pitch,(K/2)*Pitch);
            Verts.Add(Tr.InverseTransformPosition(P.Unproject(Q,Depth)));
            UVs.Add(FVector2D(K%2,K/2));Normals.Add(-P.Forward);
            Colors.Add(FLinearColor(ASTFireLife::ToneCode(Cell.Temperature),0.f,0.f,1.f));
            Tangents.Add(FProcMeshTangent(1,0,0));
        }
        Tris.Append({First,First+2,First+1,First+1,First+2,First+3});
    }
    const FProcMeshSection* Old=Render->GetProcMeshSection(1);
    if(Verts.IsEmpty())Render->ClearMeshSection(1);
    else if(Old && Old->ProcVertexBuffer.Num()==Verts.Num())
        Render->UpdateMeshSection_LinearColor(1,Verts,Normals,UVs,Colors,Tangents,false);
    else Render->CreateMeshSection_LinearColor(1,Verts,Tris,Normals,UVs,Colors,Tangents,false,false);
    Render->SetMaterial(1,Material);
    return Pitch;
}

void UASTFireFreeFlameComponent::RenderFieldFragments(float Pitch,TArray<FVector>& Verts,
    TArray<FVector2D>& UVs,TArray<FLinearColor>& Colors,TArray<int32>& Triangles)
{
    const ASTFireLife::FFireProjection P(Source->GetComponentLocation(),GetWorld());
    const FTransform Tr=Render->GetComponentTransform();
    for(const FFieldFragment& F:FieldFragments)
    {
        const float Fade=ASTFireLife::ArcFade(F.Age,F.Life);
        const float Shrink=FMath::Pow(Fade,.80f);
        if(Fade<=0.f)continue;
        const float Depth=P.Depth(F.Center);
        if(FVector::DotProduct(F.Center-P.Cam,P.Forward)<=10.f)continue;
        const FVector2D Center=P.Project(F.Center);
        const float Scale=ASTFireLife::FFireProjection::Reference/Depth;
        const float Angle=.50f*F.Age*FMath::Sin(F.Age*5.f+F.Serial*1.71f);
        const float CS=FMath::Cos(Angle),SN=FMath::Sin(Angle);
        TMap<FIntPoint,float> Pixels;
        for(const auto& C:F.Cells)
        {
            const FVector2D Rotated(C.Offset.X*CS-C.Offset.Y*SN,C.Offset.X*SN+C.Offset.Y*CS);
            const FVector2D Q=Center+Rotated*(Scale*Shrink);
            const FIntPoint K(FMath::FloorToInt(Q.X/Pitch),FMath::FloorToInt(Q.Y/Pitch));
            float& Temperature=Pixels.FindOrAdd(K);
            Temperature=FMath::Max(Temperature,C.Temperature*FMath::Pow(Fade,.83f));
        }
        for(const auto& Pair:Pixels)
        {
            if(Pair.Value<.025f)continue;
            const int32 First=Verts.Num();
            for(int32 K=0;K<4;++K)
            {
                const FVector2D Q((Pair.Key.X+K%2)*Pitch,(Pair.Key.Y+K/2)*Pitch);
                Verts.Add(Tr.InverseTransformPosition(P.Unproject(Q,Depth)));
                UVs.Add(FVector2D(K%2,K/2));
                Colors.Add(FLinearColor(ASTFireLife::ToneCode(Pair.Value),0.f,0.f,1.f));
            }
            Triangles.Append({First,First+2,First+1,First+1,First+2,First+3});
        }
    }
}

float UASTFireFreeFlameComponent::UpdateConnectedSource(
    bool bCreate,const FVector& View,const FVector& Right)
{
    if(!IsValid(Render)||!IsValid(Source))return 1.f;
    const FVector Origin=Source->GetComponentLocation();
    const ASTFireLife::FFireProjection Projection(Origin,GetWorld());
    const FTransform Tr=Render->GetComponentTransform();
    const float OriginDepth=Projection.Depth(Origin);
    const float S=FMath::Clamp(SizeScale,.25f,2.f);
    const float Bias=FMath::Clamp(1.8f*S,.5f,3.f);
    const bool bFirst=MeshFieldClock<0.f || Elapsed<MeshFieldClock;
    if(bFirst)
    {
        MeshPrevious.Reset();FieldFragments.Reset();MeshCuts=MeshTransferred=0;
        for(int32 I=0;I<Tongues.Num();++I)MeshGeneration[I]=Tongues[I].Generation;
        MeshFieldClock=Elapsed;
    }
    const float Dt=FMath::Max(0.f,Elapsed-MeshFieldClock);
    MeshFieldClock=Elapsed;
    for(int32 I=FieldFragments.Num()-1;I>=0;--I)
    {
        auto& F=FieldFragments[I];F.Age+=Dt;
        if(F.Age>=F.Life){FieldFragments.RemoveAtSwap(I);continue;}
        F.Velocity+=(FVector::UpVector*(62.f+65.f*F.Age/F.Life)+
            F.Side*(32.f*FMath::Sin(F.Age*8.f+F.Serial*1.71f)))*S*Dt+Wind*.035f*Dt;
        F.Center+=F.Velocity*Dt;
    }
    if(Heat<.015f)
    {
        MeshPrevious.Reset();Render->ClearMeshSection(1);
        return FMath::Max(.1875f,FirePixelPitch);
    }
    USkinnedMeshComponent* Skel=Cast<USkinnedMeshComponent>(Source);
    const FSkeletalMeshLODRenderData* Lod=nullptr;
    FSkinWeightVertexBuffer* Weights=nullptr;
    if(Skel)
        if(USkeletalMesh* Asset=Cast<USkeletalMesh>(Skel->GetSkinnedAsset()))
            if(FSkeletalMeshRenderData* Data=Asset->GetResourceForRendering())
                if(Data->LODRenderData.Num()>0)
                {Lod=&Data->LODRenderData[0];Weights=const_cast<FSkeletalMeshLODRenderData*>(Lod)->GetSkinWeightVertexBuffer();}
    // Simulation roots describe the available fuel and phase, not drawable
    // objects. The same roots are retained for burn contact, smoke and sparks.
    bool Cut[MaxTongues]={};
    float Heights[MaxTongues]={};
    FVector Roots[MaxTongues];
    ConnectedRoots.Reset();
    const float Dying=FMath::SmoothStep(0.f,.30f,Heat);
    const float MaxHeight=(Skel?28.f:36.f)*FMath::Sqrt(S)*Dying;
    for(int32 I=0;I<Tongues.Num();++I)
    {
        FTongue& T=Tongues[I];Roots[I]=SampleRoot(T);ConnectedRoots.Add(Roots[I]);
        Cut[I]=!bFirst && MeshGeneration[I]!=T.Generation;
        MeshGeneration[I]=T.Generation;
        if(!T.bActive || T.Age<0.f)continue;
        const float Growth=FMath::Clamp(T.CurrentLength/FMath::Max(1.f,T.MaxLength),0.f,1.f);
        Heights[I]=(5.f+MaxHeight*(.60f+.40f*FMath::Abs(Uniform(I*103+T.Generation*37))))*
            (.15f+.85f*Growth);
        T.DisplayClock=Elapsed;T.DisplayHeat=Heat;T.DisplayGeneration=T.Generation;
        T.bDisplayedPinch=T.Life==EFlameLife::Bridging && T.PhaseAge>=T.BridgeSeconds;
        for(int32 K=0;K<RingsPerTongue;++K)
        {
            const float V=float(K)/(RingsPerTongue-1);
            T.DisplayArc[K]=TongueCenterAt(T,Roots[I],V,Right);
            T.DisplayRadius[K]=TongueRadiusAt(T,V);
        }
    }
    struct FFuelSeed { FVector2D P; float Depth,Radius,Influence;int32 Owner; };
    TArray<FFuelSeed> Fuel;
    Fuel.Reserve(SourceAnchors.Num()+Tongues.Num());
    FVector2D Lo(BIG_NUMBER,BIG_NUMBER),Hi(-BIG_NUMBER,-BIG_NUMBER);
    const float Reach=(Skel?FMath::Lerp(24.f,55.f,Spread):FMath::Lerp(25.f,65.f,Spread));
    auto AddFuel=[&](const FVector& World,const FVector& Rest,int32 Forced)
    {
        int32 Owner=Forced;float Best=BIG_NUMBER;
        if(Owner==INDEX_NONE)
            for(int32 I=0;I<Tongues.Num();++I)
            {
                const FTongue& T=Tongues[I];
                if(!T.bActive || T.Age<0.f)continue;
                const float D=float(FVector::DistSquared(Rest,T.RestPosition));
                if(D<Best){Best=D;Owner=I;}
            }
        if(Owner==INDEX_NONE || (Forced==INDEX_NONE && Best>Reach*Reach))return;
        if(FVector::DotProduct(World-Projection.Cam,Projection.Forward)<=12.f)return;
        float Influence=Forced!=INDEX_NONE?1.f:FMath::Clamp(1.f-FMath::Sqrt(Best)/Reach,.12f,1.f);
        if(!Skel && Forced==INDEX_NONE)
        {
            const float Wave=.61f*FMath::Sin(float(Rest.X*.071+Rest.Y*.053+Rest.Z*.059)-Elapsed*5.1f+Seed*.017f)+
                .39f*FMath::Sin(float(Rest.X*.039-Rest.Y*.083+Rest.Z*.107)+Elapsed*7.2f);
            if(Wave<-.36f) return;
            Influence*=.78f+.22f*Wave;
        }
        const float Depth=FMath::Max(10.f,Projection.Depth(World)-Bias);
        const FVector2D Q=Projection.Project(World);
        const float Radius=FMath::Lerp(.22f,1.f,Dying)*(Skel?FMath::Lerp(5.f,9.5f,Influence):FMath::Lerp(6.f,10.f,Influence))*
            FMath::Sqrt(S)*ASTFireLife::FFireProjection::Reference/Depth;
        Fuel.Add({Q,Depth,Radius,Influence,Owner});
        Lo.X=FMath::Min(Lo.X,Q.X-Radius);Lo.Y=FMath::Min(Lo.Y,Q.Y-Radius);
        Hi.X=FMath::Max(Hi.X,Q.X+Radius);Hi.Y=FMath::Max(Hi.Y,Q.Y+Radius);
    };
    for(const FSourceAnchor& A:SourceAnchors)
    {
        FVector W=Source->GetComponentTransform().TransformPosition(A.RestPosition);
        if(Skel && Lod && Weights && A.SkinnedVertexIndex>=0 &&
            A.SkinnedVertexIndex<int32(Lod->GetNumVertices()))
        {
            const FVector3f P=USkinnedMeshComponent::GetSkinnedVertexPosition(Skel,A.SkinnedVertexIndex,*Lod,*Weights);
            W=Skel->GetComponentTransform().TransformPosition(FVector(P));
        }
        AddFuel(W,A.RestPosition,INDEX_NONE);
    }
    for(int32 I=0;I<Tongues.Num();++I)
        if(Tongues[I].bActive && Tongues[I].Age>=0.f)
            AddFuel(Roots[I],Tongues[I].RestPosition,I);
    if(Fuel.IsEmpty())
    {
        MeshPrevious.Reset();Render->ClearMeshSection(1);
        return FMath::Max(.1875f,FirePixelPitch);
    }

    // A single lattice for the whole visible burning surface. Inject fuel
    // near real samples, then transport the shared field upwards. No
    // circular flame blobs, swept ribbons, or geometry per simulation root.
    constexpr int32 MaxXCells=144;
    constexpr int32 MaxYCells=224;
    const float Nominal=FMath::Max(.1875f,2.0f*ASTFireLife::FFireProjection::Reference/OriginDepth);
    const float MaxRise=MaxHeight*ASTFireLife::FFireProjection::Reference/FMath::Max(10.f,OriginDepth*.70f);
    Lo.X-=MaxRise*.55f+Nominal*2.f;Hi.X+=MaxRise*.55f+Nominal*2.f;
    Lo.Y-=Nominal*2.f;Hi.Y+=MaxRise+Nominal*3.f;
    const float Required=FMath::Max(Nominal,FMath::Max(float((Hi.X-Lo.X)/(MaxXCells-2)),float((Hi.Y-Lo.Y)/(MaxYCells-2))));
    // Store a world-space grid spacing in the existing unused width cache.
    // Changing active surface cells may enlarge the envelope, but must
    // never continuously resize pixels. Perspective changes remain smooth.
    const float WorldRequired=Required*OriginDepth/ASTFireLife::FFireProjection::Reference;
    const float WorldNominal=Nominal*OriginDepth/ASTFireLife::FFireProjection::Reference;
    if(FireVisualWidth<=0.f || WorldRequired>FireVisualWidth || WorldNominal<FireVisualWidth*.52f && WorldRequired<FireVisualWidth*.65f)
        FireVisualWidth=WorldNominal*FMath::Pow(1.20f,FMath::CeilToFloat(FMath::Loge(FMath::Max(1.f,WorldRequired/WorldNominal))/FMath::Loge(1.20f)));
    FirePixelPitch=FMath::Max(.1875f,FireVisualWidth*ASTFireLife::FFireProjection::Reference/OriginDepth);
    const float Pitch=FirePixelPitch;
    const int32 GX=FMath::FloorToInt(Lo.X/Pitch),GY=FMath::FloorToInt(Lo.Y/Pitch);
    const int32 NX=FMath::Clamp(FMath::CeilToInt(Hi.X/Pitch)-GX,1,MaxXCells);
    const int32 NY=FMath::Clamp(FMath::CeilToInt(Hi.Y/Pitch)-GY,1,MaxYCells);
    struct FGasCell
    {
        float Strength=0.f;
        float Depth=0.f;
        float Temperature=0.f;
        float Rise=0.f;
        int32 Owner=INDEX_NONE;
    };
    TArray<FGasCell> Surface,Gas;Surface.SetNum(NX*NY);Gas.SetNum(NX*NY);
    const float Phase=Seed*.017f;
    // Project actual fuel samples into a narrow surface mask, with overlap
    // resolved before the rising contour is evaluated.
    for(const FFuelSeed& F:Fuel)
    {
        const float R=FMath::Max(Pitch*.76f,F.Radius);
        const int32 X0=FMath::Clamp(FMath::FloorToInt((F.P.X-R)/Pitch)-GX,0,NX-1);
        const int32 X1=FMath::Clamp(FMath::CeilToInt((F.P.X+R)/Pitch)-GX,0,NX-1);
        const int32 Y0=FMath::Clamp(FMath::FloorToInt((F.P.Y-R)/Pitch)-GY,0,NY-1);
        const int32 Y1=FMath::Clamp(FMath::CeilToInt((F.P.Y+R)/Pitch)-GY,0,NY-1);
        for(int32 Y=Y0;Y<=Y1;++Y)for(int32 X=X0;X<=X1;++X)
        {
            const FVector2D Q((GX+X+.5f)*Pitch,(GY+Y+.5f)*Pitch);
            const float DX=float(FMath::Abs(Q.X-F.P.X))/R,DY=float(FMath::Abs(Q.Y-F.P.Y))/R;
            // A compact, irregular fuel support, not a rendered circle.
            const float Weight=FMath::Clamp(1.f-(.65f*FMath::Max(DX,DY)+.35f*(DX+DY)),0.f,1.f);
            const bool bAnchor=(X+GX==FMath::FloorToInt(F.P.X/Pitch) && Y+GY==FMath::FloorToInt(F.P.Y/Pitch));
            if(Weight<.12f && !bAnchor)continue;
            const float Value=FMath::Max(bAnchor?.65f:0.f,Weight)*(.72f+.28f*F.Influence);
            auto& Cell=Surface[Y*NX+X];
            if(Value>Cell.Strength || (Value>=Cell.Strength*.78f && F.Depth<Cell.Depth))
            {Cell.Strength=FMath::Max(Cell.Strength,Value);Cell.Depth=F.Depth;Cell.Owner=F.Owner;}
        }
    }
    // Turbulent transport of the fuel bed into a jagged, independently
    // cooling front. Distinct phase regions shape its crest, but every
    // occupied cell belongs to the SAME field. No concentric outlines.
    const float WindSide=FMath::Clamp(float(FVector::DotProduct(Wind,Projection.Right))/180.f,-1.f,1.f);
    const FVector2D Gravity=Projection.Project(Origin+FVector::UpVector*20.f)-Projection.Project(Origin);
    const FVector2D Up2=Gravity.GetSafeNormal();
    const FVector2D UpAxis=Up2.IsNearlyZero()?FVector2D(0,1):Up2;
    const FVector2D SideAxis(UpAxis.Y,-UpAxis.X);
    TArray<float> Corrugation,Flow;Corrugation.SetNum(NX*NY);Flow.SetNum(NX*NY);
    for(int32 Y=0;Y<NY;++Y)for(int32 X=0;X<NX;++X)
    {
        const float U=float(X+GX)*.31f,V=float(Y+GY)*.25f;
        const int32 I=Y*NX+X;
        Corrugation[I]=.57f*FMath::Sin(U*1.9f+V*.7f-Elapsed*6.1f+Phase)+
            .43f*FMath::Sin(U*3.7f-V*1.4f+Elapsed*8.4f+Phase*.61f);
        Flow[I]=.62f*FMath::Sin(U*1.4f+V-Elapsed*7.7f+Phase)+
            .38f*FMath::Sin(U*3.1f-V*1.7f+Elapsed*10.8f+Phase);
    }
    for(int32 Y=0;Y<NY;++Y)for(int32 X=0;X<NX;++X)
    {
        const auto& F=Surface[Y*NX+X];
        if(F.Owner==INDEX_NONE)continue;
        // Deterministic per-cell loss during final cooling, no random flicker.
        if(Dying<1.f && ASTFireLife::Hash(float((X+GX)*13+(Y+GY)*71)+Seed*.031f)>Dying)continue;
        const float H=Heights[F.Owner]*ASTFireLife::FFireProjection::Reference/F.Depth/Pitch;
        const float Crest=FMath::Max(1.f,H*(.55f+.45f*F.Strength)+Corrugation[Y*NX+X]*2.0f);
        const float Drift=FMath::Clamp(float(FVector::DotProduct(Tongues[F.Owner].WorldVelocity,Projection.Right))/180.f,-1.f,1.f);
        for(int32 Step=0;Step<=FMath::CeilToInt(Crest) && Step<=64;++Step)
        {
            const float Rise=float(Step)/FMath::Max(1.f,Crest);
            const float Side=Rise*float(Step)*(.32f*WindSide-.22f*Drift+
                .24f*FMath::Sin(float(X)*.28f+float(Step)*.41f-Elapsed*5.2f+Phase));
            const FVector2D Offset=UpAxis*float(Step)+SideAxis*Side;
            const int32 TX=X+FMath::RoundToInt(Offset.X),TY=Y+FMath::RoundToInt(Offset.Y);
            if(TX<0 || TX>=NX || TY<0 || TY>=NY)continue;
            const int32 I=TY*NX+TX;
            const float Edge=F.Strength*(1.f-.64f*FMath::Pow(Rise,1.7f))+
                .20f*Corrugation[I]*FMath::Min(1.f,Rise*3.f);
            if(Step>0 && (Rise>1.f || Edge<.22f+.13f*Rise))continue;
            const float Hot=FMath::Clamp(.18f+.49f*FMath::Clamp(Edge*1.4f,0.f,1.f)+
                .17f*Flow[I]+.14f*FMath::Sin(PI*FMath::Clamp(Rise,0.f,1.f))-.24f*Rise,0.f,1.f);
            auto& Cell=Gas[I];
            const float Score=Step==0?FMath::Max(.48f,Edge):Edge;
            if(Score>Cell.Strength)
            {
                Cell.Strength=Score;Cell.Depth=F.Depth;Cell.Owner=F.Owner;
                Cell.Rise=Rise;Cell.Temperature=Hot*Heat;
            }
            else if(Cell.Owner!=INDEX_NONE && F.Depth<Cell.Depth && Score>=Cell.Strength*.75f)
                Cell.Depth=F.Depth;
        }
    }

    // When a phase resets, only cells actually lost from the preceding
    // visible gas field may become a fragment. Reproject the old world
    // samples into today's lattice; camera motion alone cannot create a cut.
    TArray<FMeshFieldCell> Lost[MaxTongues];
    for(const auto& C:MeshPrevious)
    {
        if(C.Owner<0 || C.Owner>=Tongues.Num() || !Cut[C.Owner] || C.Rise<.35f)continue;
        const FVector2D Q=Projection.Project(C.World);
        const int32 X=FMath::FloorToInt(Q.X/Pitch)-GX,Y=FMath::FloorToInt(Q.Y/Pitch)-GY;
        if(X>=0 && X<NX && Y>=0 && Y<NY && Gas[Y*NX+X].Owner!=INDEX_NONE)continue;
        Lost[C.Owner].Add(C);
    }
    for(int32 I=0;I<Tongues.Num();++I)
    {
        if(Lost[I].Num()<2)continue;
        // Separate disconnected islands. One world-space fragment must not
        // contain a shoulder and a foot simply because both lost fuel.
        TMap<FIntPoint,int32> Keys;
        for(int32 J=0;J<Lost[I].Num();++J)
        {
            const FVector2D Q=Projection.Project(Lost[I][J].World);
            Keys.Add(FIntPoint(FMath::FloorToInt(Q.X/Pitch),FMath::FloorToInt(Q.Y/Pitch)),J);
        }
        TSet<int32> Seen;
        const FIntPoint Neighbours[4]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
        for(const auto& Entry:Keys)
        {
            if(Seen.Contains(Entry.Value))continue;
            TArray<int32> Group;Group.Add(Entry.Value);Seen.Add(Entry.Value);
            FVector Center=FVector::ZeroVector;
            for(int32 J=0;J<Group.Num();++J)
            {
                const auto& C=Lost[I][Group[J]];Center+=C.World;
                const FVector2D Q=Projection.Project(C.World);
                const FIntPoint K(FMath::FloorToInt(Q.X/Pitch),FMath::FloorToInt(Q.Y/Pitch));
                for(const auto& N:Neighbours)
                    if(const int32* Index=Keys.Find(K+N))
                        if(!Seen.Contains(*Index)){Seen.Add(*Index);Group.Add(*Index);}
            }
            if(Group.Num()<2)continue;
            if(FieldFragments.Num()>=16)FieldFragments.RemoveAt(0);
            auto& F=FieldFragments.AddDefaulted_GetRef();
            F.Center=Center/Group.Num();F.Serial=++MeshCuts;F.Side=Projection.Right;
            F.Life=.55f+.34f*FMath::Abs(Uniform(I*911+MeshCuts*37));
            F.Velocity=FVector::UpVector*(26.f+22.f*FMath::Abs(Uniform(MeshCuts*73)))*S+
                F.Side*(FMath::Abs(Uniform(MeshCuts*71))-.5f)*36.f*S+Wind*.1f;
            const float D=Projection.Depth(F.Center);
            const FVector2D CP=Projection.Project(F.Center);
            for(int32 J:Group)
            {
                const auto& C=Lost[I][J];
                // Preserve the projected cell coordinates at birth, even
                // when the original surface was at varying depths.
                F.Cells.Add({(Projection.Project(C.World)-CP)*(D/ASTFireLife::FFireProjection::Reference),C.Temperature,J});
            }
            MeshTransferred+=Group.Num();
        }
    }

    MeshPrevious.Reset();
    TArray<FVector> Verts,Normals;TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;TArray<FProcMeshTangent> Tangents;TArray<int32> Tris;
    for(int32 Y=0;Y<NY;++Y)for(int32 X=0;X<NX;++X)
    {
        const auto& Cell=Gas[Y*NX+X];
        if(Cell.Owner==INDEX_NONE)continue;
        const FVector2D Q((GX+X)*Pitch,(GY+Y)*Pitch);
        MeshPrevious.Add({Projection.Unproject(Q+FVector2D(Pitch*.5f,Pitch*.5f),Cell.Depth),Cell.Temperature,Cell.Rise,Cell.Owner});
        const int32 First=Verts.Num();
        for(int32 K=0;K<4;++K)
        {
            const FVector2D P=Q+FVector2D(K%2,K/2)*Pitch;
            Verts.Add(Tr.InverseTransformPosition(Projection.Unproject(P,Cell.Depth)));
            UVs.Add(FVector2D(K%2,K/2));Normals.Add(-Projection.Forward);
            Colors.Add(FLinearColor(ASTFireLife::ToneCode(Cell.Temperature),0.f,0.f,1.f));
            Tangents.Add(FProcMeshTangent(1,0,0));
        }
        Tris.Append({First,First+2,First+1,First+1,First+2,First+3});
    }
    const FProcMeshSection* Old=Render->GetProcMeshSection(1);
    if(Verts.IsEmpty())Render->ClearMeshSection(1);
    else if(Old && Old->ProcVertexBuffer.Num()==Verts.Num())
        Render->UpdateMeshSection_LinearColor(1,Verts,Normals,UVs,Colors,Tangents,false);
    else Render->CreateMeshSection_LinearColor(1,Verts,Tris,Normals,UVs,Colors,Tangents,false,false);
    Render->SetMaterial(1,Material);
    return Pitch;
}

void UASTFireFreeFlameComponent::TickComponent(
    float Dt,ELevelTick LevelTick,FActorComponentTickFunction* Function)
{
    Super::TickComponent(Dt,LevelTick,Function);
    if (!bGenerated || !IsValid(Render) || Dt<=KINDA_SMALL_NUMBER) return;
    bool bHasSparks=false;
    for (const FSpark& S:Sparks)
        if (S.bAlive) { bHasSparks=true; break; }
    bool bHasSmoke=false;
    for (const FSmoke& S:Smoke)
        if (S.bAlive) { bHasSmoke=true; break; }
    if ((!bVisible || Heat<.005f) && !bHasSparks && !bHasSmoke && FieldFragments.IsEmpty() && SmokeAfterglowSeconds<=0.f)
    {
        // No active fire or embers: skip expensive field reconstruction.
        VisualEnergy=FMath::FInterpTo(VisualEnergy,0.f,Dt,6.f);
        if (IsValid(Render)) Render->SetVisibility(false);
        if (IsValid(SmokeRender)) SmokeRender->SetVisibility(false);
        return;
    }
    const float SimDt=FMath::Min(Dt,.08f);
    AdvanceSimulation(SimDt);
    UpdateDetachedVortices(SimDt);
    UpdateSparks(SimDt);
    UpdateSmoke(SimDt);
    UpdateGeometry(false);
    if (IsValid(Render)) Render->SetVisibility(true);
}

bool UASTFireFreeFlameComponent::GetFireLightCenter(FVector& Out) const
{
    if(!IsValid(Source)||!bGenerated)return false;
    if(PointField.Num()==FieldX*FieldY)
    {
        // Logical field height is independent of camera/depth quantization.
        float Sum=0.f,Height=0.f;
        for(int32 I=0;I<PointField.Num();++I)
        {
            const auto& C=PointField[I];
            if(!C.bLit)continue;
            const float W=C.Temperature*C.Temperature;
            Sum+=W;Height+=W*(float(I/FieldX)-3.f);
        }
        if(Sum<=.001f)return false;
        Out=IgnitionPoints.IsEmpty()?Source->GetComponentLocation():
            Source->GetComponentTransform().TransformPosition(IgnitionPoints[0]);
        Out.Z+=FMath::Clamp(Height/Sum*2.55f*SizeScale,3.f*SizeScale,60.f*SizeScale);
        return true;
    }
    // Root positions are actual simulated surface samples in world space.
    Out=FVector::ZeroVector;float Sum=0.f;
    for(const auto& T:Tongues)
    {
        if(!T.bActive||T.Age<0.f)continue;
        const float W=FMath::Max(1.f,T.CurrentLength*T.CurrentWidth);
        Out+=T.WorldPrevious*W;Sum+=W;
    }
    if(Sum<=0.f)return false;
    Out/=Sum;return true;
}
