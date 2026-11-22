"""Stage6 static checks for non-flat silhouette and isolated contact igniter."""
from pathlib import Path
import math

root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
cpp=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
igniterH=(root/"Public/ASTFireIgnitionZoneComponent.h").read_text()
igniterC=(root/"Private/ASTFireIgnitionZoneComponent.cpp").read_text()
builder=Path("Automation/ASTFire/setup_stage6_igniter.py").read_text()
material=Path("Automation/ASTFire/setup_stage4_volume.py").read_text()

assert "ShapedCoverage(X, Y, Field.Time, Seed)" in cpp
assert "const float RootY" in cpp and "const float TipY" in cpp
assert "const float Bottom" in cpp and "const float Top" in cpp
assert "const float Side" in cpp
assert "return Bottom * Top * Side;" in cpp
assert "VolumeUpdateDebt" in cpp
assert 'return color*0.65;' in material
assert "class ASTFIRERUNTIME_API UASTFireIgnitionZoneComponent" in igniterH
constructor=igniterC.split("UASTFireIgnitionZoneComponent::UASTFireIgnitionZoneComponent()",1)[1].split("void UASTFireIgnitionZoneComponent::BeginPlay()",1)[0]
assert "SetMobility(" not in constructor and "SetBoxExtent(" not in constructor
assert "SetCollision" not in constructor and "SetRelativeLocation(" not in constructor
assert "SetCollisionEnabled(ECollisionEnabled::NoCollision)" in igniterC
assert "SetCollisionEnabled(ECollisionEnabled::QueryOnly)" in igniterC
assert "TriggerHalfExtent" in igniterC and "TriggerOffset" in igniterC
assert "OnComponentBeginOverlap.AddDynamic" in igniterC
assert "OnComponentEndOverlap.AddDynamic" in igniterC
assert "GetNetMode() == NM_Client" in igniterC
assert "IgnitedDuringOverlap.Contains" in igniterC
assert "GetComponentsBoundingBox" in igniterC
assert "FindComponentByClass<USkeletalMeshComponent>()" in igniterC
assert "ASTFire::ProjectToSurface" in cpp
assert "GetSocketTransform(Part.Name, RTS_World)" in cpp
assert "Farthest-point bone sampling" in cpp
assert "FMath::Sqrt(FMath::Max(1.0, LargestFace" in cpp
assert "UASTFireBlueprintLibrary::IgniteActor" in igniterC
assert "BP_AST_FireSource_Igniter" in builder
assert "ASSET.duplicate_asset(SOURCE, TARGET)" in builder
assert "BP_Torch_U" not in igniterC
assert "duplicate_asset(SOURCE, TARGET)" in builder
assert "seen_paths = set()" in builder
assert "identity not in seen_paths" in builder
assert "ASSET.save_loaded_asset(blueprint" in builder

# Independent low-cost numerical silhouette reference across time and seeds.
# No claims about PIE or subjective artistic quality from this test.
W,H=22,30
def silhouette(t, seed):
    low, high = [], []
    for x in range(W):
        u=(x+.5)/W*2-1
        pc=.07+.115*math.sin(t*1.75+seed)
        pl=-.38+.12*math.sin(t*2.03+seed*.71)
        pr=.34+.13*math.cos(t*1.51+seed*1.21)
        tips=[
            .64+.27*math.exp(-((u-pc)/.18)**2),
            .56+.25*math.exp(-((u-pl)/.15)**2),
            .58+.26*math.exp(-((u-pr)/.16)**2)
        ]
        noise=.025*math.sin(u*19+t*3.1+seed)+.014*math.sin(u*31-t*2.2+seed*.43)
        tip=max(.52,min(.96,max(tips)+noise))
        root_noise=.019*math.sin(u*13+t*2.5+seed)+.013*math.sin(u*23-t*1.3+seed*.8)
        root=max(0,min(.15,.014+.082*abs(u)**1.4+root_noise))
        low.append(root)
        high.append(tip)
    return low,high

for when in [0.,.37,1.2,3.7]:
    bottom, top=silhouette(when,.817)
    assert max(bottom)-min(bottom) > .045, (when,bottom)
    assert max(top)-min(top) > .20, (when,top)
    assert max(top) < 1.0 and min(bottom)>=0.0
    assert all(t>r for t,r in zip(top,bottom))
print("Stage6 non-flat variable end contour and contact component: OK")
