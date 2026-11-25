#include "ASTFireIgnitionZoneComponent.h"

#include "ASTFireBlueprintLibrary.h"
#include "ASTFireReactiveComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"

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
    if(GetOwner() && GetOwner()->HasAuthority())
    {
        bPrimingContacts=!bIgniteInitialOverlaps;
        PollContactOverlaps();
        bPrimingContacts=false;
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

    const FVector ZoneCenter=GetComponentLocation();
    const FQuat ZoneRotation=GetComponentQuat();
    const FVector ZoneExtent=GetScaledBoxExtent();
    const FCollisionShape Shape=FCollisionShape::MakeBox(ZoneExtent);
    auto RecordContact=[&](AActor* Actor,UPrimitiveComponent* Body)
    {
        const TWeakObjectPtr<AActor> Key(Actor);
        Touching.Add(Key);
        if(bPrimingContacts){IgnitedDuringOverlap.Add(Key);return;}
        if(IgnitedDuringOverlap.Contains(Key))return;
        FVector Contact=Body->Bounds.GetBox().GetClosestPointTo(ZoneCenter);
        FVector Closest;
        if(Body->GetCollisionEnabled()!=ECollisionEnabled::NoCollision &&
            Body->GetClosestPointOnCollision(ZoneCenter,Closest)>0.f)
            Contact=Closest;
        IgniteTarget(Actor,Contact);
    };
    FCollisionQueryParams Query(SCENE_QUERY_STAT(ASTFireContact),false,Owner);
    // Include custom physics object channels; reject static architecture below.
    const FCollisionObjectQueryParams Objects(FCollisionObjectQueryParams::AllObjects);
    TArray<FOverlapResult> Hits;
    World->OverlapMultiByObjectType(Hits,ZoneCenter,ZoneRotation,Objects,Shape,Query);
    for(const FOverlapResult& Hit:Hits)
    {
        AActor* Actor=Hit.GetActor();
        UPrimitiveComponent* Body=Hit.GetComponent();
        if(!IsValid(Actor)||Actor==Owner||!IsValid(Body))continue;
        const bool bPawn=Actor->IsA<APawn>();
        const bool bMesh=Body->IsA<UStaticMeshComponent>()||Body->IsA<USkinnedMeshComponent>();
        const bool bPhysical=Body->IsSimulatingPhysics()||
            (bMesh&&Body->GetMobility()==EComponentMobility::Movable&&Body->IsVisible()&&!Body->bHiddenInGame);
        if(!bPawn&&!bPhysical)continue;
        RecordContact(Actor,Body);
    }

    // AST carry props deliberately disable overlap notifications, and while
    // held they disable collision altogether. A world scene query cannot
    // reliably discover such bodies. Iterate only the project's carry-prop
    // class (never all actors), and use its actual rotating visible mesh.
    if(UClass* CarryClass=FindObject<UClass>(nullptr,TEXT("/Script/Sirroh_Reimagineg_v3.SirrohCarryProp")))
    {
        for(TActorIterator<AActor> It(World,CarryClass);It;++It)
        {
            AActor* Actor=*It;
            if(!IsValid(Actor)||Actor==Owner||Actor->IsHidden()||Touching.Contains(Actor))continue;
            auto* Body=Cast<UStaticMeshComponent>(Actor->GetRootComponent());
            if(!IsValid(Body)||!Body->IsRegistered()||!Body->IsVisible()||Body->bHiddenInGame||!Body->GetStaticMesh())continue;
            const FBox Local=Body->GetStaticMesh()->GetBoundingBox();
            const FTransform& Transform=Body->GetComponentTransform();
            const FVector Center=Transform.TransformPosition(Local.GetCenter());
            const FVector Extent=Local.GetExtent()*Transform.GetScale3D().GetAbs();
            // Cheap broad phase before the 15 separating-axis tests.
            if(FVector::DistSquared(Center,ZoneCenter)>FMath::Square(Extent.Size()+ZoneExtent.Size()))continue;
            const FQuat Rotation=Transform.GetRotation();
            const FVector A[3]={ZoneRotation.GetAxisX(),ZoneRotation.GetAxisY(),ZoneRotation.GetAxisZ()};
            const FVector B[3]={Rotation.GetAxisX(),Rotation.GetAxisY(),Rotation.GetAxisZ()};
            const FVector Delta=Center-ZoneCenter;
            auto Separated=[&](const FVector& Axis)
            {
                if(Axis.SizeSquared()<1.e-10)return false;
                double RA=0.,RB=0.;
                for(int32 I=0;I<3;++I)
                {
                    RA+=ZoneExtent[I]*FMath::Abs(FVector::DotProduct(A[I],Axis));
                    RB+=Extent[I]*FMath::Abs(FVector::DotProduct(B[I],Axis));
                }
                return FMath::Abs(FVector::DotProduct(Delta,Axis))>RA+RB+.001;
            };
            bool bSeparated=false;
            for(int32 I=0;I<3&&!bSeparated;++I)
                bSeparated=Separated(A[I])||Separated(B[I]);
            for(int32 I=0;I<3&&!bSeparated;++I)
                for(int32 J=0;J<3&&!bSeparated;++J)
                    bSeparated=Separated(FVector::CrossProduct(A[I],B[J]));
            if(!bSeparated)RecordContact(Actor,Body);
        }
    }
    // Also retry native contacts if the source was not lit on entry.
    if(!bPrimingContacts)for(AActor* Actor:Native)
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

FString UASTFireIgnitionZoneComponent::RunContactAudit(UClass* BoxClass,UClass* BarrelClass)
{
#if WITH_EDITOR
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if(!World||!BoxClass||!BarrelClass){if(World)World->DestroyWorld(false);return TEXT("FAIL:world/assets");}
    AActor* Source=World->SpawnActor<AActor>();
    if(!Source){World->DestroyWorld(false);return TEXT("FAIL:source");}
    auto* Zone=NewObject<UASTFireIgnitionZoneComponent>(Source);
    Source->AddInstanceComponent(Zone);
    Source->SetRootComponent(Zone);
    Zone->SetBoxExtent(FVector(75));
    Zone->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Zone->SetCollisionObjectType(ECC_WorldDynamic);
    Zone->SetCollisionResponseToAllChannels(ECR_Overlap);
    Zone->SetMobility(EComponentMobility::Movable);
    Zone->RegisterComponent();
    Zone->bRequireSourceBurning=false;
    bool bPass=true;
    FString Details;
    for(UClass* Class:{BoxClass,BarrelClass})
    {
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Prop=World->SpawnActor<AActor>(Class,FVector(1000,0,0),FRotator::ZeroRotator,Spawn);
        auto* Body=Prop?Cast<UStaticMeshComponent>(Prop->GetRootComponent()):nullptr;
        if(!Body||!Body->GetStaticMesh()){bPass=false;Details+=TEXT(";missing_prop_mesh");continue;}
        Body->SetGenerateOverlapEvents(false);
        Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        auto* Fire=NewObject<UASTFireReactiveComponent>(Prop);
        Prop->AddInstanceComponent(Fire);
        Fire->bStartLit=false;Fire->Intensity=0.f;
        Fire->bEnableProceduralFlames=false;Fire->bEnableFlameCards=false;
        Fire->bEnableNiagara=false;Fire->bEnableSurfaceFire=false;
        Fire->RegisterComponent();
        auto Move=[&](const FVector& Point,const FRotator& Rotation)
        {
            // Place the actual mesh center rather than assuming its pivot.
            Body->SetWorldRotation(Rotation,false,nullptr,ETeleportType::TeleportPhysics);
            const FVector Offset=Body->GetComponentTransform().TransformVector(Body->GetStaticMesh()->GetBoundingBox().GetCenter());
            Body->SetWorldLocation(Point-Offset,false,nullptr,ETeleportType::TeleportPhysics);
            Zone->PollContactOverlaps();
        };
        Move(FVector(1000,0,0),FRotator::ZeroRotator);
        const bool bOutside=!UASTFireBlueprintLibrary::IsActorBurning(Prop);
        Move(FVector::ZeroVector,FRotator(23,40,17));
        const bool bIgnited=UASTFireBlueprintLibrary::IsActorBurning(Prop);
        UASTFireBlueprintLibrary::ExtinguishActor(Prop);
        Zone->PollContactOverlaps();
        const bool bNoDuplicate=!UASTFireBlueprintLibrary::IsActorBurning(Prop);
        Move(FVector(1000,0,0),FRotator::ZeroRotator);
        // Match the collision-disabled carried state, with a different pose.
        Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Move(FVector::ZeroVector,FRotator(65,12,40));
        const bool bCarriedReentry=UASTFireBlueprintLibrary::IsActorBurning(Prop);
        bPass&=bOutside&&bIgnited&&bNoDuplicate&&bCarriedReentry;
        Details+=FString::Printf(TEXT(";%s:outside=%d;no_overlap_events=%d;no_duplicate=%d;reentry=%d"),
            *Class->GetName(),int32(bOutside),int32(bIgnited),int32(bNoDuplicate),int32(bCarriedReentry));
        Prop->Destroy();
    }
    World->DestroyWorld(false);
    return FString(bPass?TEXT("PASS:"):TEXT("FAIL:"))+Details;
#else
    return TEXT("EDITOR_ONLY");
#endif
}
