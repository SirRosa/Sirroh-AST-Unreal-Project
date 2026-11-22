# -*- coding: utf-8 -*-
"""Read-only AST Fire visibility audit for UE 5.8.1 commandlet.
No PIE, no world creation, no asset edits, no saving.
"""
import os, json, traceback
import unreal as u

P = "/Game/VFX/Fire"
OUT = {"engine":u.SystemLibrary.get_engine_version(),"blueprint":{},"system":{},"emitters":{},"materials":{},"errors":[]}
L = u.EditorAssetLibrary

def prop(obj, name):
    try:
        x=obj.get_editor_property(name)
        if isinstance(x, (str,int,float,bool)) or x is None:return x
        if hasattr(x,"get_path_name"):return x.get_path_name()
        if isinstance(x,(list,tuple)):return [str(y)[:200] for y in x][:15]
        return str(x)[:400]
    except Exception as e:
        return "[NOT_EXPOSED: "+str(e)[:150]+"]"

def inspection(o,names):
    return {n:prop(o,n) for n in names}

def run():
    paths={
      "master":P+"/Systems/NS_Fire_Master",
      "core":P+"/Emitters/NE_AST_Fire_Core_V2",
      "ember":P+"/Emitters/NE_AST_Fire_Ember_V2",
      "smoke":P+"/Emitters/NE_AST_Fire_Smoke_V2",
    }
    sys=L.load_asset(paths["master"])
    if not sys:raise RuntimeError("NS_Fire_Master cannot be loaded")
    OUT["system"]={"path":sys.get_path_name(),"class":sys.get_class().get_name(),
      "properties":inspection(sys,["warmup_time","warmup_tick_count","auto_deactivate","fixed_bounds","b_is_valid","b_is_active","exposed_parameters","effect_type","emitter_handles"])}
    try:
        data=u.NiagaraFunctionLibrary.get_all_emitters(sys)
        OUT["system"]["exposed_emitters"]=[str(e) for e in data]
    except Exception as e: OUT["system"]["emitter_fetch_error"]=str(e)
    for key in ("core","ember","smoke"):
        obj=L.load_asset(paths[key])
        if not obj:continue
        OUT["emitters"][key]={"class":obj.get_class().get_name(),
          "properties":inspection(obj,["sim_target","local_space","renderer_properties",
           "renderers","event_handler_properties","emitter_spawn_script_props","emitter_update_script_props",
           "interpolated_spawning","determinism","fixed_bounds","emitter_state","b_is_enabled"])}
    for kind in ("Core","Ember","Smoke"):
        obj=L.load_asset(P+"/Materials/M_AST_FirePixel_"+kind)
        if obj is None:continue
        OUT["materials"][kind]={"properties":inspection(obj,["blend_mode","shading_model",
            "used_with_niagara_sprites","used_with_particle_sprites","two_sided","opacity_mask_clip_value"]),
            "expressions":[]}
        try:
            expressions=u.MaterialEditingLibrary.get_material_expressions(obj)
            for e in expressions:
                d={"class":e.get_class().get_name(),"name":e.get_name()}
                if e.get_class().get_name()=="MaterialExpressionCustom":
                    d.update(inspection(e,["code","output_type","inputs"]))
                OUT["materials"][kind]["expressions"].append(d)
        except Exception as ex:
            OUT["materials"][kind]["expression_audit_unavailable"]=str(ex)[:300]
    bpath=P+"/Blueprints/BP_AST_FireSource"
    blueprint=L.load_asset(bpath)
    clazz=L.load_blueprint_class(bpath)
    OUT["blueprint"]={"asset":str(blueprint),"class_path":str(clazz)}
    cdo=u.get_default_object(clazz) if clazz else None
    if cdo:
        OUT["blueprint"]["cdo"]={
          "hidden_in_game":prop(cdo,"hidden"),
          "components":[]}
        for c in cdo.get_components_by_class(u.ActorComponent):
            n=c.get_class().get_name()
            d={"name":c.get_name(),"class":n}
            d["properties"]=inspection(c,["auto_activate","visible","hidden_in_game",
                "asset","intensity","attenuation_radius","relative_location","relative_scale3d",
                "attach_parent","component_tags","active","is_editor_only",
                "rendering_enabled","owner","template","is_registered"])
            if n=="NiagaraComponent":
                d["properties"]["system_asset"]=prop(c,"asset")
            OUT["blueprint"]["cdo"]["components"].append(d)
    try:
        sub=u.get_engine_subsystem(u.SubobjectDataSubsystem)
        handles=sub.k2_gather_subobject_data_for_blueprint(blueprint)
        b=u.SubobjectDataBlueprintFunctionLibrary
        OUT["blueprint"]["subobjects"]=[]
        for h in handles:
            try:
                d=b.get_data(h)
                c=b.get_object(d)
                if c:
                    OUT["blueprint"]["subobjects"].append({"name":c.get_name(),"class":c.get_class().get_name(),
                        "properties":inspection(c,["asset","auto_activate","visible","hidden_in_game",
                            "is_editor_only","active","relative_location","relative_scale3d",
                            "intensity","attenuation_radius","component_tags","attach_parent"])})
            except Exception:pass
    except Exception as e:OUT["blueprint"]["subobject_error"]=str(e)[:500]

try:
    run()
except Exception as e:
    OUT["errors"].append({"type":type(e).__name__,"message":str(e),"traceback":traceback.format_exc()[-3500:]})
directory=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","Stage3")
os.makedirs(directory,exist_ok=True)
with open(os.path.join(directory,"visibility_audit.json"),"w",encoding="utf8") as f:json.dump(OUT,f,ensure_ascii=False,indent=2,default=str)
print("AST_FIRE_VISIBILITY_AUDIT "+json.dumps(OUT,ensure_ascii=False,default=str)[:23000],flush=True)
