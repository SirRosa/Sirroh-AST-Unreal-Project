# -*- coding: utf-8 -*-
"""Stage27: shader-safe Rain-World-style smoke without Custom HLSL.

The old two MaterialExpressionCustom nodes could fail GPU permutations and
render Unreal's checkerboard fallback. Use only native material graph nodes.
The 5x5 CPU mesh owns deformation, pixel-band density and life fading.
"""
import json,os,traceback
import unreal as u

BASE="/Game/VFX/Fire/Materials"
PATH=BASE+"/M_AST_FireSmoke"
OUT={"stage":27,"status":"STARTED","asset":PATH,"errors":[]}
LIB=u.EditorAssetLibrary
EDIT=u.MaterialEditingLibrary

def need(x,reason):
    if not x: raise RuntimeError(reason)

try:
    LIB.make_directory(BASE)
    m=LIB.load_asset(PATH) if LIB.does_asset_exist(PATH) else None
    if m is None:
        m=u.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_AST_FireSmoke",BASE,u.Material,u.MaterialFactoryNew())
    need(m,"Smoke material creation failed")
    m.set_editor_property("blend_mode",u.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model",u.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided",True)
    EDIT.delete_all_material_expressions(m)

    vertex=EDIT.create_material_expression(
        m,u.MaterialExpressionVertexColor,-430,-150)
    alpha=EDIT.create_material_expression(
        m,u.MaterialExpressionComponentMask,-190,60)
    grey=EDIT.create_material_expression(
        m,u.MaterialExpressionConstant3Vector,-190,-180)
    need(vertex and alpha and grey,"Native smoke material expressions missing")
    alpha.set_editor_property("r",False)
    alpha.set_editor_property("g",False)
    alpha.set_editor_property("b",False)
    alpha.set_editor_property("a",True)
    grey.set_editor_property("constant",u.LinearColor(.14,.145,.136,1.))
    need(EDIT.connect_material_expressions(vertex,"",alpha,"Input"),
         "Failed to route smoke vertex opacity")
    need(EDIT.connect_material_property(grey,"",
        u.MaterialProperty.MP_EMISSIVE_COLOR),"No smoke color output")
    need(EDIT.connect_material_property(alpha,"",
        u.MaterialProperty.MP_OPACITY),"No smoke opacity output")

    # Require a completely authored material and save the graph once.
    EDIT.recompile_material(m)
    need(LIB.save_loaded_asset(m,only_if_is_dirty=False),
         "Smoke material save failed")
    OUT["status"]="STAGE24_SMOKE_MATERIAL_READY_VISUAL_TEST_PENDING"
    OUT["native_expressions"]=3
except Exception as exc:
    OUT["status"]="FAILED"
    OUT["errors"].append({"error":str(exc),"trace":traceback.format_exc()[-4000:]})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(
        u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","Stage24")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"stage24_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,ensure_ascii=False,indent=2)
    print("AST_FIRE_STAGE27_SMOKE "+json.dumps(OUT,ensure_ascii=False),flush=True)
