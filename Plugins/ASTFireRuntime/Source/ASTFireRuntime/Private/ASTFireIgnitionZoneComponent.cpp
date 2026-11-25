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
    ClearContacts();
    Super::EndPlay(EndPlayReason);
}

void UASTFireIgnitionZoneComponent::HandleBeginOverlap(
    UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex,
    bool bFromSweep, const FHitResult& SweepResult)
{
    // The reconciled contact poll is the only ignition path. A begin event
    // must never bypass the continuous heating requirement.
}

void UASTFireIgnitionZoneComponent::HandleEndOverlap(
    UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex)
{
    if (!IsValid(OtherActor)) return;
    // Poll reconciles native and explicit contacts together: an end event
    // from one primitive must not reset an actor whose other body still touches.
}

bool UASTFireIgnitionZoneComponent::IsHeatActive() const
{
    const AActor* Owner=GetOwner();
    return IsValid(Owner)&&Owner->HasAuthority()&&IsRegistered()&&
        GetCollisionEnabled()!=ECollisionEnabled::NoCollision&&
        (!bRequireSourceBurning||UASTFireBlueprintLibrary::IsActorBurning(Owner));
}

bool UASTFireIgnitionZoneComponent::IsSustaining(const AActor* Target) const
{
    return IsHeatActive() && IsValid(Target) &&
        IgnitedDuringOverlap.Contains(TWeakObjectPtr<AActor>(const_cast<AActor*>(Target)));
}

void UASTFireIgnitionZoneComponent::ClearContacts()
{
    for(const auto& Key:IgnitedDuringOverlap)
        if(auto* Fire=UASTFireBlueprintLibrary::FindFireComponent(Key.Get()))
            Fire->SetIgniterContact(this,false);
    IgnitedDuringOverlap.Reset();
    ContactStartedAt.Reset();
    SuppressedInitialContacts.Reset();
}

float UASTFireIgnitionZoneComponent::ContactDelay(float SizeCm)
{
    // 50 cm or smaller: 1.5 s; character ~2.8 s; larger bodies heat longer.
    return 1.5f*FMath::Sqrt(FMath::Max(1.f,SizeCm/50.f));
}

void UASTFireIgnitionZoneComponent::PollContactOverlaps()
{
    if(UWorld* World=GetWorld())PollContactOverlapsAt(World->GetTimeSeconds());
}

void UASTFireIgnitionZoneComponent::PollContactOverlapsAt(double Now)
{
    AActor* Owner=GetOwner();
    UWorld* World=GetWorld();
    if(!IsValid(Owner)||!Owner->HasAuthority()||!World)return;
    if(!IsHeatActive()){ClearContacts();return;}
    TSet<TWeakObjectPtr<AActor>> Touching;
    TMap<TWeakObjectPtr<AActor>,FVector> Points;
    TArray<AActor*> Native;
    GetOverlappingActors(Native);

    const FVector ZoneCenter=GetComponentLocation();
    const FQuat ZoneRotation=GetComponentQuat();
    const FVector ZoneExtent=GetScaledBoxExtent();
    const FCollisionShape Shape=FCollisionShape::MakeBox(ZoneExtent);
    auto RecordContact=[&](AActor* Actor,UPrimitiveComponent* Body)
    {
        const TWeakObjectPtr<AActor> Key(Actor);
        Touching.Add(Key);
        if(Points.Contains(Key))return;
        FVector Contact=Body->Bounds.GetBox().GetClosestPointTo(ZoneCenter);
        FVector Closest;
        if(Body->GetCollisionEnabled()!=ECollisionEnabled::NoCollision &&
            Body->GetClosestPointOnCollision(ZoneCenter,Closest)>0.f)
            Contact=Closest;
        Points.Add(Key,Contact);
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
    // Native-only initial scenery remains opt-in; actual physical contacts
    // (including props initially resting in fire) always start heating.
    for(AActor* Actor:Native)
    {
        if(!IsValid(Actor)||Actor==Owner)continue;
        const TWeakObjectPtr<AActor> Key(Actor);
        if(Touching.Contains(Key))continue;
        Touching.Add(Key);
        if(bPrimingContacts)SuppressedInitialContacts.Add(Key);
        Points.Add(Key,Actor->GetActorLocation());
    }
    for(auto It=SuppressedInitialContacts.CreateIterator();It;++It)
        if(!It->IsValid()||!Touching.Contains(*It))It.RemoveCurrent();
    for(auto It=ContactStartedAt.CreateIterator();It;++It)
        if(!It.Key().IsValid()||!Touching.Contains(It.Key()))It.RemoveCurrent();
    for(auto It=IgnitedDuringOverlap.CreateIterator();It;++It)
        if(!It->IsValid()||!Touching.Contains(*It))
        {
            if(auto* Fire=UASTFireBlueprintLibrary::FindFireComponent(It->Get()))
                Fire->SetIgniterContact(this,false);
            It.RemoveCurrent();
        }
    for(const auto& Pair:Points)
    {
        AActor* Target=Pair.Key.Get();
        if(!IsValid(Target)||SuppressedInitialContacts.Contains(Pair.Key))continue;
        if(!bIgniteOtherFireSources && Target->GetClass()->GetName().StartsWith(TEXT("BP_AST_FireSource")))continue;
        auto* Fire=UASTFireBlueprintLibrary::FindFireComponent(Target);
        if(Fire && Fire->bStartLit)
        {
            IgnitedDuringOverlap.Add(Pair.Key);
            Fire->SetIgniterContact(this,true);
            continue;
        }
        if(IgnitedDuringOverlap.Contains(Pair.Key))
        {
            // The component may have been replaced externally. Restart heating.
            IgnitedDuringOverlap.Remove(Pair.Key);
            ContactStartedAt.Remove(Pair.Key);
        }
        const double Started=ContactStartedAt.FindOrAdd(Pair.Key,Now);
        const USceneComponent* Mesh=Fire?Fire->FindTargetComponent():
            UASTFireReactiveComponent::ResolveTargetComponent(Target,NAME_None);
        const float Size=UASTFireReactiveComponent::MeasureFuelSize(Mesh,180.f);
        if(Now-Started+1.e-6>=ContactDelay(Size))
            IgniteTarget(Target,Pair.Value);
    }
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
    if (auto* Fire=UASTFireBlueprintLibrary::FindFireComponent(Target); Fire && Fire->bStartLit)
    {
        IgnitedDuringOverlap.Add(WeakTarget);
        Fire->SetIgniterContact(this,true);
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

    if (auto* Fire=UASTFireBlueprintLibrary::IgniteActor(
        Target, ContactPosition, IgnitionStrength, Points))
    {
        IgnitedDuringOverlap.Add(WeakTarget);
        Fire->SetIgniterContact(this,true);
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
    auto MakeZone=[&]()
    {
        AActor* Source=World->SpawnActor<AActor>();
        if(!Source)return static_cast<UASTFireIgnitionZoneComponent*>(nullptr);
        auto* Zone=NewObject<UASTFireIgnitionZoneComponent>(Source);
        Source->AddInstanceComponent(Zone);Source->SetRootComponent(Zone);
        Zone->SetBoxExtent(FVector(75));
        Zone->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Zone->SetCollisionObjectType(ECC_WorldDynamic);
        Zone->SetCollisionResponseToAllChannels(ECR_Overlap);
        Zone->SetGenerateOverlapEvents(false);
        Zone->SetMobility(EComponentMobility::Movable);
        Zone->RegisterComponent();
        Zone->bRequireSourceBurning=false;
        return Zone;
    };
    auto* Zone=MakeZone();
    auto* OtherZone=MakeZone();
    if(!Zone||!OtherZone){World->DestroyWorld(false);return TEXT("FAIL:source");}
    OtherZone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    bool bPass=FMath::IsNearlyEqual(ContactDelay(30.f),1.5f)&&ContactDelay(180.f)>ContactDelay(50.f)&&ContactDelay(600.f)>ContactDelay(180.f);
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
        double Now=0.;
        auto Poll=[&](double Advance)
        {
            Now+=Advance;
            Zone->PollContactOverlapsAt(Now);
            OtherZone->PollContactOverlapsAt(Now);
        };
        auto Move=[&](const FVector& Point,const FRotator& Rotation)
        {
            Body->SetWorldRotation(Rotation,false,nullptr,ETeleportType::TeleportPhysics);
            const FVector Offset=Body->GetComponentTransform().TransformVector(Body->GetStaticMesh()->GetBoundingBox().GetCenter());
            Body->SetWorldLocation(Point-Offset,false,nullptr,ETeleportType::TeleportPhysics);
            Poll(0.);
        };
        const float Delay=ContactDelay(UASTFireReactiveComponent::MeasureFuelSize(Body,180.f));
        Move(FVector(1000,0,0),FRotator::ZeroRotator);
        const bool bOutside=!UASTFireBlueprintLibrary::IsActorBurning(Prop);
        Move(FVector::ZeroVector,FRotator(23,40,17));
        Poll(Delay*.75);
        const bool bEarly=!UASTFireBlueprintLibrary::IsActorBurning(Prop);
        Move(FVector(1000,0,0),FRotator::ZeroRotator);
        Poll(Delay*2.);
        Move(FVector::ZeroVector,FRotator(65,12,40));
        Poll(Delay*.75);
        const bool bReset=!UASTFireBlueprintLibrary::IsActorBurning(Prop);
        Poll(Delay*.25+.001);
        const bool bIgnited=UASTFireBlueprintLibrary::IsActorBurning(Prop);
        Fire->bInitialized=true;Fire->bPlaying=true;
        const float Duration=Fire->BurnDurationSeconds;
        // Hold longer than a whole lifetime, with fuel updates from real ticks.
        for(float T=0;T<Duration+1.f;T+=.25f)
        {
            Poll(.25);
            Fire->TickComponent(.25f,LEVELTICK_All,&Fire->PrimaryComponentTick);
        }
        const bool bHeld=Fire->bStartLit && FMath::IsNearlyZero(Fire->BurnElapsedSeconds) &&
            FMath::IsNearlyEqual(Fire->Intensity,Fire->InitialBurnIntensity);
        const int32 Anchors=Fire->ExtraLocalPoints.Num();
        UASTFireBlueprintLibrary::ExtinguishActor(Prop);
        Poll(.1);
        const bool bNoDuplicate=Fire->bStartLit && Fire->ExtraLocalPoints.Num()==Anchors;
        // Two simultaneously active sources must not release each other's hold.
        OtherZone->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Poll(.05);
        const bool bTwo=Fire->ActiveIgniters.Num()==2;
        Zone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Poll(.05);
        Fire->TickComponent(1.f,LEVELTICK_All,&Fire->PrimaryComponentTick);
        const bool bOtherHolds=Fire->bStartLit&&FMath::IsNearlyZero(Fire->BurnElapsedSeconds);
        // Source extinguishing releases the hold even before the next poll.
        auto* SourceFire=NewObject<UASTFireReactiveComponent>(OtherZone->GetOwner());
        OtherZone->GetOwner()->AddInstanceComponent(SourceFire);
        SourceFire->bStartLit=true;SourceFire->Intensity=1.f;
        OtherZone->bRequireSourceBurning=true;
        const bool bSourceActive=Fire->HasActiveIgniter();
        SourceFire->bStartLit=false;SourceFire->Intensity=0.f;
        const bool bSourceOut=!Fire->HasActiveIgniter();
        OtherZone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        OtherZone->bRequireSourceBurning=false;
        SourceFire->DestroyComponent();
        Poll(.05);
        Fire->TickComponent(1.f,LEVELTICK_All,&Fire->PrimaryComponentTick);
        const bool bReleased=Fire->bStartLit&&FMath::IsNearlyEqual(Fire->BurnElapsedSeconds,1.f);
        // Reentry while already burning replenishes without another delay.
        Zone->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Poll(.05);
        const bool bCarriedReentry=Fire->bStartLit&&FMath::IsNearlyZero(Fire->BurnElapsedSeconds);
        Move(FVector(1000,0,0),FRotator::ZeroRotator);
        float Elapsed=0.f;
        while(Fire->bStartLit&&Elapsed<Duration+1.f)
        {
            Fire->TickComponent(.05f,LEVELTICK_All,&Fire->PrimaryComponentTick);
            Elapsed+=.05f;
        }
        const bool bFullOutside=!Fire->bStartLit&&FMath::Abs(Elapsed-Duration)<.075f;
        // Collision-disabled object also needs a fresh full heating interval.
        Move(FVector::ZeroVector,FRotator(23,40,17));
        Poll(Delay-.05);
        const bool bCarriedEarly=!Fire->bStartLit;
        Poll(.051);
        const bool bCarriedIgnite=Fire->bStartLit;
        bPass&=bOutside&&bEarly&&bReset&&bIgnited&&bHeld&&bNoDuplicate&&bTwo&&bOtherHolds&&bSourceActive&&bSourceOut&&bReleased&&bCarriedReentry&&bFullOutside&&bCarriedEarly&&bCarriedIgnite;
        Details+=FString::Printf(TEXT(";%s:delay=%.2f;duration=%.2f;outside=%.2f;early=%d;reset=%d;ignited=%d;hold=%d;no_duplicate=%d;two=%d;other_holds=%d;release=%d;reentry=%d;full=%d;carried_delay=%d"),
            *Class->GetName(),Delay,Duration,Elapsed,int32(bEarly),int32(bReset),int32(bIgnited),int32(bHeld),int32(bNoDuplicate),int32(bTwo),int32(bOtherHolds),int32(bReleased),int32(bCarriedReentry),int32(bFullOutside),int32(bCarriedEarly&&bCarriedIgnite));
        Zone->ClearContacts();OtherZone->ClearContacts();
        Prop->Destroy();
    }
    World->DestroyWorld(false);
    return FString(bPass?TEXT("PASS:"):TEXT("FAIL:"))+Details;
#else
    return TEXT("EDITOR_ONLY");
#endif
}
