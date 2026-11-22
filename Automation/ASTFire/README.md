# AST Fire / Niagara — Stage 2

Branch: `work/niagara-fire-stage2`

## Status (2026-11-22)
- ASTFireRuntime / ASTFireEditor compiled independently in BuildPlugin, according to available logs.
- The most recent Unreal Python result recorded `Unconnected [True, True, False, False, True, True]` for each material.
- `Automation/ASTFire/fix_stage2_materials.py` eliminates the failing Custom(float4) -> ComponentMask connections, using independent float3 and float1 Custom expressions.
- This fix has NOT yet been executed in Unreal Engine. It must not be described as verified.
- The master Niagara system was not fully assembled at that checkpoint. Emitter assembly and runtime light behavior remain unverified.

## Scope and safety
- Original BP_Torch_U is untouched.
- Do not reuse existing placeholder torch lights, GrimFX or unrelated asset packs.
- New fire system owns the dynamic light and is reusable for torch, campfire, arrow, spells and ground fires.
- Do not run automated playtests/renders; manual artistic acceptance belongs to the project owner.
- The fix script writes only the three AST-specific fire materials in `/Game/VFX/Fire/Materials` and the diagnostic JSON in Saved.
- It is **not** an emitter or Blueprint finalizer.

## Execution
Copy `Automation/ASTFire/fix_stage2_materials.py` to the project (or use its absolute path), and run under **UnrealEditor-Cmd.exe** with `-run=pythonscript -script="<absolute-path>"`. Do not run with the system Python interpreter.

Expected report: `Saved/ChatGPT/NiagaraFire/Stage2/stage2_material_fix_result.json`.

After checking that all three material results are marked saved, proceed with the emitter/System integration script, then verify the actual Niagara graph and reactive lighting.

## Source control
- This repo began empty. GitHub does not automatically mirror the Google Drive synced project or Windows local project.
- Generated folders (Binaries, Intermediate, Saved, DerivedDataCache) are ignored.
- Large `.uasset` / `.umap` files should be transferred by a Git workflow with Git LFS configured before importing the full project.
