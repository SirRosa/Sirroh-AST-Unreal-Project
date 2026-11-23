# -*- coding: utf-8 -*-
"""Stage30: separately masked, emissive high-contrast heat-pixel sparks."""
import json,os,traceback
import unreal as u
BASE="/Game/VFX/Fire/Materials"
PATH=BASE+"/M_AST_FireSpark"
OUT={"stage":30,"status":"STARTED","asset":PATH,"errors":[]}
LIB=u.EditorAssetLibrary
EDIT=u.MaterialEditingLibrary

def need(x,msg):
    if not x: raise RuntimeError(msg)

try:
    LIB.make_directory(BASE)
    m=LIB.load_asset(PATH) if LIB.does_asset_exist(PATH) else None
    if m is None:
        m=u.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_AST_FireSpark",BASE,u.Material,u.MaterialFactoryNew())
    need(m,"Cannot create spark material")
    m.set_editor_property("blend_mode",u.BlendMode.BLEND_MASKED)
    m.set_editor_property("shading_model",u.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided",True)
    EDIT.delete_all_material_expressions(m)
    vertex=EDIT.create_material_expression(
        m,u.MaterialExpressionVertexColor,-460,-180)
    gain=EDIT.create_material_expression(
        m,u.MaterialExpressionMultiply,-150,-190)
    boost=EDIT.create_material_expression(
        m,u.MaterialExpressionConstant,-440,30)
    need(vertex and gain and boost,"Missing spark material nodes")
    boost.set_editor_property("r",2.8)
    need(EDIT.connect_material_expressions(vertex,"",gain,"A"),
         "Disconnected spark palette")
    need(EDIT.connect_material_expressions(boost,"",gain,"B"),
         "Disconnected spark heat multiplier")
    need(EDIT.connect_material_property(gain,"",
         u.MaterialProperty.MP_EMISSIVE_COLOR),"No spark emissive")
    need(EDIT.connect_material_property(vertex,"A",
         u.MaterialProperty.MP_OPACITY_MASK),"No spark hard-edge visibility")
    EDIT.recompile_material(m)
    need(LIB.save_loaded_asset(m,only_if_is_dirty=False),
         "Could not save spark material")
    OUT["status"]="STAGE30_SPARK_MATERIAL_READY_VISUAL_TEST_PENDING"
    OUT["native_expressions"]=3
except Exception as err:
    OUT["status"]="FAILED"
    OUT["errors"].append({"error":str(err),"trace":traceback.format_exc()[-4000:]})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(
        u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","Stage30")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"stage30_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,ensure_ascii=False,indent=2)
    print("AST_STAGE30_SPARK "+json.dumps(OUT,ensure_ascii=False),flush=True)
