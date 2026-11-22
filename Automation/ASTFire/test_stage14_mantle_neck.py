"""Stage14: protect approved life cycle and test mantle-to-tongue continuity.

This is a structural/numerical test, not a visual, RHI, or PIE validation.
All lifecycle reference hashes were captured from the protected backup
b53ecb7906823f93f2d8c67941e1e7b1467ffc27.
"""
from pathlib import Path
from math import sqrt

c = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()

def fnv1a32(s):
    h = 2166136261
    for character in s:
        h = (h ^ ord(character)) * 16777619 & 0xffffffff
    return f"{h:08x}"

LOCKS = [
    {
        "start": "void UASTFireFreeFlameComponent::ConfigureGeneration(",
        "end": "void UASTFireFreeFlameComponent::RebuildClusters(",
        "hash": "f29d248e"
    },
    {
        "start": "int32 UASTFireFreeFlameComponent::EmitTip(",
        "end": "void UASTFireFreeFlameComponent::AdvanceSimulation(",
        "hash": "e09c2831"
    },
    {
        "start": "void UASTFireFreeFlameComponent::AdvanceSimulation(",
        "end": "void UASTFireFreeFlameComponent::UpdateSurfaceMap(",
        "hash": "99c50fdc"
    },
    {
        "start": "void UASTFireFreeFlameComponent::AddStrip(",
        "end": "void UASTFireFreeFlameComponent::UpdateGeometry(",
        "hash": "f599415d"
    }
]
for item in LOCKS:
    begin, end = c.index(item["start"]), c.index(item["end"])
    assert begin < end
    actual = fnv1a32(c[begin:end])
    assert actual == item["hash"], (
        f"Approved fire lifecycle changed at {item['start']}: "
        f"{actual} != {item['hash']}"
    )

# Stage15: a single source mesh section owns full living tongue silhouettes.
src=c[c.index("void UASTFireFreeFlameComponent::UpdateConnectedSource("):
      c.index("void UASTFireFreeFlameComponent::TickComponent(")]
ribbon=c[c.index("void UASTFireFreeFlameComponent::UpdateGeometry("):
         c.index("void UASTFireFreeFlameComponent::BuildSourceAnchors()")]
assert "Necks.Reserve(Tongues.Num()*(RingsPerTongue-1)+Tongues.Num());" in src
assert "static constexpr float NeckFractions[9] =" in src
assert "const FVector WorldB=TongueCenterAt(T,Root,V1,Right);" in src
assert "Necks.Add(Neck);" in src
assert "const float Support=Cover*(.19f+" in src
assert "const float Thermal=FMath::Clamp(" in src
assert "Heat*NeckSupport*Flicker" in src
assert "Depth=FMath::Max(Depth,NeckDepth+1.5f);" in src
assert "StrokeU=FMath::Clamp(" in src
assert "UVs.Add(FVector2D(StrokeU,StrokeV));" in src
assert "Samples.Add(P)" in src and "TArray<float> SampleDepths;" in src
assert "Parent[J]=Best" not in src
assert "FMath::Sqrt(PointD2)" not in src
assert "FMath::Sqrt(EdgeD2)" not in src
assert "H.Add(0.f);" in ribbon
assert ribbon.count("AddStrip(C,W,H")==2  # attached hidden; detached unchanged
assert ribbon.count("AddStrip(BC,BW,BH")==1
assert ribbon.index("UpdateConnectedSource(bCreate, View, Right)") < ribbon.index(
    "const FVector Base=ConnectedRoots.IsValidIndex(I)")
assert "PatchRadius=FMath::Clamp(" in c

from math import sin
def radius(v,width=18.):
    ridge=max(.4,min(1.05,.85+.15*sin(v*13.8)))
    return max(0.,width*(.94-.18*v)*max(0.,1.-v)**.73*ridge)
fractions=[0.,.125,.25,.375,.5,.625,.75,.875,1.]
assert len(fractions)==9
assert radius(0.)>radius(.5)>radius(.875)>radius(1.)==0.
def heat_at(distance,r):
    cover=max(0.,min(1.,1.-(distance-r)/max(4.,r*.45)))
    core=max(0.,min(1.,(r-distance)/max(1.,r)))
    return cover*(.19+.81*core**.56)
assert heat_at(0.,14.)>heat_at(7.,14.)>heat_at(14.,14.)>0
assert heat_at(21.,14.)==0
assert "static constexpr int32 SourceGridX = 32;" in Path(
    "Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireFreeFlameComponent.h"
).read_text()
print("Stage15: lifecycle hashes intact, single living field, no full-body MST/blanket")
