# -*- coding: utf-8 -*-
"""Fire v4: update ONLY the shared flame material, never smoke or blueprints."""
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
    # Linear RGB converted from the reference sRGB palette. No artificial
    # dark border or local UV-based radial mask. Geometry owns the pixels.
    color.set_editor_property("code",(
        "float h=saturate(C.r);"
        "float3 c=float3(.181,.019,.040);"
        "c=lerp(c,float3(.392,.045,.032),step(.18,h));"
        "c=lerp(c,float3(.638,.109,.036),step(.34,h));"
        "c=lerp(c,float3(.807,.305,.061),step(.49,h));"
        "c=lerp(c,float3(.839,.533,.156),step(.65,h));"
        "c=lerp(c,float3(.888,.723,.445),step(.81,h));"
        "return c*1.25;"
    ))
    require(EDIT.connect_material_expressions(vertex,"",color,"C"),"Color input")
    require(EDIT.connect_material_property(color,"",u.MaterialProperty.MP_EMISSIVE_COLOR),"Emissive")
    require(EDIT.connect_material_property(vertex,"A",u.MaterialProperty.MP_OPACITY_MASK),"Coverage")
    EDIT.recompile_material(mat)
    require(LIB.save_loaded_asset(mat,only_if_is_dirty=False),"Save failed")
    OUT["assets"].append(path)
    OUT["status"]="FIRE_V4_MATERIAL_READY_VISUAL_PENDING"
except Exception as exc:
    OUT["errors"].append({"error":str(exc),"trace":traceback.format_exc()})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"material_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,indent=2)
    print("AST_FIRE_V4 "+json.dumps(OUT),flush=True)
