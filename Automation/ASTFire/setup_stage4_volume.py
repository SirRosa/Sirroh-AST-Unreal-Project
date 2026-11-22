# -*- coding: utf-8 -*-
"""Stage 4: create the original material for CPU-deformed, 3D AST Fire volumes.
Only /Game/VFX/Fire/Materials and our own BP_AST_FireSource may be modified.
No PIE, gameplay automation or map edits.
"""
import json
import os
import traceback
import unreal as u

ROOT = "/Game/VFX/Fire"
NAME = "M_AST_FirePixel_Volume"
PATH = ROOT + "/Materials/" + NAME
OUT = {"stage": 4, "status": "STARTED", "asset": PATH, "errors": []}
LIB = u.EditorAssetLibrary
EDIT = u.MaterialEditingLibrary


def pin(name):
    item = u.CustomInput()
    item.set_editor_property("input_name", u.Name(name))
    return item


def require(result, message):
    if not result:
        raise RuntimeError(message)


try:
    LIB.make_directory(ROOT + "/Materials")
    asset = LIB.load_asset(PATH) if LIB.does_asset_exist(PATH) else None
    if asset is None:
        asset = u.AssetToolsHelpers.get_asset_tools().create_asset(
            NAME, ROOT + "/Materials", u.Material, u.MaterialFactoryNew())
    require(asset, "Could not load/create the AST volume material")
    asset.set_editor_property("blend_mode", u.BlendMode.BLEND_MASKED)
    asset.set_editor_property("shading_model", u.MaterialShadingModel.MSM_UNLIT)
    asset.set_editor_property("two_sided", True)
    EDIT.delete_all_material_expressions(asset)

    col = EDIT.create_material_expression(asset, u.MaterialExpressionVertexColor, -560, -180)
    uv = EDIT.create_material_expression(asset, u.MaterialExpressionTextureCoordinate, -560, -10)
    time = EDIT.create_material_expression(asset, u.MaterialExpressionTime, -560, 150)
    shader = EDIT.create_material_expression(asset, u.MaterialExpressionCustom, -170, -40)
    opaque = EDIT.create_material_expression(asset, u.MaterialExpressionCustom, -170, 220)
    require(all((col, uv, time, shader, opaque)), "Material graph creation failed")
    shader.set_editor_property("output_type", u.CustomMaterialOutputType.CMOT_FLOAT3)
    shader.set_editor_property("inputs", [pin("C"), pin("UV"), pin("T")])
    # The temperature field supplies shape and temperature. Mask empty cells.
    # Restrained *linear* emission avoids the clipped white/yellow silhouette
    # caused by Stage 4's 2.5x HDR multiplier.
    shader.set_editor_property("code",
        "float h=saturate(C.r);"
        "float2 p=floor(UV*float2(20,30))/float2(20,30);"
        "h=saturate(h+0.02*sin(T*5.3+p.x*17+C.g*12));"
        "float3 color=float3(.10,.008,.003);"
        "color=lerp(color,float3(.39,.026,.008),step(.24,h));"
        "color=lerp(color,float3(.68,.085,.013),step(.38,h));"
        "color=lerp(color,float3(.88,.23,.025),step(.56,h));"
        "color=lerp(color,float3(1.00,.47,.055),step(.76,h));"
        "color=lerp(color,float3(1.00,.70,.18),step(.91,h));"
        "return color*0.82;")
    opaque.set_editor_property("output_type", u.CustomMaterialOutputType.CMOT_FLOAT1)
    opaque.set_editor_property("inputs", [pin("C")])
    opaque.set_editor_property("code", "return step(.21,C.r);")
    for label, src, output, dest in (
        ("color", col, "", "C"),
        ("uv", uv, "", "UV"),
        ("time", time, "", "T")):
        require(EDIT.connect_material_expressions(src, output, shader, dest),
                "Disconnected shader input: " + label)
    require(EDIT.connect_material_expressions(col, "", opaque, "C"),
            "Disconnected temperature mask")
    require(EDIT.connect_material_property(
        shader, "", u.MaterialProperty.MP_EMISSIVE_COLOR), "No emissive")
    require(EDIT.connect_material_property(
        opaque, "", u.MaterialProperty.MP_OPACITY_MASK), "No mask")
    EDIT.recompile_material(asset)
    require(LIB.save_loaded_asset(asset, only_if_is_dirty=False), "Material save failed")

    # Fix only OUR fire-source blueprint. Original torches and maps are untouched.
    blueprint = LIB.load_asset(ROOT + "/Blueprints/BP_AST_FireSource")
    require(blueprint, "Fire source BP missing")
    subsystem = u.get_engine_subsystem(u.SubobjectDataSubsystem)
    funcs = u.SubobjectDataBlueprintFunctionLibrary
    modified = 0
    disabled_legacy_niagara = 0
    adjusted_lights = 0
    for handle in subsystem.k2_gather_subobject_data_for_blueprint(blueprint):
        component = funcs.get_object(funcs.get_data(handle))
        if component is None:
            continue
        if component.get_class().get_name() == "NiagaraComponent":
            # This is the standalone Niagara from Stage3, not the reactive volume.
            component.set_editor_property("auto_activate", False)
            component.set_editor_property("visible", False)
            disabled_legacy_niagara += 1
        elif component.get_class().get_name() == "PointLightComponent":
            component.set_editor_property("intensity", 450.0)
            adjusted_lights += 1
        elif component.get_class().get_name() == "ASTFireReactiveComponent":
            component.set_editor_property("enable_procedural_flames", True)
            component.set_editor_property("enable_flame_cards", False)
            component.set_editor_property("enable_niagara", False)
            component.set_editor_property("volume_material", asset)
            component.set_editor_property("light_power", 450.0)
            modified += 1
    require(modified == 1, "Expected one ASTFireReactiveComponent in fire source BP")
    require(disabled_legacy_niagara >= 1, "Expected Stage3 Niagara component to disable")
    u.BlueprintEditorLibrary.compile_blueprint(blueprint)
    require(LIB.save_loaded_asset(blueprint, only_if_is_dirty=False), "BP save failed")
    OUT.update({"status": "VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW", "modified_components": modified, "disabled_legacy_niagara": disabled_legacy_niagara, "adjusted_lights": adjusted_lights})
except Exception as exc:
    OUT["status"] = "FAILED"
    OUT["errors"].append({"message": str(exc), "trace": traceback.format_exc()[-2600:]})
finally:
    report_dir = os.path.join(
        u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
        "ChatGPT", "NiagaraFire", "Stage4")
    os.makedirs(report_dir, exist_ok=True)
    with open(os.path.join(report_dir, "stage4_result.json"), "w", encoding="utf-8") as stream:
        json.dump(OUT, stream, indent=2, ensure_ascii=False, default=str)
    print("AST_FIRE_STAGE4_RESULT " + json.dumps(OUT, default=str), flush=True)
