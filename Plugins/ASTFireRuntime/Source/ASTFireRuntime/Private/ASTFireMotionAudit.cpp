#include "ASTFireFreeFlameComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
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
    // Measure the actual submitted quads (section 1), not the velocity or
    // the number of cuts. Surface mode compares elevated gas cells against
    // the corresponding owner's fuel bed, independent of object dimensions.
    auto Measure=[](UASTFireFreeFlameComponent* Fire,bool bMesh)
    {
        const auto* Section=Fire->Render->GetProcMeshSection(1);
        if(!Section||Section->ProcVertexBuffer.Num()<4)return FVector::ZeroVector;
        const auto& Buffer=Section->ProcVertexBuffer;
        const FTransform Tr=Fire->Render->GetComponentTransform();
        auto Quad=[&](int32 I)
        {
            FVector Center=FVector::ZeroVector;
            for(int32 K=0;K<4;++K)Center+=FVector(Buffer[I*4+K].Position);
            return Tr.TransformPosition(Center*.25);
        };
        if(!bMesh)
        {
            FVector Total=FVector::ZeroVector;
            for(int32 I=0;I<Buffer.Num()/4;++I)Total+=Quad(I)-Fire->PointMotionOrigin();
            return Total/(Buffer.Num()/4);
        }
        if(Fire->MeshPrevious.Num()!=Buffer.Num()/4)return FVector::ZeroVector;
        FVector Low[MaxTongues],High[MaxTongues];
        int32 NL[MaxTongues]={},NH[MaxTongues]={};
        for(int32 I=0;I<MaxTongues;++I)Low[I]=High[I]=FVector::ZeroVector;
        for(int32 I=0;I<Fire->MeshPrevious.Num();++I)
        {
            const auto& Cell=Fire->MeshPrevious[I];
            if(Cell.Owner<0||Cell.Owner>=MaxTongues)continue;
            if(Cell.Rise<.12f){Low[Cell.Owner]+=Quad(I);++NL[Cell.Owner];}
            if(Cell.Rise>.60f){High[Cell.Owner]+=Quad(I);++NH[Cell.Owner];}
        }
        FVector Sum=FVector::ZeroVector;int32 Count=0;
        for(int32 I=0;I<MaxTongues;++I)if(NL[I]>0&&NH[I]>0)
        {
            Sum+=High[I]/NH[I]-Low[I]/NL[I];++Count;
        }
        return Count?Sum/Count:FVector::ZeroVector;
    };
    const FVector Directions[]={FVector::UpVector,-FVector::UpVector,FVector::RightVector,-FVector::RightVector,FVector::UpVector};
    const TCHAR* Names[]={TEXT("up"),TEXT("down"),TEXT("right"),TEXT("left"),TEXT("up6")};
    for(bool bMesh:{false,true})
    {
        for(int32 Case=0;Case<5;++Case)
        {
            auto* Fire=Make(bMesh);
            if(!Fire){bPass=false;continue;}
            for(int32 I=0;I<70;++I)Step(Fire);
            const FVector Direction=Directions[Case];
            const float Speed=Case==4?600.f:1000.f;
            FVector Geometry=FVector::ZeroVector;
            int32 Births=0,OppositeBirths=0;
            for(int32 I=0;I<65;++I)
            {
                Fire->GetOwner()->SetActorLocation(Direction*((I+1)*Speed*.04f));
                Step(Fire);
                if(I<25)continue;
                Geometry+=Measure(Fire,bMesh);
                for(const auto& F:Fire->FieldFragments)if(F.Age<.001f)
                {
                    ++Births;
                    if(FVector::DotProduct(F.Velocity,-Direction)>35.f)++OppositeBirths;
                }
            }
            Geometry/=40.;
            const FVector Planar(0.,Geometry.Y,Geometry.Z);
            const float Alignment=float(FVector::DotProduct(Planar.GetSafeNormal(),-Direction));
            const float Backward=float(FVector::DotProduct(Planar,-Direction));
            const bool bDirected=Alignment>.60f&&Backward>3.f;
            const bool bFragments=Births>0&&OppositeBirths*2>=Births;
            FVector Settled=FVector::ZeroVector;
            for(int32 I=0;I<65;++I)
            {
                Step(Fire);
                if(I>=35)Settled+=Measure(Fire,bMesh);
            }
            Settled/=30.;
            const bool bRest=MotionStrength(Fire->AeroVelocity)==0.f&&Settled.Z>3.f&&
                FMath::Abs(Settled.Y)<Settled.Z;
            bPass&=bDirected&&bFragments&&bRest;
            Details+=FString::Printf(TEXT(";%s_%s:alignment=%.2f;back_cm=%.2f;births=%d;opposite=%d;rest_z=%.2f;rest=%d"),
                bMesh?TEXT("mesh"):TEXT("point"),Names[Case],Alignment,Backward,Births,OppositeBirths,float(Settled.Z),int32(bRest));
            Fire->GetOwner()->Destroy();
        }
    }
    World->DestroyWorld(false);
    return FString(bPass?TEXT("PASS:"):TEXT("FAIL:"))+Details;
#else
    return TEXT("EDITOR_ONLY");
#endif
}
