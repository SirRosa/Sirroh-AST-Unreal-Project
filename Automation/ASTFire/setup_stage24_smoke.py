# -*- coding: utf-8 -*-
"""Stage24 original quantized gray smoke material, no stock Niagara textures."""
import json,os,traceback
import unreal as u
BASE="/Game/VFX/Fire/Materials"
PATH=BASE+"/M_AST_FireSmoke"
OUT={"stage":24,"status":"STARTED","asset":PATH,"errors":[]}
LIB=u.EditorAssetLibrary
EDIT=u.MaterialEditingLibrary
def need(x,what):
    if not x: raise RuntimeError(what)
def pin(name):
    x=u.CustomInput()
    x.set_editor_property("input_name",u.Name(name))
    return x
try:
    LIB.make_directory(BASE)
    m=LIB.load_asset(PATH) if LIB.does_asset_exist(PATH) else None
    if m is None:
        m=u.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_AST_FireSmoke",BASE,u.Material,u.MaterialFactoryNew())
    need(m,"No smoke material")
    m.set_editor_property("blend_mode",u.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model",u.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided",True)
    EDIT.delete_all_material_expressions(m)
    col=EDIT.create_material_expression(m,u.MaterialExpressionVertexColor,-500,-200)
    uv=EDIT.create_material_expression(m,u.MaterialExpressionTextureCoordinate,-500,0)
    ec=EDIT.create_material_expression(m,u.MaterialExpressionCustom,-90,-170)
    opacity=EDIT.create_material_expression(m,u.MaterialExpressionCustom,-90,110)
    need(all((col,uv,ec,opacity)),"Missing smoke shader nodes")
    ec.set_editor_property("output_type",u.CustomMaterialOutputType.CMOT_FLOAT3)
    opacity.set_editor_property("output_type",u.CustomMaterialOutputType.CMOT_FLOAT1)
    for node in (ec,opacity):
        node.set_editor_property("inputs",[pin("C"),pin("UV")])
        need(EDIT.connect_material_expressions(col,"",node,"C"),"No color input")
        need(EDIT.connect_material_expressions(uv,"",node,"UV"),"No UV input")
    ec.set_editor_property("code",
        "float2 p=(floor(UV*float2(14.,14.))+.5)/14.;"
        "float2 d=p-.5;float r=length(d)*2.;"
        "float band=step(.34,r)+step(.64,r);"
        "float3 a=float3(.108,.111,.103)-band*.023;"
        "return a+float3(.09,.028,.004)*saturate(C.r)*(.6-r*.4);")
    opacity.set_editor_property("code",
        "float2 p=(floor(UV*float2(14.,14.))+.5)/14.;"
        "float2 d=p-.5;float a=atan2(d.y,d.x);"
        "float warp=.08*sin(a*5.+C.b*19.)+.06*sin(a*9.-C.b*17.);"
        "float radius=length(d)*2.;"
        "float m=1.-smoothstep(.72+warp,.94+warp,radius);"
        "float holes=.85+.15*sin(p.x*49.+p.y*38.+C.b*17.);"
        "return saturate(C.a*m*holes);")
    need(EDIT.connect_material_property(ec,"",u.MaterialProperty.MP_EMISSIVE_COLOR),
        "No smoke emissive output")
    need(EDIT.connect_material_property(opacity,"",u.MaterialProperty.MP_OPACITY),
        "No smoke opacity output")
    EDIT.recompile_material(m)
    need(LIB.save_loaded_asset(m,only_if_is_dirty=False),"Smoke save failed")
    OUT["status"]="STAGE24_SMOKE_MATERIAL_READY_VISUAL_TEST_PENDING"
except Exception as e:
    OUT["status"]="FAILED"
    OUT["errors"].append({"error":str(e),"trace":traceback.format_exc()[-4000:]})
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(
        u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","Stage24")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"stage24_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,ensure_ascii=False,indent=2)
    print("AST_FIRE_STAGE24 "+json.dumps(OUT,ensure_ascii=False),flush=True)
