# -*- coding: utf-8 -*-
"""Fire v4 material check with real RHI. Never saves or changes assets.
Run without -nullrhi, with -AllowCommandletRendering -RenderOffscreen.
Inspect shader compiler log as well as this report.
"""
import json,os,traceback
import unreal as u
OUT={"status":"FAILED","materials":[],"errors":[]}
try:
    path="/Game/VFX/Fire/Materials/M_AST_FireTongue"
    m=u.EditorAssetLibrary.load_asset(path)
    if not m:
        raise RuntimeError("Missing material")
    if m.get_editor_property("blend_mode")!=u.BlendMode.BLEND_MASKED:
        raise RuntimeError("Expected masked fire coverage")
    if not m.get_editor_property("two_sided"):
        raise RuntimeError("Expected two-sided procedural mesh material")
    u.MaterialEditingLibrary.recompile_material(m)
    OUT["materials"].append(path)
    spark_path="/Game/VFX/Fire/Materials/M_AST_FireSpark"
    spark=u.EditorAssetLibrary.load_asset(spark_path)
    if not spark or spark.get_editor_property("blend_mode")!=u.BlendMode.BLEND_TRANSLUCENT:
        raise RuntimeError("Expected continuously translucent sparks")
    u.MaterialEditingLibrary.recompile_material(spark)
    OUT["materials"].append(spark_path)
    folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
    with open(os.path.join(folder,"material_result.json"),encoding="utf-8") as f:
        audit=json.load(f)
    skin=u.load_asset(audit["surface_asset"])
    OUT["skin_capture"]=u.ASTFireFreeFlameComponent.run_fire_surface_audit(m,skin,True)
    if not OUT["skin_capture"].startswith("PASS:"):
        raise RuntimeError("Skeletal GPU capture failed: "+OUT["skin_capture"])
    OUT["capture"]=u.ASTFireFreeFlameComponent.run_fire_v4_capture(m)
    if not OUT["capture"].startswith("PASS:"):
        raise RuntimeError("GPU capture failed: "+OUT["capture"])
    OUT["status"]="FIRE_V4_GPU_GRAPH_CHECKED_SHADER_LOG_REQUIRED"
except Exception as exc:
    OUT["errors"].append({"error":str(exc),"trace":traceback.format_exc()})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
                        "ChatGPT","NiagaraFire","FireV4")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"gpu_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,indent=2)
    print("AST_FIRE_V4_GPU "+json.dumps(OUT),flush=True)
