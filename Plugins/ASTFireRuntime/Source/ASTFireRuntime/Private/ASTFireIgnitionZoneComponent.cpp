#include "ASTFireIgnitionZoneComponent.h"

#include "ASTFireBlueprintLibrary.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"

UASTFireIgnitionZoneComponent::UASTFireIgnitionZoneComponent()
{
    // IMPORTANT: do not call any UPrimitive/Scene setters here. UE 5.8
    // triggers NewObject while building the component CDO and fatally aborts.
    PrimaryComponentTick.bCanEverTick = false;
}

void UASTFireIgnitionZoneComponent::BeginPlay()
{
    Super::BeginPlay();
    OnComponentBeginOverlap.AddDynamic(
        this, &UASTFireIgnitionZoneComponent::HandleBeginOverlap);
    OnComponentEndOverlap.AddDynamic(
        this, &UASTFireIgnitionZoneComponent::HandleEndOverlap);

    // Initialize physical overlap only on a registered runtime component.
    // Disable collision first, configure the extent and profile, then enable.
    SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetMobility(EComponentMobility::Movable);
    SetBoxExtent(TriggerHalfExtent, false);
    SetRelativeLocation(TriggerOffset);
    SetCollisionObjectType(ECC_WorldDynamic);
    SetCollisionResponseToAllChannels(ECR_Overlap);
    SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
    SetGenerateOverlapEvents(true);
    SetHiddenInGame(true);
    SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    UpdateOverlaps();
    // Native notifications require GenerateOverlapEvents on BOTH components.
    // Carry props deliberately disable them; query actual collision shapes instead.
    if(GetOwner() && GetOwner()->HasAuthority() && GetWorld())
        GetWorld()->GetTimerManager().SetTimer(ContactPollTimer,this,
            &UASTFireIgnitionZoneComponent::PollContactOverlaps,.05f,true);

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
    if(GetWorld())GetWorld()->GetTimerManager().ClearTimer(ContactPollTimer);
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
    // Poll reconciles native and explicit contacts together: an end event
    // from one primitive must not reset an actor whose other body still touches.
}

void UASTFireIgnitionZoneComponent::PollContactOverlaps()
{
    AActor* Owner=GetOwner();
    UWorld* World=GetWorld();
    if(!IsValid(Owner)||!Owner->HasAuthority()||!World)return;
    TSet<TWeakObjectPtr<AActor>> Touching;
    TArray<AActor*> Native;
    GetOverlappingActors(Native);
    for(AActor* Actor:Native)
        if(IsValid(Actor)&&Actor!=Owner)Touching.Add(Actor);

    FCollisionQueryParams Query(SCENE_QUERY_STAT(ASTFireContact),false,Owner);
    const FCollisionObjectQueryParams Objects(FCollisionObjectQueryParams::AllDynamicObjects);
    TArray<FOverlapResult> Hits;
    World->OverlapMultiByObjectType(Hits,GetComponentLocation(),GetComponentQuat(),
        Objects,FCollisionShape::MakeBox(GetScaledBoxExtent()),Query);
    for(const FOverlapResult& Hit:Hits)
    {
        AActor* Actor=Hit.GetActor();
        UPrimitiveComponent* Body=Hit.GetComponent();
        if(!IsValid(Actor)||Actor==Owner||!IsValid(Body))continue;
        // Exclude generated FX, trigger volumes and static level architecture.
        // Pawns can be represented by their capsule, even with hidden skin.
        const bool bPawn=Actor->IsA<APawn>();
        const bool bMesh=Body->IsA<UStaticMeshComponent>()||Body->IsA<USkinnedMeshComponent>();
        const bool bPhysical=Body->IsSimulatingPhysics()||
            (bMesh&&Body->Mobility==EComponentMobility::Movable&&Body->IsVisible()&&!Body->bHiddenInGame);
        if(!bPawn&&!bPhysical)continue;
        const TWeakObjectPtr<AActor> Key(Actor);
        Touching.Add(Key);
        FVector Contact=Body->Bounds.Origin;
        FVector Closest;
        if(Body->GetClosestPointOnCollision(GetComponentLocation(),Closest)>=0.f)
            Contact=Closest;
        IgniteTarget(Actor,Contact);
    }
    // Also retry native contacts if the source was not lit on entry.
    for(AActor* Actor:Native)
        if(IsValid(Actor))IgniteTarget(Actor,Actor->GetActorLocation());
    for(auto It=IgnitedDuringOverlap.CreateIterator();It;++It)
        if(!It->IsValid()||!Touching.Contains(*It))It.RemoveCurrent();
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
    if (LongestDimension >= 70.0f) Points = 2;
    if (LongestDimension >= 140.0f) Points = 4;
    if (LongestDimension >= 220.0f) Points = 6;
    if (LongestDimension >= 500.0f) Points = 8;
    if (LongestDimension >= 950.0f) Points = 12;
    if (Target->FindComponentByClass<USkinnedMeshComponent>())
        Points = FMath::Max(Points, 7);
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
