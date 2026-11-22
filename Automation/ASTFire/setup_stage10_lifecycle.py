# -*- coding: utf-8 -*-
"""Stage12: preserve approved pixel tongues; connect their roots procedurally.

Stage11's WPO overlay caused giant faceted crystals. NEVER recreate or
assign that material in Stage12. One dense, skin-following procedural
section uses exactly M_AST_FireTongue for both mantle and tongue ribbons.
"""
import json
import os
import traceback
import unreal as u

BASE="/Game/VFX/Fire/Materials"
OUT={"stage":12,"status":"STARTED","assets":[],"updated":[],"errors":[]}
LIB=u.EditorAssetLibrary
EDIT=u.MaterialEditingLibrary

def require(value,message):
    if not value:
        raise RuntimeError(message)

def pin(name):
    item=u.CustomInput()
    item.set_editor_property("input_name",u.Name(name))
    return item

try:
    LIB.make_directory(BASE)
    path=BASE+"/M_AST_FireTongue"
    tongue=LIB.load_asset(path) if LIB.does_asset_exist(path) else None
    if tongue is None:
        tongue=u.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_AST_FireTongue",BASE,u.Material,u.MaterialFactoryNew())
    require(tongue,"Missing shared pixel tongue material")
    tongue.set_editor_property("blend_mode",u.BlendMode.BLEND_MASKED)
    tongue.set_editor_property("shading_model",
                               u.MaterialShadingModel.MSM_UNLIT)
    tongue.set_editor_property("two_sided",True)
    EDIT.delete_all_material_expressions(tongue)

    vertex=EDIT.create_material_expression(
        tongue,u.MaterialExpressionVertexColor,-540,-170)
    uv=EDIT.create_material_expression(
        tongue,u.MaterialExpressionTextureCoordinate,-540,0)
    color=EDIT.create_material_expression(
        tongue,u.MaterialExpressionCustom,-90,-210)
    mask=EDIT.create_material_expression(
        tongue,u.MaterialExpressionCustom,-90,155)
    require(vertex and uv and color and mask,"Tongue material graph creation failed")
    color.set_editor_property("output_type",
                              u.CustomMaterialOutputType.CMOT_FLOAT3)
    mask.set_editor_property("output_type",
                             u.CustomMaterialOutputType.CMOT_FLOAT1)
    color.set_editor_property("inputs",[pin("C"),pin("UV")])
    mask.set_editor_property("inputs",[pin("C"),pin("UV")])
    palette=(
        "float3 c=float3(.075,.004,.004);"
        "c=lerp(c,float3(.25,.012,.007),step(.12,h));"
        "c=lerp(c,float3(.72,.05,.009),step(.24,h));"
        "c=lerp(c,float3(.97,.21,.015),step(.43,h));"
        "c=lerp(c,float3(1.00,.62,.055),step(.61,h));"
        "c=lerp(c,float3(1.00,.85,.28),step(.77,h));"
        "c=lerp(c,float3(1.00,.95,.78),step(.90,h));"
        "return c*1.12;"
    )
    # Identical pixel/heat palette to the user's BEST tongue iteration.
    field=(
        "float2 q=floor(UV*float2(15.,45.))/float2(15.,45.);"
        "float side=abs(q.x-.5)*2.;"
        "float ridge=.95+.05*sin(q.y*17.+C.b*19.);"
        "float h=saturate(C.r*(1.-.51*side)*ridge);"
    )
    color.set_editor_property("code",field+palette)
    mask.set_editor_property("code",field+"return step(.075,h);")
    for n,target in ((vertex,"C"),(uv,"UV")):
        require(EDIT.connect_material_expressions(n,"",color,target),
                "Tongue color input disconnected: "+target)
        require(EDIT.connect_material_expressions(n,"",mask,target),
                "Tongue mask input disconnected: "+target)
    require(EDIT.connect_material_property(
        color,"",u.MaterialProperty.MP_EMISSIVE_COLOR),
        "Tongue emissive output disconnected")
    require(EDIT.connect_material_property(
        mask,"",u.MaterialProperty.MP_OPACITY_MASK),
        "Tongue opacity mask disconnected")
    EDIT.recompile_material(tongue)
    require(LIB.save_loaded_asset(tongue,only_if_is_dirty=False),
            "Could not save M_AST_FireTongue")
    OUT["assets"].append(path)

    # The old surface asset is deliberately left on disk as a rollback
    # resource, but the pixelated 3D cloak does not use it.
    subsystem=u.get_engine_subsystem(u.SubobjectDataSubsystem)
    funcs=u.SubobjectDataBlueprintFunctionLibrary
    for name in ("BP_AST_FireSource","BP_AST_FireSource_Igniter"):
        bp=LIB.load_asset("/Game/VFX/Fire/Blueprints/"+name)
        require(bp,"Missing owned Blueprint "+name)
        visited=set()
        found=0
        for handle in subsystem.k2_gather_subobject_data_for_blueprint(bp):
            component=funcs.get_object(funcs.get_data(handle))
            if component is None:
                continue
            ident=str(component.get_path_name())
            if ident in visited:
                continue
            visited.add(ident)
            if component.get_class().get_name()!="ASTFireReactiveComponent":
                continue
            component.set_editor_property("enable_procedural_flames",True)
            component.set_editor_property("enable_surface_fire",False)
            component.set_editor_property("use_legacy_heat_grid",False)
            component.set_editor_property("enable_flame_cards",False)
            component.set_editor_property("enable_niagara",False)
            component.set_editor_property("volume_material",tongue)
            found+=1
        require(found==1,"Expected exactly one fire component on "+name)
        u.BlueprintEditorLibrary.compile_blueprint(bp)
        require(LIB.save_loaded_asset(bp,only_if_is_dirty=False),
                "Could not save "+name)
        OUT["updated"].append(name)
    OUT["status"]="STAGE10_THERMAL_MATERIALS_CONFIGURED_REQUIRES_VISUAL_TEST"
except Exception as exc:
    OUT["status"]="FAILED"
    OUT["errors"].append({
        "error":str(exc),
        "trace":traceback.format_exc()[-4000:]
    })
finally:
    folder=os.path.join(
        u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
        "ChatGPT","NiagaraFire","Stage10")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"stage10_result.json"),
              "w",encoding="utf-8") as output:
        json.dump(OUT,output,ensure_ascii=False,indent=2)
    print("AST_FIRE_STAGE12 "+json.dumps(OUT,ensure_ascii=False),flush=True)
