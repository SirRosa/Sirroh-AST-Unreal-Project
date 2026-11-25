"""Read-only audit: run real fuel clocks plus accepted visual regressions, no asset saves."""
import json, os, traceback
import unreal as u
out={"status":"FAIL","errors":[]}
gpu="-nullrhi" not in u.SystemLibrary.get_command_line().lower()
result_name="fire_light_gpu_result.json" if gpu else "fire_light_result.json"
try:
    skin=u.load_asset("/Game/Characters/UE5_Mannequins/Meshes/SKM_Manny")
    mat=u.load_asset("/Game/VFX/Fire/Materials/M_AST_FireTongue")
    smoke=u.load_asset("/Game/VFX/Fire/Materials/M_AST_FireSmoke")
    if not all((skin,mat,smoke)): raise RuntimeError("Missing audit assets")
    out["light"]=u.ASTFireReactiveComponent.run_fire_light_audit()
    if gpu:
        torch=u.EditorAssetLibrary.load_blueprint_class("/Game/POIKit/CarryProps/BP_AST_Torch")
        if not torch: raise RuntimeError("Actual BP_AST_Torch not found")
        out["capture"]=u.ASTFireReactiveComponent.run_fire_light_capture(torch)
        if not out["capture"].startswith("PASS:"):
            raise RuntimeError("Manual exposure capture failed: "+out["capture"])
    box=u.EditorAssetLibrary.load_blueprint_class("/Game/POIKit/CarryProps/BP_AST_Box")
    barrel=u.EditorAssetLibrary.load_blueprint_class("/Game/POIKit/CarryProps/BP_AST_Barrel")
    if not box or not barrel: raise RuntimeError("Missing actual carry prop Blueprints")
    out["contact"]=u.ASTFireIgnitionZoneComponent.run_contact_audit(box,barrel)
    beam=u.load_asset("/Game/ZL_retroCaves/Meshes/decos/Wood_Beam")
    fence=u.load_asset("/Game/ZL_retroCaves/Meshes/decos/Wood_Fence_L")
    rock=u.load_asset("/Game/ZL_retroCaves/Meshes/decos/m_deco_cave_rockA")
    if not all((beam,fence,rock)): raise RuntimeError("Missing actual wood/rock assets")
    out["wood"]=u.ASTFireIgnitionZoneComponent.run_wood_audit(beam,fence,rock)
    out["carry_visual"]=u.ASTFireFreeFlameComponent.run_carry_visual_audit(box,barrel)
    out["prop_char"]=u.ASTFireReactiveComponent.run_prop_char_audit(box,barrel)
    out["burn"]=u.ASTFireReactiveComponent.run_burn_duration_audit(skin)
    out["fire"]=u.ASTFireFreeFlameComponent.run_fire_v4_audit(mat)
    out["motion"]=u.ASTFireFreeFlameComponent.run_motion_audit(mat)
    out["smoke"]=u.ASTFireFreeFlameComponent.run_smoke_audit(smoke)
    out["surface"]=u.ASTFireFreeFlameComponent.run_fire_surface_audit(mat,skin,False)
    if not all(out[k].startswith("PASS:") for k in ("light","contact","wood","carry_visual","prop_char","burn","fire","motion","smoke","surface")):
        raise RuntimeError("Native audit failed")
    out["status"]="PASS"
except Exception as exc:
    out["errors"].append(str(exc)+"\n"+traceback.format_exc())
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,result_name),"w",encoding="utf-8") as f:
        json.dump(out,f,indent=2)
    print("AST_FIRE_LIGHT "+json.dumps(out),flush=True)
