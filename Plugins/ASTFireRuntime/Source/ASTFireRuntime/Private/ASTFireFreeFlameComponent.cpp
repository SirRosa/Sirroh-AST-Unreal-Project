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

namespace ASTFireLife
{
    // Deterministic per-flame irregularity: no copies of a shared sine sprite.
    float Hash(float V)
    {
        return FMath::Frac(FMath::Sin(V * 78.233f + 3.17f) * 43758.5453f);
    }

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
    // Preserve the approved growing/bridging/retracting timing. Only the
    // per-generation shape is diversified: some short/thin licks, some
    // long broad tongues, all within the same living source field.
    Tongue.GrowSeconds = .48f + .76f * Uniform(Salt + 1);
    Tongue.MaxLength = (19.f + 158.f *
        FMath::Pow(Uniform(Salt + 2),1.34f)) * Tongue.Scale;
    Tongue.BaseWidth = (5.5f + 38.f *
        FMath::Pow(Uniform(Salt + 3),1.65f)) * Tongue.Scale;
    Tongue.RemainingFraction = .30f + .27f * Uniform(Salt + 4);
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
    int32 DesiredCount = 5; // retain compact free-standing fire
    if (Cast<USkeletalMeshComponent>(Source))
    {
        // A whole character needs head/torso/arms/legs, not 3 foot roots.
        DesiredCount = 10 + FMath::RoundToInt(Uniform(731) * 4.f);
    }
    else if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Source))
    {
        const FVector Size = Static->Bounds.BoxExtent * 2.f;
        const float Area = 2.f * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
        const float MaxSide = Size.GetMax();
        DesiredCount = MaxSide < 140.f ? 8 + FMath::RoundToInt(Uniform(732)*3.f)
            : MaxSide > 340.f ? 12 + FMath::RoundToInt(Uniform(733)*3.f)
            : FMath::Clamp(FMath::RoundToInt(Area / 14500.f) + 7, 9, 14);
    }
    DesiredCount = FMath::Clamp(DesiredCount, 3, MaxTongues);

    if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Source))
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
    else if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Source))
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
                    if(!Static->LineTraceComponent(Hit,
                        Transform.TransformPosition(Point+Normal*18.f),
                        Transform.TransformPosition(Point-Normal*Far),Query))
                        continue; // Never invent anchors outside real collision.
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
    if (Tongues.IsEmpty() && !Cast<UMeshComponent>(Source))
    {
        // Free-standing source uses compact flame base, never a rectangular field.
        for (int32 I=0; I<DesiredCount; ++I)
        {
            FTongue Root;
            const float Angle=I*2.399963f;
            Root.RestPosition=FVector(FMath::Cos(Angle)*13.f,
                FMath::Sin(Angle)*13.f,0.f);
            Root.Seed = Uniform(I+501)*103.f;
            Root.Scale = .85f;
            Tongues.Add(Root);
        }
    }
    RebuildClusters();
    for (int32 I=0; I<Tongues.Num(); ++I)
    {
        FTongue& Root=Tongues[I];
        ConfigureGeneration(Root);
        Root.Age = -(.07f + Uniform(I+611)*.42f);
        Root.CurrentLength = Root.MaxLength * Root.RemainingFraction;
        Root.RestartLength = Root.CurrentLength;
        Root.CurrentWidth = Root.BaseWidth;
        Root.PreviousWidth = Root.BaseWidth;
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
    return Base+FVector::UpVector*Tongue.CurrentLength*V+
        Flow*V*V*.095f+Right*(Curl+Curl2+Lean*V+Dance+TipHook);
}

int32 UASTFireFreeFlameComponent::EmitTip(
    FTongue& Tongue, const FVector& Base, float Length, const FVector& Flow)
{
    FDetached& Child=Detached[DetachCursor % MaxDetached];
    ++DetachCursor;
    const int32 Salt=DetachCursor*137+Tongue.Generation*91;
    Child.bAlive=true;
    Child.Age=0.f;
    Child.Lifetime=.52f + .68f*Uniform(Salt+3);
    // Preserve phase timing. Match the visible detached mass to the
    // (1 - remaining fraction) of the mother tongue about to retract.
    Child.Length=Length*(1.f-Tongue.RemainingFraction)*
        (.82f+.25f*Uniform(Salt+4));
    Child.Width=Tongue.CurrentWidth*(.76f+.36f*Uniform(Salt+5));
    // The detached piece starts at the EXACT current dancing curve tip,
    // otherwise introducing independent sway would break the approved bridge.
    FVector View=FVector::ForwardVector;
    if(const UWorld* World=GetWorld())
        if(const APlayerController* PC=World->GetFirstPlayerController())
            if(const APlayerCameraManager* Cam=PC->PlayerCameraManager)
                View=(Cam->GetCameraLocation()-GetOwner()->GetActorLocation())
                    .GetSafeNormal();
    FVector Right=FVector::CrossProduct(FVector::UpVector,View).GetSafeNormal();
    if(Right.IsNearlyZero())Right=FVector::RightVector;
    Child.Position=TongueCenterAt(Tongue,Base,1.f,Right);
    // Distinct lateral ejection even in still air; old rise/heat/life remains.
    Child.Velocity=FVector::UpVector*(65.f+60.f*Uniform(Salt+7))+
        Flow*.28f-Tongue.WorldVelocity*.12f+
        FVector::RightVector*(Uniform(Salt+10)*2.f-1.f)*110.f+
        FVector::ForwardVector*(Uniform(Salt+11)*2.f-1.f)*95.f;
    // Same thermal palette as the parent mantle: do not spawn an orange
    // ember from a nearly white/yellow parent. Cooling occurs during life.
    Child.Temperature=.92f + .07f*Uniform(Salt+8);
    Child.Seed=Uniform(Salt+9)*102.f;
    Child.Right=Right;
    return (DetachCursor-1)%MaxDetached;
}

void UASTFireFreeFlameComponent::AdvanceSimulation(float Dt)
{
    Elapsed += Dt;
    Spread = FMath::Clamp(Spread + Dt * (Heat > .01f ? .23f : 0.f), 0.f, 1.f);
    float SumEnergy = 0.f;
    for (int32 TongueIndex=0;TongueIndex<Tongues.Num();++TongueIndex)
    {
        FTongue& T=Tongues[TongueIndex];
        const FVector Base = SampleRoot(T);
        const FVector Velocity = (Base - T.WorldPrevious) / FMath::Max(.004f,Dt);
        T.WorldPrevious = Base;
        T.WorldVelocity = FMath::VInterpTo(T.WorldVelocity,Velocity,Dt,13.f);
        if (!T.bActive)
        {
            if (Spread<T.ActivationSpread) continue;
            T.bActive=true;
            T.Age=-.06f; // gradual ignition from the shared hot mantle
        }
        if (T.Age < 0.f)
        {
            T.Age += Dt;
            continue;
        }
        if (Heat < .015f) continue;

        switch (T.Life)
        {
        case EFlameLife::Growing:
        {
            T.Age += Dt;
            const float Growth = ASTFireLife::SmoothStep(
                T.Age / FMath::Max(.1f,T.GrowSeconds));
            T.CurrentLength = FMath::Lerp(T.RestartLength,T.MaxLength,Growth);
            T.CurrentWidth = FMath::Lerp(T.PreviousWidth,T.BaseWidth,Growth);
            if (T.Age>=T.GrowSeconds)
            {
                const FVector Flow=(Wind-T.WorldVelocity*.65f)
                    .GetClampedToMaxSize(1600.f);
                T.SplitLength = T.CurrentLength = T.MaxLength;
                // Detachment originates from the rendered, cloak-bound tip
                // rather than an invisible bone/mesh location behind it.
                const FVector VisualBase=ConnectedRoots.IsValidIndex(TongueIndex)
                    ? ConnectedRoots[TongueIndex] : Base;
                T.BridgedChild = EmitTip(T,VisualBase,T.SplitLength,Flow);
                T.BridgeSeconds = .12f + Uniform(T.Generation*83+9)*.06f;
                T.PhaseAge = 0.f;
                T.Life = EFlameLife::Bridging;
            }
            break;
        }
        case EFlameLife::Bridging:
        {
            // Several visible frames: stretched neck, thinning, then rupture.
            T.PhaseAge += Dt;
            T.CurrentLength = T.SplitLength;
            if (T.PhaseAge >= T.BridgeSeconds)
            {
                T.Life = EFlameLife::Retracting;
                T.PhaseAge = 0.f;
                T.RetractSeconds = .16f + Uniform(T.Generation*101+12)*.11f;
                T.RetractTarget = T.SplitLength *
                    FMath::Clamp(T.RemainingFraction,.30f,.60f);
                T.BridgedChild = INDEX_NONE;
            }
            break;
        }
        case EFlameLife::Retracting:
        {
            T.PhaseAge += Dt;
            const float Ease = ASTFireLife::SmoothStep(
                T.PhaseAge / FMath::Max(.01f,T.RetractSeconds));
            T.CurrentLength = FMath::Lerp(T.SplitLength,T.RetractTarget,Ease);
            if (T.PhaseAge >= T.RetractSeconds)
            {
                T.CurrentLength = T.RetractTarget;
                T.RestartLength = T.CurrentLength;
                T.PreviousWidth = T.CurrentWidth;
                T.PreviousPhase = T.Phase;
                T.Age = 0.f;
                ConfigureGeneration(T);
                T.PhaseAge = 0.f;
                T.Life = EFlameLife::Growing;
            }
            break;
        }
        }
        SumEnergy += Heat*(.40f + .60f*FMath::Clamp(
            T.CurrentLength/FMath::Max(1.f,T.MaxLength),0.f,1.2f));
    }
    for (FDetached& D:Detached)
    {
        if (!D.bAlive) continue;
        D.Age += Dt;
        if (D.Age>=D.Lifetime || Heat<.01f)
        {
            D.bAlive=false;
            continue;
        }
        const float Fade=1.f-D.Age/D.Lifetime;
        D.Velocity+=(FVector::UpVector*(160.f+210.f*(1.f-Fade))+
            Wind*.09f)*Dt;
        D.Position+=D.Velocity*Dt;
        SumEnergy+=D.Temperature*Fade*.4f;
    }
    VisualEnergy=FMath::FInterpTo(VisualEnergy,
        Tongues.IsEmpty()?0.f:FMath::Clamp(
            SumEnergy/FMath::Max(1,Tongues.Num())*.81f,.05f,1.2f),Dt,8.f);
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
        float Radius=FMath::Min(Headroom,S.InitialRadius*Growth*
            (.91f+.09f*FMath::Sin(S.Seed+S.Age*.8f)));
        Radius=FMath::Min(Radius,S.Radius+Dt*8.f);
        // Bounded clearance checks; moving obstacles can displace/dissolve
        // a carrier, but cannot leave a big rendered patch inside them.
        for (int32 Try=0;Try<3 && Radius>=3.f && !IsClear(S.Position,Radius);++Try)
            Radius*=.72f;
        if (Radius<3.f || !IsClear(S.Position,Radius))
        { S.bAlive=false; continue; }
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
        const FCollisionShape Shape=FCollisionShape::MakeSphere(Radius+1.f);
        FHitResult Hit;
        if (World->SweepSingleByChannel(Hit,S.Position,S.Position+Delta,
                FQuat::Identity,SmokeChannel,Shape,Query))
        {
            if (Hit.bStartPenetrating) { S.bAlive=false; continue; }
            S.Position=Hit.Location+Hit.Normal*.85f;
            FVector Slide=FVector::VectorPlaneProject(
                Delta*(1.f-Hit.Time),Hit.Normal);
            if (Hit.Normal.Z<-.45f)
            {
                // Accumulation under a roof: tangent drift, varied by
                // carrier; after passing an edge buoyancy resumes.
                const FVector Side(FMath::Cos(S.Seed),FMath::Sin(S.Seed),0.f);
                Slide+=FVector::VectorPlaneProject(
                    Side*S.RiseSpeed*.43f*Dt,Hit.Normal);
            }
            FHitResult Second;
            if (World->SweepSingleByChannel(Second,S.Position,
                    S.Position+Slide,FQuat::Identity,SmokeChannel,Shape,Query))
            {
                if (!Second.bStartPenetrating)
                    S.Position=Second.Location+Second.Normal*.85f;
            }
            else S.Position+=Slide;
            S.Velocity=FVector::VectorPlaneProject(S.Velocity,Hit.Normal);
        }
        else S.Position+=Delta;
        // Final clearance and height check also covers the slide leg.
        const float Remaining=S.MaxRise-FMath::Max(0.f,float(S.Position.Z-S.BaseZ))-2.f;
        S.Radius=FMath::Min(S.Radius,Remaining);
        if (S.Radius<3.f || !IsClear(S.Position,S.Radius)) S.bAlive=false;
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
void UASTFireFreeFlameComponent::UpdateDetachedVortices(float Dt)
{
    for(FDetached& D:Detached)
    {
        if(!D.bAlive) continue;
        const float Fade=FMath::Clamp(
            1.f-D.Age/FMath::Max(.1f,D.Lifetime),0.f,1.f);
        const FVector Side=D.Right.GetSafeNormal();
        const FVector Depth=FVector::CrossProduct(
            FVector::UpVector,Side).GetSafeNormal();
        const float Speed=4.0f+3.3f*ASTFireLife::Hash(D.Seed*.41f);
        const float Phase=D.Age*Speed+D.Seed*.19f;
        // Two nonharmonic accelerations produce actual curved, irregular
        // world-space trajectories, even with no wind (not a sprite wiggle).
        const FVector Vortex=Side*FMath::Sin(Phase)*
            (195.f+105.f*ASTFireLife::Hash(D.Seed*.65f))+
            Depth*FMath::Cos(Phase*.77f+D.Seed*.31f)*145.f;
        D.Velocity=(D.Velocity+Vortex*Dt*(.43f+.57f*Fade))
            .GetClampedToMaxSize(900.f);
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
        Colors.Add(FLinearColor(Thermal,V,FMath::Frac(ShapeSeed*.017f),1.f));
        Colors.Add(FLinearColor(Thermal,V,FMath::Frac(ShapeSeed*.017f),1.f));
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

    // Calculate the continuous source FIRST. It also publishes the exact
    // per-tongue emergence positions on its front surface, in the same
    // camera basis and frame as the actual source geometry.
    UpdateConnectedSource(bCreate, View, Right);

    for (int32 I=0;I<Tongues.Num();++I)
    {
        const FTongue& T=Tongues[I];
        const FVector Base=ConnectedRoots.IsValidIndex(I)
            ? ConnectedRoots[I] : SampleRoot(T);
        const float Fraction=FMath::Clamp(T.Age/FMath::Max(.1f,T.GrowSeconds),0.f,1.f);
        const float Growth=ASTFireLife::SmoothStep(Fraction);
        const float Width=T.CurrentWidth;
        const float ShapePhase=FMath::Lerp(T.PreviousPhase,T.Phase,Growth);
        TArray<FVector> C;
        TArray<float> W;
        TArray<float> H;
        for (int32 K=0;K<RingsPerTongue;++K)
        {
            const float V=float(K)/float(RingsPerTongue-1);
            C.Add(TongueCenterAt(T,Base,V,Right));
            // One continuous silhouette. Vary each ring instead of stacking
            // identical camera-facing sprite/rectangle pieces.
            const float Ridge=FMath::Clamp(.80f+.20f*
                FMath::Sin(V*13.8f+ShapePhase+.57f*Elapsed),.38f,1.08f);
            // The root has ZERO planar width: its heat comes from the mesh
            // surface mantle, not a horizontal sprite glued to one vertex.
            const float RootBlend=ASTFireLife::SmoothStep(V/.20f);
            const float Tip=FMath::Pow(FMath::Max(0.f,1.f-V),.73f);
            W.Add(Width*RootBlend*Tip*Ridge);
            // Section 1 owns the COMPLETE living flame silhouette.
            // Section 0 preserves split bridges and detached fragments only.
            H.Add(0.f);
        }
        if (!T.bActive || T.Age<0.f || Heat<.015f)
            for (float& Temperature:H)Temperature=0.f;
        AddStrip(C,W,H,T.Seed,1.f,Right,Verts,UVs,Colors);

        // Fixed-size bridge geometry: 3–5 visible simulation frames of hot
        // stretched gas between the old tip and the departing child. The
        // width eases to zero before the parent begins its gradual retreat.
        TArray<FVector> BC;
        TArray<float> BW;
        TArray<float> BH;
        const bool bBridge=T.Life==EFlameLife::Bridging &&
            T.BridgedChild>=0 && T.BridgedChild<Detached.Num() &&
            Detached[T.BridgedChild].bAlive;
        const float BridgeAlpha=bBridge?ASTFireLife::SmoothStep(
            T.PhaseAge/FMath::Max(.04f,T.BridgeSeconds)):1.f;
        const FVector BridgeEnd=bBridge?Detached[T.BridgedChild].Position:C.Last();
        for (int32 K=0;K<BridgeRings;++K)
        {
            const float V=float(K)/float(BridgeRings-1);
            BC.Add(FMath::Lerp(C.Last(),BridgeEnd,V)+Right*
                (bBridge?FMath::Sin(PI*V)*1.5f:0.f));
            BW.Add(bBridge?Width*.31f*(1.f-BridgeAlpha)*
                FMath::Sin(PI*(.08f+.84f*V)):.0f);
            BH.Add(bBridge?(.88f-.28f*V)*(1.f-BridgeAlpha):0.f);
        }
        AddStrip(BC,BW,BH,T.Seed,1.f,Right,Verts,UVs,Colors);
    }

    // Stage18: detached fire must use the SAME radial hot-core/red-rim
    // field as the continuous mantle. Stage17's TWO vertices per row
    // could only interpolate two uniformly orange boundary temperatures;
    // changing Child.Temperature alone could never fix this palette.
    static constexpr int32 DetachedColumns=7;
    const FTransform RenderTransform=Render->GetComponentTransform();
    for (const FDetached& D:Detached)
    {
        TArray<FVector> C;
        TArray<float> W;
        const float Age=D.bAlive
            ? FMath::Clamp(D.Age/FMath::Max(.1f,D.Lifetime),0.f,1.f):1.f;
        const float Fade=D.bAlive?1.f-Age:0.f;
        const float SpinSpeed=3.3f+5.f*ASTFireLife::Hash(D.Seed*.37f);
        const float Spin=D.Seed*.11f+D.Age*SpinSpeed;
        const float Angle=FMath::Sin(Spin)*.75f;
        // Perspective-correct billboard centered on THIS fragment, not
        // the actor root. Project camera screen-right onto its view plane.
        // Unlike world-up crossing, this remains stable directly overhead.
        const FVector ToCamera=(CameraLocation-D.Position).GetSafeNormal();
        const FVector BillboardView=ToCamera.IsNearlyZero()?View:ToCamera;
        const FVector ProjectedRight=CameraRight-BillboardView*
            FVector::DotProduct(CameraRight,BillboardView);
        FVector BillboardRight=ProjectedRight.GetSafeNormal();
        if (BillboardRight.IsNearlyZero())
            BillboardRight=FVector::CrossProduct(
                FVector::UpVector,BillboardView).GetSafeNormal();
        if (BillboardRight.IsNearlyZero())
            BillboardRight=FVector::CrossProduct(
                FVector::ForwardVector,BillboardView).GetSafeNormal();
        if (BillboardRight.IsNearlyZero())
            BillboardRight=FVector::CrossProduct(
                FVector::RightVector,BillboardView).GetSafeNormal();
        const FVector BillboardUp=FVector::CrossProduct(
            BillboardView,BillboardRight).GetSafeNormal();
        // Keep the approved independent spin and irregular silhouette,
        // but rotate in the actual camera-facing billboard plane.
        const FVector LocalUp=(BillboardUp*FMath::Cos(Angle)+
            BillboardRight*FMath::Sin(Angle)).GetSafeNormal();
        const FVector LocalSide=(BillboardRight*FMath::Cos(Angle)-
            BillboardUp*FMath::Sin(Angle)).GetSafeNormal();
        for (int32 K=0;K<RingsPerDetached;++K)
        {
            const float V=float(K)/float(RingsPerDetached-1);
            const float Curl=(FMath::Sin(V*9.8f+Spin)*.24f+
                FMath::Sin(V*16.3f-Spin*.72f)*.13f)*
                D.Width*FMath::Pow(V,1.2f)*Fade;
            C.Add(D.Position+LocalUp*D.Length*V*Fade+
                LocalSide*Curl);
            const float Lobes=FMath::Clamp(1.f+
                .31f*FMath::Sin(V*14.2f+Spin+D.Seed) +
                .17f*FMath::Sin(V*24.4f-Spin*.54f),.43f,1.35f);
            W.Add(D.Width*FMath::Pow(Fade,.72f)*
                FMath::Sin(PI*(.08f+.88f*V))*Lobes);
        }
        // Seven columns provide real red edges, orange transition, and a
        // yellow/white core. No extra material or independent sprite pass.
        // Vertex colors now follow the same Cover/Core heat equation used
        // by UpdateConnectedSource(), with identical 16-bit palette steps.
        for(int32 K=0;K<RingsPerDetached;++K)
        {
            const float V=float(K)/float(RingsPerDetached-1);
            const float Radius=FMath::Max(.001f,W[K]);
            const float EdgeWidth=FMath::Max(4.f,Radius*.45f);
            for(int32 X=0;X<DetachedColumns;++X)
            {
                const float U=float(X)/float(DetachedColumns-1);
                const float Side=U*2.f-1.f;
                const float DFromCenter=FMath::Abs(Side)*
                    (Radius+EdgeWidth*.04f);
                const float Cover=FMath::Clamp(1.f-
                    (DFromCenter-Radius)/EdgeWidth,0.f,1.f);
                const float Core=FMath::Clamp((Radius-DFromCenter)/
                    FMath::Max(1.f,Radius),0.f,1.f);
                const float LocalHeat=Cover*(.19f+
                    .81f*FMath::Pow(Core,.56f))*(1.f-.19f*V);
                const float Thermal=FMath::Clamp(
                    Heat*D.Temperature*LocalHeat*
                    FMath::Pow(Fade,.66f),0.f,1.f);
                const FVector World=C[K]+LocalSide*
                    (Side*(Radius+EdgeWidth*.04f));
                Verts.Add(RenderTransform.InverseTransformPosition(World));
                UVs.Add(FVector2D(U,V));
                Colors.Add(FLinearColor(Thermal,V,
                    FMath::Frac(D.Seed*.017f),1.f));
            }
        }
    }

    TArray<int32> Triangles;
    const int32 StripCount=Tongues.Num()*2;
    Triangles.Reserve((Tongues.Num()*(RingsPerTongue+BridgeRings-2))*6+
        MaxDetached*(RingsPerDetached-1)*(DetachedColumns-1)*6);
    int32 Offset=0;
    for(int32 I=0;I<StripCount;++I)
    {
        const int32 N=I%2==0?RingsPerTongue:BridgeRings;
        for(int32 K=0;K<N-1;++K)
        {
            const int32 A=Offset+2*K;
            Triangles.Add(A);Triangles.Add(A+1);Triangles.Add(A+2);
            Triangles.Add(A+1);Triangles.Add(A+3);Triangles.Add(A+2);
        }
        Offset+=N*2;
    }
    // Unlike the original two-column strip, each detached fragment
    // has its own continuous, fixed-topology transverse heat gradient.
    for(int32 I=0;I<MaxDetached;++I)
    {
        for(int32 K=0;K<RingsPerDetached-1;++K)
            for(int32 X=0;X<DetachedColumns-1;++X)
            {
                const int32 A=Offset+K*DetachedColumns+X;
                Triangles.Add(A);
                Triangles.Add(A+1);
                Triangles.Add(A+DetachedColumns);
                Triangles.Add(A+1);
                Triangles.Add(A+DetachedColumns+1);
                Triangles.Add(A+DetachedColumns);
            }
        Offset+=RingsPerDetached*DetachedColumns;
    }
    check(Offset==Verts.Num());
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    Normals.Init(FVector::ForwardVector,Verts.Num());
    Tangents.Init(FProcMeshTangent(1,0,0),Verts.Num());
    if (bCreate)
        Render->CreateMeshSection_LinearColor(0,Verts,Triangles,Normals,UVs,
            Colors,Tangents,false);
    else
        Render->UpdateMeshSection_LinearColor(0,Verts,Normals,UVs,Colors,Tangents);

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

    // Section 1 was authored before this section, so each tongue's
    // zero-width starting point lies inside the same filled fire source.
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
void UASTFireFreeFlameComponent::UpdateConnectedSource(
    bool bCreate,const FVector& View,const FVector& Right)
{
    if (!IsValid(Render)) return;
    const FVector Up=FVector::UpVector;
    FVector Normal=FVector::CrossProduct(Right,Up).GetSafeNormal();
    if (Normal.IsNearlyZero()) Normal=View;
    const FVector Origin=IsValid(Source)?Source->GetComponentLocation():
        GetOwner()->GetActorLocation();
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
        const FVector Diff=World-Origin;
        const FVector2D P(
            FVector::DotProduct(Diff,Right),
            FVector::DotProduct(Diff,Up));
        Samples.Add(P);
        SampleDepths.Add(
            float(FVector::DotProduct(Diff,Normal))-DepthCompensation);
        // Mesh samples are used ONLY for foreground depth, never heat bounds.
        HighestDepth=FMath::Max(HighestDepth,
            float(FVector::DotProduct(Diff,Normal)));
    };

    for (const FSourceAnchor& A:SourceAnchors)
    {
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

    if(!Cast<UMeshComponent>(Source))
        Include(Origin); // no donut in free-standing fire
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
        const FVector Diff=Root-Origin;
        const FVector2D P(
            FVector::DotProduct(Diff,Right),
            FVector::DotProduct(Diff,Up));
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
        const float RootDepth=float(FVector::DotProduct(Diff,Normal));
        ConnectedRoots.Add(Root+Normal*(Front+6.5f-RootDepth));
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
        const FVector Delta=World-Origin;
        return FVector2D(
            FVector::DotProduct(Delta,Right),
            FVector::DotProduct(Delta,Up));
    };
    auto AppendNeck=[&](const FVector& WorldA,const FVector& WorldB,
        float RadiusA,float RadiusB,float V0,float V1)
    {
        FNeckSegment Neck;
        Neck.Start=Project(WorldA);
        Neck.End=Project(WorldB);
        Neck.StartRadius=FMath::Max(0.f,RadiusA);
        Neck.EndRadius=FMath::Max(0.f,RadiusB);
        Neck.StartDepth=FVector::DotProduct(WorldA-Origin,Normal);
        Neck.EndDepth=FVector::DotProduct(WorldB-Origin,Normal);
        Neck.StartV=V0;
        Neck.EndV=V1;
        Necks.Add(Neck);
        const float Extent=FMath::Max(Neck.StartRadius,Neck.EndRadius)+12.f;
        MinX=FMath::Min(MinX,FMath::Min(float(Neck.Start.X),
            float(Neck.End.X))-Extent);
        MaxX=FMath::Max(MaxX,FMath::Max(float(Neck.Start.X),
            float(Neck.End.X))+Extent);
        MinY=FMath::Min(MinY,FMath::Min(float(Neck.Start.Y),
            float(Neck.End.Y))-Extent);
        MaxY=FMath::Max(MaxY,FMath::Max(float(Neck.Start.Y),
            float(Neck.End.Y))+Extent);
    };
    for(int32 I=0;I<Tongues.Num();++I)
    {
        const FTongue& T=Tongues[I];
        if(!T.bActive || T.Age<0.f || Heat<.015f) continue;
        const FVector& Root=ConnectedRoots[I];
        const float Phase=FMath::Lerp(T.PreviousPhase,T.Phase,
            ASTFireLife::SmoothStep(T.Age/FMath::Max(.1f,T.GrowSeconds)));
        auto RadiusAt=[&](float V)
        {
            // Stage19: keep the anti-tentacle width floor, but remove the
            // oversized balloon profile from Stage18. This affects the
            // living source silhouette ONLY, never detached fragments.
            const float PlumeWidth=FMath::Max(T.CurrentWidth,
                T.CurrentLength*(.058f+.010f*
                    ASTFireLife::Hash(T.Seed*.51f)));
            // Place most volume in the lower third, retaining small moving
            // asymmetry rather than a second broad upper oval.
            const float BulgeA=.26f*FMath::Exp(-FMath::Square(
                (V-(.28f+.04f*ASTFireLife::Hash(T.Seed*.31f)))/.18f));
            const float BulgeB=.12f*FMath::Exp(-FMath::Square(
                (V-(.46f+.03f*ASTFireLife::Hash(T.Seed*.71f)))/.16f));
            const float Waist=.14f*FMath::Exp(-FMath::Square(
                (V-.58f)/.09f));
            const float UpperTaper=FMath::Lerp(1.f,.55f,
                ASTFireLife::SmoothStep((V-.42f)/.40f));
            const float MovingRim=.10f*FMath::Sin(
                V*18.f+T.Seed*.37f+Elapsed*3.2f)+
                .06f*FMath::Sin(V*31.f-T.Seed*.21f-Elapsed*4.8f);
            const float Profile=FMath::Clamp(
                (.88f+BulgeA+BulgeB-Waist+MovingRim)*UpperTaper,
                .42f,1.28f);
            return FMath::Max(0.f,PlumeWidth*
                FMath::Pow(FMath::Max(0.f,1.f-V),.69f)*Profile);
        };
        FVector WorldA=TongueCenterAt(T,Root,NeckFractions[0],Right);
        for(int32 K=1;K<RingsPerTongue;++K)
        {
            const float V0=NeckFractions[K-1];
            const float V1=NeckFractions[K];
            const FVector WorldB=TongueCenterAt(T,Root,V1,Right);
            AppendNeck(WorldA,WorldB,RadiusAt(V0),RadiusAt(V1),V0,V1);
            WorldA=WorldB;
        }
        // Short swept lobes curve upward out of the SAME living plume,
        // rather than rigid horizontal antennae sticking off a thin shaft.
        const float ForkSeed=ASTFireLife::Hash(T.Seed*.173f);
        const int32 ForkCount=ForkSeed>.87f?2:(ForkSeed>.20f?1:0);
        for(int32 Fork=0;Fork<ForkCount;++Fork)
        {
            const float Attach=.37f+.33f*
                ASTFireLife::Hash(T.Seed*.47f+Fork*2.1f);
            const float Reach=FMath::Min(T.CurrentLength*
                (.13f+.13f*ASTFireLife::Hash(T.Seed*.57f+Fork)),34.f);
            const float Direction=(Fork==0?1.f:-1.f)*
                (ASTFireLife::Hash(T.Seed*.73f)>.5f?1.f:-1.f);
            const FVector BranchRoot=TongueCenterAt(T,Root,Attach,Right);
            const float Sway=FMath::Sin(Elapsed*3.7f+T.Seed+Fork)*3.4f;
            const FVector Mid=BranchRoot+Up*Reach*.54f+
                Right*Direction*Reach*.48f;
            const FVector Tip=BranchRoot+Up*Reach*1.15f+
                Right*Direction*(Reach*.74f+Sway);
            const float BranchRadius=RadiusAt(Attach)*
                (.59f+.16f*ASTFireLife::Hash(T.Seed*.38f+Fork));
            AppendNeck(BranchRoot,Mid,BranchRadius,
                BranchRadius*.55f,Attach,Attach+.09f);
            AppendNeck(Mid,Tip,BranchRadius*.55f,
                0.f,Attach+.09f,Attach+.20f);
        }
    }

    // Fuse nearby source roots with thin animated bridges. Far-apart
    // ignition patches stay separate rather than forming a body-sized blob.
    for(int32 I=1;I<ConnectedRoots.Num();++I)
    {
        const FTongue& T=Tongues[I];
        if(!T.bActive || T.Age<0.f) continue;
        int32 Best=INDEX_NONE;
        float BestDist=FMath::Square(75.f);
        for(int32 J=0;J<I;++J)
        {
            if(!Tongues[J].bActive || Tongues[J].Age<0.f)continue;
            const float D=float(FVector2D::DistSquared(
                Project(ConnectedRoots[I]),Project(ConnectedRoots[J])));
            if(D<BestDist){BestDist=D;Best=J;}
        }
        if(Best==INDEX_NONE) continue;
        FNeckSegment Link;
        Link.Start=Project(ConnectedRoots[I]);
        Link.End=Project(ConnectedRoots[Best]);
        Link.StartRadius=FMath::Clamp(T.CurrentWidth*.58f,4.f,12.f);
        Link.EndRadius=FMath::Clamp(Tongues[Best].CurrentWidth*.58f,4.f,12.f);
        Link.StartDepth=FVector::DotProduct(ConnectedRoots[I]-Origin,Normal);
        Link.EndDepth=FVector::DotProduct(ConnectedRoots[Best]-Origin,Normal);
        Link.StartV=0.f;
        Link.EndV=.12f;
        Necks.Add(Link);
    }
    if(Necks.IsEmpty())
    {
        MinX=-24.f;MaxX=24.f;MinY=-24.f;MaxY=24.f;
    }
    const float Extent=FMath::Max(20.f,FMath::Max(MaxX-MinX,MaxY-MinY));
    const float Radius=FMath::Clamp(
        Extent/(FMath::Sqrt(float(Samples.Num()))*1.5f),10.f,26.f);
    const float Outer=FMath::Clamp(Radius*.85f,9.f,18.f);
    const float Inner=FMath::Clamp(Radius*.65f,5.f,12.f);
    const float Margin=Radius+Outer+12.f;
    MinX-=Margin;MaxX+=Margin;MinY-=Margin;MaxY+=Margin;
    HighestDepth+=3.f; // fallback only; depth follows nearest skin points

    const int32 N=Samples.Num(); // depth-only skinned sample field
    const int32 Width=SourceGridX+1,Height=SourceGridY+1;
    TArray<FVector> Verts;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;
    Verts.Reserve(Width*Height);
    UVs.Reserve(Width*Height);
    Colors.Reserve(Width*Height);
    for(int32 Row=0;Row<Height;++Row)
        for(int32 Col=0;Col<Width;++Col)
        {
            const float X=FMath::Lerp(MinX,MaxX,
                float(Col)/float(SourceGridX));
            const float Y=FMath::Lerp(MinY,MaxY,
                float(Row)/float(SourceGridY));
            const FVector2D P(X,Y);
            // Two square roots per grid cell, NOT one per skin sample:
            // ~100x cheaper than repeatedly evaluating distances at 25Hz.
            // The silhouette is the living tongue union, not depth samples.
            // Interpolate the depth of nearby animated mesh samples, rather
            // than drawing a floating plane at the furthest vertex.
            float BestD2[3]={BIG_NUMBER,BIG_NUMBER,BIG_NUMBER};
            float BestDepth[3]={0.f,0.f,0.f};
            for(int32 I=0;I<N;++I)
            {
                const float D2=float(FVector2D::DistSquared(P,Samples[I]));
                if(D2<BestD2[2])
                {
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
            }
            const float Flicker=.90f+.10f*
                FMath::Sin(Elapsed*7.2f+X*.077f+Y*.039f);
            // A signed capsule union includes all living tongue necks in
            // the SAME mantle. Their geometry and the ribbon use the same
            // TongueCenterAt curve, current length, wind and curl.
            float NeckSupport=0.f;
            float NeckDepth=0.f;
            float StrokeU=.5f,StrokeV=.2f;
            for(const FNeckSegment& Neck:Necks)
            {
                // Early cull keeps cost bounded for large animated meshes.
                const float MaxNeckReach=FMath::Max(41.f,
                    FMath::Max(Neck.StartRadius,Neck.EndRadius)+12.f);
                if(P.X<FMath::Min(Neck.Start.X,Neck.End.X)-MaxNeckReach ||
                   P.X>FMath::Max(Neck.Start.X,Neck.End.X)+MaxNeckReach ||
                   P.Y<FMath::Min(Neck.Start.Y,Neck.End.Y)-MaxNeckReach ||
                   P.Y>FMath::Max(Neck.Start.Y,Neck.End.Y)+MaxNeckReach)
                    continue;
                const FVector2D Axis=Neck.End-Neck.Start;
                const float Along=FMath::Clamp(float(FVector2D::DotProduct(
                    P-Neck.Start,Axis))/
                    FMath::Max(1.f,float(Axis.SizeSquared())),0.f,1.f);
                const FVector2D Center=Neck.Start+Axis*Along;
                const float R=FMath::Lerp(Neck.StartRadius,Neck.EndRadius,Along);
                const float FadeWidth=FMath::Max(4.f,R*.45f);
                const float D2=float((P-Center).SizeSquared());
                if(D2>FMath::Square(R+FadeWidth)) continue;
                const float Cover=FMath::Clamp(1.f-
                    (FMath::Sqrt(D2)-R)/FadeWidth,0.f,1.f);
                const float V=FMath::Lerp(Neck.StartV,Neck.EndV,Along);
                const float D=FMath::Sqrt(D2);
                const float Core=FMath::Clamp((R-D)/
                    FMath::Max(1.f,R),0.f,1.f);
                const float Support=Cover*(.19f+
                    .81f*FMath::Pow(Core,.56f))*(1.f-.19f*V);
                if(Support>NeckSupport)
                {
                    NeckSupport=Support;
                    NeckDepth=FMath::Lerp(
                        Neck.StartDepth,Neck.EndDepth,Along);
                    const FVector2D Side=FVector2D(-Axis.Y,Axis.X)
                        .GetSafeNormal();
                    StrokeU=FMath::Clamp(.5f+.47f*
                        float(FVector2D::DotProduct(P-Center,Side))/
                        FMath::Max(1.f,R),0.f,1.f);
                    StrokeV=V;
                }
            }
            const float Thermal=FMath::Clamp(
                Heat*NeckSupport*Flicker,0.f,1.f);

            // Use the same nearest-front-depth rule as ConnectedRoots so
            // ribbon mouths and mantle pixels occupy a single depth surface.
            float FrontDepth=-BIG_NUMBER;
            for(int32 I=0;I<3;++I)
                if(BestD2[I]<BIG_NUMBER*.5f)
                    FrontDepth=FMath::Max(FrontDepth,BestDepth[I]);
            float Depth=FrontDepth>-BIG_NUMBER*.5f
                ? FrontDepth+5.f : HighestDepth;
            // The hot neck has the SAME depth as the strip that will emerge
            // from it: no hidden root on the opposite side of the character.
            if(NeckSupport>.02f)
                Depth=FMath::Max(Depth,NeckDepth+1.5f);
            const FVector World=Origin+Right*X+Up*Y+Normal*Depth;
            Verts.Add(RenderTransform.InverseTransformPosition(World));
            // UV.x=.5 gives the same bright center as tongue ribbons;
            // UV.y adds the exact same 16-bit stepped palette variation.
            UVs.Add(FVector2D(StrokeU,StrokeV));
            Colors.Add(FLinearColor(Thermal,StrokeV,FMath::Frac(
                (Seed+Col*7+Row*13)*.017f),1.f));
        }

    TArray<int32> Tris;
    if(bCreate)
    {
        Tris.Reserve(SourceGridX*SourceGridY*6);
        for(int32 Y=0;Y<SourceGridY;++Y)
            for(int32 X=0;X<SourceGridX;++X)
            {
                const int32 A=Y*Width+X;
                Tris.Add(A);Tris.Add(A+Width);Tris.Add(A+1);
                Tris.Add(A+1);Tris.Add(A+Width);Tris.Add(A+Width+1);
            }
    }
    TArray<FVector> Normals;
    Normals.Init(Normal,Verts.Num());
    TArray<FProcMeshTangent> Tangents;
    Tangents.Init(FProcMeshTangent(1,0,0),Verts.Num());
    if(bCreate)
        Render->CreateMeshSection_LinearColor(
            1,Verts,Tris,Normals,UVs,Colors,Tangents,false);
    else
        Render->UpdateMeshSection_LinearColor(
            1,Verts,Normals,UVs,Colors,Tangents);
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
