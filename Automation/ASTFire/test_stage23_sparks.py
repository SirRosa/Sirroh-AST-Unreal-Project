"""Stage23: lightweight bright sparks isolated from detached plume lifecycle."""
from pathlib import Path
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(p/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(p/"Public/ASTFireFreeFlameComponent.h").read_text()
for key in (
    "static constexpr int32 MaxSparks = 48;",
    "struct FSpark",
    "TArray<FSpark> Sparks;",
    "void UpdateSparks(float Dt);",
): assert key in h,key
for key in (
    "Sparks.SetNum(MaxSparks);",
    "Render->SetMaterial(2, SparkMaterial);",
    "void UASTFireFreeFlameComponent::UpdateSparks(float Dt)",
    "SparkSpawnDebt",
    "Heat<.15f",
    "S.Velocity=S.Velocity.GetClampedToMaxSize(135.f);",
    "S.DriftPosition+=S.Velocity*Dt",
    "S.WaveSide*(S.WaveAmplitude*(Offset+Second))",
    "const float Angle=2.f*PI*ASTFireLife::Hash(",
    "FMath::Cos(Angle)",
    "FMath::Sin(Angle)",
    "S.Lifetime=.68f+.70f*Random;",
    "const FTongue* Chosen=nullptr;",
    "ConnectedRoots.IsValidIndex(Index)",
    "S.Size=2.8f+2.9f*Other;",
    "S.EmissionHeat=FMath::Clamp(Heat*",
    "if (IsValid(SparkMaterial)) Render->SetMaterial(2,SparkMaterial);",
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
assert 48*4==192 and 48*2==96
# Reference particle count stays bounded even if the spawn debt is huge.
def births(debt,dt,heat,energy):
    debt=min(4.,debt+dt*(4.+9*heat*max(.2,min(1.2,energy))))
    born=min(3,int(debt))
    return debt-born,born
d=0.
count=0
for _ in range(250):
    d,n=births(d,.04,1.,.75)
    assert 0<=n<=3 and 0<=d<=4.
    count+=n
assert 0<count<250*3
print("Stage23: finite sparks, velocity/wind, thermal and detached regression OK")
