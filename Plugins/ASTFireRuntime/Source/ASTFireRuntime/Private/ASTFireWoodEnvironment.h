#pragma once
#include "CoreMinimal.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"

namespace ASTFireWood
{
    inline bool IsWoodAsset(const UStaticMesh* Asset)
    {
        return IsValid(Asset) &&
            Asset->GetPathName().StartsWith(TEXT("/Game/ZL_retroCaves/Meshes/decos/"),ESearchCase::IgnoreCase) &&
            Asset->GetName().Contains(TEXT("Wood"),ESearchCase::IgnoreCase);
    }
    // POI Light's published decoration actors are independent single meshes.
    // A whole ISM/HISM batch must never become one fuel object.
    inline bool IsWoodTarget(const AActor* Actor,const UStaticMeshComponent* Mesh)
    {
        return IsValid(Actor)&&Actor->IsA<AStaticMeshActor>()&&IsValid(Mesh)&&
            Mesh==Actor->GetRootComponent()&&!Mesh->IsA<UInstancedStaticMeshComponent>()&&
            IsWoodAsset(Mesh->GetStaticMesh())&&Mesh->IsVisible()&&!Mesh->bHiddenInGame&&!Actor->IsHidden();
    }
}
