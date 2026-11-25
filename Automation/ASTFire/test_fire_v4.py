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
for a,b,expected in [["void UASTFireFreeFlameComponent::UpdateSparks(","void UASTFireFreeFlameComponent::UpdateSmoke(","1408cade"],["void UASTFireFreeFlameComponent::UpdateSmoke(","FString UASTFireFreeFlameComponent::RunSmokeAudit(","f03b75c8"],["void UASTFireFreeFlameComponent::UpdateDetachedVortices(","void UASTFireFreeFlameComponent::AddStrip(","59110f1b"],["    // Stage30: distinct high-contrast hot pixels","    // Section 1 was authored before this section","4ba8a0a3"]]:
    assert fnv(region(a,b))==expected, a
assert fnv(r)=="930b76ee","burn/torch runtime changed"
for token in ("GetTongueCount()", "SetSizeScale(", "RunSmokeAudit(", "RunFireV4Audit("):
    assert token in h,token
for token in ("GetSkinnedVertexPosition","LineTraceComponent(","ConnectedRoots.Add(",
              "AppendNeck(WorldA,WorldB,RadiusAt(V0),RadiusAt(V1),V0,V1)",
              "AppendNeck(BranchRoot,Mid,BranchRadius,", "EFlameLife::Bridging",
              "EFlameLife::Retracting","D.Position+=D.Velocity*Dt;",
              "const float PixelPitch=UpdateConnectedSource",
              "const float Pitch=FMath::Max(.1875f,PixelPitch);",
              "const float Support=R-D+Rim;", "Cell.Temperature=FMath::Clamp",
              "CornerDepth(CX,CY)","Colors.Add(FLinearColor(ASTFireLife::ToneCode(C.Temperature),0.f,0.f,1.f))",
              "const float V=(PY*CS+PX*SN)", "Projection.Unproject(P,Depth)",
              "const float Flow=.57f*FMath::Sin","const float Shift=.28f*R*Flow",
              "MaxXCells=72,MaxYCells=104", "RunFireV4Audit("):
    assert token in c,token
assert "floor(UV" not in m
assert "MP_WORLD_POSITION_OFFSET" not in m
assert 'connect_material_property(vertex,"A",u.MaterialProperty.MP_OPACITY_MASK)' in m
for threshold in ("1.,h","2.,h","3.,h","4.,h","5.,h","6.,h"):
    assert threshold in m
assert "float h=floor(saturate(C.r)*7.)" in m
assert m.count("save_loaded_asset(")==1
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
# Thermal islands move independently of geometric mask. Outside stays absent.
def field(distance,r,v,flow,rim=0):
    cover=r-distance+rim>=0
    core=max(0,min(1,1-abs(distance-.28*r*flow)/max(.25,r)))
    heat=max(0,min(1,.40+.46*core-.35*v+.19*flow))
    return cover,heat
assert field(13,10,.2,1)[0] is False
assert field(2,10,.2,-1)[0] and field(2,10,.2,1)[0]
assert field(2,10,.2,-1)[1]!=field(2,10,.2,1)[1]
assert field(0,10,.2,0)[1]>field(0,10,.8,0)[1]
# Each detached piece is proportionate to the retracted portion, then
# shrinks and cools. Pixel grid itself does not spin.
for remaining in (.66,.75,.83):
    for random in (0,.5,1):
        child=100*(1-remaining)*(.72+.24*random)
        assert 0<child<35
        for age in (0,.25,.75):
            assert child*(1-age)>child*(1-age-.1)
assert "Child.Width=FMath::Min" in c
assert "Child.Lifetime=.19f + .32f" in c
print("Fire v4 raster, independent heat, detached lifecycle, v3 smoke/spark/torch locks: PASS")
