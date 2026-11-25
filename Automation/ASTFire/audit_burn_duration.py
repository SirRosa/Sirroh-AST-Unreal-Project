"""Read-only audit: run real fuel clocks plus accepted visual regressions, no asset saves."""
import json, os, traceback
import unreal as u
out={"status":"FAIL","errors":[]}
try:
    skin=u.load_asset("/Game/Characters/UE5_Mannequins/Meshes/SKM_Manny")
    mat=u.load_asset("/Game/VFX/Fire/Materials/M_AST_FireTongue")
    smoke=u.load_asset("/Game/VFX/Fire/Materials/M_AST_FireSmoke")
    if not all((skin,mat,smoke)): raise RuntimeError("Missing audit assets")
    out["burn"]=u.ASTFireReactiveComponent.run_burn_duration_audit(skin)
    out["fire"]=u.ASTFireFreeFlameComponent.run_fire_v4_audit(mat)
    out["smoke"]=u.ASTFireFreeFlameComponent.run_smoke_audit(smoke)
    out["surface"]=u.ASTFireFreeFlameComponent.run_fire_surface_audit(mat,skin,False)
    if not all(out[k].startswith("PASS:") for k in ("burn","fire","smoke","surface")):
        raise RuntimeError("Native audit failed")
    out["status"]="PASS"
except Exception as exc:
    out["errors"].append(str(exc)+"\n"+traceback.format_exc())
finally:
    folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
    os.makedirs(folder,exist_ok=True)
    with open(os.path.join(folder,"burn_duration_result.json"),"w",encoding="utf-8") as f:
        json.dump(out,f,indent=2)
    print("AST_BURN_DURATION "+json.dumps(out),flush=True)
