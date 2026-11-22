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

    System->MarkPackageDirty();
    System->RequestCompile(true);
    System->PollForCompilationComplete(true);
    return TEXT("OK_3_EMITTERS");
#else
    return TEXT("EDITOR_REQUIRED");
#endif
}
