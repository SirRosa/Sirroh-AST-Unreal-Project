# -*- coding: utf-8 -*-
"""Stage33: five-tone density field, opaque body and true terminal alpha fade.
The native graph reads CPU-generated RGB and opacity; no custom shader or texture.
"""
import json,os,traceback
import unreal as u
BASE="/Game/VFX/Fire/Materials"
PATH=BASE+"/M_AST_FireSmoke"
OUT={"stage":33,"status":"STARTED","asset":PATH,"errors":[]}
LIB=u.EditorAssetLibrary
EDIT=u.MaterialEditingLibrary

def need(value,description):
    if not value: raise RuntimeError(description)

try:
    LIB.make_directory(BASE)
    m=LIB.load_asset(PATH) if LIB.does_asset_exist(PATH) else None
    if m is None:
        m=u.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_AST_FireSmoke",BASE,u.Material,u.MaterialFactoryNew())
    need(m,"Could not create smoke material")
    m.set_editor_property("blend_mode",u.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model",u.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided",True)
    EDIT.delete_all_material_expressions(m)
    vertex=EDIT.create_material_expression(
        m,u.MaterialExpressionVertexColor,-400,-90)
    need(vertex,"Cannot create vertex palette material")
    # UE 5.8 VertexColor exposes ["", "R", "G", "B", "A"].
    need(EDIT.connect_material_property(vertex,"",
        u.MaterialProperty.MP_EMISSIVE_COLOR),
        "Failed to connect opaque smoke RGB palette")
    need(EDIT.connect_material_property(vertex,"A",
        u.MaterialProperty.MP_OPACITY),
        "Failed to connect terminal smoke alpha")
    EDIT.recompile_material(m)
    need(LIB.save_loaded_asset(m,only_if_is_dirty=False),
         "Failed to save density smoke")
    # Call the production C++ smoke path in a transient collision world.
    audit=u.ASTFireFreeFlameComponent.run_smoke_audit(m)
    OUT["runtime_audit"]=audit
    need(audit.startswith("PASS:"),"Smoke runtime audit: "+audit)
    OUT["status"]="STAGE24_SMOKE_MATERIAL_READY_VISUAL_TEST_PENDING"
    OUT["native_expressions"]=1
    OUT["render_mode"]="DENSITY_FIVE_TONE_TERMINAL_ALPHA"
except Exception as error:
    OUT["status"]="FAILED"
    OUT["errors"].append({"error":str(error),
                          "trace":traceback.format_exc()[-4000:]})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(
        u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","Stage24")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"stage24_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,ensure_ascii=False,indent=2)
    print("AST_STAGE33_SMOKE "+json.dumps(OUT,ensure_ascii=False),flush=True)
