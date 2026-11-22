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
    Render->RegisterComponent();

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
    bGenerated = false;
    Super::OnUnregister();
}

void UASTFireFreeFlameComponent::ConfigureGeneration(FTongue& Tongue)
{
    ++Tongue.Generation;
    const int32 Salt = Tongue.Generation * 113 + int32(Tongue.Seed * 917.f);
    Tongue.GrowSeconds = .48f + .76f * Uniform(Salt + 1);
    // Avoid long identical tentacles: the mesh-wide mantle is the real source.
    Tongue.MaxLength = (26.f + 80.f * Uniform(Salt + 2)) * Tongue.Scale;
    Tongue.BaseWidth = (8.f + 18.f * Uniform(Salt + 3)) * Tongue.Scale;
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
    int32 DesiredCount = 5; // animated characters: 3–6
    if (Cast<USkeletalMeshComponent>(Source))
    {
        DesiredCount = 3 + FMath::RoundToInt(Uniform(731) * 3.f);
    }
    else if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Source))
    {
        const FVector Size = Static->Bounds.BoxExtent * 2.f;
        const float Area = 2.f * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
        const float MaxSide = Size.GetMax();
        DesiredCount = MaxSide < 140.f ? 4 + FMath::RoundToInt(Uniform(732))
            : MaxSide > 340.f ? 10 + FMath::RoundToInt(Uniform(733) * 5.f)
            : FMath::Clamp(FMath::RoundToInt(Area / 21000.f) + 4, 5, 10);
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
            for (int32 Slot=0; Slot<DesiredCount && Candidates.Num()>0; ++Slot)
            {
                int32 Best = INDEX_NONE;
                double BestScore = -TNumericLimits<double>::Max();
                for (int32 Id : Candidates)
                {
                    const FVector P(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Id));
                    double Distance = Tongues.IsEmpty()
                        ? -FVector::DistSquared(P, FVector::ZeroVector)
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
                Root.Scale = .65f + .38f * Uniform(Slot + 270);
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

        for (int32 I=0; I<DesiredCount*4 && Tongues.Num()<DesiredCount; ++I)
        {
            const float A = Uniform(I*3+201)*1.84f-.92f;
            const float B = Uniform(I*3+202)*1.84f-.92f;
            const int32 Face = I%5;
            FVector Normal = FVector::UpVector, Point=Bounds.Origin;
            if (Face==0) Point+=FVector(A*Half.X,B*Half.Y,Half.Z);
            if (Face==1) {Normal=FVector(1,0,0);Point+=FVector(Half.X,A*Half.Y,B*Half.Z);}
            if (Face==2) {Normal=FVector(-1,0,0);Point+=FVector(-Half.X,A*Half.Y,B*Half.Z);}
            if (Face==3) {Normal=FVector(0,1,0);Point+=FVector(A*Half.X,Half.Y,B*Half.Z);}
            if (Face==4) {Normal=FVector(0,-1,0);Point+=FVector(A*Half.X,-Half.Y,B*Half.Z);}
            const float Far=2.6f*Half.GetMax()+12.f;
            FHitResult Hit;
            FCollisionQueryParams Query;
            Query.bTraceComplex=true;
            const FTransform Transform=Static->GetComponentTransform();
            if (!Static->LineTraceComponent(Hit,
                Transform.TransformPosition(Point+Normal*18.f),
                Transform.TransformPosition(Point-Normal*Far),Query))
                continue; // No fabricated points when collision is missing.
            const FVector WorldHit(Hit.ImpactPoint.X,Hit.ImpactPoint.Y,Hit.ImpactPoint.Z);
            FTongue Root;
            Root.RestPosition = Transform.InverseTransformPosition(WorldHit);
            Root.Seed = Uniform(I+380)*103.f;
            Root.Scale = FMath::Clamp(Half.GetMax()/190.f,.45f,1.25f);
            Tongues.Add(Root);
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

int32 UASTFireFreeFlameComponent::EmitTip(
    FTongue& Tongue, const FVector& Base, float Length, const FVector& Flow)
{
    FDetached& Child=Detached[DetachCursor % MaxDetached];
    ++DetachCursor;
    const int32 Salt=DetachCursor*137+Tongue.Generation*91;
    Child.bAlive=true;
    Child.Age=0.f;
    Child.Lifetime=.52f + .68f*Uniform(Salt+3);
    Child.Length=Length*(.22f+.21f*Uniform(Salt+4));
    Child.Width=Tongue.BaseWidth*(.48f+.28f*Uniform(Salt+5));
    // Detached tip starts exactly where the parent ended.
    Child.Position=Base+FVector::UpVector*Length+Flow*.095f;
    Child.Velocity=FVector::UpVector*(65.f+60.f*Uniform(Salt+7))+
        Flow*.28f-Tongue.WorldVelocity*.12f;
    Child.Temperature=.70f + .19f*Uniform(Salt+8);
    Child.Seed=Uniform(Salt+9)*102.f;
    Child.Right=FVector::RightVector;
    return (DetachCursor-1)%MaxDetached;
}

void UASTFireFreeFlameComponent::AdvanceSimulation(float Dt)
{
    Elapsed += Dt;
    Spread = FMath::Clamp(Spread + Dt * (Heat > .01f ? .23f : 0.f), 0.f, 1.f);
    float SumEnergy = 0.f;
    for (FTongue& T : Tongues)
    {
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
                T.BridgedChild = EmitTip(T,Base,T.SplitLength,Flow);
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
        MaxDetached*RingsPerDetached)*2;
    Verts.Reserve(Vertices);
    UVs.Reserve(Vertices);
    Colors.Reserve(Vertices);

    FVector View=FVector::ForwardVector;
    if (const UWorld* World=GetWorld())
        if (const APlayerController* PC=World->GetFirstPlayerController())
            if (const APlayerCameraManager* Cam=PC->PlayerCameraManager)
                View=(Cam->GetCameraLocation()-GetOwner()->GetActorLocation()).GetSafeNormal();
    FVector Right=FVector::CrossProduct(FVector::UpVector,View).GetSafeNormal();
    if (Right.IsNearlyZero()) Right=FVector::RightVector;

    for (int32 I=0;I<Tongues.Num();++I)
    {
        const FTongue& T=Tongues[I];
        const FVector Base=SampleRoot(T);
        const float Fraction=FMath::Clamp(T.Age/FMath::Max(.1f,T.GrowSeconds),0.f,1.f);
        const float Growth=ASTFireLife::SmoothStep(Fraction);
        const float Length=T.CurrentLength;
        const float Width=T.CurrentWidth;
        const float ShapePhase=FMath::Lerp(T.PreviousPhase,T.Phase,Growth);
        const FVector Flow=(Wind-T.WorldVelocity*.64f).GetClampedToMaxSize(1600.f);
        TArray<FVector> C;
        TArray<float> W;
        TArray<float> H;
        for (int32 K=0;K<RingsPerTongue;++K)
        {
            const float V=float(K)/float(RingsPerTongue-1);
            const float Curl=FMath::Sin(V*8.1f+Elapsed*8.3f+ShapePhase)*
                V*V*(1.2f+5.f*T.Scale);
            const float Curl2=FMath::Sin(V*13.3f-Elapsed*5.1f+T.Seed)*
                V*(.9f+2.8f*T.Scale);
            C.Add(Base+FVector::UpVector*Length*V+
                Flow*V*V*.095f+Right*(Curl+Curl2));
            // One continuous silhouette. Vary each ring instead of stacking
            // identical camera-facing sprite/rectangle pieces.
            const float Ridge=FMath::Clamp(.80f+.20f*
                FMath::Sin(V*13.8f+ShapePhase+.57f*Elapsed),.38f,1.08f);
            // The root has ZERO planar width: its heat comes from the mesh
            // surface mantle, not a horizontal sprite glued to one vertex.
            const float RootBlend=ASTFireLife::SmoothStep(V/.20f);
            const float Tip=FMath::Pow(FMath::Max(0.f,1.f-V),.73f);
            W.Add(Width*RootBlend*Tip*Ridge);
            H.Add(FMath::Clamp(1.04f-.66f*V + .09f*
                FMath::Sin(V*8.2f+ShapePhase),.10f,1.f));
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

    for (const FDetached& D:Detached)
    {
        TArray<FVector> C;
        TArray<float> W;
        TArray<float> H;
        const float Age=D.bAlive?FMath::Clamp(D.Age/D.Lifetime,0.f,1.f):1.f;
        const float Fade=D.bAlive?1.f-Age:0.f;
        for (int32 K=0;K<RingsPerDetached;++K)
        {
            const float V=float(K)/float(RingsPerDetached-1);
            const float Wiggle=FMath::Sin(V*11.0f+Elapsed*8.1f+D.Seed)*
                2.3f*Fade;
            C.Add(D.Position+FVector::UpVector*D.Length*V*Fade+
                Right*Wiggle);
            W.Add(D.Width*Fade*FMath::Sin(PI*(.13f+.84f*V)));
            H.Add(D.Temperature*(1.f-.46f*V)*
                FMath::Pow(Fade,1.25f));
        }
        AddStrip(C,W,H,D.Seed,1.f,Right,Verts,UVs,Colors);
    }

    TArray<int32> Triangles;
    const int32 StripCount=Tongues.Num()*2+MaxDetached;
    Triangles.Reserve((Tongues.Num()*(RingsPerTongue+BridgeRings-2)+
        MaxDetached*(RingsPerDetached-1))*6);
    int32 Offset=0;
    for (int32 I=0;I<StripCount;++I)
    {
        const int32 N=I<Tongues.Num()*2
            ? (I%2==0?RingsPerTongue:BridgeRings)
            : RingsPerDetached;
        for (int32 K=0;K<N-1;++K)
        {
            const int32 A=Offset+2*K;
            Triangles.Add(A);Triangles.Add(A+1);Triangles.Add(A+2);
            Triangles.Add(A+1);Triangles.Add(A+3);Triangles.Add(A+2);
        }
        Offset+=N*2;
    }
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    Normals.Init(FVector::ForwardVector,Verts.Num());
    Tangents.Init(FProcMeshTangent(1,0,0),Verts.Num());
    if (bCreate)
        Render->CreateMeshSection_LinearColor(0,Verts,Triangles,Normals,UVs,
            Colors,Tangents,false);
    else
        Render->UpdateMeshSection_LinearColor(0,Verts,Normals,UVs,Colors,Tangents);

    // Render one continuous source (section 1) in the exact tongue material.
    // It is NOT a WPO shell made from the original low-poly triangles.
    UpdateConnectedSource(bCreate, View, Right);
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
    Samples.Reserve(MaxSourceAnchors+Tongues.Num());
    float HighestDepth=-BIG_NUMBER;
    float MinX=BIG_NUMBER,MaxX=-BIG_NUMBER,MinY=BIG_NUMBER,MaxY=-BIG_NUMBER;

    auto Include=[&](const FVector& World)
    {
        const FVector Diff=World-Origin;
        const FVector2D P(
            FVector::DotProduct(Diff,Right),
            FVector::DotProduct(Diff,Up));
        Samples.Add(P);
        MinX=FMath::Min(MinX,float(P.X));
        MaxX=FMath::Max(MaxX,float(P.X));
        MinY=FMath::Min(MinY,float(P.Y));
        MaxY=FMath::Max(MaxY,float(P.Y));
        HighestDepth=FMath::Max(HighestDepth,
            float(FVector::DotProduct(Diff,Normal)));
    };

    for (const FSourceAnchor& A:SourceAnchors)
    {
        // Progressive ignition: surface samples become active as one
        // cluster expands. Existing burning source never restarts.
        if (Spread<.95f && !BurnClusters.IsEmpty())
        {
            bool bCovered=false;
            for (const FBurnCluster& C:BurnClusters)
                if (FVector::DistSquared(A.RestPosition,C.RestCenter)<
                    FMath::Square(C.Radius*(.30f+Spread*1.35f)))
                {bCovered=true;break;}
            if (!bCovered) continue;
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
            Include(SampleRoot(T));

    if(Samples.IsEmpty())
        Include(Origin);

    const float Extent=FMath::Max(20.f,FMath::Max(MaxX-MinX,MaxY-MinY));
    const float Radius=FMath::Clamp(
        Extent/(FMath::Sqrt(float(Samples.Num()))*1.5f),10.f,26.f);
    const float Outer=FMath::Clamp(Radius*.85f,9.f,18.f);
    const float Inner=FMath::Clamp(Radius*.65f,5.f,12.f);
    const float Margin=Radius+Outer+12.f;
    MinX-=Margin;MaxX+=Margin;MinY-=Margin;MaxY+=Margin;
    HighestDepth+=3.f;

    // Prim MST provides a connected supporting source, including long limbs
    // and compact fire without a physical mesh. Links are thickened small
    // capsules, not stand-alone flame cards/independent "tentacles".
    const int32 N=Samples.Num();
    TArray<int32> Parent;
    Parent.Init(INDEX_NONE,N);
    TArray<uint8> Added;
    Added.Init(0,N);
    TArray<float> Nearest;
    Nearest.Init(BIG_NUMBER,N);
    Nearest[0]=0.f;
    for(int32 Iter=0;Iter<N;++Iter)
    {
        int32 Best=INDEX_NONE;
        float Score=BIG_NUMBER;
        for(int32 J=0;J<N;++J)
            if(!Added[J] && Nearest[J]<Score)
            {Best=J;Score=Nearest[J];}
        if(Best==INDEX_NONE)break;
        Added[Best]=1;
        for(int32 J=0;J<N;++J)
            if(!Added[J])
            {
                const float D=FVector2D::DistSquared(
                    Samples[Best],Samples[J]);
                if(D<Nearest[J])
                {Nearest[J]=D;Parent[J]=Best;}
            }
    }

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
            float PointD2=BIG_NUMBER,EdgeD2=BIG_NUMBER;
            for(int32 I=0;I<N;++I)
            {
                PointD2=FMath::Min(PointD2,
                    float(FVector2D::DistSquared(P,Samples[I])));
                const int32 J=Parent[I];
                if(J==INDEX_NONE)continue;
                const FVector2D A=Samples[I],B=Samples[J];
                const FVector2D AB=B-A;
                const float T=FMath::Clamp(
                    FVector2D::DotProduct(P-A,AB)/
                    FMath::Max(1.f,float(AB.SizeSquared())),0.f,1.f);
                EdgeD2=FMath::Min(EdgeD2,
                    float((P-(A+AB*T)).SizeSquared()));
            }
            const float Distance=FMath::Min(
                FMath::Sqrt(PointD2)-Radius,
                FMath::Sqrt(EdgeD2)-Radius*.68f);
            // Small bounded flicker: never a 30cm WPO polygon extrusion.
            const float Curl=FMath::Sin(X*.091f+Elapsed*6.6f+Seed*.023f)*
                (2.3f+2.5f*Heat)+
                FMath::Sin(Y*.123f-X*.054f-Elapsed*9.4f)*1.8f;
            const float Dist=Distance-Curl;
            const float Near=FMath::Clamp((Dist+Inner)/Inner,0.f,1.f);
            const float Far=FMath::Clamp(1.f-Dist/Outer,0.f,1.f);
            const float Thermal=FMath::Clamp(
                Heat*Near*Far*(.91f+.08f*FMath::Sin(Elapsed*7.f+
                X*.061f+Y*.029f)),0.f,1.f);

            const FVector World=Origin+Right*X+Up*Y+
                Normal*HighestDepth;
            Verts.Add(RenderTransform.InverseTransformPosition(World));
            // UV.x=.5 gives the same bright center as tongue ribbons;
            // UV.y adds the exact same 16-bit stepped palette variation.
            UVs.Add(FVector2D(.5f,float(Row)/float(SourceGridY)));
            Colors.Add(FLinearColor(Thermal,.12f,FMath::Frac(
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
    if (!bVisible || Heat<.005f)
    {
        // no ticking geometry off-screen or when extinguished
        VisualEnergy=FMath::FInterpTo(VisualEnergy,0.f,Dt,6.f);
        return;
    }
    AdvanceSimulation(FMath::Min(Dt,.08f));
    UpdateGeometry(false);
}
