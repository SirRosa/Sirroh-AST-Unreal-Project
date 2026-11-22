# -*- coding: utf-8 -*-
"""Finalize AST Fire Stage 2 in Unreal Editor 5.8.1 (no PIE/render tests).

Run *after* the ASTFireRuntime plugin has been compiled and enabled.
Uses only /Game/VFX/Fire assets; never modifies BP_Torch_U or source placeholders.
Idempotent: preserves existing configured emitters and backs up factory-created
blank emitters before replacing them with built-in Niagara Fountain templates.
"""
import json
import os
import sys
import traceback
import unreal as u

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import fix_stage2_materials as materials

ROOT = "/Game/VFX/Fire"
NAMES = ("Core", "Ember", "Smoke")
ASSET_LIB = u.EditorAssetLibrary
REPORT = {
    "phase": "AST Fire stage 2 finalization",
    "engine": str(u.SystemLibrary.get_engine_version()),
    "status": "STARTED",
    "steps": {},
    "errors": [],
    "warning": [],
    "source_blueprint_untouched": "/Game/POIKit/Underground/Actors/BP_Torch_U",
}


def log_step(name, payload):
    REPORT["steps"][name] = payload
    u.log("ASTFIRE " + name + " " + json.dumps(payload, default=str))


def load(path):
    result = ASSET_LIB.load_asset(path)
    if result is None:
        raise RuntimeError("Missing or unloadable asset: " + path)
    return result


def require(cond, description):
    if not cond:
        raise RuntimeError(description)


def validate_plugin():
    library = getattr(u, "ASTFireEditorLibrary", None)
    runtime = getattr(u, "ASTFireReactiveComponent", None)
    require(library is not None, "ASTFireEditor plugin is not loaded")
    require(runtime is not None, "ASTFireRuntime plugin is not loaded")
    require(callable(getattr(library, "build_system", None)),
            "ASTFireEditorLibrary.build_system is not exposed to Python")
    log_step("plugin", {"editor_api": True, "runtime_component": True})
    return library, runtime


def create_materials():
    result = {}
    for kind in NAMES:
        materials.repair(kind)
        result[kind] = materials.REPORT["materials"][kind]
    log_step("materials", result)
    return {
        kind: load(ROOT + "/Materials/M_AST_FirePixel_" + kind)
        for kind in NAMES
    }


def load_native_emitter(template):
    # EditorAssetLibrary.does_asset_exist() can report False for hidden
    # engine-plugin templates in UE 5.8 commandlets, even if the uasset
    # physically exists. Loading by object path is more reliable.
    attempts = []
    for candidate in (template, template + ".Fountain"):
        try:
            obj = u.load_asset(candidate)
            if obj is not None:
                require(isinstance(obj, u.NiagaraEmitter),
                        "Template loaded but has wrong class: " + str(obj.get_class().get_name()))
                return obj
            attempts.append(candidate + " returned None")
        except Exception as exc:
            attempts.append(candidate + ": " + str(exc)[:260])

    registry = u.AssetRegistryHelpers.get_asset_registry()
    try:
        registry.scan_paths_synchronous(["/Niagara"], True)
    except Exception as exc:
        attempts.append("registry scan failed: " + str(exc)[:260])
    try:
        obj = u.load_asset(template)
        if isinstance(obj, u.NiagaraEmitter):
            return obj
    except Exception as exc:
        attempts.append("after registry scan: " + str(exc)[:260])

    raise RuntimeError(
        "Engine Niagara Fountain.uasset exists on disk but cannot be loaded "
        "through the /Niagara mount: " + "; ".join(attempts)
    )


def prepare_emitter(kind):
    destination = ROOT + "/Emitters/NE_AST_Fire_" + kind + "_V2"
    if ASSET_LIB.does_asset_exist(destination):
        obj = load(destination)
        require(isinstance(obj, u.NiagaraEmitter),
                "Existing V2 asset is not NiagaraEmitter: " + destination)
        return obj, "existing-V2"

    template = "/Niagara/DefaultAssets/Templates/Emitters/Fountain"
    source = load_native_emitter(template)
    target_name = "NE_AST_Fire_" + kind + "_V2"
    tools = u.AssetToolsHelpers.get_asset_tools()
    copied = None
    try:
        copied = tools.duplicate_asset(target_name, ROOT + "/Emitters", source)
    except Exception as exc:
        REPORT["warning"].append(
            "AssetTools duplicate " + kind + ": " + str(exc)[:260]
        )
    if copied is None:
        try:
            if ASSET_LIB.duplicate_asset(template, destination):
                copied = load(destination)
        except Exception as exc:
            REPORT["warning"].append(
                "EditorAssetLibrary duplicate " + kind + ": " + str(exc)[:260]
            )
    require(isinstance(copied, u.NiagaraEmitter),
            "Failed to duplicate native Niagara template: " + destination)
    require(ASSET_LIB.save_loaded_asset(copied, only_if_is_dirty=False),
            "Could not save copied Niagara emitter: " + destination)
    return copied, "duplicated-native-Fountain"


def prepare_emitters():
    out = {}
    result = {}
    for kind in NAMES:
        obj, mode = prepare_emitter(kind)
        out[kind] = obj
        result[kind] = {"path": obj.get_path_name(), "operation": mode}
    log_step("emitters", result)
    return out


def count_emitters(system):
    # Native Niagara API available in prior UE 5.8.1 audit.
    return len(u.NiagaraFunctionLibrary.get_all_emitters(system))


def configure_system(editor_lib, emitters, mats):
    system = load(ROOT + "/Systems/NS_Fire_Master")
    before = count_emitters(system)
    if before == 0:
        response = str(editor_lib.build_system(
            system, emitters["Core"], emitters["Ember"], emitters["Smoke"],
            mats["Core"], mats["Ember"], mats["Smoke"]
        ))
        require(response == "OK_3_EMITTERS" or response == "ALREADY_CONFIGURED",
                "Native Niagara builder reported: " + response)
    else:
        response = "ALREADY_CONFIGURED"
    after = count_emitters(system)
    require(after == 3,
            "NS_Fire_Master must contain exactly 3 emitters, actual " + str(after))
    for obj in emitters.values():
        require(ASSET_LIB.save_loaded_asset(obj, only_if_is_dirty=False),
                "Emitter save failed: " + obj.get_name())
    require(ASSET_LIB.save_loaded_asset(system, only_if_is_dirty=False),
            "NS_Fire_Master could not be saved")
    log_step("system", {
        "native_result": response, "emitters_before": before, "emitters_after": after,
        "path": system.get_path_name(), "saved": True,
    })
    return system


def get_blueprint_subsystem():
    target = u.SubobjectDataSubsystem
    for method in (u.get_engine_subsystem, u.get_editor_subsystem):
        try:
            obj = method(target)
            if obj is not None:
                return obj
        except Exception:
            continue
    raise RuntimeError("Cannot access SubobjectDataSubsystem")


def configure_blueprint(system, runtime_type):
    bp = load(ROOT + "/Blueprints/BP_AST_FireSource")
    subsystem = get_blueprint_subsystem()
    handles = list(subsystem.k2_gather_subobject_data_for_blueprint(bp))
    require(handles, "Blueprint has no subobject handles")
    fns = u.SubobjectDataBlueprintFunctionLibrary
    by_type = {}
    for handle in handles:
        try:
            data = fns.get_data(handle)
            component = fns.get_object(data)
            if component is not None:
                by_type[component.get_class().get_name()] = component
        except Exception:
            pass
    niagara = by_type.get("NiagaraComponent")
    light = by_type.get("PointLightComponent")
    require(niagara is not None, "BP_AST_FireSource is missing NiagaraComponent")
    require(light is not None, "BP_AST_FireSource is missing PointLightComponent")
    niagara.set_editor_property("asset", system)
    # This PointLight belongs to the *fire object*, not BP_Torch_U.
    light.set_editor_property("intensity", 1300.0)
    light.set_editor_property("attenuation_radius", 525.0)
    light.set_editor_property("cast_shadows", False)
    added = False
    if by_type.get("ASTFireReactiveComponent") is None:
        params = u.AddNewSubobjectParams(
            parent_handle=handles[0],
            new_class=runtime_type.static_class(),
            blueprint_context=bp,
        )
        result = subsystem.add_new_subobject(params)
        if isinstance(result, tuple):
            handle, failure = result
            require(handle is not None, "Subobject add failed: " + str(failure))
        added = True
    require(ASSET_LIB.save_loaded_asset(bp, only_if_is_dirty=False),
            "Failed to save BP_AST_FireSource")
    log_step("fire_actor", {
        "niagara_system": system.get_path_name(), "own_point_light": True,
        "reactive_added": added, "saved": True,
        "original_torch_modified": False,
    })


def persist():
    directory = os.path.join(
        u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
        "ChatGPT", "NiagaraFire", "Stage2"
    )
    os.makedirs(directory, exist_ok=True)
    output = os.path.join(directory, "stage2_complete_result.json")
    with open(output, "w", encoding="utf-8") as f:
        json.dump(REPORT, f, ensure_ascii=False, indent=2, default=str)
    print("AST_FIRE_STAGE2_COMPLETE " + json.dumps(REPORT, ensure_ascii=False, default=str), flush=True)


def main():
    phases = [
        ("plugin", validate_plugin),
        ("materials", create_materials),
        ("emitters", prepare_emitters),
    ]
    state = {}
    for name, function in phases:
        try:
            state[name] = function()
        except Exception as exc:
            REPORT["errors"].append({
                "phase": name, "type": type(exc).__name__,
                "message": str(exc), "traceback": traceback.format_exc()[-2200:]
            })
            REPORT["status"] = "BLOCKED_" + name.upper()
            persist()
            return
    for name, function in [
        ("system", lambda: configure_system(
            state["plugin"][0], state["emitters"], state["materials"])),
        ("blueprint", lambda: configure_blueprint(
            state["system"], state["plugin"][1])),
    ]:
        try:
            state[name] = function()
        except Exception as exc:
            REPORT["errors"].append({
                "phase": name, "type": type(exc).__name__,
                "message": str(exc), "traceback": traceback.format_exc()[-2200:]
            })
            REPORT["status"] = "BLOCKED_" + name.upper()
            persist()
            return
    REPORT["status"] = "ASSETS_ASSEMBLED_REQUIRES_VISUAL_VALIDATION"
    REPORT["warning"].append(
        "Stock Fountain simulation and exposed User parameters still need artistic/runtime validation."
    )
    persist()


if __name__ == "__main__":
    main()
