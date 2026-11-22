# -*- coding: utf-8 -*-
"""Stage22: own transparent char-and-ember overlay on ORIGINAL mesh triangles.

UV coordinates are already attached to deforming SkeletalMesh and StaticMesh
geometry. No PreSkinnedPosition pixel shader, WPO, material slot replacement,
or third-party assets. The original Stage10 tongue material stays unchanged.
"""
import json,os,traceback
import unreal as u
BASE="/Game/VFX/Fire/Materials"
PATH=BASE+"/M_AST_FireChar"
OUT={"stage":22,"status":"STARTED","asset":PATH,"errors":[]}
LIB=u.EditorAssetLibrary
EDIT=u.MaterialEditingLibrary
def require(v,msg):
    if not v: raise RuntimeError(msg)
def pin(name):
    a=u.CustomInput()
    a.set_editor_property("input_name",u.Name(name))
    return a
try:
    LIB.make_directory(BASE)
    asset=LIB.load_asset(PATH) if LIB.does_asset_exist(PATH) else None
    if asset is None:
        asset=u.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_AST_FireChar",BASE,u.Material,u.MaterialFactoryNew())
    require(asset,"Char material could not be created")
    asset.set_editor_property("blend_mode",u.BlendMode.BLEND_TRANSLUCENT)
    asset.set_editor_property("shading_model",u.MaterialShadingModel.MSM_UNLIT)
    asset.set_editor_property("two_sided",True)
    # The char overlay must have an actual SkeletalMesh shader permutation.
    asset.set_editor_property("used_with_skeletal_mesh",True)
    EDIT.delete_all_material_expressions(asset)
    uv=EDIT.create_material_expression(
        asset,u.MaterialExpressionTextureCoordinate,-650,-200)
    require(uv,"UV coordinate unavailable")
    values={}
    for i,(name,initial) in enumerate((
        ("CharProgress",0.0),("CharGlow",0.0),
        ("CharSeed",1729.0),("CharTiling",34.0))):
        node=EDIT.create_material_expression(
            asset,u.MaterialExpressionScalarParameter,-680,30+i*115)
        require(node,"Missing "+name)
        node.set_editor_property("parameter_name",u.Name(name))
        node.set_editor_property("default_value",initial)
        values[name]=node
    emissive=EDIT.create_material_expression(
        asset,u.MaterialExpressionCustom,-80,-160)
    opacity=EDIT.create_material_expression(
        asset,u.MaterialExpressionCustom,-80,155)
    require(emissive and opacity,"Could not create custom shader")
    pins=[pin("UV"),pin("Progress"),pin("Glow"),pin("Seed"),pin("Tiling")]
    emissive.set_editor_property("inputs",pins)
    opacity.set_editor_property("inputs",[pin(x) for x in (
        "UV","Progress","Glow","Seed","Tiling")])
    emissive.set_editor_property(
        "output_type",u.CustomMaterialOutputType.CMOT_FLOAT3)
    opacity.set_editor_property(
        "output_type",u.CustomMaterialOutputType.CMOT_FLOAT1)
    # Irregular hard-edged patches and curved subpixel cracks in UV space.
    # Quantized input creates discrete Rain-World-like surfaces, not noise.
    field=(
        "float2 p=floor(UV*max(6.,Tiling)*3.)/3.+Seed*.00019;"
        "p+=float2(sin(p.y*.74+Seed*.02),sin(p.x*.67-Seed*.02))*.47;"
        "float n=saturate(.5+.24*sin(p.x*.69+sin(p.y*.21)*2.1)"
        "+.24*sin(p.y*.83-sin(p.x*.26)*1.7));"
        "float coverage=smoothstep(1.-saturate(Progress)*.96,"
        "1.05-saturate(Progress)*.96,n);"
        "float crackA=abs(sin(p.x*1.31+sin(p.y*.78)*2.4));"
        "float crackB=abs(sin(p.y*1.57-sin(p.x*.93)*1.8));"
        "float crack=1.-smoothstep(.027,.085,min(crackA,crackB));"
        "crack*=coverage;"
        "float hot=saturate(Glow)*crack;"
    )
    emissive.set_editor_property("code",field+
        "float3 soot=float3(.027,.030,.023);"
        "float3 core=float3(1.7,.11,.012)*hot;"
        "return soot+core;")
    opacity.set_editor_property("code",field+
        "return saturate(saturate(Progress)*(.26+.61*coverage)+hot*.22);")
    for node in (emissive,opacity):
        for key,source in (("UV",uv),("Progress",values["CharProgress"]),
            ("Glow",values["CharGlow"]),("Seed",values["CharSeed"]),
            ("Tiling",values["CharTiling"])):
            require(EDIT.connect_material_expressions(source,"",node,key),
                    "Disconnected char parameter "+key)
    require(EDIT.connect_material_property(
        emissive,"",u.MaterialProperty.MP_EMISSIVE_COLOR),
        "No emissive material output")
    require(EDIT.connect_material_property(
        opacity,"",u.MaterialProperty.MP_OPACITY),
        "No transparency output")
    EDIT.recompile_material(asset)
    require(LIB.save_loaded_asset(asset,only_if_is_dirty=False),
            "Failed to save char material")
    OUT["status"]="STAGE22_CHAR_MATERIAL_READY_VISUAL_TEST_PENDING"
except Exception as e:
    OUT["status"]="FAILED"
    OUT["errors"].append({"error":str(e),"trace":traceback.format_exc()[-4000:]})
finally:
    outdir=os.path.join(u.Paths.convert_relative_path_to_full(
        u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","Stage22")
    os.makedirs(outdir,exist_ok=True)
    with open(os.path.join(outdir,"stage22_result.json"),"w",encoding="utf-8") as f:
        json.dump(OUT,f,ensure_ascii=False,indent=2)
    print("AST_FIRE_STAGE22 "+json.dumps(OUT,ensure_ascii=False),flush=True)
