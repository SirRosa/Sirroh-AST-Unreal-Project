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

def new_material(name, is_overlay=False):
    path = BASE + "/" + name
    asset = LIB.load_asset(path) if LIB.does_asset_exist(path) else None
    if asset is None:
        asset = u.AssetToolsHelpers.get_asset_tools().create_asset(
            name, BASE, u.Material, u.MaterialFactoryNew())
    require(asset, "Material creation failed: " + path)
    # UMeshComponent::OverlayMaterial is explicitly a TRANSLUCENT pass.
    # Masked flame sprites render fine; a masked mesh overlay may not render.
    blend = (u.BlendMode.BLEND_TRANSLUCENT if is_overlay
             else u.BlendMode.BLEND_MASKED)
    asset.set_editor_property("blend_mode", blend)
    asset.set_editor_property("shading_model",
                              u.MaterialShadingModel.MSM_UNLIT)
    asset.set_editor_property("two_sided", True)
    if is_overlay:
        # Without the skeletal usage permutation Unreal substitutes its
        # default material even when the Blueprint and source graph are valid.
        asset.set_editor_property("used_with_skeletal_mesh", True)
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

def finish(asset, color, mask, is_overlay=False, wpo=None):
    require(EDIT.connect_material_property(
        color, "", u.MaterialProperty.MP_EMISSIVE_COLOR),
        "Emissive property disconnected")
    require(EDIT.connect_material_property(
        mask, "", (u.MaterialProperty.MP_OPACITY
                   if is_overlay else u.MaterialProperty.MP_OPACITY_MASK)),
        "Opacity disconnected")
    if wpo is not None:
        require(EDIT.connect_material_property(
            wpo, "", u.MaterialProperty.MP_WORLD_POSITION_OFFSET),
            "Fire cloak world-position offset disconnected")
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
    surface = new_material("M_AST_FireSurfaceLife", is_overlay=True)
    pre = EDIT.create_material_expression(
        surface, u.MaterialExpressionPreSkinnedPosition, -1030, -300)
    interpolated = EDIT.create_material_expression(
        surface, u.MaterialExpressionVertexInterpolator, -830, -300)
    clock = EDIT.create_material_expression(
        surface, u.MaterialExpressionTime, -860, -160)
    require(pre and interpolated and clock,
            "PreSkinnedPosition/VertexInterpolator/Time missing")
    # PreSkinnedPosition is VERTEX-ONLY. Feeding it straight to a custom
    # pixel shader produced the observed PCD3D_SM6 compile failure:
    # 'External code identifier PreSkinnedPosition is not available'.
    pin_names = EDIT.get_material_expression_input_names(interpolated)
    require(len(pin_names) == 1, "Unexpected vertex interpolator pins")
    require(EDIT.connect_material_expressions(
        pre, "", interpolated, str(pin_names[0])),
        "Could not route pre-skinned position through vertex stage")
    scalar = {}
    for i,(name, initial) in enumerate((
        ("FireIntensity",1.0), ("FireEnergy",0.8),
        ("FireSpread",0.0), ("FireSeed",1729.0),
        ("FireCloakDepth",9.0)
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
    cluster_radii = []
    for i in range(8):
        p=EDIT.create_material_expression(
            surface,u.MaterialExpressionVectorParameter,-820,560+i*110)
        r=EDIT.create_material_expression(
            surface,u.MaterialExpressionScalarParameter,-820,605+i*110)
        require(p and r,"Surface hotspot or radius parameter missing")
        p.set_editor_property("parameter_name",u.Name("FireCluster%d"%i))
        # Unreal's vector parameter default output is RGB (float3).
        # A former C%d.w access compiled with an out-of-bounds swizzle.
        # Every cluster radius is now a separate float scalar parameter.
        p.set_editor_property("default_value",u.LinearColor(0,0,0,0))
        r.set_editor_property("parameter_name",u.Name("FireClusterRadius%d"%i))
        r.set_editor_property("default_value",0.0)
        clusters.append(p)
        cluster_radii.append(r)

    union = []
    for i in range(8):
        union.append(
            "float grow%d=smoothstep(%0.3f,%0.3f,Spread+.15);"
            "float radius%d=R%d*grow%d;"
            "float cover%d=radius%d>0.1?"
            "saturate((radius%d-distance(p,C%d.xyz))/"
            "max(3.,radius%d*.33)):0.;"
            "hot=max(hot,cover%d);mass+=cover%d;"
            % (i,i*.045,i*.045+.36,i,i,i,i,i,i,i,i,i,i)
        )
    # The original translucent overlay only RECOLORED the underlying mesh.
    # Create an actual OUTWARD-DISPLACED cloak using the SAME skinned mesh
    # polygons. A pixel falloff makes the original body remain visible while
    # the expanded silhouette burns around it. Geometry stays animated with
    # the source's existing skin weights, without a duplicate 100k-vertex
    # SkeletalMeshComponent or costly CPU skinning.
    sfield = (
        "float3 p=floor(P/6.)*6.;"
        "float hot=0.;float mass=0.;"
        + "".join(union) +
        "float merged=saturate((mass-1.)*.50);"
        "float full=smoothstep(.40,.98,Spread);"
        "float cover=saturate(max(hot,full));"
        "float3 adv=p-float3(M.x,M.y,M.z)*T*.11;"
        "float n=.84+.10*sin(adv.x*.71+adv.z*.43-T*4.7+Seed*.02)"
        "+.06*sin(adv.y*.59-adv.x*.24+T*6.1);"
        "float h=saturate(cover*(.44+.35*hot+.15*merged+.09*full)"
        "*n*I*(.82+.18*E));"
    )
    fresnel=EDIT.create_material_expression(
        surface,u.MaterialExpressionFresnel,-880,-430)
    require(fresnel,"Cloak silhouette fresnel missing")
    inputs=("P","T","I","E","Spread","Seed","M")+tuple(
        "C%d"%i for i in range(8))+tuple(
        "R%d"%i for i in range(8))+("F",)
    # The center of the actor no longer gets a solid yellow coat. Nearly
    # clear at frontal angles; brighter, thick, flickering at the outline.
    # The opacity and emissive share the same mesh-bound hot-spot mask.
    sc,sm=custom(surface,inputs,sfield+palette,
        sfield+"return saturate(cover*I*(.065+.76*pow(saturate(F),.60))"
               "*(.56+.44*n)*(.82+.18*E));")
    for node, target in (
        (interpolated,"P"),(clock,"T"),(scalar["FireIntensity"],"I"),
        (scalar["FireEnergy"],"E"),(scalar["FireSpread"],"Spread"),
        (scalar["FireSeed"],"Seed"),(flow,"M")
    ):
        connect(surface,node,target,sc,sm)
    for i, node in enumerate(clusters):
        connect(surface,node,"C%d"%i,sc,sm)
    for i, node in enumerate(cluster_radii):
        connect(surface,node,"R%d"%i,sc,sm)
    connect(surface,fresnel,"F",sc,sm)

    normal=EDIT.create_material_expression(
        surface,u.MaterialExpressionVertexNormalWS,-1040,1490)
    wpo=EDIT.create_material_expression(
        surface,u.MaterialExpressionCustom,-120,1510)
    require(normal and wpo,"Cloak vertex normal or geometry displacement missing")
    wpo.set_editor_property("output_type",
                            u.CustomMaterialOutputType.CMOT_FLOAT3)
    wpo_inputs=("P","N","T","Spread","Seed","M","Depth","I")+tuple(
        "C%d"%i for i in range(8))+tuple(
        "R%d"%i for i in range(8))
    wpo.set_editor_property("inputs",[pin(name) for name in wpo_inputs])
    # These vertex-stage measurements use the undisturbed rest-pose
    # position, so the cloak conforms to deforming limbs AND static props.
    # Every point in a hot cluster belongs to one connected displaced
    # mesh envelope; tongues remain additive extensions of this source.
    wfield=(
        "float3 p=floor(P/7.)*7.;"
        "float hot=0.;float mass=0.;"
        + "".join(union) +
        "float full=smoothstep(.40,.98,Spread);"
        "float cover=saturate(max(hot,full));"
        "float wave=.56+.27*sin(p.x*.143+p.y*.071+T*8.9+Seed*.023)"
        "+.19*sin(p.y*.117+p.z*.159-T*6.4+Seed*.011);"
        "float spike=saturate((wave-.39)*2.0);"
        "float depth=Depth*cover*saturate(I)*(.57+1.33*spike);"
        "float3 dir=normalize(N+float3(0.,0.,.23));"
        "float3 drift=float3(M.x,M.y,0.)*.014*cover;"
        "return dir*depth+float3(0.,0.,1.)*"
        "(Depth*cover*spike*.52)+drift;"
    )
    wpo.set_editor_property("code",wfield)
    wconnections=[
        (pre,"P"),(normal,"N"),(clock,"T"),
        (scalar["FireSpread"],"Spread"),(scalar["FireSeed"],"Seed"),
        (flow,"M"),(scalar["FireCloakDepth"],"Depth"),
        (scalar["FireIntensity"],"I")
    ]
    for node,target in wconnections:
        require(EDIT.connect_material_expressions(node,"",wpo,target),
                "Cloak WPO input not wired: "+target)
    for i,node in enumerate(clusters):
        require(EDIT.connect_material_expressions(node,"",wpo,"C%d"%i),
                "Cloak WPO cluster input disconnected")
    for i,node in enumerate(cluster_radii):
        require(EDIT.connect_material_expressions(node,"",wpo,"R%d"%i),
                "Cloak WPO radius input disconnected")
    finish(surface,sc,sm,is_overlay=True,wpo=wpo)

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
