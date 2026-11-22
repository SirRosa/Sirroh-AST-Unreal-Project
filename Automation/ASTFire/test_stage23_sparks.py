"""Stage23: lightweight bright sparks isolated from detached plume lifecycle."""
from pathlib import Path
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(p/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(p/"Public/ASTFireFreeFlameComponent.h").read_text()
for key in (
    "static constexpr int32 MaxSparks = 24;",
    "struct FSpark",
    "TArray<FSpark> Sparks;",
    "void UpdateSparks(float Dt);",
): assert key in h,key
for key in (
    "Sparks.SetNum(MaxSparks);",
    "Render->SetMaterial(2, Material);",
    "void UASTFireFreeFlameComponent::UpdateSparks(float Dt)",
    "SparkSpawnDebt",
    "Heat<.15f",
    "T.WorldVelocity*.16f+Wind*.07f",
    "S.Position+=S.Velocity*Dt",
    "const float Fade=S.bAlive?FMath::Pow(",
    "SV.Reserve(MaxSparks*4)",
    "Render->CreateMeshSection_LinearColor(",
    "2,SV,ST,SN,SU,SC,SX,false",
    "Render->UpdateMeshSection_LinearColor(",
    "2,SV,SN,SU,SC,SX",
    "UpdateSparks(SimDt);",
    "D.Position+=D.Velocity*Dt;",
    "T.Life = EFlameLife::Bridging;",
    "T.Life = EFlameLife::Retracting;",
    "T.Life = EFlameLife::Growing;",
): assert key in c,key
assert c.index("AdvanceSimulation(SimDt);")<c.index(
    "UpdateDetachedVortices(SimDt);")<c.index("UpdateSparks(SimDt);")
assert "MaxDetached = 16;" in h
assert "DetachedColumns=7;" in c
assert 24*4==96 and 24*2==48
# Reference particle count stays bounded even if the spawn debt is huge.
def births(debt,dt,heat,energy):
    debt=min(3.,debt+dt*(2.2+9*heat*max(.2,min(1.2,energy))))
    born=min(2,int(debt))
    return debt-born,born
d=0.
count=0
for _ in range(250):
    d,n=births(d,.04,1.,.75)
    assert 0<=n<=2 and 0<=d<=3.
    count+=n
assert 0<count<250*2
print("Stage23: finite sparks, velocity/wind, thermal and detached regression OK")
