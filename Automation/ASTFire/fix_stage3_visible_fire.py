# -*- coding: utf-8 -*-
"""AST Fire stage 3: repair visible fire, not gameplay tests. UE 5.8.1.
Changes only /Game/VFX/Fire/Systems/NS_Fire_Master and BP_AST_FireSource.
Never edits BP_Torch_U, map levels, authored mesh/material placeholders.
"""
import os
import json
import traceback
import unreal as u

ROOT="/Game/VFX/Fire"
LIB=u.EditorAssetLibrary
RESULT={"stage":3,"status":"STARTED","parts":{},"errors":[]}

def require(condition,message):
    if not condition: raise RuntimeError(message)
def record(name,val):
    RESULT["parts"][name]=val
    print("AST_FIRE_STAGE3_"+name.upper()+" "+str(val),flush=True)
def props(obj,names):
    p={}
    for n in names:
        try:
            value=obj.get_editor_property(n)
            p[n]=str(value)[:400]
        except Exception as e:p[n]="NOT_EXPOSED:"+str(e)[:120]
    return p

def main():
    sys_path=ROOT+"/Systems/NS_Fire_Master"
    sys=LIB.load_asset(sys_path)
    require(sys is not None,"Missing Niagara master")
    build=getattr(u,"ASTFireEditorLibrary",None)
    require(build is not None,"Unreal C++ ASTFireEditorLibrary not loaded")
    f=getattr(build,"repair_visibility_bounds",None)
    require(callable(f),"New RepairVisibilityBounds function unavailable; plugin not rebuilt")
    reply=str(f(sys,1200.0))
    require(reply.startswith("OK_EMITTERS=3_"),"Bounds repair failed: "+reply)
    require(LIB.save_loaded_asset(sys,only_if_is_dirty=False),"Niagara system save failed")
    record("system",{"path":sys_path,"native":reply,"fixed_bounds":str(sys.get_editor_property("fixed_bounds"))})

    bp_path=ROOT+"/Blueprints/BP_AST_FireSource"
    bp=LIB.load_asset(bp_path)
    require(bp is not None,"Missing BP_AST_FireSource")
    subsys=u.get_engine_subsystem(u.SubobjectDataSubsystem)
    handles=subsys.k2_gather_subobject_data_for_blueprint(bp)
    fns=u.SubobjectDataBlueprintFunctionLibrary
    out=[]
    seen=set()
    for h in handles:
        data=fns.get_data(h)
        try: obj=fns.get_object(data)
        except Exception:continue
        if obj is None or obj.get_class().get_name() not in (
            "NiagaraComponent","PointLightComponent","ASTFireReactiveComponent"):
            continue
        identity=str(obj.get_path_name())
        if identity in seen:continue
        seen.add(identity)
        name=obj.get_class().get_name()
        if name=="NiagaraComponent":
            obj.set_editor_property("asset",sys)
            obj.set_editor_property("auto_activate",True)
            obj.set_editor_property("visible",True)
            tag="ASTFireOwnedFX"
        elif name=="PointLightComponent":
            obj.set_editor_property("intensity",1300.0)
            obj.set_editor_property("attenuation_radius",525.0)
            tag="ASTFireOwnedLight"
        else:
            obj.set_editor_property("b_start_lit",True)
            obj.set_editor_property("b_enable_flame_cards",True)
            obj.set_editor_property("b_enable_niagara",True)
            obj.set_editor_property("b_editor_preview",True)
            obj.set_editor_property("flame_size",1.0)
            obj.set_editor_property("flame_points",1)
            tag=None
        if tag:
            tags=list(obj.get_editor_property("component_tags"))
            if u.Name(tag) not in tags:
                tags.append(u.Name(tag))
                obj.set_editor_property("component_tags",tags)
        out.append({"class":name,"name":obj.get_name(),"props":props(obj,
            ["auto_activate","asset","intensity","b_enable_flame_cards",
             "b_start_lit","b_editor_preview","component_tags"])})
    require(any(x["class"]=="NiagaraComponent" for x in out),"Blueprint has no Niagara")
    require(any(x["class"]=="PointLightComponent" for x in out),"Blueprint has no owned light")
    require(any(x["class"]=="ASTFireReactiveComponent" for x in out),"Blueprint no reactive component")
    compilefn=getattr(u.BlueprintEditorLibrary,"compile_blueprint",None)
    if callable(compilefn):
        compilefn(bp)
        compiled="BlueprintEditorLibrary.compile_blueprint"
    else:
        compiled="API_UNAVAILABLE"
    require(LIB.save_loaded_asset(bp,only_if_is_dirty=False),"Blueprint save failed")
    record("blueprint",{"path":bp_path,"components":out,"compile_route":compiled,"saved":True})
    RESULT["status"]="CONFIGURED_EDITOR_VISUAL_REQUIRES_USER_VIEW"

try:main()
except Exception as e:
    RESULT["errors"].append({"message":str(e),"type":type(e).__name__,
        "trace":traceback.format_exc()[-2800:]})
    RESULT["status"]="FAILED"
folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
    "ChatGPT","NiagaraFire","Stage3")
os.makedirs(folder,exist_ok=True)
with open(os.path.join(folder,"stage3_result.json"),"w",encoding="utf8") as f:
    json.dump(RESULT,f,ensure_ascii=False,indent=2,default=str)
print("AST_FIRE_STAGE3_RESULT "+json.dumps(RESULT,ensure_ascii=False,default=str)[:17000],flush=True)
