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
assert "const float PlumeWidth=FMath::Max(T.CurrentWidth," in field
assert "T.CurrentLength*(.098f+.017f*" in field
assert "const float BulgeA=.46f*FMath::Exp(" in field
assert "const float BulgeB=.55f*FMath::Exp(" in field
assert "const float Waist=.22f*FMath::Exp(" in field
assert "const float Reach=FMath::Min(T.CurrentLength*" in field
assert "Right*Direction*Reach*.48f" in field
assert "Right*Direction*(Reach*.74f+Sway)" in field
assert "Reach,42.f" not in field  # old rigid antenna branches

# Effective half-width rises on tall, skinny flames; irregular bulges
# prevent long parallel, uniform pipes in a .3-.8 height interval.
def plume_width(width,length,hash_value=.5):
    return max(width,length*(.098+.017*hash_value))
def contour(v,width=8.,length=170.,seed_a=.5,seed_b=.5):
    base=plume_width(width,length)
    bulge_a=.46*exp(-((v-(.29+.09*seed_a))/.17)**2)
    bulge_b=.55*exp(-((v-(.64+.08*seed_b))/.13)**2)
    waist=.22*exp(-((v-.52)/.085)**2)
    profile=max(.53,min(1.85,.87+bulge_a+bulge_b-waist))
    return base*max(0.,1.-v)**.69*profile
assert plume_width(8.,170.)>16
assert contour(.38)>contour(.52)
assert contour(.69)>contour(.80)
assert contour(1.)==0.
assert contour(.69)>contour(.52)  # distinct upper bulge
assert contour(.38)/contour(.52)>1.3

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
print("Stage18: plume contours and shared radial detached palette OK; topology under fixed budget")
