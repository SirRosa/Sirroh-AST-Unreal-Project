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
    Render->SetMaterial(2, Material); // tiny bright embers in matching palette
    SmokeMaterial = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/VFX/Fire/Materials/M_AST_FireSmoke.M_AST_FireSmoke"));
    if (IsValid(SmokeMaterial)) Render->SetMaterial(3, SmokeMaterial);
    Render->RegisterComponent();

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
    Render = nullptr;
    Tongues.Reset();
    BurnClusters.Reset();
    SourceAnchors.Reset();
    ConnectedRoots.Reset();
    Sparks.Reset();
    SparkSpawnDebt = 0.f;
    Smoke.Reset();
    SmokeSpawnDebt = 0.f;
    SmokeMaterial = nullptr;
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
void UASTFireFreeFlameComponent::UpdateSparks(float Dt)
{
    for (FSpark& S:Sparks)
    {
        if (!S.bAlive) continue;
        S.Age+=Dt;
        if (S.Age>=S.Lifetime) { S.bAlive=false; continue; }
        const float Phase=S.Seed*.17f+S.Age*(12.f+S.Seed*.003f);
        const FVector Whirl(
            FMath::Sin(Phase)*75.f,FMath::Cos(Phase*1.37f)*55.f,
            225.f+45.f*FMath::Sin(Phase*.67f));
        S.Velocity+=(Whirl+Wind*.12f)*Dt;
        S.Velocity=S.Velocity.GetClampedToMaxSize(750.f);
        S.Position+=S.Velocity*Dt;
    }
    if (!bVisible || Heat<.15f || Tongues.IsEmpty()) return;
    SparkSpawnDebt=FMath::Min(3.f,SparkSpawnDebt+
        Dt*(2.2f+9.f*Heat*FMath::Clamp(Energy,.2f,1.2f)));
    const int32 Born=FMath::Min(2,FMath::FloorToInt(SparkSpawnDebt));
    SparkSpawnDebt-=Born;
    for (int32 I=0;I<Born;++I)
    {
        FSpark& S=Sparks[SparkCursor%MaxSparks];
        const int32 Salt=SparkCursor++;
        const FTongue& T=Tongues[Salt%Tongues.Num()];
        if (!T.bActive || T.Age<0.f) { S.bAlive=false; continue; }
        const float Random=ASTFireLife::Hash(float(Salt)+T.Seed*.91f);
        const float Other=ASTFireLife::Hash(float(Salt)*.71f+T.Seed);
        const FVector Root=SampleRoot(T);
        S.Seed=float(Salt)*.731f+T.Seed*.38f;
        S.Position=Root+FVector::UpVector*
            (T.CurrentLength*(.28f+.55f*Random));
        S.Position+=FVector(
            (Other-.5f)*T.CurrentWidth,
            (Random-.5f)*T.CurrentWidth,
            0.f);
        S.Velocity=FVector(
            (Other-.5f)*170.f,(Random-.5f)*150.f,
            120.f+170.f*Random)+T.WorldVelocity*.16f+Wind*.07f;
        S.Size=1.1f+2.1f*Other;
        S.Lifetime=.21f+.45f*Random;
        S.Age=0.f;
        S.bAlive=true;
    }
}

// Stage24: world-space wisps from actual source geometry, small fixed pool.
// Drift remains reactive after extinguishing; never touches detached flames.
void UASTFireFreeFlameComponent::UpdateSmoke(float Dt)
{
    for (FSmoke& S:Smoke)
    {
        if (!S.bAlive) continue;
        S.Age+=Dt;
        if (S.Age>=S.Lifetime) { S.bAlive=false; continue; }
        const float Phase=S.Seed*.11f+S.Age*(2.1f+
            ASTFireLife::Hash(S.Seed*.43f)*1.3f);
        const FVector Eddy(FMath::Sin(Phase*1.17f)*28.f,
            FMath::Cos(Phase*.89f)*25.f,
            40.f+21.f*FMath::Sin(Phase*.73f));
        S.Velocity+=(Eddy+Wind*.036f)*Dt;
        S.Velocity=S.Velocity.GetClampedToMaxSize(380.f);
        S.Position+=S.Velocity*Dt;
    }
    if (!bVisible || Heat<.10f || Tongues.IsEmpty()) return;
    const float BurnRate=Heat*FMath::Clamp(Energy,.15f,1.25f);
    SmokeSpawnDebt=FMath::Min(3.f,SmokeSpawnDebt+
        Dt*(2.f+5.2f*BurnRate));
    const int32 Born=FMath::Min(2,FMath::FloorToInt(SmokeSpawnDebt));
    SmokeSpawnDebt-=Born;
    for (int32 I=0;I<Born;++I)
    {
        FSmoke& S=Smoke[SmokeCursor%MaxSmoke];
        const int32 Salt=SmokeCursor++;
        const FTongue& T=Tongues[Salt%Tongues.Num()];
        if (!T.bActive || T.Age<0.f) { S.bAlive=false; continue; }
        const float R=ASTFireLife::Hash(float(Salt)+T.Seed*.67f);
        const float J=ASTFireLife::Hash(float(Salt)*.39f+T.Seed);
        S.Position=SampleRoot(T)+FVector::UpVector*
            (6.f+T.CurrentLength*(.13f+.18f*R));
        S.Velocity=FVector((R-.5f)*26.f,(J-.5f)*35.f,
            38.f+55.f*R)+T.WorldVelocity*.22f+Wind*.045f;
        S.Radius=9.f+11.f*R;
        S.Lifetime=1.05f+1.3f*J;
        S.Seed=float(Salt)*.931f+T.Seed*.27f;
        S.EmissionHeat=FMath::Clamp(Heat*(.6f+.4f*R),0.f,1.f);
        S.Age=0.f;
        S.bAlive=true;
    }
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

    // Stage23: compact pixel-hot sparks, in fixed-size quad section 2.
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
            SC.Add(FLinearColor(Heat*Fade*(.87f+.13f*
                ASTFireLife::Hash(S.Seed)),0.15f,
                FMath::Frac(S.Seed*.017f),1.f));
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
        Render->CreateMeshSection_LinearColor(
            2,SV,ST,SN,SU,SC,SX,false);
    else
        Render->UpdateMeshSection_LinearColor(
            2,SV,SN,SU,SC,SX);

    // Stage24: 5x5 warped billboard surface per evolving smoke fragment.
    // Every puff has fixed topology, independent velocity and thermal fade.
    if (IsValid(SmokeMaterial))
    {
        TArray<FVector> MV;
        TArray<FVector2D> MU;
        TArray<FLinearColor> MC;
        TArray<int32> MT;
        MV.Reserve(MaxSmoke*SmokeGrid*SmokeGrid);
        MU.Reserve(MaxSmoke*SmokeGrid*SmokeGrid);
        MC.Reserve(MaxSmoke*SmokeGrid*SmokeGrid);
        if (bCreate) MT.Reserve(MaxSmoke*(SmokeGrid-1)*(SmokeGrid-1)*6);
        for (int32 I=0;I<MaxSmoke;++I)
        {
            const FSmoke& S=Smoke[I];
            const float Age=S.bAlive?FMath::Clamp(
                S.Age/FMath::Max(.05f,S.Lifetime),0.f,1.f):1.f;
            const float Birth=ASTFireLife::SmoothStep(Age/.16f);
            const float Fade=(1.f-Age)*(1.f-Age)*Birth;
            const float Radius=S.Radius*(.62f+1.65f*Age);
            const FVector Ray=(CameraLocation-S.Position).GetSafeNormal();
            const FVector N=Ray.IsNearlyZero()?View:Ray;
            FVector Side=(CameraRight-N*
                FVector::DotProduct(CameraRight,N)).GetSafeNormal();
            if (Side.IsNearlyZero())
                Side=FVector::CrossProduct(FVector::UpVector,N).GetSafeNormal();
            if (Side.IsNearlyZero())
                Side=FVector::CrossProduct(FVector::ForwardVector,N).GetSafeNormal();
            const FVector Up=FVector::CrossProduct(N,Side).GetSafeNormal();
            for (int32 Y=0;Y<SmokeGrid;++Y)
                for (int32 X=0;X<SmokeGrid;++X)
                {
                    const float U=float(X)/float(SmokeGrid-1);
                    const float V=float(Y)/float(SmokeGrid-1);
                    const float DX=U*2.f-1.f, DY=V*2.f-1.f;
                    const float Phase=S.Seed*.18f+S.Age*4.3f;
                    const float Warp=.12f*FMath::Sin(
                        DX*7.1f+DY*5.8f+Phase)+.07f*
                        FMath::Sin(DY*11.4f-DX*3.7f-Phase*.71f);
                    const FVector P=S.Position+
                        Side*(DX+Warp*DY)*Radius+
                        Up*(DY+Warp*DX)*Radius*.82f;
                    MV.Add(RenderTransform.InverseTransformPosition(P));
                    MU.Add(FVector2D(U,V));
                    MC.Add(FLinearColor(S.EmissionHeat,V,
                        FMath::Frac(S.Seed*.017f),
                        S.bAlive?Fade*(.34f+.36f*S.EmissionHeat):0.f));
                }
            if (bCreate)
                for (int32 Y=0;Y<SmokeGrid-1;++Y)
                    for (int32 X=0;X<SmokeGrid-1;++X)
                    {
                        const int32 A=I*SmokeGrid*SmokeGrid+
                            Y*SmokeGrid+X;
                        MT.Add(A);MT.Add(A+SmokeGrid);MT.Add(A+1);
                        MT.Add(A+1);MT.Add(A+SmokeGrid);
                        MT.Add(A+SmokeGrid+1);
                    }
        }
        TArray<FVector> MN;
        TArray<FProcMeshTangent> MX;
        MN.Init(FVector::ForwardVector,MV.Num());
        MX.Init(FProcMeshTangent(1,0,0),MV.Num());
        if (bCreate)
            Render->CreateMeshSection_LinearColor(
                3,MV,MT,MN,MU,MC,MX,false);
        else
            Render->UpdateMeshSection_LinearColor(
                3,MV,MN,MU,MC,MX);
    }

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
