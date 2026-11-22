#include "ASTFireEditorLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraSpriteRendererProperties.h"
#include "Materials/MaterialInterface.h"

FString UASTFireEditorLibrary::BuildSystem(
    UNiagaraSystem* System,
    UNiagaraEmitter* Core,
    UNiagaraEmitter* Ember,
    UNiagaraEmitter* Smoke,
    UMaterialInterface* CMat,
    UMaterialInterface* EMat,
    UMaterialInterface* SMat)
{
#if WITH_EDITOR
    if (!System || !Core || !Ember || !Smoke || !CMat || !EMat || !SMat)
    {
        return TEXT("NULL_INPUT");
    }
    if (System->GetEmitterHandles().Num() != 0)
    {
        return TEXT("ALREADY_CONFIGURED");
    }

    struct FEntry
    {
        UNiagaraEmitter* Emitter;
        UMaterialInterface* Material;
        const TCHAR* Name;
        FVersionedNiagaraEmitterData* Data = nullptr;
    };
    FEntry Entries[] = {
        {Core, CMat, TEXT("Flame")},
        {Ember, EMat, TEXT("Embers")},
        {Smoke, SMat, TEXT("Smoke")}
    };

    // Validate all inputs BEFORE changing any emitter or modifying the system.
    for (FEntry& Entry : Entries)
    {
        const FGuid Version = Entry.Emitter->GetExposedVersion().VersionGuid;
        Entry.Data = Entry.Emitter->GetEmitterData(Version);
        if (Entry.Data == nullptr)
        {
            return FString::Printf(TEXT("NO_EMITTER_DATA:%s"), Entry.Name);
        }
        bool bHasSpriteRenderer = false;
        for (UNiagaraRendererProperties* Renderer : Entry.Data->GetRenderers())
        {
            if (Cast<UNiagaraSpriteRendererProperties>(Renderer))
            {
                bHasSpriteRenderer = true;
                break;
            }
        }
        if (!bHasSpriteRenderer)
        {
            return FString::Printf(TEXT("NO_SPRITE:%s"), Entry.Name);
        }
    }

    for (FEntry& Entry : Entries)
    {
        for (UNiagaraRendererProperties* Renderer : Entry.Data->GetRenderers())
        {
            if (UNiagaraSpriteRendererProperties* Sprite =
                Cast<UNiagaraSpriteRendererProperties>(Renderer))
            {
                Sprite->Modify();
                Sprite->Material = Entry.Material;
            }
        }
        Entry.Emitter->MarkPackageDirty();
        System->AddEmitterHandle(
            *Entry.Emitter, FName(Entry.Name),
            Entry.Emitter->GetExposedVersion().VersionGuid);
    }

    System->SetFixedBounds(FBox(FVector(-1200.0f), FVector(1200.0f)));
    System->MarkPackageDirty();
    System->RequestCompile(true);
    System->PollForCompilationComplete(true);
    return TEXT("OK_3_EMITTERS");
#else
    return TEXT("EDITOR_REQUIRED");
#endif
}

FString UASTFireEditorLibrary::RepairVisibilityBounds(
    UNiagaraSystem* System, float Radius)
{
#if WITH_EDITOR
    if (!System) return TEXT("NULL_SYSTEM");
    if (System->GetEmitterHandles().Num() == 0)
        return TEXT("EMPTY_SYSTEM");

    const float SafeRadius = FMath::Clamp(Radius, 200.0f, 20000.0f);
    System->Modify();
    System->SetFixedBounds(FBox(
        FVector(-SafeRadius, -SafeRadius, -SafeRadius),
        FVector(SafeRadius, SafeRadius, SafeRadius)));
    System->MarkPackageDirty();
    System->RequestCompile(true);
    System->PollForCompilationComplete(true);
    return FString::Printf(TEXT("OK_EMITTERS=%d_BOUNDS=%.0f"),
        System->GetEmitterHandles().Num(), SafeRadius);
#else
    return TEXT("EDITOR_REQUIRED");
#endif
}
