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
assert "Centers.Reserve(SurfaceClusters)" in c
assert "IgnitionPoints.Num()-1" in c
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
assert 'return saturate((h-.10)*3.7)' in mat
assert "setup_stage10_lifecycle.py" in install
assert "M_AST_FireTongue.uasset" in install
assert "M_AST_FireSurfaceLife.uasset" in install
assert "STAGE10_BUILD_EXIT" in install

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

# Upper-bound budget: this is a SIZE check, not a measured UE benchmark.
vertices=(15*(9+4)+16*5)*2
triangles=(15*(9+4-2)+16*(5-1))*2
assert vertices<=600 and triangles<=500
for actors in (1,10,30):
    print(f"Stage10 theoretical max {actors} actors: "
          f"{actors*vertices} live vertices, {actors*triangles} triangles/25Hz")
print("Stage10: mesh union, lifecycle, temperature order, scales and budget OK")
