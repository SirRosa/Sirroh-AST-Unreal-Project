"""Stage17 regression: irregular living tips, same thermal palette, curved detached paths.

These are structural/numerical guards, not PIE, GPU, or FPS tests.
"""
from pathlib import Path
from math import sin
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
h=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireFreeFlameComponent.h").read_text()
m=Path("Automation/ASTFire/setup_stage10_lifecycle.py").read_text()
def fnv(s):
    value=2166136261
    for char in s:
        value=((value^ord(char))*16777619)&0xffffffff
    return f"{value:08x}"
def region(a,b):
    return c[c.index(a):c.index(b)]
# Critical approved Stage16 lifecycle/timing and strip visual input locked.
locks=[
("void UASTFireFreeFlameComponent::ConfigureGeneration(",
 "void UASTFireFreeFlameComponent::RebuildClusters(","957fd72e"),
("void UASTFireFreeFlameComponent::AdvanceSimulation(",
 "void UASTFireFreeFlameComponent::UpdateSurfaceMap(","99c50fdc"),
("void UASTFireFreeFlameComponent::AddStrip(",
 "void UASTFireFreeFlameComponent::UpdateGeometry(","f599415d")]
for start,end,expected in locks:
    assert fnv(region(start,end))==expected,start
field=region("void UASTFireFreeFlameComponent::UpdateConnectedSource(",
             "void UASTFireFreeFlameComponent::TickComponent(")
# All attached flames/forks belong to ONE live-field source, no extra sprites.
for token in (
    "const float TipHook=Upper*",
    "Lean*V+Dance+TipHook",
    "const float ForkSeed=ASTFireLife::Hash(T.Seed*.173f);",
    "ForkSeed>.82f?2:(ForkSeed>.28f?1:0)",
    "AppendNeck(WorldA,WorldB,RadiusAt(V0),RadiusAt(V1),V0,V1);",
    "AppendNeck(BranchRoot,Mid,BranchRadius,",
    "AppendNeck(Mid,Tip,BranchRadius*.51f,",
    "const float Tips=FMath::Clamp(1.f+Upper*(",
    "Necks.Reserve(Tongues.Num()*(RingsPerTongue+4));"):
    assert token in c,token
assert c.count("H.Add(0.f);")==1
assert "Parent[J]=Best" not in field
assert "Heat*NeckSupport*Flicker" in field
# Existing material and palette, comparable birth heat vs parent, delayed cooling.
assert "Render->SetMaterial(0, Material)" in c
assert "Render->SetMaterial(1, Material)" in c
assert "float2 q=floor(UV*float2(15.,45.))" in m
for stop in (".12,h", ".24,h", ".43,h", ".61,h", ".77,h", ".90,h"):
    assert stop in m
assert "Child.Temperature=.92f + .07f*Uniform(Salt+8);" in c
assert "D.Temperature=.70f" not in c
assert "H.Add(D.Temperature*(1.04f-.18f*V)*" in c
assert "FMath::Pow(Fade,.66f)" in c
def heat(temp,v,fade):return temp*(1.04-.18*v)*fade**.66
assert .9<heat(.95,.5,1.)<1.
assert heat(.95,.5,.5)>.5
assert heat(.95,.5,0.)==0.
# Curved world-space flight: separate vortex pass, preserved approved simulation.
assert "void UpdateDetachedVortices(float Dt);" in h
assert "void UASTFireFreeFlameComponent::UpdateDetachedVortices(float Dt)" in c
assert "D.Right=Right;" in c
assert "D.Velocity=(D.Velocity+Vortex*Dt*(.43f+.57f*Fade))" in c
assert "const FVector Vortex=Side*FMath::Sin(Phase)*" in c
assert "Depth*FMath::Cos(Phase*.77f+D.Seed*.31f)*145.f" in c
assert "AdvanceSimulation(SimDt);" in c
assert "UpdateDetachedVortices(SimDt);" in c
assert c.index("AdvanceSimulation(SimDt);")<c.index("UpdateDetachedVortices(SimDt);")
assert "const float Angle=FMath::Sin(Spin)*.75f;" in c
assert "LocalUp*D.Length*V*Fade" in c
assert "FMath::Sin(V*14.2f+Spin+D.Seed)" in c
# Nonlinear force changes direction through lifetime.
points=[(sin(i*.12*5.3),sin(i*.12*5.3*.77+1.2)) for i in range(8)]
assert len({tuple(round(x,2) for x in p) for p in points})==len(points)
assert any(points[i][0]*points[i+1][0]<0 for i in range(7))
# Existing lifecycle and actual spatial integration remain intact.
for token in (
"T.Life = EFlameLife::Bridging;",
"T.Life = EFlameLife::Retracting;",
"T.Life = EFlameLife::Growing;",
"D.Position+=D.Velocity*Dt;",
"T.CurrentLength = FMath::Lerp(T.SplitLength,T.RetractTarget,Ease);"):
    assert token in c
print("Stage17: approved life/timing intact; organic tip forks, palette parity, swirling detached flames OK")
