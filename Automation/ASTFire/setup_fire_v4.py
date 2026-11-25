# -*- coding: utf-8 -*-
"""Fire v4: update flame and spark materials only; never smoke or blueprints."""
import json, os, traceback
import unreal as u
LIB=u.EditorAssetLibrary
EDIT=u.MaterialEditingLibrary
OUT={"status":"FAILED","assets":[],"errors":[]}
def require(value,message):
    if not value:
        raise RuntimeError(message)
try:
    path="/Game/VFX/Fire/Materials/M_AST_FireTongue"
    mat=LIB.load_asset(path)
    require(mat,"Existing flame material missing")
    mat.set_editor_property("blend_mode",u.BlendMode.BLEND_MASKED)
    mat.set_editor_property("shading_model",u.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided",True)
    EDIT.delete_all_material_expressions(mat)
    vertex=EDIT.create_material_expression(mat,u.MaterialExpressionVertexColor,-400,0)
    color=EDIT.create_material_expression(mat,u.MaterialExpressionCustom,-80,-100)
    require(vertex and color,"Missing expressions")
    pin=u.CustomInput()
    pin.set_editor_property("input_name",u.Name("C"))
    color.set_editor_property("inputs",[pin])
    color.set_editor_property("output_type",u.CustomMaterialOutputType.CMOT_FLOAT3)
    # Exact seven colours sampled from the supplied reference, linearized.
    # The vertex red channel stores a discrete tone index. No interpolation
    # across a cell and no dependency on independently stretched UVs.
    color.set_editor_property("code",(
        "float h=floor(saturate(C.r)*7.);"
        "float3 c=float3(0.052861,0.012286,0.030713);"
        "c=lerp(c,float3(0.177888,0.017642,0.039546),step(1.,h));"
        "c=lerp(c,float3(0.376262,0.029557,0.029557),step(2.,h));"
        "c=lerp(c,float3(0.623960,0.095307,0.045186),step(3.,h));"
        "c=lerp(c,float3(0.701102,0.238398,0.048172),step(4.,h));"
        "c=lerp(c,float3(0.806952,0.533276,0.162029),step(5.,h));"
        "c=lerp(c,float3(0.799103,0.665387,0.450786),step(6.,h));"
        "return c;"
    ))
    require(EDIT.connect_material_expressions(vertex,"",color,"C"),"Color input")
    require(EDIT.connect_material_property(color,"",u.MaterialProperty.MP_EMISSIVE_COLOR),"Emissive")
    require(EDIT.connect_material_property(vertex,"A",u.MaterialProperty.MP_OPACITY_MASK),"Coverage")
    EDIT.recompile_material(mat)
    require(LIB.save_loaded_asset(mat,only_if_is_dirty=False),"Save failed")
    OUT["assets"].append(path)
    spark_path="/Game/VFX/Fire/Materials/M_AST_FireSpark"
    spark=LIB.load_asset(spark_path)
    require(spark,"Existing spark material missing")
    spark.set_editor_property("blend_mode",u.BlendMode.BLEND_TRANSLUCENT)
    spark.set_editor_property("shading_model",u.MaterialShadingModel.MSM_UNLIT)
    spark.set_editor_property("two_sided",True)
    EDIT.delete_all_material_expressions(spark)
    sv=EDIT.create_material_expression(spark,u.MaterialExpressionVertexColor,-400,0)
    gain=EDIT.create_material_expression(spark,u.MaterialExpressionMultiply,-120,-100)
    boost=EDIT.create_material_expression(spark,u.MaterialExpressionConstant,-400,150)
    require(sv and gain and boost,"Spark nodes missing")
    boost.set_editor_property("r",2.8)
    require(EDIT.connect_material_expressions(sv,"",gain,"A"),"Spark RGB")
    require(EDIT.connect_material_expressions(boost,"",gain,"B"),"Spark gain")
    require(EDIT.connect_material_property(gain,"",u.MaterialProperty.MP_EMISSIVE_COLOR),"Spark emissive")
    require(EDIT.connect_material_property(sv,"A",u.MaterialProperty.MP_OPACITY),"Spark continuous opacity")
    EDIT.recompile_material(spark)
    require(LIB.save_loaded_asset(spark,only_if_is_dirty=False),"Spark save failed")
    OUT["assets"].append(spark_path)
    # Use an actual project character asset, not a primitive masquerading as skin.
    registry=u.AssetRegistryHelpers.get_asset_registry()
    mesh_assets=registry.get_assets_by_class(u.TopLevelAssetPath("/Script/Engine","SkeletalMesh"),True)
    names=("skm_manny","skm_quinn","sk_mannequin","skm_manny_simple","skm_quinn_simple")
    candidates=[a for a in mesh_assets if str(a.asset_name).lower() in names or any(v in str(a.asset_name).lower() for v in ('manny','quinn','mannequin'))]
    candidates.sort(key=lambda a:(not str(a.package_name).startswith('/Game/'), names.index(str(a.asset_name).lower()) if str(a.asset_name).lower() in names else len(names),str(a.package_name)))
    require(candidates,"No mannequin skeletal mesh found for mandatory regression test")
    skin=candidates[0].get_asset()
    OUT["surface_audit"]=u.ASTFireFreeFlameComponent.run_fire_surface_audit(mat,skin)
    require(OUT["surface_audit"].startswith("PASS:"),"Skeletal mantle regression: "+OUT["surface_audit"])
    OUT["fire_audit"]=u.ASTFireFreeFlameComponent.run_fire_v4_audit(mat)
    smoke=LIB.load_asset("/Game/VFX/Fire/Materials/M_AST_FireSmoke")
    require(smoke,"Smoke material missing")
    OUT["smoke_audit"]=u.ASTFireFreeFlameComponent.run_smoke_audit(smoke)
    require(OUT["fire_audit"].startswith("PASS:"),"Fire audit failed: "+OUT["fire_audit"])
    require(OUT["smoke_audit"].startswith("PASS:"),"Smoke audit failed: "+OUT["smoke_audit"])
    OUT["status"]="FIRE_V4_MATERIAL_READY_VISUAL_PENDING"
except Exception as exc:
    OUT["errors"].append({"error":str(exc),"trace":traceback.format_exc()})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"material_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,indent=2)
    print("AST_FIRE_V4 "+json.dumps(OUT),flush=True)
