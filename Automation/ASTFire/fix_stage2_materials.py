# -*- coding: utf-8 -*-
"""AST Fire — naprawa trzech autorskich materiałów Niagara dla UE 5.8.1.

Skrypt działa tylko w /Game/VFX/Fire/Materials i nie modyfikuje BP_Torch_U.
Wymaga uruchomienia przez Unreal Editor Python (nie zwykłego Pythona).
Nie uruchamia renderów, PIE ani playtestów.
"""
import json
import os
import traceback
import unreal as u

ROOT = "/Game/VFX/Fire/Materials"
REPORT = {"status": "PARTIAL", "materials": {}, "errors": [], "engine_verified": False}
LIB = u.EditorAssetLibrary
EDIT = u.MaterialEditingLibrary
TOOLS = u.AssetToolsHelpers.get_asset_tools()

SHADERS = {
    "Core": (
        "float2 q=floor(UV*float2(22,34))/float2(22,34);"
        "float h=1-q.y;float c=.5+.08*sin(h*10-T*3.5)+.04*sin(h*20+T*1.7);"
        "float r=.4*pow(saturate(1-h),.68)+.015;float d=abs(q.x-c);"
        "float k=saturate(1-d/max(.02,r));"
        "float3 v=lerp(float3(.3,.04,.012),float3(1,.24,.025),step(.3,k));"
        "v=lerp(v,float3(1,.68,.10),step(.65,k));return v*3;",
        "float2 q=floor(UV*float2(22,34))/float2(22,34);"
        "float h=1-q.y;float c=.5+.08*sin(h*10-T*3.5)+.04*sin(h*20+T*1.7);"
        "float r=.4*pow(saturate(1-h),.68)+.015;float d=abs(q.x-c);"
        "return step(d,max(.02,r+.028*sin(q.x*50+T*6)))*step(.03,h);",
    ),
    "Ember": (
        "return float3(1,.48,.1)*4;",
        "float2 q=floor(UV*12)/12;"
        "return step(.33,q.x)*step(q.x,.67)*step(.33,q.y)*step(q.y,.67);",
    ),
    "Smoke": (
        "return float3(.16,.17,.14);",
        "float2 q=floor(UV*float2(14,20))/float2(14,20);"
        "float d=length(q-float2(.5+.08*sin(T+q.y*8),.5));"
        "return saturate(.34-d)*.45;",
    ),
}


def input_pin(name):
    p = u.CustomInput()
    p.set_editor_property("input_name", u.Name(name))
    return p


def require(ok, detail):
    if ok is False or ok is None:
        raise RuntimeError(detail)


def repair(kind):
    name = "M_AST_FirePixel_" + kind
    package = ROOT + "/" + name
    obj = LIB.load_asset(package) if LIB.does_asset_exist(package) else None
    if obj is None:
        LIB.make_directory(ROOT)
        obj = TOOLS.create_asset(name, ROOT, u.Material, u.MaterialFactoryNew())
    require(obj, "Cannot create or load " + package)

    obj.set_editor_property(
        "blend_mode",
        u.BlendMode.BLEND_TRANSLUCENT if kind == "Smoke"
        else u.BlendMode.BLEND_MASKED,
    )
    obj.set_editor_property("shading_model", u.MaterialShadingModel.MSM_UNLIT)
    obj.set_editor_property("two_sided", True)
    EDIT.delete_all_material_expressions(obj)

    uv = EDIT.create_material_expression(obj, u.MaterialExpressionTextureCoordinate, -520, -140)
    timer = EDIT.create_material_expression(obj, u.MaterialExpressionTime, -520, 110)
    rgb = EDIT.create_material_expression(obj, u.MaterialExpressionCustom, -190, -140)
    alpha = EDIT.create_material_expression(obj, u.MaterialExpressionCustom, -190, 150)
    if not all((uv, timer, rgb, alpha)):
        raise RuntimeError("Failed to create material expressions: " + package)

    color_code, opacity_code = SHADERS[kind]
    rgb.set_editor_property("code", color_code)
    rgb.set_editor_property("output_type", u.CustomMaterialOutputType.CMOT_FLOAT3)
    alpha.set_editor_property("code", opacity_code)
    alpha.set_editor_property("output_type", u.CustomMaterialOutputType.CMOT_FLOAT1)
    rgb.set_editor_property("inputs", [input_pin("UV"), input_pin("T")])
    alpha.set_editor_property("inputs", [input_pin("UV"), input_pin("T")])

    links = {
        "uv_color": EDIT.connect_material_expressions(uv, "", rgb, "UV"),
        "time_color": EDIT.connect_material_expressions(timer, "", rgb, "T"),
        "uv_opacity": EDIT.connect_material_expressions(uv, "", alpha, "UV"),
        "time_opacity": EDIT.connect_material_expressions(timer, "", alpha, "T"),
        "emissive": EDIT.connect_material_property(
            rgb, "", u.MaterialProperty.MP_EMISSIVE_COLOR
        ),
        "coverage": EDIT.connect_material_property(
            alpha, "", u.MaterialProperty.MP_OPACITY
            if kind == "Smoke" else u.MaterialProperty.MP_OPACITY_MASK
        ),
    }
    for label, connected in links.items():
        require(connected, "Unconnected pin " + kind + "." + label)

    EDIT.recompile_material(obj)
    require(LIB.save_loaded_asset(obj, only_if_is_dirty=False),
            "Failed to save " + package)
    REPORT["materials"][kind] = {"path": package, "saved": True, "links": links}


def main():
    for kind in ("Core", "Ember", "Smoke"):
        try:
            repair(kind)
        except Exception as exc:
            REPORT["errors"].append({
                "material": kind,
                "exception": type(exc).__name__,
                "message": str(exc),
                "traceback": traceback.format_exc()[-1800:],
            })
    REPORT["status"] = "MATERIALS_SAVED" if not REPORT["errors"] else "PARTIAL"
    REPORT["engine_verified"] = not REPORT["errors"]
    directory = os.path.join(
        str(u.Paths.project_saved_dir()), "ChatGPT", "NiagaraFire", "Stage2"
    )
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "stage2_material_fix_result.json"),
              "w", encoding="utf-8") as stream:
        json.dump(REPORT, stream, ensure_ascii=False, indent=2)
    print("AST_FIRE_MATERIAL_FIX " + json.dumps(REPORT, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
