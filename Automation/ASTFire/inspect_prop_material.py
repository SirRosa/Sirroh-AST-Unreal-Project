import unreal as u,json,os,traceback
E=u.MaterialEditingLibrary
L=u.EditorAssetLibrary
out={}
try:
    m=L.load_asset("/GASPALS/Systems/EquipmentSystem/Assets/Materials/M_Prop")
    out["class"]=m.get_class().get_name()
    if not isinstance(m,u.Material):raise RuntimeError("Not base material")
    out["properties"]={p:str(m.get_editor_property(p)) for p in ("blend_mode","shading_model","use_material_attributes","two_sided")}
    out["inputs"]={}
    for p in (u.MaterialProperty.MP_BASE_COLOR,u.MaterialProperty.MP_EMISSIVE_COLOR,u.MaterialProperty.MP_NORMAL,u.MaterialProperty.MP_ROUGHNESS):
        n=E.get_material_property_input_node(m,p)
        out["inputs"][str(p)]={"name":n.get_name(),"class":n.get_class().get_name(),"pin":E.get_material_property_input_node_output_name(m,p)} if n else None
    out["params"]={name:[str(x) for x in getattr(E,name)(m)] for name in ("get_scalar_parameter_names","get_vector_parameter_names","get_texture_parameter_names")}
    out["status"]="PASS"
except Exception:
    out["status"]="FAIL";out["error"]=traceback.format_exc()
folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
os.makedirs(folder,exist_ok=True)
with open(os.path.join(folder,"prop_material_inspect.json"),"w") as f:json.dump(out,f,indent=2)
print(json.dumps(out),flush=True)
