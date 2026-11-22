"""Stage16 numerical/structural guard for burn coverage, detached mass and dance.

This is NOT an Unreal GPU or gameplay/PIE test. It prevents reintroduction
of Stage15's feet-only confinement and tiny uniform detached ember shapes.
"""
from pathlib import Path
from math import pow, sin
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()

# Coverage of an animated SkeletalMesh spans all its skinned geometry.
assert "DesiredCount = 10 + FMath::RoundToInt(Uniform(731) * 4.f);" in c
assert "FBox CandidateBounds(ForceInit);" in c
assert "-FVector::DistSquared(P, SurfaceCenter)" in c
assert "PatchRadius=FMath::Clamp(" not in c
assert "Skeletal->Bounds.BoxExtent.GetMax()*.42f" not in c
assert "Root.SkinnedVertexIndex = Best;" in c
assert "Candidates.Remove(Best);" in c
# Real collision traces on SIX static faces, then furthest-point selection.
assert "SurfaceCandidates.Reserve(96);" in c
assert "for(int32 Face=0;Face<6;++Face)" in c
assert "for(int32 A=0;A<4;++A)" in c
assert "for(int32 B=0;B<4;++B)" in c
assert "SurfaceCandidates.RemoveAtSwap(Best);" in c
assert "Query.bTraceComplex=true;" in c
assert "if(!Static->LineTraceComponent(Hit," in c
assert "continue; // Never invent anchors outside real collision." in c
assert "DesiredCount = MaxSide < 140.f ? 8 +" in c
assert "MaxTongues = 15" in h
assert "int32 DesiredCount = 5; // retain compact free-standing fire" in c

# Shape distributions have noticeably different lengths and widths,
# rather than the narrow former ranges (26..106 cm, 8..26 cm).
assert "(19.f + 158.f *" in c
assert "FMath::Pow(Uniform(Salt + 2),1.34f)" in c
assert "(5.5f + 38.f *" in c
assert "FMath::Pow(Uniform(Salt + 3),1.65f)" in c
def length(u): return 19+158*u**1.34
def width(u): return 5.5+38*u**1.65
assert length(.9)>3.5*length(.1)
assert width(.9)>4*width(.1)
assert 18<length(0.)<length(.5)<length(1.)<180
assert width(0.)<width(.5)<width(1.)<45

# Independent upward/lateral motion. The root stays fixed at V=0;
# sustained nonzero motion at the tips uses each tongue's own seed/phase.
assert "const float SwaySpeed=1.7f+3.6f*" in c
assert "SwayAmplitude*FMath::Pow(V,1.35f)" in c
assert "Lean*V+Dance" in c
assert "Tongue.Seed*.79f" in c
assert "Tongue.Seed*.31f" in c

# A detached ember must be comparable to the actual length of mother flame
# lost at retraction (1 - RemainingFraction) * mature height.
assert "Child.Length=Length*(1.f-Tongue.RemainingFraction)*" in c
assert "(.82f+.25f*Uniform(Salt+4))" in c
assert "Child.Width=Tongue.CurrentWidth*(.76f+.36f*Uniform(Salt+5));" in c
assert "Child.Position=TongueCenterAt(Tongue,Base,1.f,Right);" in c
assert "FVector::RightVector*(Uniform(Salt+10)*2.f-1.f)*110.f" in c
assert "FVector::ForwardVector*(Uniform(Salt+11)*2.f-1.f)*95.f" in c
for remaining in (.30,.40,.57):
    lost=1-remaining
    for u in (0,.5,1):
        detached=lost*(.82+.25*u)
        assert .8*lost<=detached<=1.1*lost
# Actual split bridge timings, retraction, and detached simulation remain
# exactly unchanged from Stage15; their full function hash is separately
# locked in test_stage14_mantle_neck.py.
assert "T.BridgedChild = EmitTip(T,VisualBase,T.SplitLength,Flow);" in c
assert "T.CurrentLength = FMath::Lerp(T.SplitLength,T.RetractTarget,Ease);" in c
assert "D.Position+=D.Velocity*Dt;" in c
assert "Child.Lifetime=.52f + .68f*Uniform(Salt+3);" in c
assert "Child.Temperature=.92f + .07f*Uniform(Salt+8);" in c
assert "UpdateDetachedVortices(SimDt);" in c

print("Stage16 coverage/variance: skeletal all-body, static six-face, detached mass and asymmetric dance OK")
