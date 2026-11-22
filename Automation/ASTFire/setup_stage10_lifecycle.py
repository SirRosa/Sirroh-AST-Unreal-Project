# -*- coding: utf-8 -*-
"""Stage10: coupled real mesh burn coverage + temperature palette for free tongues.

Creates two NEW AST-owned materials; does not touch Stage4/8 materials, maps,
unrelated actors, gameplay meshes or original AST material slots.
"""
import json
import os
import traceback
import unreal as u

BASE = "/Game/VFX/Fire/Materials"
OUT = {"stage": 10, "status": "STARTED", "assets": [], "updated": [],
       "errors": []}
EDIT = u.MaterialEditingLibrary
LIB = u.EditorAssetLibrary
PARAMETERS = [
    "FireIntensity", "FireEnergy", "FireSpread", "FireSeed",
]

def require(ok, reason):
    if not ok:
        raise RuntimeError(reason)

def pin(name):
    item = u.CustomInput()
    item.set_editor_property("input_name", u.Name(name))
    return item

def new_material(name):
    path = BASE + "/" + name
    asset = LIB.load_asset(path) if LIB.does_asset_exist(path) else None
    if asset is None:
        asset = u.AssetToolsHelpers.get_asset_tools().create_asset(
            name, BASE, u.Material, u.MaterialFactoryNew())
    require(asset, "Material creation failed: " + path)
    asset.set_editor_property("blend_mode", u.BlendMode.BLEND_MASKED)
    asset.set_editor_property("shading_model",
                              u.MaterialShadingModel.MSM_UNLIT)
    asset.set_editor_property("two_sided", True)
    EDIT.delete_all_material_expressions(asset)
    OUT["assets"].append(path)
    return asset

def custom(asset, inputs, code_color, code_mask):
    c = EDIT.create_material_expression(
        asset, u.MaterialExpressionCustom, -90, -210)
    a = EDIT.create_material_expression(
        asset, u.MaterialExpressionCustom, -90, 155)
    require(c and a, "Custom nodes missing")
    c.set_editor_property("output_type",
                          u.CustomMaterialOutputType.CMOT_FLOAT3)
    a.set_editor_property("output_type",
                          u.CustomMaterialOutputType.CMOT_FLOAT1)
    c.set_editor_property("inputs", [pin(x) for x in inputs])
    a.set_editor_property("inputs", [pin(x) for x in inputs])
    c.set_editor_property("code", code_color)
    a.set_editor_property("code", code_mask)
    return c, a

def connect(asset, node, dest, color, opacity):
    require(EDIT.connect_material_expressions(node, "", color, dest),
            "Emissive input disconnected: " + dest)
    require(EDIT.connect_material_expressions(node, "", opacity, dest),
            "Opacity input disconnected: " + dest)

def finish(asset, color, mask):
    require(EDIT.connect_material_property(
        color, "", u.MaterialProperty.MP_EMISSIVE_COLOR),
        "Emissive property disconnected")
    require(EDIT.connect_material_property(
        mask, "", u.MaterialProperty.MP_OPACITY_MASK),
        "Opacity mask disconnected")
    EDIT.recompile_material(asset)
    require(LIB.save_loaded_asset(asset, only_if_is_dirty=False),
            "Material save failed: " + str(asset.get_path_name()))

# Temperature follows the requested physical order: dead coal red-black
# -> dying red -> mature orange -> fresh yellow -> white-hot rooted core.
palette = (
    "float3 c=float3(.075,.004,.004);"
    "c=lerp(c,float3(.25,.012,.007),step(.12,h));"
    "c=lerp(c,float3(.72,.05,.009),step(.24,h));"
    "c=lerp(c,float3(.97,.21,.015),step(.43,h));"
    "c=lerp(c,float3(1.00,.62,.055),step(.61,h));"
    "c=lerp(c,float3(1.00,.85,.28),step(.77,h));"
    "c=lerp(c,float3(1.00,.95,.78),step(.90,h));"
    "return c*1.12;"
)

try:
    LIB.make_directory(BASE)

    # A CONTINUOUS ribbon, not stacked rectangles. UVx shapes hotter centre;
    # vertex color red is actual life temperature, green is flame height,
    # blue is a per-generation seed. No temporal sine drives the lifecycle.
    tongue = new_material("M_AST_FireTongue")
    tc = EDIT.create_material_expression(
        tongue, u.MaterialExpressionVertexColor, -540, -170)
    uv = EDIT.create_material_expression(
        tongue, u.MaterialExpressionTextureCoordinate, -540, 0)
    require(tc and uv, "Tongue vertex color/UV graph creation failed")
    tfield = (
        "float2 q=floor(UV*float2(15.,45.))/float2(15.,45.);"
        "float side=abs(q.x-.5)*2.;"
        "float ridge=.95+.05*sin(q.y*17.+C.b*19.);"
        "float h=saturate(C.r*(1.-.51*side)*ridge);"
    )
    tc_color, tc_mask = custom(tongue, ("C", "UV"),
                               tfield + palette,
                               tfield + "return step(.075,h);")
    connect(tongue,tc,"C",tc_color,tc_mask)
    connect(tongue,uv,"UV",tc_color,tc_mask)
    finish(tongue,tc_color,tc_mask)

    # Exact animated mesh surface overlay. Local rest-pose points stay on
    # deformed skin polygons, and nearby burning areas merge by union.
    surface = new_material("M_AST_FireSurfaceLife")
    pre = EDIT.create_material_expression(
        surface, u.MaterialExpressionPreSkinnedPosition, -860, -300)
    clock = EDIT.create_material_expression(
        surface, u.MaterialExpressionTime, -860, -160)
    require(pre and clock, "PreSkinnedPosition/Time missing")
    scalar = {}
    for i,(name, initial) in enumerate((
        ("FireIntensity",1.0), ("FireEnergy",0.8),
        ("FireSpread",0.0), ("FireSeed",1729.0)
    )):
        n=EDIT.create_material_expression(
            surface,u.MaterialExpressionScalarParameter,-850,-20+i*105)
        require(n, "Scalar missing: " + name)
        n.set_editor_property("parameter_name",u.Name(name))
        n.set_editor_property("default_value",initial)
        scalar[name]=n
    flow = EDIT.create_material_expression(
        surface,u.MaterialExpressionVectorParameter,-850,430)
    require(flow,"Motion vector missing")
    flow.set_editor_property("parameter_name",u.Name("FireMotion"))
    flow.set_editor_property("default_value",u.LinearColor(0,0,0,0))

    clusters = []
    for i in range(8):
        p=EDIT.create_material_expression(
            surface,u.MaterialExpressionVectorParameter,-820,560+i*95)
        require(p,"Surface hotspot parameter missing")
        p.set_editor_property("parameter_name",u.Name("FireCluster%d"%i))
        p.set_editor_property("default_value",u.LinearColor(0,0,0,-1))
        clusters.append(p)

    union = []
    for i in range(8):
        union.append(
            "float grow%d=smoothstep(%0.3f,%0.3f,Spread+.15);"
            "float radius%d=C%d.w*grow%d;"
            "float cover%d=radius%d>0.1?"
            "saturate((radius%d-distance(p,C%d.xyz))/"
            "max(3.,radius%d*.33)):0.;"
            "hot=max(hot,cover%d);mass+=cover%d;"
            % (i,i*.045,i*.045+.36,i,i,i,i,i,i,i,i,i,i)
        )
    sfield = (
        "float3 p=floor(P/6.)*6.;"
        "float hot=0.;float mass=0.;"
        + "".join(union) +
        "float merged=saturate((mass-1.)*.50);"
        # Connected burning mantle is skinned to the actual mesh polygons.
        "float full=smoothstep(.40,.98,Spread);"
        "float cover=saturate(max(hot,full));"
        "float3 adv=p-float3(M.x,M.y,M.z)*T*.11;"
        "float n=.87+.08*sin(adv.x*.71+adv.z*.43-T*4.7+Seed*.02)"
        "+.05*sin(adv.y*.59-adv.x*.24+T*6.1);"
        "float h=saturate(cover*(.69+.27*hot+.22*merged+.24*full)"
        "*n*I*(.85+.15*E));"
    )
    inputs=("P","T","I","E","Spread","Seed","M")+tuple(
        "C%d"%i for i in range(8))
    sc,sm=custom(surface,inputs,sfield+palette,
                 sfield+"return step(.16,h);")
    for node, target in (
        (pre,"P"),(clock,"T"),(scalar["FireIntensity"],"I"),
        (scalar["FireEnergy"],"E"),(scalar["FireSpread"],"Spread"),
        (scalar["FireSeed"],"Seed"),(flow,"M")
    ):
        connect(surface,node,target,sc,sm)
    for i, node in enumerate(clusters):
        connect(surface,node,"C%d"%i,sc,sm)
    finish(surface,sc,sm)

    # Configure only AST-owned actors. Runtime dynamically ignited arbitrary
    # meshes use the same built-in default material references.
    subsystem=u.get_engine_subsystem(u.SubobjectDataSubsystem)
    funcs=u.SubobjectDataBlueprintFunctionLibrary
    for name in ("BP_AST_FireSource","BP_AST_FireSource_Igniter"):
        bp=LIB.load_asset("/Game/VFX/Fire/Blueprints/"+name)
        require(bp,"Missing owned fire Blueprint "+name)
        seen=set()
        found=0
        for handle in subsystem.k2_gather_subobject_data_for_blueprint(bp):
            obj=funcs.get_object(funcs.get_data(handle))
            if obj is None: continue
            key=str(obj.get_path_name())
            if key in seen: continue
            seen.add(key)
            if obj.get_class().get_name()!="ASTFireReactiveComponent":
                continue
            obj.set_editor_property("enable_procedural_flames",True)
            obj.set_editor_property("enable_surface_fire",True)
            obj.set_editor_property("use_legacy_heat_grid",False)
            obj.set_editor_property("enable_flame_cards",False)
            obj.set_editor_property("enable_niagara",False)
            obj.set_editor_property("volume_material",tongue)
            obj.set_editor_property("surface_material",surface)
            found+=1
        require(found==1,"Expected exactly one fire component "+name)
        u.BlueprintEditorLibrary.compile_blueprint(bp)
        require(LIB.save_loaded_asset(bp,only_if_is_dirty=False),
                "Could not save "+name)
        OUT["updated"].append(name)
    OUT["status"]="STAGE10_THERMAL_MATERIALS_CONFIGURED_REQUIRES_VISUAL_TEST"
except Exception as exc:
    OUT["status"]="FAILED"
    OUT["errors"].append({"error":str(exc),"trace":traceback.format_exc()[-4000:]})
finally:
    d=os.path.join(
        u.Paths.convert_relative_path_to_full(u.Paths.project_saved_dir()),
        "ChatGPT","NiagaraFire","Stage10")
    os.makedirs(d,exist_ok=True)
    with open(os.path.join(d,"stage10_result.json"),"w",encoding="utf-8") as file:
        json.dump(OUT,file,ensure_ascii=False,indent=2)
    print("AST_FIRE_STAGE10 "+json.dumps(OUT,ensure_ascii=False),flush=True)
