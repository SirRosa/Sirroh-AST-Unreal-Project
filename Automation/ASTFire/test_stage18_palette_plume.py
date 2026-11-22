"""Stage18: prevent return to tentacles or orange two-vertex detached sprites.

Numerical, topology and source structure tests; not a screenshot/PIE proof.
"""
from pathlib import Path
from math import exp, sin, sqrt

p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp")
c=p.read_text()
h=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireFreeFlameComponent.h").read_text()
material=Path("Automation/ASTFire/setup_stage10_lifecycle.py").read_text()

field=c[c.index("void UASTFireFreeFlameComponent::UpdateConnectedSource("):
        c.index("void UASTFireFreeFlameComponent::TickComponent(")]
geometry=c[c.index("void UASTFireFreeFlameComponent::UpdateGeometry("):
           c.index("void UASTFireFreeFlameComponent::BuildSourceAnchors()")]
# The main flame remains a single source driven by all living tongues.
assert "UpdateConnectedSource(bCreate, View, Right);" in geometry
assert "H.Add(0.f);" in geometry
assert "Parent[J]=Best" not in field
assert "TArray<FNeckSegment> Necks;" in field
assert "AppendNeck(WorldA,WorldB,RadiusAt(V0),RadiusAt(V1),V0,V1);" in field
# Stage19 adjusts the living profile only; keep this suite focused on
# Stage18's preserved cross-section and palette. Detailed shape checks
# belong to test_stage19_live_silhouette.py.
assert "const float PlumeWidth=FMath::Max(T.CurrentWidth," in field
assert "T.CurrentLength*(.058f+.010f*" in field
assert "const float BulgeA=.26f*FMath::Exp(" in field
assert "const float BulgeB=.12f*FMath::Exp(" in field
assert "const float UpperTaper=FMath::Lerp(1.f,.55f," in field
assert "const float Reach=FMath::Min(T.CurrentLength*" in field
assert "Right*Direction*Reach*.48f" in field
assert "Right*Direction*(Reach*.74f+Sway)" in field
assert "Reach,42.f" not in field

# The detached fragment is no longer a TWO-VERTEX-wide rectangle. Seven
# radial columns sample the SAME smooth core/rim field as the living source;
# the same six-band+hot-white material palette is used for both sections.
assert "static constexpr int32 DetachedColumns=7;" in geometry
assert "for(int32 X=0;X<DetachedColumns;++X)" in geometry
assert "for(int32 X=0;X<DetachedColumns-1;++X)" in geometry
assert "const float DFromCenter=FMath::Abs(Side)*" in geometry
assert "const float LocalHeat=Cover*(.19f+" in geometry
assert "Heat*D.Temperature*LocalHeat*" in geometry
assert "UVs.Add(FVector2D(U,V));" in geometry
assert "check(Offset==Verts.Num());" in geometry
assert "AddStrip(C,W,H,D.Seed" not in geometry
assert "Render->SetMaterial(0, Material)" in c
assert "Render->SetMaterial(1, Material)" in c
for cutoff in (".12,h",".24,h",".43,h",".61,h",".77,h",".90,h"):
    assert cutoff in material

def clamp(x):return max(0.,min(1.,x))
def thermal(u,r=14.,fade=1.,v=.5,temp=.95):
    edge=max(4.,r*.45)
    d=abs(2*u-1)*(r+edge*.04)
    cover=clamp(1.-(d-r)/edge)
    core=clamp((r-d)/max(1.,r))
    return clamp(temp*cover*(.19+.81*core**.56)*(1.-.19*v)*fade**.66)
def shader_heat(u):return thermal(u)*(1-.51*abs(u-.5)*2)
center=shader_heat(.5)
middle=shader_heat(2/6)
edge=shader_heat(0.)
assert center>.75
assert .3<middle<.8
assert .075<=edge<.12
assert center>middle>edge
assert thermal(.5,fade=0.)==0.
assert thermal(.5,fade=.5)<thermal(.5,fade=1.)

# 25 Hz / unchanged 32x44 mesh plus budgeted 7x5 detached topology.
assert "SourceGridX = 32" in h and "SourceGridY = 44" in h
verts=33*45+2*15*(9+4)+16*5*7
tris=32*44*2+15*(9+4-2)*2+16*(5-1)*(7-1)*2
assert (verts,tris)==(2435,3914)
assert "T.Life = EFlameLife::Bridging;" in c
assert "T.Life = EFlameLife::Retracting;" in c
assert "T.Life = EFlameLife::Growing;" in c
print("Stage18 preserved: detached thermal palette and topology unchanged, Stage19 silhouette check delegated")
