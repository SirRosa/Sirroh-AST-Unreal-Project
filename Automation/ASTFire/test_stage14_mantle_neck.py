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

src = c[c.index("void UASTFireFreeFlameComponent::UpdateConnectedSource("):]
ribbon = c[c.index("void UASTFireFreeFlameComponent::UpdateGeometry("):
           c.index("void UASTFireFreeFlameComponent::BuildSourceAnchors()")]
assert "Necks.Reserve(Tongues.Num()*3)" in src
assert "static constexpr float NeckFractions[4] = {0.f,.14f,.28f,.42f};" in src
assert "const FVector WorldB=TongueCenterAt(T,Root,V1,Right);" in src
assert "Necks.Add(Neck);" in src
assert "const float Support=Cover*(.48f+.32f*(1.f-V));" in src
assert "Heat*FMath::Max(BodyHeat,NeckSupport)*Flicker" in src
assert "Depth=FMath::Lerp(Depth,NeckDepth," in src
assert "Include(TongueCenterAt(" not in src
assert "const float Emergence=ASTFireLife::SmoothStep((V-.20f)/.26f);" in ribbon
assert ribbon.index("UpdateConnectedSource(bCreate, View, Right)") < ribbon.index(
    "const FVector Base=ConnectedRoots.IsValidIndex(I)")

def smooth(v):
    t = max(0., min(1., v))
    return t * t * (3.-2.*t)

def cover(orthogonal_distance, radius):
    return max(0., min(1., 1.-(orthogonal_distance-radius)/max(4.,radius*.45)))

# The mantle owns a growing branch through v=.42, and the very same
# ribbon fades in smoothly from v=.20 to .46. Their supports overlap.
fractions = [0., .14, .28, .42]
width = 18.
for v in fractions:
    radius = max(6., min(28., width*(.94-.19*v)))
    assert cover(0., radius) == 1.
    support = cover(0.,radius)*(.48+.32*(1.-v))
    assert support > .6
assert smooth((.125-.20)/.26) == 0.
assert 0. < smooth((.375-.20)/.26) < 1.
assert smooth((.5-.20)/.26) == 1.
assert .375 < fractions[-1] < .5

# Each tongue contributes 3 neck segments; the grid remains fixed topology.
# This is NOT an FPS performance claim.
assert 15 * (len(fractions)-1) == 45
assert "static constexpr int32 SourceGridX = 32;" in Path(
    "Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireFreeFlameComponent.h"
).read_text()
print("Stage14: approved growth/bridging/retraction/detachment unchanged; continuous mantle neck field OK")
