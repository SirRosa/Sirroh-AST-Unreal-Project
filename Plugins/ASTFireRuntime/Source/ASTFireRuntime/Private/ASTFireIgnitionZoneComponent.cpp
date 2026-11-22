#include "ASTFireIgnitionZoneComponent.h"

#include "ASTFireBlueprintLibrary.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"

UASTFireIgnitionZoneComponent::UASTFireIgnitionZoneComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetMobility(EComponentMobility::Movable);
    SetBoxExtent(FVector(75.0f, 75.0f, 84.0f), false);
    SetRelativeLocation(FVector(0.0f, 0.0f, 68.0f));
    SetCollisionObjectType(ECC_WorldDynamic);
    SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    SetCollisionResponseToAllChannels(ECR_Overlap);
    SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
    SetGenerateOverlapEvents(true);
    SetHiddenInGame(true);
}

void UASTFireIgnitionZoneComponent::BeginPlay()
{
    Super::BeginPlay();
    OnComponentBeginOverlap.AddDynamic(
        this, &UASTFireIgnitionZoneComponent::HandleBeginOverlap);
    OnComponentEndOverlap.AddDynamic(
        this, &UASTFireIgnitionZoneComponent::HandleEndOverlap);

    if (GetOwner() && GetOwner()->GetNetMode() != NM_Client && bIgniteInitialOverlaps)
    {
        TArray<AActor*> AlreadyOverlapping;
        GetOverlappingActors(AlreadyOverlapping);
        for (AActor* Actor : AlreadyOverlapping)
        {
            if (IsValid(Actor))
                IgniteTarget(Actor, Actor->GetActorLocation());
        }
    }
}

void UASTFireIgnitionZoneComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    OnComponentBeginOverlap.RemoveDynamic(
        this, &UASTFireIgnitionZoneComponent::HandleBeginOverlap);
    OnComponentEndOverlap.RemoveDynamic(
        this, &UASTFireIgnitionZoneComponent::HandleEndOverlap);
    IgnitedDuringOverlap.Reset();
    Super::EndPlay(EndPlayReason);
}

void UASTFireIgnitionZoneComponent::HandleBeginOverlap(
    UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex,
    bool bFromSweep, const FHitResult& SweepResult)
{
    if (!IsValid(OtherActor) || !IsValid(OtherComponent)) return;
    FVector Point = OtherComponent->Bounds.Origin;
    if (bFromSweep && !SweepResult.ImpactPoint.IsNearlyZero())
    {
        // UE 5.8 HitResult stores a quantized vector; do not use a ternary
        // with a large-world-coordinate FVector (incompatible scalar types).
        Point = FVector(SweepResult.ImpactPoint.X,
            SweepResult.ImpactPoint.Y, SweepResult.ImpactPoint.Z);
    }
    IgniteTarget(OtherActor, Point);
}

void UASTFireIgnitionZoneComponent::HandleEndOverlap(
    UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex)
{
    if (!IsValid(OtherActor)) return;
    // Multi-component actors can still overlap with other body parts.
    TArray<AActor*> StillTouching;
    GetOverlappingActors(StillTouching);
    if (!StillTouching.Contains(OtherActor))
        IgnitedDuringOverlap.Remove(TWeakObjectPtr<AActor>(OtherActor));
}

void UASTFireIgnitionZoneComponent::IgniteTarget(
    AActor* Target, const FVector& ContactPosition)
{
    AActor* Owner = GetOwner();
    if (!IsValid(Owner) || !IsValid(Target) || Target == Owner) return;
    if (Owner->GetNetMode() == NM_Client) return; // authoritative ignition
    if (!bIgniteOtherFireSources && Target->GetClass()->GetName().StartsWith(
        TEXT("BP_AST_FireSource"))) return;
    if (bRequireSourceBurning && !UASTFireBlueprintLibrary::IsActorBurning(Owner))
        return;

    const TWeakObjectPtr<AActor> WeakTarget(Target);
    if (IgnitedDuringOverlap.Contains(WeakTarget)) return;
    if (UASTFireBlueprintLibrary::IsActorBurning(Target))
    {
        IgnitedDuringOverlap.Add(WeakTarget);
        return;
    }

    // Use bounds for small characters, medium props and large meshes; avoid
    // a fixed flame count / size on objects with very different dimensions.
    const FVector BoxSize = Target->GetComponentsBoundingBox(true).GetSize();
    const float LongestDimension = BoxSize.GetMax();
    if (LongestDimension <= KINDA_SMALL_NUMBER) return;
    int32 Points = 1;
    if (LongestDimension >= 135.0f) Points = 2;
    if (LongestDimension >= 240.0f) Points = 4;
    if (LongestDimension >= 500.0f) Points = 6;
    if (LongestDimension >= 950.0f) Points = 8;
    Points = FMath::Clamp(Points, 1, FMath::Clamp(MaxDistributedFlames, 1, 12));

    if (UASTFireBlueprintLibrary::IgniteActor(
        Target, ContactPosition, IgnitionStrength, Points))
    {
        IgnitedDuringOverlap.Add(WeakTarget);
        UE_LOG(LogTemp, Log,
            TEXT("AST Fire: contact ignited %s from %s (%d flame points)"),
            *Target->GetName(), *Owner->GetName(), Points);
    }
}
