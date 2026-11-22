# -*- coding: utf-8 -*-
"""Stage 8: one coupled flame on GPU-skinned/StaticMesh triangles + buoyant heat.

Creates an original masked overlay material. It preserves base materials, mesh
assets, maps, gameplay logic and original POIKit torches. Updates two owned BPs.
"""
import json
import os
import traceback
import unreal as u

ROOT = "/Game/VFX/Fire"
DEST = ROOT + "/Materials/M_AST_FireSurface"
OUT = {"status": "STARTED", "asset": DEST, "updated": [], "errors": []}
LIB = u.EditorAssetLibrary
EDIT = u.MaterialEditingLibrary

def require(ok, reason):
    if not ok:
        raise RuntimeError(reason)

def input_pin(name):
    x = u.CustomInput()
    x.set_editor_property("input_name", u.Name(name))
    return x

try:
    LIB.make_directory(ROOT + "/Materials")
    asset = LIB.load_asset(DEST) if LIB.does_asset_exist(DEST) else None
    if asset is None:
        asset = u.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_AST_FireSurface", ROOT + "/Materials", u.Material,
            u.MaterialFactoryNew())
    require(asset, "Surface material could not be loaded/created")
    asset.set_editor_property("blend_mode", u.BlendMode.BLEND_MASKED)
    asset.set_editor_property("shading_model", u.MaterialShadingModel.MSM_UNLIT)
    asset.set_editor_property("two_sided", True)
    EDIT.delete_all_material_expressions(asset)

    # PreSkin is stable in rest-pose local coordinates but rendered on actual
    # animated polygons after GPU skinning. It never trails sprinting characters.
    rest = EDIT.create_material_expression(
        asset, u.MaterialExpressionPreSkinnedPosition, -650, -240)
    time = EDIT.create_material_expression(
        asset, u.MaterialExpressionTime, -650, -130)
    emissive = EDIT.create_material_expression(
        asset, u.MaterialExpressionCustom, -110, -210)
    opacity = EDIT.create_material_expression(
        asset, u.MaterialExpressionCustom, -110, 120)
    require(all((rest, time, emissive, opacity)),
            "Required pre-skinned/custom/time node is missing")
    parameters = {}
    for idx, (name, initial) in enumerate((
        ("FireIntensity", 1.0), ("FireEnergy", 0.9), ("FireSeed", 1729.0)
    )):
        node = EDIT.create_material_expression(
            asset, u.MaterialExpressionScalarParameter, -650, 15 + idx * 112)
        require(node, "Scalar parameter could not be created: " + name)
        node.set_editor_property("parameter_name", u.Name(name))
        node.set_editor_property("default_value", initial)
        parameters[name] = node
    motion = EDIT.create_material_expression(
        asset, u.MaterialExpressionVectorParameter, -650, 390)
    require(motion, "Vector flow parameter could not be created")
    motion.set_editor_property("parameter_name", u.Name("FireMotion"))
    motion.set_editor_property("default_value",
                               u.LinearColor(0.0, 0.0, 0.0, 0.0))
    names = ("P", "T", "I", "E", "Seed", "M")
    emissive.set_editor_property("output_type",
                                 u.CustomMaterialOutputType.CMOT_FLOAT3)
    opacity.set_editor_property("output_type",
                                u.CustomMaterialOutputType.CMOT_FLOAT1)
    emissive.set_editor_property("inputs", [input_pin(x) for x in names])
    opacity.set_editor_property("inputs", [input_pin(x) for x in names])

    # Pixelized warm cores grow and die on the actual triangle surface.
    # Base fire remains connected, while tongue pieces are separate geometry.
    field = (
        "float3 p=floor(P/7.5);"
        "float t=T*5.7+Seed*0.017;"
        "float3 q=p-float3(M.x,M.y,M.z)*T*0.23;"
        "float l1=sin(q.x*.82+q.z*.47-t*.72);"
        "float l2=sin(q.y*1.17-q.x*.31+t*1.43);"
        "float l3=sin(q.z*.91+q.x*.38-t*1.83);"
        "float n=.50+.23*l1+.20*l2+.16*l3;"
        "float life=.72+.28*sin(t*.71+p.x*.3+p.y*.5);"
        "float h=saturate(n*life*max(.01,I)*(.76+.35*E));"
    )
    emissive.set_editor_property("code", field +
        "float3 c=float3(.12,.01,.003);"
        "c=lerp(c,float3(.58,.045,.009),step(.28,h));"
        "c=lerp(c,float3(.84,.20,.012),step(.40,h));"
        "c=lerp(c,float3(1.0,.51,.042),step(.55,h));"
        "c=lerp(c,float3(1.0,.80,.17),step(.70,h));"
        "return c*(.70+.48*E);")
    opacity.set_editor_property("code", field + "return step(.32,h);")
    for node, parameter in (
        (rest, "P"), (time, "T"),
        (parameters["FireIntensity"], "I"),
        (parameters["FireEnergy"], "E"),
        (parameters["FireSeed"], "Seed"), (motion, "M")
    ):
        require(EDIT.connect_material_expressions(node, "", emissive, parameter),
                "Disconnected emissive input " + parameter)
        require(EDIT.connect_material_expressions(node, "", opacity, parameter),
                "Disconnected opacity input " + parameter)
    require(EDIT.connect_material_property(
        emissive, "", u.MaterialProperty.MP_EMISSIVE_COLOR), "Missing emissive")
    require(EDIT.connect_material_property(
        opacity, "", u.MaterialProperty.MP_OPACITY_MASK), "Missing opacity")
    EDIT.recompile_material(asset)
    require(LIB.save_loaded_asset(asset, only_if_is_dirty=False),
            "Could not save surface material")

    # Only our own two fire source BPs may be updated.
    subsystem = u.get_engine_subsystem(u.SubobjectDataSubsystem)
    funcs = u.SubobjectDataBlueprintFunctionLibrary
    for name in ("BP_AST_FireSource", "BP_AST_FireSource_Igniter"):
        bp = LIB.load_asset(ROOT + "/Blueprints/" + name)
        require(bp, "Missing AST-owned BP " + name)
        handled = set()
        affected = 0
        for handle in subsystem.k2_gather_subobject_data_for_blueprint(bp):
            obj = funcs.get_object(funcs.get_data(handle))
            if obj is None: continue
            key = str(obj.get_path_name())
            if key in handled: continue
            handled.add(key)
            if obj.get_class().get_name() != "ASTFireReactiveComponent":
                continue
            obj.set_editor_property("enable_surface_fire", True)
            obj.set_editor_property("surface_material", asset)
            obj.set_editor_property("enable_procedural_flames", True)
            affected += 1
        require(affected == 1, "Expected exactly one ASTFireReactiveComponent in " + name)
        u.BlueprintEditorLibrary.compile_blueprint(bp)
        require(LIB.save_loaded_asset(bp, only_if_is_dirty=False),
                "Failed to save " + name)
        OUT["updated"].append(name)
    OUT["status"] = "SURFACE_FIRE_CONFIGURED_REQUIRES_VISUAL_TEST"
except Exception as ex:
    OUT["status"] = "FAILED"
    OUT["errors"].append({"error": str(ex),
                          "trace": traceback.format_exc()[-3200:]})
finally:
    directory = os.path.join(
        u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
        "ChatGPT", "NiagaraFire", "Stage8")
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "stage8_result.json"), "w",
              encoding="utf-8") as file:
        json.dump(OUT, file, ensure_ascii=False, indent=2)
    print("AST_FIRE_STAGE8 " + json.dumps(OUT, ensure_ascii=False), flush=True)
