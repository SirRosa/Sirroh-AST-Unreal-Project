"""Create a project-owned derivative of the actual barrel material; never modify the source."""
import unreal as u, os,json,traceback
E=u.MaterialEditingLibrary
L=u.EditorAssetLibrary
src="/Game/ZL_retroCaves/Materials/mat_cave_decos"
dst="/Game/VFX/Fire/Materials/M_AST_FireCharCaveProp"
out={"status":"FAIL"}
def require(v,msg):
    if not v:raise RuntimeError(msg)
def node(m,cls,x,y):
    n=E.create_material_expression(m,cls,x,y)
    require(n,"Cannot create "+str(cls))
    return n
def connect(a,p,b,q):
    require(E.connect_material_expressions(a,p,b,q),"Connect "+q)
def get(m,p):
    n=E.get_material_property_input_node(m,p)
    return n,E.get_material_property_input_node_output_name(m,p) if n else ""
def param(m,name,default,y):
    n=node(m,u.MaterialExpressionScalarParameter,-800,y)
    n.set_editor_property("parameter_name",u.Name(name))
    n.set_editor_property("default_value",default)
    return n
def custom(m,code,inputs,y,outtype):
    n=node(m,u.MaterialExpressionCustom,-350,y)
    n.set_editor_property("code",code)
    n.set_editor_property("output_type",outtype)
    pins=[]
    for name,a,p in inputs:
        q=u.CustomInput();q.set_editor_property("input_name",u.Name(name));pins.append(q)
    n.set_editor_property("inputs",pins)
    for name,a,p in inputs:connect(a,p,n,name)
    return n
try:
    original=L.load_asset(src)
    require(isinstance(original,u.Material),"Cave decor must be a base material")
    require(not original.get_editor_property("use_material_attributes"),"Unsupported attributes")
    require(original.get_editor_property("blend_mode")==u.BlendMode.BLEND_OPAQUE,"Unsupported blend")

    bp=u.MaterialProperty.MP_BASE_COLOR
    ep=u.MaterialProperty.MP_EMISSIVE_COLOR
    out["source"]={"shading":str(original.get_editor_property("shading_model")),"base":str(get(original,bp)[0]),"emissive":str(get(original,ep)[0]),"params":[str(x) for x in E.get_scalar_parameter_names(original)]}
    require(get(original,bp)[0] or get(original,ep)[0],"No authored color input")
    if L.does_asset_exist(dst):
        m=L.load_asset(dst)
        candidates=[n for n in u.ObjectIterator(u.MaterialExpressionCustom) if n.get_outer()==m and "return Original.rgb*soot+ember;" in n.get_editor_property("code")]
        require(len(candidates)==1,"Unexpected prior derivative emissive graph")
        n=candidates[0]
        n.set_editor_property("code",n.get_editor_property("code").replace("return Original.rgb*soot+ember;","return Original.rgb*(1.-saturate(Progress*3.))+ember;"))
        E.recompile_material(m)
        require(L.save_loaded_asset(m,only_if_is_dirty=False),"Save failed")
        out["asset"]=dst;out["status"]="PASS"
        # Skip creation on an existing, verified derivative.
        raise StopIteration()
    m=L.duplicate_asset(src,dst)
    require(m,"Duplicate failed")
    b,bpin=get(m,bp);e,epin=get(m,ep)
    uv=node(m,u.MaterialExpressionTextureCoordinate,-800,800)
    progress=param(m,"ASTPropCharProgress",0.,940)
    glow=param(m,"ASTPropCharGlow",0.,1060)
    seed=param(m,"ASTPropCharSeed",0.,1180)
    inputs=[("UV",uv,""),("Progress",progress,""),("Glow",glow,""),("Seed",seed,"")]
    field=("float2 p=floor(UV*90.)/3.+Seed*.00019;"
        "p+=float2(sin(p.y*.74+Seed*.02),sin(p.x*.67-Seed*.02))*.47;"
        "float n=saturate(.5+.24*sin(p.x*.69+sin(p.y*.21)*2.1)+.24*sin(p.y*.83-sin(p.x*.26)*1.7));"
        "float coverage=saturate((n-(.68-saturate(Progress)*.78))*4.);"
        "float crack=1.-smoothstep(.035,.15,min(abs(sin(p.x*1.31+sin(p.y*.78)*2.4)),abs(sin(p.y*1.57-sin(p.x*.93)*1.8))));"
        "float soot=1.-saturate(Progress)*(.54+.28*coverage);"
        "float3 ember=float3(.65,.025,.002)*saturate(Glow)*crack*coverage*saturate(Progress*3.);")
    if b:
        dark=custom(m,field+"return Base.rgb*soot;",[("Base",b,bpin)]+inputs,800,u.CustomMaterialOutputType.CMOT_FLOAT3)
        require(E.connect_material_property(dark,"",bp),"Base color connect")
    if e:
        emit=custom(m,field+"return Original.rgb*(1.-saturate(Progress*3.))+ember;",[("Original",e,epin)]+inputs,1150,u.CustomMaterialOutputType.CMOT_FLOAT3)
    else:
        emit=custom(m,field+"return ember;",inputs,1150,u.CustomMaterialOutputType.CMOT_FLOAT3)
    require(E.connect_material_property(emit,"",ep),"Emission connect")
    E.recompile_material(m)
    require(L.save_loaded_asset(m,only_if_is_dirty=False),"Save failed")
    out["asset"]=dst;out["status"]="PASS"
except StopIteration:
    pass
except Exception:
    out["error"]=traceback.format_exc()
folder=os.path.join(u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),"ChatGPT","NiagaraFire","FireV4")
os.makedirs(folder,exist_ok=True)
with open(os.path.join(folder,"char_cave_prop_material.json"),"w") as f:json.dump(out,f,indent=2)
print(json.dumps(out),flush=True)
