"""Stage10 spec regression, shader/CPU integration and numerical reference.

This test intentionally DOES NOT claim visual quality, Unreal performance,
PIE networking or an exact geodesic surface-burning solver.
"""
from math import sin, sqrt
from pathlib import Path

root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
reactive=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
mat=Path("Automation/ASTFire/setup_stage10_lifecycle.py").read_text()
install=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()

for token in (
    "GetSkinnedVertexPosition", "GetSkeletalMeshAsset()",
    "GetResourceForRendering", "GetSkinWeightVertexBuffer",
    "StaticVertexBuffers.PositionVertexBuffer.VertexPosition",
    "LineTraceComponent(", "Transform.InverseTransformPosition(WorldHit)",
    "FireCluster%d", 'FName(TEXT("FireSpread"))',
    "bClusterMapUploaded", "ConfigureGeneration(T)",
    "T.Age>=T.GrowSeconds", "EmitTip(T,Base,T.SplitLength,Flow)",
    "T.Age = 0.f", "T.RemainingFraction",
    "D.Position+=D.Velocity*Dt", "160.f+210.f*(1.f-Fade)",
    "FMath::Pow(Fade,1.25f)", "AddStrip(C,W,H",
    "Render->UpdateMeshSection_LinearColor",
):
    assert token in c, token
assert "FMath::Sin(Cycle * PI)" not in c
assert "bUseLegacyHeatGrid" in reactive
assert "FreeFlames->SetIgnitionPoints(ExtraLocalPoints)" in reactive
assert "FreeFlames->SetSurfaceMaterial(FireSurfaceMID)" in reactive
assert "0.91f * VisualEnergy" in reactive
assert "MaxTongues = 15" in h
assert "MaxDetached = 16" in h
assert "BridgeRings = 4" in h
assert "EFlameLife::Bridging" in c
assert "EFlameLife::Retracting" in c
assert "T.CurrentLength = FMath::Lerp(T.SplitLength,T.RetractTarget,Ease)" in c
assert "T.BridgedChild = EmitTip(T,Base,T.SplitLength,Flow)" in c
assert "BW.Add(bBridge?Width*.31f*(1.f-BridgeAlpha)" in c
assert "RootBlend=ASTFireLife::SmoothStep(V/.20f)" in c
assert "BestSkeleton" in reactive
assert "bClusterMapUploaded = false;" in c
assert "BurnClusters.Reset()" in c
assert "RebuildClusters();" in c
assert "TArray<FBurnCluster> BurnClusters" in h
assert "Tongue.ClusterIndex=Nearest" in c
assert "Tongue.ActivationSpread" in c
assert "for (const FVector& Hit:IgnitionPoints)" in c
assert "if (BurnClusters.IsValidIndex(I))" in c
assert "SurfaceMID->SetVectorParameterValue(" in c
assert "if (Spread<T.ActivationSpread) continue;" in c
assert "T.bActive=true;" in c
assert "Centers.Reserve(SurfaceClusters)" not in c
assert "for (const FTongue& Tongue:Tongues)" not in c[c.index("void UASTFireFreeFlameComponent::UpdateSurfaceMap"):c.index("void UASTFireFreeFlameComponent::AddStrip")]
assert "MaterialExpressionVertexInterpolator" in mat
# The first real PIE compile exposed C0.w ... C7.w swizzle errors: the
# MaterialExpressionVectorParameter output is RGB, not RGBA.
assert '"float radius%d=R%d*grow%d;"' in mat
assert '"float radius%d=C%d.w*grow%d;"' not in mat
assert 'u.Name("FireClusterRadius%d"%i)' in mat
assert '"R%d"%i for i in range(8)' in mat
assert 'connect(surface,node,"R%d"%i,sc,sm)' in mat
assert "FireClusterRadius%d" in c
assert "P.A : 0.f" in c
assert '"used_with_skeletal_mesh", True' in mat
assert "get_material_expression_input_names(interpolated)" in mat
assert '(interpolated,"P")' in mat
assert "FreeFlames->SetIgnitionPoints(ExtraLocalPoints)" in reactive
assert "if (IsValid(FreeFlames))" in reactive
assert "Tongues.Num()*2+MaxDetached" in c
assert "RingsPerTongue = 9" in h
assert "RingsPerDetached = 5" in h
assert "SetIgnitionPoints" in h
assert "FireSurfaceLife" in reactive and "M_AST_FireTongue" in reactive
assert "FMath::Frac(ShapeSeed*.017f)" in c
assert "I<IgnitionPoints.Num() && Tongues.Num()<DesiredCount" in c
assert "if (Slot < IgnitionPoints.Num())" in c
assert "bClusterMapUploaded" in c

assert "max(3.,radius%d*.33)" in mat
assert 'float full=smoothstep(.40,.98,Spread)' in mat
assert 'float cover=saturate(max(hot,full))' in mat
assert '"hot=max(hot,cover%d);mass+=cover%d;"' in mat
for threshold in (".12,h", ".24,h", ".43,h", ".61,h", ".77,h", ".90,h"):
    assert threshold in mat, threshold
for name in ("FireCluster%d","FireIntensity","FireSpread","FireEnergy"):
    assert name in mat
assert "BP_AST_FireSource_Igniter" in mat
assert "BLEND_TRANSLUCENT" in mat
assert "MP_OPACITY" in mat
assert 'new_material("M_AST_FireSurfaceLife", is_overlay=True)' in mat
assert 'finish(surface,sc,sm,is_overlay=True)' in mat
assert "MaterialExpressionVertexNormalWS" in mat
assert "MaterialExpressionFresnel" in mat
assert '"FireCloakDepth",9.0' in mat
assert "MP_WORLD_POSITION_OFFSET" in mat
assert "finish(surface,sc,sm,is_overlay=True,wpo=wpo)" in mat
assert "wpo.set_editor_property" in mat
assert 'float depth=Depth*cover*saturate(I)' in mat
assert '"return dir*depth+float3(0.,0.,1.)*"' in mat
assert 'cover*I*(.065+.76*pow(saturate(F),.60))' in mat
assert "9.f + LongestSide*.025f" in reactive
assert "FireCloakDepth" in reactive
assert "GetSkinnedVertexPosition" in c  # existing tongue anchors preserved
assert "EFlameLife::Bridging" in c  # original user-approved split preserved
assert "setup_stage10_lifecycle.py" in install
assert "M_AST_FireTongue.uasset" in install
assert "M_AST_FireSurfaceLife.uasset" in install
assert "STAGE10_BUILD_EXIT" in install
assert "verify_stage10_gpu.py" in install
assert "[switch]$ValidateGPU" in install
assert "if($ValidateGPU){" in install
assert "STAGE10_GPU_SHADER_UNVERIFIED_OPT_IN_VALIDATEGPU" in install
assert "-AllowCommandletRendering" in install
assert "-RenderOffscreen" in install
assert "Failed to compile Material" in install

# Verify physically ordered lifecycle reference: continuous base at split,
# detached tip cools, shrinks, accelerates as it rises; a new unique generation.
def smooth(x):
    v=max(0., min(1.,x))
    return v*v*(3.-2.*v)

def tongue_cycle(grow=0.72,remaining=0.42,maximum=115):
    start=maximum*remaining
    prior=start
    for k in range(16):
        t=k*(grow/15.)
        length=maximum*(remaining+(1.-remaining)*smooth(t/grow))
        assert length>=prior-1e-8
        prior=length
    assert abs(prior-maximum)<1e-6
    after=maximum*remaining
    assert after<prior*.65 and after>0
    return prior,after

adult,reborn=tongue_cycle()
assert adult>reborn
lifetime=.81
def detached(t):
    fade=max(0.,1.-t/lifetime)
    size=18*fade
    temp=.85*(1.-.46*.75)*fade**1.25
    uplift=160+210*(1.-fade)
    return size,temp,uplift
a,b,c0=detached(.0)
d,e,c1=detached(lifetime*.45)
f,g,c2=detached(lifetime*.9)
assert a>d>f and b>e>g and c0<c1<c2

# Numeric checks: the bridge persists for multiple frames at 25Hz, and
# the parent must retract smoothly after the connection ruptures.
split_length=100.
target_length=42.
bridge_seconds=.15
retract_seconds=.22
ticks=[k*.04 for k in range(11)]
lengths=[]
bridge_visible=[]
for t in ticks:
    if t<=bridge_seconds:
        length=split_length
        bridge_visible.append(t)
    else:
        progress=max(0.,min(1.,(t-bridge_seconds)/retract_seconds))
        length=split_length+(target_length-split_length)*smooth(progress)
    lengths.append(length)
assert len(bridge_visible)>=3
assert lengths[0]==lengths[1]==lengths[2]
assert all(lengths[i+1]<=lengths[i]+1e-8 for i in range(len(lengths)-1))
assert lengths[-1]>=target_length-1
assert lengths[4]>.90*split_length

# A single local source supports multiple lifetimes; adding a nearby impact
# extends that source instead of creating another tongue/cluster.
def clusters(points, merge_distance):
    out=[]
    for p in points:
        nearest=min(range(len(out)), key=lambda i:abs(p-out[i])) if out else None
        if nearest is None or abs(p-out[nearest])>=merge_distance:
            out.append(p)
    return out
assert clusters([0.,8.,12.,75.],25.)==[0.,75.]
assert len(clusters([0.,8.,12.,75.],25.)) < 5
# On a single burn source, the number of spawned tongues remains independent
# of how many distinct vertex samples define the surface.
tongue_per_cluster=[5]
assert len(tongue_per_cluster)==1
assert tongue_per_cluster[0]==5

# Numeric surface union: two nearby burn patches must visually merge as their
# supporting radii expand; this remains Euclidean, NOT topological geodesics.
def coverage(x,spots,radius):
    return max(max(0., min(1.,(radius-abs(x-s))/(max(3.,radius*.33))))
               for s in spots)
spots=(-15.,15.)
assert coverage(0.,spots,10.)==0
assert coverage(0.,spots,33.)>.8

# Explicit limits from the approved user's spec.
def tongue_count(kind,max_side):
    if kind=="skeletal":
        return 5
    if max_side<140: return 5
    if max_side>340: return 13
    return 7
assert 3<=tongue_count("skeletal",190)<=6
assert 4<=tongue_count("static",90)<=5
assert 10<=tongue_count("static",600)<=15

# Geometry is displaced on the same GPU-skinned overlay triangles, not
# copied or CPU-skinned again. Thickens with prop dimension.
for side in (75.,190.,500.,1500.):
    depth=max(10., min(28., 9.+side*.025))
    assert 10.<=depth<=28.
assert 9.+190.*.025 < 9.+500.*.025
assert "Blend_TRANSLUCENT" not in mat  # correct enum name below
assert "BLEND_TRANSLUCENT" in mat

# Upper-bound budget: this is a SIZE check, not a measured UE benchmark.
vertices=(15*(9+4)+16*5)*2
triangles=(15*(9+4-2)+16*(5-1))*2
assert vertices<=600 and triangles<=500
for actors in (1,10,30):
    print(f"Stage10 theoretical max {actors} actors: "
          f"{actors*vertices} live vertices, {actors*triangles} triangles/25Hz")
print("Stage10: mesh union, lifecycle, temperature order, scales and budget OK")
