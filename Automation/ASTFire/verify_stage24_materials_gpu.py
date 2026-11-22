# -*- coding: utf-8 -*-
"""Stage24 real-RHI shader compilation smoke and char; does NOT start PIE.

Run with -AllowCommandletRendering -RenderOffscreen and WITHOUT -nullrhi.
The caller must also scan the log for LogMaterial/LogShaderCompilers failures.
"""
import json,os,traceback
import unreal as u
OUT={"status":"STARTED","materials":[],"errors":[]}
try:
    for name in ("M_AST_FireChar","M_AST_FireSmoke"):
        path="/Game/VFX/Fire/Materials/"+name
        material=u.EditorAssetLibrary.load_asset(path)
        if material is None:
            raise RuntimeError("Missing material: "+path)
        if material.get_editor_property("blend_mode") != u.BlendMode.BLEND_TRANSLUCENT:
            raise RuntimeError("Smoke/char material is not translucent: "+name)
        if name=="M_AST_FireChar" and not material.get_editor_property(
                "used_with_skeletal_mesh"):
            raise RuntimeError("Char material missing skeletal shader usage")
        u.MaterialEditingLibrary.recompile_material(material)
        OUT["materials"].append(path)
    OUT["status"]="STAGE24_GPU_MATERIAL_GRAPHS_CHECKED_SHADER_LOG_REQUIRED"
except Exception as e:
    OUT["status"]="FAILED"
    OUT["errors"].append({"error":str(e),"trace":traceback.format_exc()[-3000:]})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(
        u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","Stage24")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"gpu_material_check.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,ensure_ascii=False,indent=2)
    print("AST_FIRE_STAGE24_GPU "+json.dumps(OUT,ensure_ascii=False),flush=True)
