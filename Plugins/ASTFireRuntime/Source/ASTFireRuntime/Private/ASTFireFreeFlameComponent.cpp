#include "ASTFireFreeFlameComponent.h"

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
    float ArcHeat(float V,float Core,float Seed,float Clock)
    {
        const float Flow=.64f*FMath::Sin(V*13.f-Clock*8.8f+Seed)+
            .36f*FMath::Sin(V*24.f-Clock*12.1f+Seed*.7f);
        return FMath::Clamp(.23f+.73f*Core-.30f*V+.12f*Flow,0.f,1.f);
    }
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
    Heat = FMath::Clamp(InHeat, 0.f, 1.f);
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
    if (IsValid(Render)) Render->SetVisibility(bVisible);
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
    SmokeLightScanDebt = 0.f;
    SmokeSimDebt = 0.f;
    SmokeFireSize = 0.f;
    SmokePixelPitch = 0.f;
    FirePixelPitch = FireVisualHeight = FireVisualWidth = 0.f;
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
    Tongue.GrowSeconds = .32f + .66f * Uniform(Salt + 1);
    Tongue.MaxLength = (55.f + 100.f *
        FMath::Pow(Uniform(Salt + 2),1.22f)) * Tongue.Scale * SizeScale;
    Tongue.BaseWidth = (12.f + 23.f *
        FMath::Pow(Uniform(Salt + 3),1.15f)) * Tongue.Scale * SizeScale;
    // Exact ring cut: the upper 5/8 or 6/8 becomes the detached flame.
    Tongue.RemainingFraction = Uniform(Salt + 4)<.60f?.25f:.375f;
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
    int32 DesiredCount = 3; // staggered main and two subordinate living tongues
    const bool bPoint=ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty());
    if (!bPoint && Cast<USkeletalMeshComponent>(Source))
    {
        // A whole character needs head/torso/arms/legs, not 3 foot roots.
        DesiredCount = 10 + FMath::RoundToInt(Uniform(731) * 4.f);
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
    DesiredCount = FMath::Clamp(DesiredCount, 3, MaxTongues);

    if (const USkeletalMeshComponent* Skeletal = bPoint?nullptr:Cast<USkeletalMeshComponent>(Source))
    {
        USkeletalMesh* Asset = Skeletal->GetSkeletalMeshAsset();
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
            const float Angle=I*2.399963f;
            const FVector Offset=IgnitionPoints.IsEmpty()?FVector::ZeroVector:IgnitionPoints[0];
            // The explicit wick position is component-local; keep root spacing in world cm.
            Root.RestPosition=Offset+Source->GetComponentTransform().InverseTransformVector(
                FVector(FMath::Cos(Angle)*5.f,FMath::Sin(Angle)*5.f,0.f)*FMath::Min(1.f,SizeScale));
            Root.Seed = Uniform(I+501)*103.f;
            Root.Scale = I==0?1.f:(.56f+.23f*Uniform(I+777));
            Tongues.Add(Root);
        }
    }
    RebuildClusters();
    for (int32 I=0; I<Tongues.Num(); ++I)
    {
        FTongue& Root=Tongues[I];
        ConfigureGeneration(Root);
        Root.Age = -(.07f + Uniform(I+611)*.42f);
        Root.CurrentLength = Root.MaxLength * .16f;
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
        if (USkeletalMeshComponent* Skeletal=Cast<USkeletalMeshComponent>(Source))
        {
            USkeletalMesh* Asset=Skeletal->GetSkeletalMeshAsset();
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
    const float Shape=(Curl+Curl2+Dance+TipHook)*S+Lean*V;
    const float Side=bPoint?FMath::Clamp(Shape*.62f,-L*.23f*V,L*.23f*V):
        FMath::Clamp(Shape,-L*.44f*V,L*.44f*V);
    const FVector Drift=(Flow*V*V*.095f*S).GetClampedToMaxSize(L*.60f*V*V);
    return Base+FVector::UpVector*L*V+Drift+Right*Side;
}

float UASTFireFreeFlameComponent::TongueRadiusAt(const FTongue& T,float V) const
{
    // Rounded root, fuller lower/central gas, then an asymmetric taper.
    // Radius is one field; the pinch acts on exactly the ring that is cut.
    const float Bulge=.66f+.34f*FMath::Sin(PI*FMath::Clamp(V,0.f,1.f));
    const float Profile=Bulge*FMath::Pow(FMath::Max(0.f,1.f-V),.64f);
    const float Pulse=.91f+.09f*FMath::Sin(V*12.f-T.Age*8.f+T.Phase);
    float Radius=FMath::Min(T.CurrentWidth,T.CurrentLength*.43f)*Profile*Pulse;
    if(T.Life==EFlameLife::Bridging)
    {
        const float Pinch=ASTFireLife::SmoothStep(T.PhaseAge/FMath::Max(.01f,T.BridgeSeconds));
        const float Near=FMath::Exp(-FMath::Square((V-T.RemainingFraction)/.085f));
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
    Child.Position=TongueCenterAt(Tongue,Base,float(Cut)/(RingsPerTongue-1),Right);
    Child.ArcCount=RingsPerTongue-Cut;
    Child.Width=0.f;
    for(int32 K=0;K<Child.ArcCount;++K)
    {
        const float V=float(Cut+K)/(RingsPerTongue-1);
        Child.SourceV[K]=V;
        Child.Arc[K]=TongueCenterAt(Tongue,Base,V,Right)-Child.Position;
        Child.Radius[K]=TongueRadiusAt(Tongue,V);
        Child.Width=FMath::Max(Child.Width,Child.Radius[K]);
    }
    Child.Length=Length*(1.f-Tongue.RemainingFraction);
    Child.ParentGeneration=Tongue.Generation;
    Child.BirthClock=Elapsed;
    Child.BirthPhase=Tongue.Seed+Tongue.Phase;
    Child.BirthHeat=Heat;
    Child.Age=0.f;
    Child.Lifetime=.30f+.34f*Uniform(Salt+3);
    Child.Temperature=1.f;
    Child.Seed=Uniform(Salt+9)*102.f;
    Child.Right=Right;
    const float Travel=FMath::Clamp(Length/90.f,.08f,1.8f);
    Child.Velocity=FVector::UpVector*(24.f+30.f*Uniform(Salt+7))*Travel+
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
        const float Travel=FMath::Clamp(D.Length/65.f,.08f,1.8f);
        D.Velocity+=FVector::UpVector*(125.f+170.f*(1.f-Fade))*Travel*Dt+
            FVector(Wind.X,Wind.Y,0.f)*(.07f*Dt);
        D.Position+=D.Velocity*Dt;
        D.Temperature=FMath::Pow(Fade,1.25f);
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
            const float Growth=ASTFireLife::SmoothStep(T.Age/FMath::Max(.1f,T.GrowSeconds));
            T.CurrentLength=FMath::Lerp(T.RestartLength,T.MaxLength,Growth);
            T.CurrentWidth=FMath::Lerp(T.PreviousWidth,T.BaseWidth,Growth);
            if(T.Age>=T.GrowSeconds)
            {
                T.SplitLength=T.CurrentLength=T.MaxLength;
                T.BridgeSeconds=.085f+.085f*Uniform(T.Generation*83+int32(T.Seed)*9);
                T.PhaseAge=0.f;
                T.Life = EFlameLife::Bridging;
            }
        }
        else
        {
            // Pinch only. The upper arc does not retract toward the root.
            T.PhaseAge=FMath::Min(T.BridgeSeconds,T.PhaseAge+Dt);
            if(T.PhaseAge>=T.BridgeSeconds)
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
                    T.RestartLength=FMath::Min(Remaining,T.MaxLength*.34f);
                    T.CurrentLength=T.RestartLength;
                    T.PreviousWidth=T.CurrentWidth=FMath::Min(T.CurrentWidth,T.BaseWidth*.64f);
                    T.BridgedChild=INDEX_NONE;
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
    float Peak=-BIG_NUMBER,WidthSum=0.f,LengthSum=0.f;
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
        ++Active;
    }
    if (Active>0)
    {
        const FVector Size=FlameBounds.GetSize();
        const float Width=WidthSum/Active;
        const float MeanLength=LengthSum/Active;
        const FVector RootSize=RootBounds.GetSize();
        const float Footprint=FMath::Max(RootSize.X,RootSize.Y);
        const float Measured=FMath::Clamp(
            (Width*1.4f+Footprint*.30f+MeanLength*.25f)*
            FMath::Sqrt(FMath::Clamp(Heat,.02f,1.f)),8.f,280.f);
        SmokeFireSize=SmokeFireSize>0.f?
            FMath::FInterpTo(SmokeFireSize,Measured,Dt,1.8f):Measured;
        SmokeFireTop=Highest;
    }
    const float Scale=FMath::Clamp((SmokeFireSize-16.f)/220.f,0.f,1.f);

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

    if (bVisible && Heat>.10f && Active>0)
    {
        // Small flames produce modest smoke. 10m is a cap, not a default.
        const float Rate=FMath::Lerp(4.2f,5.3f,Scale)*
            FMath::Lerp(.72f,1.f,Heat);
        SmokeSpawnDebt=FMath::Min(2.f,SmokeSpawnDebt+Dt*Rate);
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
            S.InitialRadius=FMath::Lerp(10.f,46.f,Scale)*(.90f+.20f*R);
            S.Radius=S.InitialRadius;
            S.MaxRise=FMath::Lerp(125.f,1000.f,FMath::Pow(Scale,1.25f))*
                (.83f+.17f*J);
            S.RiseSpeed=FMath::Lerp(35.f,85.f,Scale)*(.94f+.12f*R);
            S.Lifetime=FMath::Min(14.f,(S.MaxRise-S.Radius)/
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
    int32 HotPixels=0,AllPixels=0,FragmentPixels=0,PitchChanges=0;
    int32 ChangedColors=0,CommonColors=0;
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
            if(I>25 && LastPitch>0.f && Pitch!=LastPitch)++PitchChanges;
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
            if(Main.IsEmpty())continue;
            bSeen=true;
            MaxCells=FMath::Max(MaxCells,Main.Num());
            bFragment|=!DetachedPixels.IsEmpty();
            if(bOnMesh)bMesh|=Fire->Tongues.Num()>=4 && Main.Num()>20;
            if(bHiddenWick)bWick|=Fire->Tongues.Num()==3 && Main.Num()>10 &&
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
                GoodShape+=Aspect>=.33f && Aspect<=.95f && Fill>=.40f && Fill<=.86f;
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
        for(int32 J=0;J<20;++J)Fire->AdvanceSimulation(.04f);
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
    if(auto* Single=NewFire(1.f,1729,false))
    {
        Single->Tongues.SetNum(1);
        Single->Tongues[0].bActive=true;Single->Tongues[0].Age=0.f;
        for(int32 I=0;I<180;++I)
        {
            const FTongue Old=Single->Tongues[0];
            const TArray<FDetached> Before=Single->Detached;
            const FVector OldRoot=Single->ConnectedRoots.IsValidIndex(0)?Single->ConnectedRoots[0]:Single->SampleRoot(Old);
            Step(Single,FVector::ZeroVector);
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
                FTongue AtCut=Old;AtCut.PhaseAge=AtCut.BridgeSeconds;
                for(const FDetached& D:Single->Detached)
                    if(D.bAlive && D.Age==0.f && D.ParentGeneration==Old.Generation)
                    {
                        bFound=true;
                        for(int32 K=0;K<D.ArcCount;++K)
                        {
                            const FVector Expected=Single->TongueCenterAt(AtCut,OldRoot,D.SourceV[K],D.Right);
                            MaxTransferError=FMath::Max(MaxTransferError,float(FVector::Dist(Expected,D.Position+D.Arc[K])));
                            bCycle&=FMath::Abs(D.Radius[K]-Single->TongueRadiusAt(AtCut,D.SourceV[K]))<.01f;
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
        AActor* Actor=Single->GetOwner();Single->DestroyComponent();Actor->Destroy();
    }
    else bCycle=false;
    bCycle&=Splits>=3 && GrowingFrames>=30 && CoolingFrames>=12 && MaxTransferError<.05f && MaxResetRatio<=.3751f;
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
    bPass &= bCycle && bFragment && bCool && bSmall && bMesh && bWick && bMoving && bRaster &&
        MaxTones>=6 && ShapeRate>=.55f && Connectivity>=.65f &&
        HotRate<=.30f && ChangeRate>.02f && FragmentRate<.85f &&
        Crowd.Num()==20;
    return FString::Printf(TEXT("%s:small=%d;fragment=%d;cool=%d;mesh=%d;wick=%d;moving=%d;raster=%d;cells=%d;tones=%d;shape=%.3f;connected=%.3f;aspect=%.3f;fill=%.3f;hot=%.3f;thermalChange=%.3f;detachedRatio=%.3f;pitchChanges=%d;cpuMs=%.3f;p95=%.3f;crowd20Ms=%.3f;crowdP95=%.3f;cycle=%d;splits=%d;growing=%d;cooling=%d;transferError=%.5f;resetRatio=%.3f"),
        bPass?TEXT("PASS"):TEXT("FAIL"),int32(bSmall),int32(bFragment),int32(bCool),int32(bMesh),int32(bWick),int32(bMoving),int32(bRaster),
        MaxCells,MaxTones,ShapeRate,Connectivity,AspectSum/FMath::Max(1,StableFrames),
        FillSum/FMath::Max(1,StableFrames),HotRate,ChangeRate,FragmentRate,PitchChanges,
        Cost/FMath::Max(1,FrameCount),Costs.IsEmpty()?0.:Costs[FMath::Clamp(FMath::FloorToInt(Costs.Num()*.95f),0,Costs.Num()-1)],
        CrowdMs/FMath::Max(1,CrowdCosts.Num()),CrowdCosts.IsEmpty()?0.:CrowdCosts[FMath::Clamp(FMath::FloorToInt(CrowdCosts.Num()*.95f),0,CrowdCosts.Num()-1)],int32(bCycle),Splits,GrowingFrames,CoolingFrames,MaxTransferError,MaxResetRatio);
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
        Fire->UpdateGeometry(false);
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
    Fire->DestroyComponent();Capture->DestroyComponent();World->DestroyWorld(false);
    return FString::Printf(TEXT("%s:frames=%d"),Saved>=6?TEXT("PASS"):TEXT("FAIL"),Saved);
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
        D.Velocity+=(Side*FMath::Sin(Phase)*68.f+
            Depth*FMath::Cos(Phase*.77f+D.Seed*.31f)*42.f)*(Scale*Dt);
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
    const float PixelPitch=UpdateConnectedSource(bCreate, View, Right);
    const ASTFireLife::FFireProjection Projection(
        IsValid(Source)?Source->GetComponentLocation():GetOwner()->GetActorLocation(),GetWorld());
    const FTransform RenderTransform=Render->GetComponentTransform();
    TArray<int32> Triangles;
    const float Pitch=FMath::Max(.1875f,PixelPitch);
    for(const FDetached& D:Detached)
    {
        if(!D.bAlive || D.ArcCount<2)continue;
        const float Fade=ASTFireLife::ArcFade(D.Age,D.Lifetime);
        const float Shrink=FMath::Pow(Fade,.92f);
        const float Angle=FMath::Sin(D.Age*6.1f+D.Seed)*D.Age*.80f;
        FVector2D Arc[RingsPerTongue];
        float Radius[RingsPerTongue],Depths[RingsPerTongue];
        FVector2D Lo(BIG_NUMBER,BIG_NUMBER),Hi(-BIG_NUMBER,-BIG_NUMBER);
        for(int32 K=0;K<D.ArcCount;++K)
        {
            // At age zero the original world-space arc and radii are unchanged.
            const FVector World=D.Position+D.Arc[K].RotateAngleAxis(FMath::RadiansToDegrees(Angle),-Projection.Forward)*Shrink;
            Arc[K]=Projection.Project(World);
            Depths[K]=Projection.Depth(World);
            Radius[K]=D.Radius[K]*Shrink*ASTFireLife::FFireProjection::Reference/Depths[K];
            Lo.X=FMath::Min(Lo.X,Arc[K].X-Radius[K]);Lo.Y=FMath::Min(Lo.Y,Arc[K].Y-Radius[K]);
            Hi.X=FMath::Max(Hi.X,Arc[K].X+Radius[K]);Hi.Y=FMath::Max(Hi.Y,Arc[K].Y+Radius[K]);
        }
        const int32 X0=FMath::FloorToInt(Lo.X/Pitch),Y0=FMath::FloorToInt(Lo.Y/Pitch);
        const int32 X1=FMath::CeilToInt(Hi.X/Pitch),Y1=FMath::CeilToInt(Hi.Y/Pitch);
        for(int32 Y=Y0;Y<Y1;++Y)for(int32 X=X0;X<X1;++X)
        {
            const FVector2D Sample((X+.5f)*Pitch,(Y+.5f)*Pitch);
            float Thermal=0.f,Depth=0.f,Best=-BIG_NUMBER;
            for(int32 K=1;K<D.ArcCount;++K)
            {
                const FVector2D Axis=Arc[K]-Arc[K-1];
                const float Along=FMath::Clamp(float(FVector2D::DotProduct(Sample-Arc[K-1],Axis))/
                    FMath::Max(.001f,float(Axis.SizeSquared())),0.f,1.f);
                const FVector2D Center=Arc[K-1]+Axis*Along;
                const float R=FMath::Lerp(Radius[K-1],Radius[K],Along);
                const float Distance=float((Sample-Center).Size());
                const float Support=R-Distance;
                if(Support<0.f || R<.001f)continue;
                const float V=FMath::Lerp(D.SourceV[K-1],D.SourceV[K],Along);
                const float Core=FMath::Clamp(1.f-Distance/R,0.f,1.f);
                Thermal=FMath::Max(Thermal,ASTFireLife::ArcHeat(V,Core,D.BirthPhase,D.BirthClock));
                if(Support>Best){Best=Support;Depth=FMath::Lerp(Depths[K-1],Depths[K],Along);}
            }
            if(Best<0.f)continue;
            const int32 First=Verts.Num();
            for(int32 K=0;K<4;++K)
            {
                const FVector2D P((X+K%2)*Pitch,(Y+K/2)*Pitch);
                Verts.Add(RenderTransform.InverseTransformPosition(Projection.Unproject(P,Depth)));
                UVs.Add(FVector2D(K%2,K/2));
                Colors.Add(FLinearColor(ASTFireLife::ToneCode(Thermal*D.BirthHeat*D.Temperature),0.f,0.f,1.f));
            }
            Triangles.Add(First);Triangles.Add(First+2);Triangles.Add(First+1);
            Triangles.Add(First+1);Triangles.Add(First+2);Triangles.Add(First+3);
        }
    }
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    Normals.Init(-Projection.Forward,Verts.Num());
    Tangents.Init(FProcMeshTangent(1,0,0),Verts.Num());
    const FProcMeshSection* OldFragments=Render->GetProcMeshSection(0);
    if(Verts.IsEmpty()){if(OldFragments && !OldFragments->ProcVertexBuffer.IsEmpty())Render->ClearMeshSection(0);}
    else if(OldFragments && OldFragments->ProcVertexBuffer.Num()==Verts.Num())
        Render->UpdateMeshSection_LinearColor(0,Verts,Normals,UVs,Colors,Tangents);
    else Render->CreateMeshSection_LinearColor(0,Verts,Triangles,Normals,UVs,Colors,Tangents,false,false);
    Render->SetMaterial(0,Material);

    // Stage30: distinct high-contrast hot pixels, fixed-size quad section 2.
    // Even dead particles contribute degenerate, fully masked geometry;
    // section topology never changes at runtime.
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
            1.f-S.Age/FMath::Max(.05f,S.Lifetime),0.f,1.f),.72f):0.f;
        const FVector ToCamera=(CameraLocation-S.Position).GetSafeNormal();
        const FVector N=ToCamera.IsNearlyZero()?View:ToCamera;
        FVector Side=(CameraRight-N*
            FVector::DotProduct(CameraRight,N)).GetSafeNormal();
        if (Side.IsNearlyZero())
            Side=FVector::CrossProduct(FVector::UpVector,N).GetSafeNormal();
        if (Side.IsNearlyZero())
            Side=FVector::CrossProduct(FVector::ForwardVector,N).GetSafeNormal();
        const FVector Up=FVector::CrossProduct(N,Side).GetSafeNormal();
        const float W=S.Size*(.45f+.55f*Fade);
        const float L=S.Size*(1.5f+.65f*Fade);
        const FVector Direction=(Up+S.Velocity.GetSafeNormal()*.25f)
            .GetSafeNormal();
        const FVector A=S.Position-Side*W;
        const FVector B=S.Position+Side*W;
        const FVector C=S.Position+Direction*L-Side*W*.38f;
        const FVector D=S.Position+Direction*L+Side*W*.38f;
        const FVector World[4]={A,B,C,D};
        for (int32 J=0;J<4;++J)
        {
            SV.Add(RenderTransform.InverseTransformPosition(World[J]));
            SU.Add(FVector2D((J%2)?1.f:0.f,(J/2)?1.f:0.f));
            // Fixed three-shade palette, no transparency: shrink to zero
            // geometrically; keep the heat from the emission frame.
            const float Temperature=S.EmissionHeat*Fade;
            const FLinearColor SparkColor=Temperature>.63f
                ? FLinearColor(1.f,.84f,.28f,1.f)
                : Temperature>.29f
                    ? FLinearColor(1.f,.40f,.055f,1.f)
                    : FLinearColor(.80f,.115f,.019f,1.f);
            SC.Add(FLinearColor(SparkColor.R,SparkColor.G,
                SparkColor.B,S.bAlive && Fade>.03f ? 1.f : 0.f));
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
            2,SV,SN,SU,SC,SX);

    // Section 1 was authored before this section, it contains the common
    // flame field AND the narrowing split bridges.
}


void UASTFireFreeFlameComponent::BuildSourceAnchors()
{
    SourceAnchors.Reset();
    if (!IsValid(Source)) return;
    auto Add = [this](const FVector& Rest, int32 VertexIndex)
    {
        if (SourceAnchors.Num() >= MaxSourceAnchors) return;
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

    if (USkeletalMeshComponent* Skel=Cast<USkeletalMeshComponent>(Source))
    {
        USkeletalMesh* Asset=Skel->GetSkeletalMeshAsset();
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
        // Collision samples are ACTUAL geometry/primitive hits in local
        // space. Do not rely on CPU access to StaticMesh render buffers,
        // which may be stripped in cooked builds.
        const FBoxSphereBounds Box=Static->CalcBounds(FTransform::Identity);
        const FVector H=Box.BoxExtent.GetAbs();
        const FTransform Transform=Static->GetComponentTransform();
        const float Far=H.GetMax()*2.8f+20.f;
        FCollisionQueryParams Query;
        Query.bTraceComplex=true;
        for (int32 Face=0;Face<6 && SourceAnchors.Num()<MaxSourceAnchors;++Face)
            for(int32 A=0;A<4;++A)
                for(int32 B=0;B<4;++B)
                {
                    const float U=(float(A)/3.f)*1.76f-.88f;
                    const float V=(float(B)/3.f)*1.76f-.88f;
                    FVector Point=Box.Origin;
                    FVector Normal=FVector::UpVector;
                    switch(Face)
                    {
                    case 0: Normal=FVector(1,0,0);
                        Point+=FVector(H.X,U*H.Y,V*H.Z);break;
                    case 1: Normal=FVector(-1,0,0);
                        Point+=FVector(-H.X,U*H.Y,V*H.Z);break;
                    case 2: Normal=FVector(0,1,0);
                        Point+=FVector(U*H.X,H.Y,V*H.Z);break;
                    case 3: Normal=FVector(0,-1,0);
                        Point+=FVector(U*H.X,-H.Y,V*H.Z);break;
                    case 4: Point+=FVector(U*H.X,V*H.Y,H.Z);break;
                    default: Normal=FVector(0,0,-1);
                        Point+=FVector(U*H.X,V*H.Y,-H.Z);break;
                    }
                    FHitResult Hit;
                    if(Static->LineTraceComponent(Hit,
                        Transform.TransformPosition(Point+Normal*14.f),
                        Transform.TransformPosition(Point-Normal*Far),Query))
                    {
                        Add(Transform.InverseTransformPosition(
                            FVector(Hit.ImpactPoint)),INDEX_NONE);
                    }
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
float UASTFireFreeFlameComponent::UpdateConnectedSource(
    bool bCreate,const FVector& View,const FVector& Right)
{
    if (!IsValid(Render)) return 3.f;
    const FVector Up=FVector::UpVector;
    const FVector Origin=IsValid(Source)?Source->GetComponentLocation():GetOwner()->GetActorLocation();
    const ASTFireLife::FFireProjection Projection(Origin,GetWorld());
    const FVector Normal=-Projection.Forward;
    const FVector2D OriginP=Projection.Project(Origin);
    const float OriginDepth=Projection.Depth(Origin);
    const float Bias=FMath::Clamp(4.f*SizeScale,.4f,4.f);
    const bool bPoint=ASTFireLife::IsPointSource(Source,!IgnitionPoints.IsEmpty());
    const FTransform RenderTransform=Render->GetComponentTransform();

    USkeletalMeshComponent* Skel=Cast<USkeletalMeshComponent>(Source);
    const FSkeletalMeshLODRenderData* Lod=nullptr;
    FSkinWeightVertexBuffer* Weights=nullptr;
    if (Skel)
    {
        if (USkeletalMesh* Asset=Skel->GetSkeletalMeshAsset())
            if (FSkeletalMeshRenderData* Data=Asset->GetResourceForRendering())
                if(Data->LODRenderData.Num()>0)
                {
                    Lod=&Data->LODRenderData[0];
                    Weights=const_cast<FSkeletalMeshLODRenderData*>(Lod)
                        ->GetSkinWeightVertexBuffer();
                }
    }

    TArray<FVector2D> Samples;
    TArray<float> SampleDepths;
    Samples.Reserve(MaxSourceAnchors+Tongues.Num()*3+1);
    SampleDepths.Reserve(MaxSourceAnchors+Tongues.Num()*3+1);
    float HighestDepth=-BIG_NUMBER;
    float MinX=BIG_NUMBER,MaxX=-BIG_NUMBER,MinY=BIG_NUMBER,MaxY=-BIG_NUMBER;

    auto Include=[&](const FVector& World,float DepthCompensation=0.f)
    {
        const FVector2D P=Projection.Project(World);
        Samples.Add(P);
        SampleDepths.Add(-Projection.Depth(World)-DepthCompensation);
        HighestDepth=FMath::Max(HighestDepth,-Projection.Depth(World));
    };

    for (const FSourceAnchor& A:SourceAnchors)
    {
        if(bPoint)break; // no mesh-depth sampling for an explicit point wick
        // A local flame patch grows over the ACTUAL surface. The old
        // all-mesh cluster radius illuminated the whole character at once.
        // Gate sampled vertices by nearby live tongue anchors/contact hits.
        if (Cast<UMeshComponent>(Source))
        {
            const UPrimitiveComponent* Primitive =
                Cast<UPrimitiveComponent>(Source);
            const float MaxDimension = Primitive
                ? Primitive->Bounds.BoxExtent.GetMax()*2.f : 180.f;
            const float BurnRadius = FMath::Clamp(
                MaxDimension*.23f,24.f,125.f) *
                (.52f + .90f*Spread);
            bool bNearFlame=false;
            for (const FTongue& Root:Tongues)
            {
                if (!Root.bActive) continue;
                if (FVector::DistSquared(A.RestPosition,Root.RestPosition)
                    <FMath::Square(BurnRadius))
                {bNearFlame=true;break;}
            }
            if (!bNearFlame)
                for (const FVector& Contact:IgnitionPoints)
                    if (FVector::DistSquared(A.RestPosition,Contact)
                        <FMath::Square(BurnRadius))
                    {bNearFlame=true;break;}
            if (!bNearFlame) continue;
        }

        FVector World=Source->GetComponentTransform().TransformPosition(
            A.RestPosition);
        if(Skel && Lod && Weights && A.SkinnedVertexIndex>=0 &&
            A.SkinnedVertexIndex<int32(Lod->GetNumVertices()))
        {
            const FVector3f P=USkinnedMeshComponent::GetSkinnedVertexPosition(
                Skel,A.SkinnedVertexIndex,*Lod,*Weights);
            World=Skel->GetComponentTransform().TransformPosition(FVector(P));
        }
        Include(World);
    }
    // All living tongue roots are guaranteed part of the SAME connected
    // source even when no mesh is present (free-standing flame).
    for(const FTongue& T:Tongues)
        if(T.bActive && T.Age>=0.f)
        {
            const FVector Root=SampleRoot(T);
            Include(Root);
            // Merge the first part of each approved tongue into a common
            // burning body. Tongue/bridge geometry and lifetimes stay intact.
            const float Rise=FMath::Clamp(T.CurrentLength*.23f,4.f,24.f);
            Include(Root+FVector::UpVector*Rise);
        }

    if(bPoint)
        Include(IgnitionPoints.IsEmpty()?Origin:Source->GetComponentTransform().TransformPosition(IgnitionPoints[0])); // no donut in free-standing fire
    if(Samples.IsEmpty())
        Include(Origin);

    // All tongues emerge from the SAME mantle surface. Previously, the
    // source was rendered at the camera-facing depth but the tongues
    // remained at their raw skinned-bone/mesh depth (up to an entire
    // torso away), producing visibly separate flame sprites.
    //
    // Use precisely the field's three-nearest-front-samples depth policy
    // for a tongue's projected position. Keep ~2 cm camera-facing bias to
    // avoid Z-fighting while its root stays submerged in the hot field.
    ConnectedRoots.Reset();
    ConnectedRoots.Reserve(Tongues.Num());
    for (const FTongue& Tongue : Tongues)
    {
        const FVector Root=SampleRoot(Tongue);
        const FVector2D P=Projection.Project(Root);
        float BestD2[3]={BIG_NUMBER,BIG_NUMBER,BIG_NUMBER};
        float BestDepth[3]={0.f,0.f,0.f};
        for(int32 I=0;I<Samples.Num();++I)
        {
            const float D2=float(FVector2D::DistSquared(P,Samples[I]));
            if(D2>=BestD2[2])continue;
            int32 Slot=2;
            while(Slot>0 && D2<BestD2[Slot-1])
            {
                BestD2[Slot]=BestD2[Slot-1];
                BestDepth[Slot]=BestDepth[Slot-1];
                --Slot;
            }
            BestD2[Slot]=D2;
            BestDepth[Slot]=SampleDepths[I];
        }
        float Front=-BIG_NUMBER;
        for(int32 I=0;I<3;++I)
            if(BestD2[I]<BIG_NUMBER*.5f)
                Front=FMath::Max(Front,BestDepth[I]);
        if(Front<=-BIG_NUMBER*.5f)
            Front=HighestDepth;
        ConnectedRoots.Add(Projection.Unproject(P,FMath::Max(10.f,bPoint?Projection.Depth(Root)-Bias:-Front-Bias)));
    }

    // Stage14: one burning body owns the lower section of EVERY living
    // tongue. These line segments are not attached sprites or MST anchors:
    // they expand the same masked, thermally shaded mantle grid (section 1).
    // The already-approved separate ribbon starts only as this field ends.
    struct FNeckSegment
    {
        FVector2D Start;
        FVector2D End;
        float StartRadius = 0.f;
        float EndRadius = 0.f;
        float StartDepth = 0.f;
        float EndDepth = 0.f;
        float StartV = 0.f;
        float EndV = 0.f;
        float Phase = 0.f;
    };
    TArray<FNeckSegment> Necks;
    // A few short forks grow from the UPPER portion of the same flame body.
    // These are not new emitters with their own roots or life state.
    Necks.Reserve(Tongues.Num()*(RingsPerTongue+4));
    // One continuous living field from the root to the moving pointed tip.
    static constexpr float NeckFractions[9] =
        {0.f,.125f,.25f,.375f,.5f,.625f,.75f,.875f,1.f};
    auto Project=[&](const FVector& World)
    {
        return Projection.Project(World);
    };
    auto AppendNeck=[&](const FVector& WorldA,const FVector& WorldB,
        float RadiusA,float RadiusB,float V0,float V1,float Phase)
    {
        FNeckSegment Neck;
        Neck.Start=Project(WorldA);
        Neck.End=Project(WorldB);
        Neck.StartDepth=-Projection.Depth(WorldA);
        Neck.EndDepth=-Projection.Depth(WorldB);
        Neck.StartRadius=FMath::Max(0.f,RadiusA)*ASTFireLife::FFireProjection::Reference/-Neck.StartDepth;
        Neck.EndRadius=FMath::Max(0.f,RadiusB)*ASTFireLife::FFireProjection::Reference/-Neck.EndDepth;
        Neck.StartV=V0;
        Neck.EndV=V1;
        Neck.Phase=Phase;
        Necks.Add(Neck);
        const float Extent=FMath::Max(Neck.StartRadius,Neck.EndRadius)+2.f;
        MinX=FMath::Min(MinX,FMath::Min(float(Neck.Start.X),
            float(Neck.End.X))-Extent);
        MaxX=FMath::Max(MaxX,FMath::Max(float(Neck.Start.X),
            float(Neck.End.X))+Extent);
        MinY=FMath::Min(MinY,FMath::Min(float(Neck.Start.Y),
            float(Neck.End.Y))-Extent);
        MaxY=FMath::Max(MaxY,FMath::Max(float(Neck.Start.Y),
            float(Neck.End.Y))+Extent);
    };
    float MaxRise=0.f;
    int32 Living=0;
    for(int32 I=0;I<Tongues.Num();++I)
    {
        const FTongue& T=Tongues[I];
        if(!T.bActive || T.Age<0.f || Heat<.015f)continue;
        ++Living;
        const FVector Root=ConnectedRoots[I];
        const float Phase=T.Seed+T.Phase;
        const float Scale=ASTFireLife::FFireProjection::Reference/Projection.Depth(Root);
        MaxRise=FMath::Max(MaxRise,T.MaxLength*Scale);
        // Only genuine growing arcs are rasterized. No independent basal blob
        // or decorative upper fork can conceal the actual split and rebirth.
        FVector WorldA=TongueCenterAt(T,Root,0.f,Right);
        for(int32 K=1;K<RingsPerTongue;++K)
        {
            const float V0=NeckFractions[K-1],V1=NeckFractions[K];
            const FVector WorldB=TongueCenterAt(T,Root,V1,Right);
            AppendNeck(WorldA,WorldB,TongueRadiusAt(T,V0),TongueRadiusAt(T,V1),V0,V1,Phase);
            WorldA=WorldB;
        }
    }

    // Fuse nearby source roots with thin animated bridges. Far-apart
    // ignition patches stay separate rather than forming a body-sized blob.
    for(int32 I=1;!bPoint && I<ConnectedRoots.Num();++I)
    {
        const FTongue& T=Tongues[I];
        if(!T.bActive || T.Age<0.f) continue;
        int32 Best=INDEX_NONE;
        float BestDist=FMath::Square(75.f*FMath::Min(1.f,SizeScale)*ASTFireLife::FFireProjection::Reference/OriginDepth);
        for(int32 J=0;J<I;++J)
        {
            if(!Tongues[J].bActive || Tongues[J].Age<0.f)continue;
            const float D=float(FVector2D::DistSquared(
                Project(ConnectedRoots[I]),Project(ConnectedRoots[J])));
            if(D<BestDist){BestDist=D;Best=J;}
        }
        if(Best==INDEX_NONE) continue;
        AppendNeck(ConnectedRoots[I],ConnectedRoots[Best],
            FMath::Min(T.CurrentWidth*.48f,10.f*SizeScale),
            FMath::Min(Tongues[Best].CurrentWidth*.48f,10.f*SizeScale),0.f,.12f,T.Seed+T.Phase);
    }
    if(Living==0)
    {
        Render->ClearMeshSection(1);
        return FMath::Max(.1875f,FirePixelPitch);
    }
    // Pitch is set by slowly filtered source size and quantized with
    // hysteresis. Match smoke where feasible without erasing small flames.
    constexpr int32 MaxXCells=72,MaxYCells=104;
    const float Base=.1875f;
    // Pitch tracks potential source size, not the instantaneous growing length.
    const float H=FMath::Max(.5f,MaxRise);
    FireVisualHeight=FireVisualHeight>0.f?FMath::Lerp(FireVisualHeight,H,.08f):H;
    float Desired=FMath::Max(Base,FireVisualHeight/(bPoint?40.f:38.f));
    if(SmokePixelPitch>0.f)
        Desired=FMath::Max(Desired,FMath::Min(SmokePixelPitch,Desired*1.55f));
    const float Required=FMath::Max(Base,FMath::Max(
        (MaxX-MinX)/(MaxXCells-2),(MaxY-MinY)/(MaxYCells-2)));
    if(FirePixelPitch<=0.f || Desired>FirePixelPitch*1.65f || Desired<FirePixelPitch*.55f)
        FirePixelPitch=Base*FMath::Pow(2.f,FMath::RoundToFloat(FMath::Log2(Desired/Base)));
    if(Required>FirePixelPitch)
        FirePixelPitch=Base*FMath::Pow(2.f,FMath::CeilToFloat(FMath::Log2(Required/Base)));
    const float Pitch=FirePixelPitch;
    const float WickDepth=bPoint && !IgnitionPoints.IsEmpty()?Projection.Depth(Source->GetComponentTransform().TransformPosition(IgnitionPoints[0])):OriginDepth;
    MinX=FMath::FloorToFloat(MinX/Pitch)*Pitch;
    MinY=FMath::FloorToFloat(MinY/Pitch)*Pitch;
    const int32 NX=FMath::Clamp(FMath::CeilToInt((MaxX-MinX)/Pitch),1,MaxXCells);
    const int32 NY=FMath::Clamp(FMath::CeilToInt((MaxY-MinY)/Pitch),1,MaxYCells);
    struct FFireCell { float Depth=0.f,Temperature=0.f,Cover=-BIG_NUMBER,NeckDepth=-BIG_NUMBER; bool bVisible=false; };
    TArray<FFireCell> Cells;
    Cells.SetNum(NX*NY);
    // Segment-first rasterization: evaluate only cells inside each segment's
    // bounding rectangle, instead of scanning all segments for every pixel.
    for(const FNeckSegment& Neck:Necks)
    {
        const float Reach=FMath::Max(Neck.StartRadius,Neck.EndRadius)+Pitch;
        const int32 X0=FMath::Clamp(FMath::FloorToInt((FMath::Min(Neck.Start.X,Neck.End.X)-Reach-MinX)/Pitch),0,NX-1);
        const int32 X1=FMath::Clamp(FMath::CeilToInt((FMath::Max(Neck.Start.X,Neck.End.X)+Reach-MinX)/Pitch),0,NX-1);
        const int32 Y0=FMath::Clamp(FMath::FloorToInt((FMath::Min(Neck.Start.Y,Neck.End.Y)-Reach-MinY)/Pitch),0,NY-1);
        const int32 Y1=FMath::Clamp(FMath::CeilToInt((FMath::Max(Neck.Start.Y,Neck.End.Y)+Reach-MinY)/Pitch),0,NY-1);
        const FVector2D Axis=Neck.End-Neck.Start;
        const float InvDenominator=FMath::Max(.001f,float(Axis.SizeSquared()));
        for(int32 Y=Y0;Y<=Y1;++Y) for(int32 X=X0;X<=X1;++X)
        {
            FFireCell& Cell=Cells[Y*NX+X];
            const FVector2D P(MinX+(X+.5f)*Pitch,MinY+(Y+.5f)*Pitch);
            float& Cover=Cell.Cover;
            float& Temperature=Cell.Temperature;
            float& NeckDepth=Cell.NeckDepth;
            const float Along=FMath::Clamp(float(FVector2D::DotProduct(P-Neck.Start,Axis))/
                InvDenominator,0.f,1.f);
            const FVector2D Center=Neck.Start+Axis*Along;
            const float R=FMath::Lerp(Neck.StartRadius,Neck.EndRadius,Along);
            const float D=float((P-Center).Size());
            const float Support=R-D;
            if(Support<0.f || R<.001f) continue;
            const float V=FMath::Lerp(Neck.StartV,Neck.EndV,Along);
            const float Core=FMath::Clamp(1.f-D/R,0.f,1.f);
            const float LocalHeat=ASTFireLife::ArcHeat(V,Core,Neck.Phase,Elapsed);
            Temperature=FMath::Max(Temperature,LocalHeat);
            if(Support>Cover)
            {
                Cover=Support;
                NeckDepth=FMath::Lerp(Neck.StartDepth,Neck.EndDepth,Along);
            }
        }
    }
    for(int32 Y=0;Y<NY;++Y) for(int32 X=0;X<NX;++X)
    {
        FFireCell& Cell=Cells[Y*NX+X];
        const float Cover=Cell.Cover,NeckDepth=Cell.NeckDepth;
        const float Temperature=Cell.Temperature;
        Cell.bVisible=Cover>=0.f && Heat>.015f;
        if(!Cell.bVisible)continue;
        const FVector2D P(MinX+(X+.5f)*Pitch,MinY+(Y+.5f)*Pitch);
        Cell.Temperature=FMath::Clamp(Heat*Temperature,0.f,1.f);
        // Only occupied cells need expensive skin depth interpolation.
        float Front=-BIG_NUMBER;
        if(bPoint) Front=-WickDepth+Bias;
        else
        {
            float BestD2[3]={BIG_NUMBER,BIG_NUMBER,BIG_NUMBER};
            float BestDepth[3]={0.f,0.f,0.f};
            for(int32 I=0;I<Samples.Num();++I)
            {
                const float D2=float(FVector2D::DistSquared(P,Samples[I]));
                if(D2>=BestD2[2]) continue;
                int32 Slot=2;
                while(Slot>0 && D2<BestD2[Slot-1])
                {BestD2[Slot]=BestD2[Slot-1];BestDepth[Slot]=BestDepth[Slot-1];--Slot;}
                BestD2[Slot]=D2;BestDepth[Slot]=SampleDepths[I];
            }
            for(int32 I=0;I<3;++I) if(BestD2[I]<BIG_NUMBER*.5f)
                Front=FMath::Max(Front,BestDepth[I]+Bias);
            if(Front<=-BIG_NUMBER*.5f) Front=HighestDepth+Bias;
        }
        Cell.Depth=FMath::Max(Front,NeckDepth+Bias*.2f);
    }
    TArray<FVector> Verts,Normals;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;
    TArray<FProcMeshTangent> Tangents;
    TArray<int32> Tris;
    // Duplicate cell vertices for constant colour, but average the same
    // corner depths across adjacent cells so the mesh has no cracks.
    auto CornerDepth=[&](int32 X,int32 Y)
    {
        float Sum=0.f;int32 Count=0;
        for(int32 DY=-1;DY<=0;++DY) for(int32 DX=-1;DX<=0;++DX)
        {
            const int32 CX=X+DX,CY=Y+DY;
            if(CX<0||CX>=NX||CY<0||CY>=NY) continue;
            const FFireCell& C=Cells[CY*NX+CX];
            if(C.bVisible){Sum+=C.Depth;++Count;}
        }
        return Count>0?Sum/Count:HighestDepth+3.f;
    };
    for(int32 Y=0;Y<NY;++Y) for(int32 X=0;X<NX;++X)
    {
        const FFireCell& C=Cells[Y*NX+X];
        if(!C.bVisible) continue;
        const int32 First=Verts.Num();
        for(int32 K=0;K<4;++K)
        {
            const int32 CX=X+K%2,CY=Y+K/2;
            const FVector2D Pixel(MinX+CX*Pitch,MinY+CY*Pitch);
            const FVector World=Projection.Unproject(Pixel,FMath::Max(10.f,-CornerDepth(CX,CY)));
            Verts.Add(RenderTransform.InverseTransformPosition(World));
            Normals.Add(Normal);UVs.Add(FVector2D(K%2,K/2));
            Colors.Add(FLinearColor(ASTFireLife::ToneCode(C.Temperature),0.f,0.f,1.f));
            Tangents.Add(FProcMeshTangent(1,0,0));
        }
        Tris.Add(First);Tris.Add(First+2);Tris.Add(First+1);
        Tris.Add(First+1);Tris.Add(First+2);Tris.Add(First+3);
    }
    const FProcMeshSection* OldBody=Render->GetProcMeshSection(1);
    if(Verts.IsEmpty()){if(OldBody && !OldBody->ProcVertexBuffer.IsEmpty())Render->ClearMeshSection(1);}
    else if(OldBody && OldBody->ProcVertexBuffer.Num()==Verts.Num())
        Render->UpdateMeshSection_LinearColor(1,Verts,Normals,UVs,Colors,Tangents);
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
    if ((!bVisible || Heat<.005f) && !bHasSparks && !bHasSmoke)
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
