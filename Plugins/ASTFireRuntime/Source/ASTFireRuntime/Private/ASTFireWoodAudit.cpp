#include "ASTFireIgnitionZoneComponent.h"
#include "ASTFireWoodEnvironment.h"
#include "ASTFireReactiveComponent.h"
#include "ASTFireBlueprintLibrary.h"
#include "ASTFireFreeFlameComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"

FString UASTFireIgnitionZoneComponent::RunWoodAudit(UStaticMesh* Beam,UStaticMesh* Fence,UStaticMesh* Rock)
{
#if WITH_EDITOR
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if(!World||!ASTFireWood::IsWoodAsset(Beam)||!ASTFireWood::IsWoodAsset(Fence)||!Rock||ASTFireWood::IsWoodAsset(Rock))
    {if(World)World->DestroyWorld(false);return TEXT("FAIL:assets");}
    AActor* Source=World->SpawnActor<AActor>();
    if(!Source){World->DestroyWorld(false);return TEXT("FAIL:source");}
    auto* Zone=NewObject<UASTFireIgnitionZoneComponent>(Source);
    Source->AddInstanceComponent(Zone);Source->SetRootComponent(Zone);
    Zone->SetMobility(EComponentMobility::Movable);
    Zone->SetBoxExtent(FVector(75));Zone->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Zone->SetCollisionObjectType(ECC_WorldDynamic);Zone->SetCollisionResponseToAllChannels(ECR_Overlap);
    Zone->SetGenerateOverlapEvents(false);Zone->bRequireSourceBurning=false;Zone->RegisterComponent();
    auto Make=[&](UStaticMesh* Asset,ECollisionEnabled::Type Collision)
    {
        FActorSpawnParameters Spawn;Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Actor=World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
        if(!Actor)return Actor;
        auto* Body=Actor->GetStaticMeshComponent();
        Body->SetMobility(EComponentMobility::Movable);Body->SetStaticMesh(Asset);
        Body->SetWorldRotation(FRotator(17,31,12));
        Body->SetWorldLocation(-Body->GetComponentTransform().TransformVector(Asset->GetBoundingBox().GetCenter()));
        Body->SetGenerateOverlapEvents(false);Body->SetCollisionObjectType(ECC_WorldStatic);
        Body->SetCollisionResponseToAllChannels(ECR_Block);Body->SetCollisionEnabled(Collision);
        Body->SetMobility(EComponentMobility::Static);
        return Actor;
    };
    auto* Stone=Make(Rock,ECollisionEnabled::QueryAndPhysics);
    bool bPass=IsValid(Stone);
    FString Details;
    double Now=0.;
    for(UStaticMesh* Asset:{Beam,Fence})
    {
        auto* Actor=Make(Asset,Asset==Beam?ECollisionEnabled::QueryAndPhysics:ECollisionEnabled::NoCollision);
        if(!Actor){bPass=false;continue;}
        auto* Body=Actor->GetStaticMeshComponent();auto* Original=Body->GetMaterial(0);
        const float Delay=ContactDelay(UASTFireReactiveComponent::MeasureFuelSize(Body,180.f));
        Zone->ClearContacts();Zone->SetWorldLocation(FVector::ZeroVector);
        Zone->bPrimingContacts=true;Zone->PollContactOverlapsAt(Now);Zone->bPrimingContacts=false;
        Now+=Delay*.75;Zone->PollContactOverlapsAt(Now);
        const bool bEarly=!UASTFireBlueprintLibrary::IsActorBurning(Actor);
        Now+=Delay*.25+.01;Zone->PollContactOverlapsAt(Now);
        auto* Fire=UASTFireBlueprintLibrary::FindFireComponent(Actor);
        const bool bIgnited=Fire&&Fire->bStartLit;
        bool bGeometry=false,bSlot=false,bHold=false,bFull=false,bCold=false,bRestore=false;
        float Duration=0.f;
        if(bIgnited)
        {
            Fire->bPlaying=true;Duration=Fire->BurnDurationSeconds;
            bGeometry=Fire->FindTargetComponent()==Body&&IsValid(Fire->FreeFlames)&&Fire->FreeFlames->GetTongueCount()>0;
            bSlot=Fire->bCharUsesPropSlot&&Body->GetMaterial(0)==Fire->FireSurfaceMID;
            for(float T=0;T<Duration+1.f;T+=.25f)
            {
                Now+=.25;Zone->PollContactOverlapsAt(Now);
                Fire->TickComponent(.25f,LEVELTICK_All,&Fire->PrimaryComponentTick);
            }
            bHold=Fire->bStartLit&&FMath::IsNearlyZero(Fire->BurnElapsedSeconds);
            Zone->SetWorldLocation(FVector(100000,0,0));Zone->PollContactOverlapsAt(Now);
            float Elapsed=0.f;
            while(Fire->bStartLit&&Elapsed<Duration+1.f)
            {
                Fire->TickComponent(.05f,LEVELTICK_All,&Fire->PrimaryComponentTick);Elapsed+=.05f;
            }
            bFull=!Fire->bStartLit&&FMath::Abs(Elapsed-Duration)<.075f;
            for(int32 I=0;I<200;++I)Fire->TickComponent(.05f,LEVELTICK_All,&Fire->PrimaryComponentTick);
            float Glow=-1.f;
            bCold=Fire->CharProgress>.5f&&IsValid(Fire->FireSurfaceMID)&&
                Fire->FireSurfaceMID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("ASTPropCharGlow")),Glow)&&Glow<.001f;
            Fire->ReleaseSurfaceFire();bRestore=Body->GetMaterial(0)==Original;
        }
        const bool bStoneSafe=!UASTFireBlueprintLibrary::FindFireComponent(Stone);
        const bool bStatic=Body->GetMobility()==EComponentMobility::Static;
        bPass&=bEarly&&bIgnited&&bGeometry&&bSlot&&bHold&&bFull&&bCold&&bRestore&&bStoneSafe&&bStatic;
        Details+=FString::Printf(TEXT(";%s:delay=%.2f;duration=%.2f;early=%d;lit=%d;geometry=%d;slot=%d;hold=%d;full=%d;cold=%d;restore=%d;stone_safe=%d;static=%d"),
            *Asset->GetName(),Delay,Duration,int32(bEarly),int32(bIgnited),int32(bGeometry),int32(bSlot),int32(bHold),int32(bFull),int32(bCold),int32(bRestore),int32(bStoneSafe),int32(bStatic));
        Zone->ClearContacts();Actor->Destroy();
    }
    World->DestroyWorld(false);
    return FString(bPass?TEXT("PASS:"):TEXT("FAIL:"))+Details;
#else
    return TEXT("EDITOR_ONLY");
#endif
}
