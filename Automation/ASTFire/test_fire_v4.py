"""Fire v4 current visual contract; replace obsolete v3 geometry snapshots.
CPU/reference math and static regression locks are not GPU/PIE tests.
"""
from pathlib import Path
from math import sin, pi, ceil, log2
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
r=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_fire_v4.py").read_text()
d=Path("Automation/ASTFire/deploy_fire_v4_windows.ps1").read_text()
def fnv(s):
    v=2166136261
    for x in s: v=((v^ord(x))*16777619)&0xffffffff
    return f"{v:08x}"
def region(a,b): return c[c.index(a):c.index(b,c.index(a))]
# The hashes are pinned to accepted v3 7d0eee1049ffbcca7f25ed39811901667a362ba5, NOT recomputed from v4.
for a,b,expected in [["void UASTFireFreeFlameComponent::UpdateSparks(","void UASTFireFreeFlameComponent::UpdateSmoke(","1408cade"],["void UASTFireFreeFlameComponent::UpdateSmoke(","FString UASTFireFreeFlameComponent::RunSmokeAudit(","f03b75c8"]]:
    assert fnv(region(a,b))==expected, a
assert fnv(r)=="930b76ee","burn/torch runtime changed"
for token in ("GetTongueCount()", "SetSizeScale(", "RunSmokeAudit(", "RunFireV4Audit("):
    assert token in h,token
for token in ("GetSkinnedVertexPosition","LineTraceComponent(","ConnectedRoots.Add(",
              "AppendNeck(WorldA,WorldB,T.DisplayRadius[K-1],T.DisplayRadius[K],V0,V1,Phase)",
              "EFlameLife::Bridging","D.Position+=D.Velocity*Dt;",
              "const float PixelPitch=UpdateConnectedSource",
              "const float Pitch=FMath::Max(.1875f,PixelPitch);",
              "const float Support=R-D;", "Cell.Temperature=FMath::Clamp",
              "CornerDepth(CX,CY)","Colors.Add(FLinearColor(ASTFireLife::ToneCode(C.Temperature),0.f,0.f,1.f))",
              "Projection.Unproject(P,Depth)", "ASTFireLife::ArcHeat(V,Core",
              "MaxXCells=72,MaxYCells=104", "RunFireV4Audit("):
    assert token in c,token
assert "floor(UV" not in m
assert "MP_WORLD_POSITION_OFFSET" not in m
assert 'connect_material_property(vertex,"A",u.MaterialProperty.MP_OPACITY_MASK)' in m
for threshold in ("1.,h","2.,h","3.,h","4.,h","5.,h","6.,h"):
    assert threshold in m
assert "float h=floor(saturate(C.r)*7.)" in m
assert m.count("save_loaded_asset(")==2
assert "run_smoke_audit(smoke)" in m and "run_fire_v4_audit(mat)" in m
for token in ("FIRE_V4_ABI_IMPORTS_CHECKED", "FIRE_V4_ROLLBACK_COMPLETE",
              "Assert-EditorClosed", "Local source changed", "FIRE_V4_COMMANDLET_EXIT"):
    assert token in d,token
assert "M_AST_FireSmoke.uasset" not in d
# Raster pitch is square, power-of-two bounded and independent of UV stretch.
def pitch(scale,w,h):
    b=.1875
    return b*2**ceil(log2(max(b,w/70,h/102)/b))
for scale in (.15,1,2):
    for w,height in ((12,22),(100,170),(350,700)):
        p=pitch(scale,w,height)
        assert ceil(w/p)+1<=72 and ceil(height/p)+1<=104
# Real lifecycle contract: the original arc is transferred at an exact
# sampled cut ring. There is no independent upper sprite or retraction.
assert "EFlameLife::Retracting" not in c
assert "BodyCenter(" not in c
assert "Child.Arc[K]=(bSnapshot?Tongue.DisplayArc[Cut+K]" in c
assert "Child.Radius[K]=bSnapshot?Tongue.DisplayRadius[Cut+K]" in c
assert "Child.BirthPhase=Tongue.Seed+Tongue.Phase;" in c
assert "T.RestartLength=FMath::Min(FMath::Min(Remaining,ResetCap),T.MaxLength*.18f);" in c
assert "D.Temperature=FMath::Pow(Fade,.88f);" in c
assert "MaxTransferError<.05f" in c and "MaxResetRatio<=.151f" in c
assert "bCycle && bFragment" in c and "bMesh && bWick" in c
assert "World->SendAllEndOfFrameUpdates();" in c
for cut in (1,2):
    vs=[k/8 for k in range(cut,9)]
    assert vs[0] in (.125,.25) and vs[-1]==1
    assert 1-vs[0]>.60
    for i in range(1,20):
        a,b=1-(i-1)/20,1-i/20
        assert b**.72<a**.72 and b**.88<a**.88
print("Fire lifecycle: original upper arc, atomic rebirth, cooling, preserved smoke/sparks")

assert 'T.PhaseAge>=T.BridgeSeconds && bCutReady' in c
assert 'const FVector Expected=Old.DisplayArc[Ring];' in c

assert "Root.Outward=Radial;" in c
assert "T.Age-T.RebirthDelay" in c
assert "Exposure>=.50f && FanRate>=.60f" in c
assert "bSparkGrid && bSparkFade" in c
assert "bInFront?Fade:0.f" in c
assert "u.BlendMode.BLEND_TRANSLUCENT" in m
assert 'u.MaterialProperty.MP_OPACITY)' in m
assert "M_AST_FireSpark.uasset" in d
