#include "ASTFireFreeFlameComponent.h"

#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Materials/MaterialInterface.h"

UASTFireFreeFlameComponent::UASTFireFreeFlameComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.04f;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
    Embers.SetNum(MaxEmbers);
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
    Energy = FMath::Clamp(InEnergy, 0.02f, 1.25f);
}

void UASTFireFreeFlameComponent::SetFlamesVisible(bool bVisible)
{
    if (IsValid(Render)) Render->SetVisibility(bVisible);
}

void UASTFireFreeFlameComponent::OnRegister()
{
    Super::OnRegister();
    if (IsTemplate() || !GetOwner() || !IsValid(Source) || !IsValid(Material))
        return;
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
    Render->RegisterComponent();
    BuildAnchors();
    UpdateGeometry(1.f / 25.f, true);
    bGenerated = true;
}

void UASTFireFreeFlameComponent::OnUnregister()
{
    if (IsValid(Render))
        Render->DestroyComponent();
    Render = nullptr;
    Roots.Reset();
    bGenerated = false;
    Super::OnUnregister();
}

void UASTFireFreeFlameComponent::BuildAnchors()
{
    Roots.Reset();
    if (!IsValid(Source)) return;

    if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Source))
    {
        USkeletalMesh* Asset = Skeletal->GetSkeletalMeshAsset();
        FSkeletalMeshRenderData* Data = Asset ? Asset->GetResourceForRendering() : nullptr;
        if (Data && Data->LODRenderData.Num() > 0)
        {
            const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[0];
            const int32 TotalVertices = LOD.GetNumVertices();
            // Sample 192 candidates from *rendered* vertices. Farthest point
            // selection distributes burn origins across actual skinned skin.
            TArray<int32> Candidates;
            const int32 SampleCount = FMath::Min(192, TotalVertices);
            for (int32 I = 0; I < SampleCount; ++I)
                Candidates.Add(FMath::Clamp(
                    (I * TotalVertices + TotalVertices / 3) /
                    FMath::Max(1, SampleCount), 0, TotalVertices - 1));

            for (int32 Slot = 0; Slot < MaxRoots && Candidates.Num() > 0; ++Slot)
            {
                int32 BestIndex = INDEX_NONE;
                double BestDistance = -1.0;
                for (int32 Index : Candidates)
                {
                    const FVector Candidate(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Index));
                    double MinDistance = Roots.IsEmpty()
                        ? FVector::DistSquared(Candidate, FVector(0, 0, 0))
                        : TNumericLimits<double>::Max();
                    for (const FRoot& Existing : Roots)
                        MinDistance = FMath::Min(MinDistance,
                            FVector::DistSquared(Candidate, Existing.LocalPosition));
                    if (MinDistance > BestDistance)
                    {
                        BestDistance = MinDistance;
                        BestIndex = Index;
                    }
                }
                if (BestIndex == INDEX_NONE) break;
                FRoot Root;
                Root.SkinnedVertexIndex = BestIndex;
                Root.LocalPosition = FVector(
                    LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(BestIndex));
                Root.Seed = float((Seed + Slot * 7919) % 1009) * .0137f;
                Root.Scale = 0.55f + 0.35f *
                    FMath::Frac(float(Slot) * 0.6180339f);
                Roots.Add(Root);
                Candidates.Remove(BestIndex);
            }
            UE_LOG(LogTemp, Log,
                TEXT("AST Fire Stage9: %s actual skinned mesh vertex anchors %d"),
                *GetOwner()->GetName(), Roots.Num());
        }
    }
    else if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Source))
    {
        const FBoxSphereBounds Box = Static->CalcBounds(FTransform::Identity);
        const FVector Half = Box.BoxExtent.GetAbs();
        const FVector Center = Box.Origin;
        for (int32 I = 0; I < MaxRoots; ++I)
        {
            const double A = FMath::Frac(float(I) * 0.618034f + 0.18f) * 1.74 - 0.87;
            const double B = FMath::Frac(float(I) * 0.754878f + 0.32f) * 1.74 - 0.87;
            const int32 Face = I % 5;
            FVector Start = Center, Direction = FVector::UpVector;
            if (Face == 0) { Start += FVector(A * Half.X, B * Half.Y, Half.Z); }
            if (Face == 1) { Direction = FVector(1,0,0); Start += FVector(Half.X, A * Half.Y, B * Half.Z); }
            if (Face == 2) { Direction = FVector(-1,0,0); Start += FVector(-Half.X, A * Half.Y, B * Half.Z); }
            if (Face == 3) { Direction = FVector(0,1,0); Start += FVector(A * Half.X, Half.Y, B * Half.Z); }
            if (Face == 4) { Direction = FVector(0,-1,0); Start += FVector(A * Half.X, -Half.Y, B * Half.Z); }
            FVector LocalStart = Start + Direction * 18.f;
            FVector LocalEnd = Start - Direction * (2.5f * Half.GetMax() + 15.f);
            FHitResult Hit;
            FCollisionQueryParams Params;
            Params.bTraceComplex = true;
            if (!Static->LineTraceComponent(
                Hit, Static->GetComponentTransform().TransformPosition(LocalStart),
                Static->GetComponentTransform().TransformPosition(LocalEnd), Params))
                continue; // No fabricated floating points inside empty bounds.
            FRoot Root;
            Root.LocalPosition = Static->GetComponentTransform().InverseTransformPosition(
                FVector(Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z) + FVector(Hit.ImpactNormal.X,Hit.ImpactNormal.Y,Hit.ImpactNormal.Z)*1.5f);
            Root.Seed = float((Seed + I * 7919) % 1009) * .0137f;
            const float MaxExtent = FMath::Max3(Half.X, Half.Y, Half.Z);
            Root.Scale = FMath::Clamp(MaxExtent / 180.f, .34f, 1.3f);
            Roots.Add(Root);
        }
    }

    if (Roots.IsEmpty() && !Cast<UMeshComponent>(Source))
    {
        // A standalone BP_AST_FireSource has no burnable render mesh.
        // Emit from the real source transform without a bounding rectangle.
        for (int32 I = 0; I < 8; ++I)
        {
            const float Angle = float(I) * 2.399963f;
            FRoot Root;
            Root.LocalPosition = FVector(FMath::Cos(Angle) * 16.f,
                FMath::Sin(Angle) * 16.f, 0.f);
            Root.Seed = float((Seed + I * 7919) % 1009) * .0137f;
            Root.Scale = 0.8f;
            Roots.Add(Root);
        }
    }

    for (FRoot& Root : Roots)
        Root.LastPosition = SampleRoot(Root);
}

FVector UASTFireFreeFlameComponent::SampleRoot(const FRoot& Root) const
{
    if (!IsValid(Source)) return FVector::ZeroVector;
    if (Root.SkinnedVertexIndex != INDEX_NONE)
    {
        if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Source))
        {
            USkeletalMesh* Asset = Skeletal->GetSkeletalMeshAsset();
            FSkeletalMeshRenderData* Data = Asset ? Asset->GetResourceForRendering() : nullptr;
            if (Data && Data->LODRenderData.Num() > 0)
            {
                const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[0];
                if (Root.SkinnedVertexIndex < int32(LOD.GetNumVertices()))
                {
                    FSkinWeightVertexBuffer* Weights =
                        const_cast<FSkeletalMeshLODRenderData&>(LOD).GetSkinWeightVertexBuffer();
                    if (Weights)
                    {
                        const FVector3f P = USkinnedMeshComponent::GetSkinnedVertexPosition(
                            Skeletal, Root.SkinnedVertexIndex, LOD, *Weights);
                        return Skeletal->GetComponentTransform().TransformPosition(FVector(P));
                    }
                }
            }
        }
    }
    return Source->GetComponentTransform().TransformPosition(Root.LocalPosition);
}

void UASTFireFreeFlameComponent::AddBillboard(
    const FVector& Center, const FVector& Right, const FVector& Up,
    float HalfWidth, float HalfHeight, float Temperature, float Y,
    TArray<FVector>& Verts, TArray<FVector2D>& UVs,
    TArray<FLinearColor>& Colors) const
{
    const FTransform Xform = IsValid(Render) ? Render->GetComponentTransform() : FTransform::Identity;
    const FVector Quad[] = {
        Center - Right * HalfWidth - Up * HalfHeight,
        Center + Right * HalfWidth - Up * HalfHeight,
        Center + Right * HalfWidth + Up * HalfHeight,
        Center - Right * HalfWidth + Up * HalfHeight
    };
    for (int32 K = 0; K < 4; ++K)
    {
        Verts.Add(Xform.InverseTransformPosition(Quad[K]));
        UVs.Add(FVector2D(K == 1 || K == 2 ? 1.f : 0.f,
            K >= 2 ? 1.f : 0.f));
        Colors.Add(FLinearColor(Temperature,
            FMath::Clamp(Y, 0.f, 1.f), 0.f, 1.f));
    }
}

void UASTFireFreeFlameComponent::UpdateGeometry(float DeltaTime, bool bCreate)
{
    if (!IsValid(Render)) return;
    Time += DeltaTime;
    TArray<FVector> Verts;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;
    TArray<int32> Triangles;
    const int32 Quads = Roots.Num() * Segments + MaxEmbers;
    Verts.Reserve(Quads * 4);
    UVs.Reserve(Quads * 4);
    Colors.Reserve(Quads * 4);
    Triangles.Reserve(Quads * 6);

    FVector View = FVector::ForwardVector;
    if (UWorld* World = GetWorld())
        if (const APlayerController* PC = World->GetFirstPlayerController())
            if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
                View = (Cam->GetCameraLocation() - GetOwner()->GetActorLocation()).GetSafeNormal();
    FVector Right = FVector::CrossProduct(FVector::UpVector, View).GetSafeNormal();
    if (Right.IsNearlyZero()) Right = FVector::RightVector;

    float AliveEnergy = 0.f;
    for (int32 I=0; I<Roots.Num(); ++I)
    {
        FRoot& Root = Roots[I];
        const FVector Base = SampleRoot(Root); // follows skinned vertex, not bone/root
        const FVector Move = (Base - Root.LastPosition) / FMath::Max(.004f,DeltaTime);
        Root.LastPosition = Base;
        Root.LastVelocity = FMath::VInterpTo(Root.LastVelocity, Move, DeltaTime, 13.f);

        const float Duration = .38f + .21f * FMath::Frac(Root.Seed * 3.37f);
        const float Cycle = FMath::Frac(Time / Duration + Root.Seed);
        // A flame tongue is born near its real surface vertex, grows,
        // stretches with buoyancy, tears and dies rather than oscillating a
        // permanent, rigid red rectangle or a fixed bounded field.
        const float Life = FMath::Clamp(
            FMath::Sin(Cycle * PI) * 1.25f, 0.f, 1.f);
        const float Width = (4.5f + 7.0f * Root.Scale) * Life;
        const float Length = (22.f + 65.f * Root.Scale) *
            (0.24f + .76f * Life) * (0.65f + 0.35f * Energy);
        const FVector Airflow = (Wind - Root.LastVelocity * .62f)
            .GetClampedToMaxSize(1800.f);

        for (int32 S=0; S<Segments; ++S)
        {
            const float T = (float(S)+.5f) / Segments;
            const float Age = T * .22f;
            const float Ripple = FMath::Sin(T*9.2f + Time * 13.5f + Root.Seed * 17.f) *
                T * (2.5f + 6.5f * Root.Scale);
            const FVector Center = Base + FVector::UpVector * (Length * T) +
                Airflow * (Age * T * .36f) + Right * Ripple;
            const float Tip = FMath::Pow(FMath::Max(0.f,1.f - T*T), 1.2f);
            const float W = FMath::Max(0.f, Width * Tip);
            const float Height = Length / float(Segments) * .64f;
            const float Hot = FMath::Clamp(Heat * Life *
                (1.1f - .40f*T) * (0.95f + 0.10f *
                FMath::Sin(Time * 18.f + Root.Seed * 22.f + T*6.f)),0.f,1.f);
            AddBillboard(Center,Right,FVector::UpVector,W,Height,Hot,T,
                Verts,UVs,Colors);
            AliveEnergy += Hot;
        }
    }

    EmberDebt += DeltaTime * Heat * (3.5f + Roots.Num()*.22f);
    for (FEmber& Ember : Embers)
    {
        if (Ember.Life > 0)
        {
            Ember.Life -= DeltaTime;
            Ember.Velocity += (FVector::UpVector * 175.f + Wind * .09f) * DeltaTime;
            Ember.Position += Ember.Velocity * DeltaTime; // WORLD-SPACE: no cage
        }
    }
    while (EmberDebt > 1.f && !Roots.IsEmpty())
    {
        EmberDebt -= 1.f;
        for (int32 K=0; K<MaxEmbers; ++K)
        {
            FEmber& Ember=Embers[K];
            if (Ember.Life > 0.f) continue;
            const int32 Pick = (Seed + int32(Time*43.f) + K*7) % Roots.Num();
            Ember.Position=SampleRoot(Roots[Pick]) +
                FVector::UpVector * (28.f+Roots[Pick].Scale*34.f);
            Ember.Velocity = Wind*.10f + FVector(
                FMath::Sin(Time*8.f+K)*15.f, FMath::Cos(Time*5.f+K)*15.f,135.f);
            Ember.MaximumLife=.32f+.22f*FMath::Frac(Roots[Pick].Seed*9.1f);
            Ember.Life=Ember.MaximumLife;
            Ember.Temperature=.61f+.28f*FMath::Frac(Roots[Pick].Seed*11.f);
            break;
        }
    }
    for (int32 K=0; K<MaxEmbers; ++K)
    {
        const FEmber& E=Embers[K];
        const float Fade=FMath::Clamp(E.Life/FMath::Max(.05f,E.MaximumLife),0.f,1.f);
        AddBillboard(E.Position,Right,FVector::UpVector,
            2.1f*Fade,2.1f*Fade,E.Temperature*Fade,1.f,
            Verts,UVs,Colors);
    }
    for (int32 I=0;I<Quads;++I)
    {
        const int32 Start=I*4;
        Triangles.Add(Start);Triangles.Add(Start+1);Triangles.Add(Start+2);
        Triangles.Add(Start);Triangles.Add(Start+2);Triangles.Add(Start+3);
    }
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    Normals.Init(FVector::ForwardVector,Verts.Num());
    Tangents.Init(FProcMeshTangent(1.f,0.f,0.f),Verts.Num());
    if (bCreate)
        Render->CreateMeshSection_LinearColor(0,Verts,Triangles,Normals,UVs,Colors,Tangents,false);
    else
        Render->UpdateMeshSection_LinearColor(0,Verts,Normals,UVs,Colors,Tangents);

    VisualEnergy = FMath::FInterpTo(VisualEnergy,
        Roots.IsEmpty() ? 0.f : FMath::Clamp(AliveEnergy /
        (float(Roots.Num()*Segments)*.61f), .08f,1.2f),DeltaTime,9.f);
}

void UASTFireFreeFlameComponent::TickComponent(
    float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime,TickType,ThisTickFunction);
    if (!bGenerated || !IsValid(Render) || DeltaTime <= 0.f) return;
    UpdateGeometry(DeltaTime,false);
}
