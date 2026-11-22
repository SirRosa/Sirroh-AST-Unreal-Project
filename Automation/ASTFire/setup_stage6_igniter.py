# -*- coding: utf-8 -*-
"""Stage 6: additive test-only fire source Blueprint with overlap ignition.

Creates /Game/VFX/Fire/Blueprints/BP_AST_FireSource_Igniter as a separate
duplicate. Never alters BP_AST_FireSource, BP_Torch_U or any gameplay map.
Run only after ASTFireRuntime with UASTFireIgnitionZoneComponent is built.
"""
import json
import os
import traceback
import unreal as u

ROOT = "/Game/VFX/Fire/Blueprints"
SOURCE = ROOT + "/BP_AST_FireSource"
TARGET = ROOT + "/BP_AST_FireSource_Igniter"
ASSET = u.EditorAssetLibrary
RESULT = {"stage": 6, "status": "STARTED", "source": SOURCE,
          "target": TARGET, "added_component": False, "errors": []}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


try:
    clazz = getattr(u, "ASTFireIgnitionZoneComponent", None)
    require(clazz is not None, "ASTFireIgnitionZoneComponent missing; rebuild plugin")
    original = ASSET.load_asset(SOURCE)
    require(original is not None, "Missing original Stage5 fire source")
    if not ASSET.does_asset_exist(TARGET):
        require(ASSET.duplicate_asset(SOURCE, TARGET),
                "Could not duplicate AST fire source into independent igniter")
    blueprint = ASSET.load_asset(TARGET)
    require(blueprint is not None, "Could not load separate igniter Blueprint")
    subsys = u.get_engine_subsystem(u.SubobjectDataSubsystem)
    funcs = u.SubobjectDataBlueprintFunctionLibrary

    def gather():
        handles = list(subsys.k2_gather_subobject_data_for_blueprint(blueprint))
        result = []
        seen_paths = set()
        for handle in handles:
            try:
                component = funcs.get_object(funcs.get_data(handle))
                if component is not None:
                    identity = str(component.get_path_name())
                    if identity not in seen_paths:
                        seen_paths.add(identity)
                        result.append((handle, component))
            except Exception:
                continue
        return handles, result

    handles, components = gather()
    require(handles, "Igniter Blueprint has no subobject handles")
    found = [obj for _, obj in components
             if obj.get_class().get_name() == "ASTFireIgnitionZoneComponent"]
    if not found:
        params = u.AddNewSubobjectParams(
            parent_handle=handles[0],
            new_class=clazz.static_class(),
            blueprint_context=blueprint)
        handle, failure = subsys.add_new_subobject(params)
        require(handle is not None, "Ignition zone subobject addition failed: " + str(failure))
        RESULT["added_component"] = True
        handles, components = gather()

    zones = [obj for _, obj in components
             if obj.get_class().get_name() == "ASTFireIgnitionZoneComponent"]
    fire = [obj for _, obj in components
            if obj.get_class().get_name() == "ASTFireReactiveComponent"]
    RESULT["zone_paths"] = [str(obj.get_path_name()) for obj in zones]
    require(len(zones) == 1, "Expected exactly one ignition zone, found "
            + str(len(zones)) + ": " + str(RESULT["zone_paths"]))
    require(len(fire) == 1, "Expected exactly one reactive fire component")
    # Native constructor already sets QueryOnly, all-object-channel overlaps,
    # 75x75x84 extent around its attachment; no movement in its constructor.
    u.BlueprintEditorLibrary.compile_blueprint(blueprint)
    require(ASSET.save_loaded_asset(blueprint, only_if_is_dirty=False),
            "Failed to save dedicated igniter Blueprint")
    RESULT["status"] = "IGNITER_CREATED_REQUIRES_CONTACT_TEST"
    RESULT["components"] = {
        "ignition_zone": zones[0].get_name(), "reactive_fire": fire[0].get_name()
    }
except Exception as exc:
    RESULT["status"] = "FAILED"
    RESULT["errors"].append({"error": str(exc),
                             "trace": traceback.format_exc()[-3000:]})
finally:
    directory = os.path.join(
        u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
        "ChatGPT", "NiagaraFire", "Stage6")
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "stage6_result.json"), "w",
              encoding="utf-8") as stream:
        json.dump(RESULT, stream, ensure_ascii=False, indent=2)
    print("AST_FIRE_STAGE6 " + json.dumps(RESULT, ensure_ascii=False), flush=True)
