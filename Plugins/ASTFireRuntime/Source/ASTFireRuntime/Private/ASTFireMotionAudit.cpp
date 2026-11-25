#include "ASTFireFreeFlameComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

FString UASTFireFreeFlameComponent::RunMotionAudit(UMaterialInterface* TestMaterial)
{
#if WITH_EDITOR
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
    if(!World||!TestMaterial||!Cube){if(World)World->DestroyWorld(false);return TEXT("FAIL:assets");}
    auto Make=[&](bool bMesh)
    {
        auto* Host=World->SpawnActor<AActor>();
        if(!Host)return static_cast<UASTFireFreeFlameComponent*>(nullptr);
        USceneComponent* Root=nullptr;
        if(bMesh)
        {
            auto* Mesh=NewObject<UStaticMeshComponent>(Host);
            Mesh->SetStaticMesh(Cube);Mesh->SetMobility(EComponentMobility::Movable);
            Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);Root=Mesh;
        }
        else Root=NewObject<USceneComponent>(Host);
        Host->SetRootComponent(Root);Root->RegisterComponent();
        auto* Fire=NewObject<UASTFireFreeFlameComponent>(Host);
        Fire->SetSizeScale(bMesh?1.f:.6f);Fire->SetSource(Root,TestMaterial,1729);
        TArray<FVector_NetQuantize10> Contacts;
        if(bMesh)for(const FVector& P:{FVector(50,-30,-20),FVector(50,30,20),FVector(0,0,50),FVector(-50,-25,20)})Contacts.Add(FVector_NetQuantize10(P));
        Fire->SetIgnitionPoints(Contacts);Fire->RegisterComponent();
        return Fire;
    };
    auto Step=[](UASTFireFreeFlameComponent* Fire)
    {
        Fire->SetThermal(1.f,FVector::ZeroVector,.75f);
        Fire->AdvanceSimulation(.04f);Fire->UpdateDetachedVortices(.04f);
        Fire->UpdateGeometry(false);
    };
    bool bPass=MotionStrength(FVector::ZeroVector)==0.f&&MotionLag(FVector(100,0,0)).IsZero()&&
        MotionStrength(FVector(1000,0,0))>.99f;
    FString Details;
    for(bool bMesh:{false,true})
    {
        auto* Still=Make(bMesh);auto* Moving=Make(bMesh);
        if(!Still||!Moving){bPass=false;continue;}
        for(int32 I=0;I<80;++I){Step(Still);Step(Moving);}
        const bool bSame=(Still->FieldSplitCount==Moving->FieldSplitCount&&Still->MeshCuts==Moving->MeshCuts&&
            Still->FieldTransferredCells==Moving->FieldTransferredCells&&Still->MeshTransferred==Moving->MeshTransferred&&
            Still->AeroVelocity.IsZero()&&Moving->AeroVelocity.IsZero());
        auto Cuts=[](const UASTFireFreeFlameComponent* F){return F->FieldSplitCount+F->MeshCuts;};
        auto Cells=[](const UASTFireFreeFlameComponent* F){return F->FieldTransferredCells+F->MeshTransferred;};
        const int32 S0=Cuts(Still),M0=Cuts(Moving),C0=Cells(Moving);
        float Trail=0.f;bool bLag=false,bFinite=true;
        for(int32 I=0;I<100;++I)
        {
            Moving->GetOwner()->SetActorLocation(FVector(0,(I+1)*40.,0));
            Step(Still);Step(Moving);
            for(const auto& F:Moving->FieldFragments)
            {
                Trail=FMath::Max(Trail,float(Moving->Source->GetComponentLocation().Y-F.Center.Y));
                bLag|=F.Velocity.Y< -10.;
                bFinite&=!F.Center.ContainsNaN()&&!F.Velocity.ContainsNaN()&&F.Cells.Num()>0;
            }
        }
        const int32 StillCuts=Cuts(Still)-S0,MoveCuts=Cuts(Moving)-M0,Transferred=Cells(Moving)-C0;
        const bool bFast=MoveCuts>StillCuts&&Transferred>0&&Trail>100.f&&bLag&&bFinite;
        // Rotation at a fixed center must still move individual mesh anchors.
        bool bRotation=!bMesh;
        if(bMesh)
        {
            for(int32 I=0;I<25;++I)
            {
                Moving->GetOwner()->SetActorRotation(FRotator(0,(I+1)*24.,0));Step(Moving);
                for(const auto& T:Moving->Tongues)bRotation|=MotionStrength(T.WorldVelocity)>.1f;
            }
        }
        Moving->GetOwner()->SetActorLocation(FVector(0,90000,0));Step(Moving);
        const bool bTeleport=Moving->bAeroDiscontinuity&&Moving->AeroVelocity.IsZero();
        for(int32 I=0;I<40;++I)Step(Moving);
        const bool bSettled=MotionStrength(Moving->AeroVelocity)==0.f;
        bPass&=bSame&&bFast&&bRotation&&bTeleport&&bSettled;
        Details+=FString::Printf(TEXT(";%s:still=%d;still_cuts=%d;moving_cuts=%d;cells=%d;trail_cm=%.1f;lag=%d;rotation=%d;teleport=%d;settled=%d"),
            bMesh?TEXT("mesh"):TEXT("point"),int32(bSame),StillCuts,MoveCuts,Transferred,Trail,int32(bLag),int32(bRotation),int32(bTeleport),int32(bSettled));
        Still->GetOwner()->Destroy();Moving->GetOwner()->Destroy();
    }
    World->DestroyWorld(false);
    return FString(bPass?TEXT("PASS:"):TEXT("FAIL:"))+Details;
#else
    return TEXT("EDITOR_ONLY");
#endif
}
