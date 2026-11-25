#include "ASTFireBlueprintLibrary.h"
#include "ASTFireReactiveComponent.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"

UASTFireReactiveComponent* UASTFireBlueprintLibrary::FindFireComponent(
    const AActor* Target)
{
    return IsValid(Target)
        ? Target->FindComponentByClass<UASTFireReactiveComponent>()
        : nullptr;
}

UASTFireReactiveComponent* UASTFireBlueprintLibrary::IgniteActor(
    AActor* Target, FVector HitWorldPosition, float Strength, int32 DesiredFlamePoints)
{
    if (!IsValid(Target))
        return nullptr;
    if (Target->GetNetMode() == NM_Client)
    {
        UE_LOG(LogTemp, Warning, TEXT("AST Fire: Ignite Any Actor must run on server for replicated combustion."));
        return nullptr;
    }

    UASTFireReactiveComponent* Fire = FindFireComponent(Target);
    if (!IsValid(Fire))
    {
        // Runtime-only component: leave the actor's authored components intact.
        Fire = NewObject<UASTFireReactiveComponent>(
            Target, NAME_None, RF_Transient);
        if (!Fire) return nullptr;
        Fire->Intensity = 0.0f;
        Fire->bStartLit = false;
        Target->AddInstanceComponent(Fire);
        Fire->SetIsReplicated(true);
        Fire->RegisterComponent();
    }

    // A character is not a cylinder: give articulated limbs multiple
    // surface-bound clusters even for callers who request a single point.
    if (Target->FindComponentByClass<USkinnedMeshComponent>())
        DesiredFlamePoints = FMath::Max(DesiredFlamePoints, 7);
    Fire->SetFireCoverage(
        EASTFireCoverage::Bounds,
        FMath::Clamp(DesiredFlamePoints, 1, 16));

    Fire->Ignite(FMath::Clamp(Strength, 0.05f, 1.0f));
    Fire->IgniteAtWorldPosition(HitWorldPosition);
    return Fire;
}

void UASTFireBlueprintLibrary::ExtinguishActor(AActor* Target)
{
    if (UASTFireReactiveComponent* Fire = FindFireComponent(Target))
        Fire->Extinguish();
}

bool UASTFireBlueprintLibrary::IsActorBurning(const AActor* Target)
{
    if (const UASTFireReactiveComponent* Fire = FindFireComponent(Target))
        return Fire->bStartLit && Fire->Intensity > 0.005f;
    return false;
}
