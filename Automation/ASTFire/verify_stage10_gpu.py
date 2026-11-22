# -*- coding: utf-8 -*-
"""Real-RHI material validation after Stage10 asset authoring.

Must run WITHOUT -nullrhi and WITH -AllowCommandletRendering so UE compiles
the material for PCD3D_SM6. PowerShell additionally checks the engine log for
shader compile failures. This verifies compile/usage, NOT in-game aesthetics.
"""
import json
import os
import traceback
import unreal as u

result = {"status": "STARTED", "assets": [], "errors": []}
path = "/Game/VFX/Fire/Materials/M_AST_FireSurfaceLife"
try:
    material = u.EditorAssetLibrary.load_asset(path)
    if not material:
        raise RuntimeError("Missing burn-surface overlay material")
    if material.get_editor_property("blend_mode") != u.BlendMode.BLEND_TRANSLUCENT:
        raise RuntimeError("Burn-surface overlay must be TRANSLUCENT")
    if not material.get_editor_property("used_with_skeletal_mesh"):
        raise RuntimeError("Burn-surface material lacks SkeletalMesh shader usage")
    u.MaterialEditingLibrary.recompile_material(material)
    result["assets"].append(path)
    result["status"] = "STAGE10_GPU_GRAPH_AND_USAGE_CHECKED_SHADER_LOG_REQUIRED"
except Exception as e:
    result["status"] = "FAILED"
    result["errors"].append({
        "message": str(e),
        "trace": traceback.format_exc()[-2000:]
    })
finally:
    folder = os.path.join(
        u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
        "ChatGPT", "NiagaraFire", "Stage10")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "gpu_shader_check.json"), "w",
              encoding="utf-8") as out:
        json.dump(result, out, indent=2, ensure_ascii=False)
    print("STAGE10_GPU_SHADER_CHECK " + json.dumps(result), flush=True)
