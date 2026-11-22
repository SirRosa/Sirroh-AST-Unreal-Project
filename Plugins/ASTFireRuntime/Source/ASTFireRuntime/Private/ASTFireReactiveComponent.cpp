#include "ASTFireReactiveComponent.h"

#include "NiagaraComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

UASTFireReactiveComponent::UASTFireReactiveComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.04f; // 25 Hz
}

void UASTFireReactiveComponent::SetFireIntensity(float NewIntensity)
{
    Intensity = FMath::Clamp(NewIntensity, 0.0f, 1.0f);
}

void UASTFireReactiveComponent::BeginPlay()
{
    Super::BeginPlay();
    AActor* Owner = GetOwner();
    if (!Owner) return;

    FX = Owner->FindComponentByClass<UNiagaraComponent>();
    Light = Owner->FindComponentByClass<UPointLightComponent>();
    LastOwnerPosition = Owner->GetActorLocation();
    SmoothIntensity = Intensity;
    if (FX)
    {
        InitialFXRotation = FX->GetRelativeRotation();
        InitialFXScale = FX->GetRelativeScale3D();
    }
    if (Light)
    {
        Light->SetIntensity(0.0f);
    }

    // Only local overlap notifications. No all-player scan per fire source.
    InfluenceSphere = NewObject<USphereComponent>(
        Owner, TEXT("ASTFire_InfluenceSphere"), RF_Transient);
    if (InfluenceSphere)
    {
        Owner->AddInstanceComponent(InfluenceSphere);
        if (USceneComponent* Root = Owner->GetRootComponent())
        {
            InfluenceSphere->SetupAttachment(Root);
        }
        InfluenceSphere->SetSphereRadius(InfluenceRadius);
        InfluenceSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        InfluenceSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
        InfluenceSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
        InfluenceSphere->SetGenerateOverlapEvents(true);
        InfluenceSphere->OnComponentBeginOverlap.AddDynamic(
            this, &UASTFireReactiveComponent::OnInfluenceBegin);
        InfluenceSphere->OnComponentEndOverlap.AddDynamic(
            this, &UASTFireReactiveComponent::OnInfluenceEnd);
        InfluenceSphere->RegisterComponent();
        InfluenceSphere->SetHiddenInGame(true);
    }
}

void UASTFireReactiveComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    NearbyPawns.Reset();
    if (InfluenceSphere)
    {
        InfluenceSphere->DestroyComponent();
        InfluenceSphere = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

void UASTFireReactiveComponent::OnInfluenceBegin(
    UPrimitiveComponent* Overlapped, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
    bool bFromSweep, const FHitResult& SweepResult)
{
    if (APawn* Pawn = Cast<APawn>(OtherActor))
    {
        NearbyPawns.Add(Pawn);
    }
}

void UASTFireReactiveComponent::OnInfluenceEnd(
    UPrimitiveComponent* Overlapped, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
    if (APawn* Pawn = Cast<APawn>(OtherActor))
    {
        NearbyPawns.Remove(Pawn);
    }
}

void UASTFireReactiveComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World || DeltaTime <= KINDA_SMALL_NUMBER) return;

    if (!FX) FX = Owner->FindComponentByClass<UNiagaraComponent>();
    if (!Light) Light = Owner->FindComponentByClass<UPointLightComponent>();

    const FVector Position = Owner->GetActorLocation();
    const FVector OwnerVelocity = (Position - LastOwnerPosition) / DeltaTime;
    LastOwnerPosition = Position;
    float LocalDisturbance = 0.0f;
    FVector LocalWind = FVector::ZeroVector;

    for (auto It = NearbyPawns.CreateIterator(); It; ++It)
    {
        APawn* Pawn = It->Get();
        if (!IsValid(Pawn))
        {
            It.RemoveCurrent();
            continue;
        }
        const float Range = FVector::Dist(Position, Pawn->GetActorLocation());
        if (Range > InfluenceRadius) continue;
        const FVector Velocity = Pawn->GetVelocity();
        const float Strength =
            (1.0f - Range / FMath::Max(InfluenceRadius, 1.0f)) *
            FMath::Clamp(Velocity.Size() / 600.0f, 0.0f, 1.0f);
        if (Strength > LocalDisturbance)
        {
            LocalDisturbance = Strength;
            LocalWind = Velocity;
        }
    }
    Disturbance = FMath::FInterpTo(
        Disturbance, LocalDisturbance, DeltaTime,
        LocalDisturbance > Disturbance ? 10.0f : 3.0f);
    SmoothIntensity = FMath::FInterpTo(SmoothIntensity, Intensity, DeltaTime, 5.0f);

    const float Time = World->GetTimeSeconds();
    const float Pulse = 0.85f
        + 0.09f * FMath::Sin(Time * 4.4f)
        + 0.045f * FMath::Sin(Time * 12.8f)
        + 0.025f * FMath::Sin(Time * 18.9f);
    const float Motion = FMath::Clamp(
        OwnerVelocity.Size() / 600.0f, 0.0f, 1.0f);
    const float Response = FMath::Max(Motion, Disturbance);
    const float Brightness = SmoothIntensity *
        FMath::Max(0.0f, Pulse + Response * 0.14f);

    if (Light)
    {
        Light->SetIntensity(LightPower * Brightness);
        Light->SetAttenuationRadius(
            LightRadius * (0.92f + Pulse * 0.1f));
        Light->SetLightColor(FLinearColor(
            1.0f, 0.43f + Pulse * 0.07f, 0.29f));
    }

    if (FX)
    {
        const FVector Wind = -OwnerVelocity * 0.4f + LocalWind * 0.18f;
        const FVector Local = Owner->GetActorTransform()
            .InverseTransformVectorNoScale(Wind);
        const float TiltScale = FMath::Clamp(
            Local.Size2D() / 600.0f, 0.0f, 1.0f) * MaxLeanDegrees;
        const float TargetPitch = -FMath::Clamp(
            Local.X / 600.0f, -1.0f, 1.0f) * TiltScale;
        const float TargetRoll = FMath::Clamp(
            Local.Y / 600.0f, -1.0f, 1.0f) * TiltScale;
        const FRotator Wanted =
            InitialFXRotation + FRotator(TargetPitch, 0.0f, TargetRoll);
        FX->SetRelativeRotation(FMath::RInterpTo(
            FX->GetRelativeRotation(), Wanted, DeltaTime, 7.0f));
        const float Size = SmoothIntensity * (0.93f + 0.07f * Pulse);
        FX->SetRelativeScale3D(InitialFXScale *
            FMath::Max(0.01f, Size));
        FX->SetVariableFloat(
            FName(TEXT("User.FireIntensity")), SmoothIntensity);
        FX->SetVariableFloat(
            FName(TEXT("User.DisturbanceStrength")), Response);
        FX->SetVariableFloat(
            FName(TEXT("User.FlamePulse")), Pulse);
        FX->SetVariableVec3(
            FName(TEXT("User.WindVector")), Wind);
        FX->SetVisibility(SmoothIntensity > 0.005f, true);
    }
}
