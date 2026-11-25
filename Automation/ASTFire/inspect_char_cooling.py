import unreal as u, json, os, traceback
E=u.MaterialEditingLibrary
L=u.EditorAssetLibrary
out={}
try:
    m=L.load_asset("/Game/VFX/Fire/Materials/M_AST_FireChar")
    out["char"]={p:str(m.get_editor_property(p)) for p in ("blend_mode","shading_model","two_sided","used_with_skeletal_mesh")}
    out["char"]["nodes"]=[]
    for n in u.ObjectIterator(u.MaterialExpressionCustom):
        if n.get_outer()==m:out["char"]["nodes"].append(n.get_editor_property("code"))
    out["props"]=[]
    for name in ("BP_AST_Box","BP_AST_Barrel"):
        cls=L.load_blueprint_class("/Game/POIKit/CarryProps/"+name)
        cdo=u.get_default_object(cls)
        root=cdo.get_editor_property("root_component")
        mesh=root.get_editor_property("static_mesh")
        row={"name":name,"mesh":mesh.get_path_name(),"nanite":str(mesh.get_editor_property("nanite_settings").get_editor_property("enabled")),"materials":[]}
        for slot in mesh.get_editor_property("static_materials"):
            mi=slot.get_editor_property("material_interface")
            row["materials"].append(mi.get_path_name() if mi else None)
        out["props"].append(row)
    out["status"]="PASS"
except Exception:
    out["status"]="FAIL";out["error"]=traceback.format_exc()
folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
os.makedirs(folder,exist_ok=True)
with open(os.path.join(folder,"char_cooling_inspect.json"),"w") as f:json.dump(out,f,indent=2)
print(json.dumps(out),flush=True)
