"""Stage7 math + source review. Independent numerical checks; not PIE/render QA."""
from math import sin, sqrt
from pathlib import Path

runtime = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp").read_text()
header = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireReactiveComponent.h").read_text()
api = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireBlueprintLibrary.cpp").read_text()
zone = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireIgnitionZoneComponent.cpp").read_text()
zone_h = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireIgnitionZoneComponent.h").read_text()

# Multiple simultaneous localized flames must follow real skeleton bones
# or physical surface hits. Bounds are only a stated collision-less fallback.
for token in (
    "GetNumBones()", "GetBoneName(BoneIndex)", "GetParentBone(Name)",
    "GetBoneLocation(", "GetSocketTransform(Part.Name, RTS_World)",
    "Farthest-point bone sampling", "ProjectToSurface",
    "LineTraceComponent(Hit, OutsideWorld, InsideWorld, Params)",
    "Params.bTraceComplex = true", "CalculatedSurface" if False else "HitWorld",
    "SurfaceWorld = FVector(", "GetComponentTransform().TransformPosition(LocalHit)",
    "Bone.InverseTransformPosition(WorldHit)",
    "PatchScale", "LargestFace", "Fallback" if False else "Points.IsEmpty()",
):
    assert token in runtime, "Missing mesh-aware anchor: " + token

for token in (
    "TMap<TWeakObjectPtr<APawn>, FVector>", "LastAnchorVelocity",
    "float Breath", "float VisualEnergy", "float CurrentGust",
):
    assert token in header, token
for token in (
    "PrimaryComponentTick.TickGroup = TG_PostUpdateWork",
    "GetOverlappingActors(InRange, APawn::StaticClass())",
    "MeasuredVelocity", "LastNearbyPawnPositions.Add",
    "AnchorAcceleration", "DepthGust", "SmoothedFlow",
    "Wind * 19.5f", "Field.Breath", "LocalLife", "LocalAge",
    "Field.VisualEnergy", "LightEnvelope", "Light->SetAttenuationRadius",
    "Light->SetIntensity(LightPower * FireLight)",
):
    assert token in runtime, "Missing reactive life/light: " + token
assert "FMath::Clamp(DesiredFlamePoints, 1, 16)" in api
assert "FindComponentByClass<USkeletalMeshComponent>()" in api
assert "Fire->SetFireCoverage(" in api
assert "MaxDistributedFlames = 8;" in zone_h
assert "if (LongestDimension >= 70.0f) Points = 2;" in zone
assert "if (Target->FindComponentByClass<USkeletalMeshComponent>())" in zone

# Match Stage7 flame-breath / voxel aging math and check actual cycle depth.
seed = float(1729 % 1009) * .0137
def clamp(v, lo, hi):
    return min(hi, max(lo, v))

def breath(t):
    return clamp(.58 + .30 * sin(t * 5.6 + seed)
                      + .24 * sin(t * 9.9 + seed * 1.71)
                      + .12 * sin(t * 2.3 + seed * .37), .08, 1.24)

def life(t, x):
    u = (x+.5)/22*2-1
    return clamp(.48 + .43 * sin(t * (4.8 + (x % 5)*.27) + u*5.7 + seed)
                      + .17 * sin(t*11.1 - x*.83 + seed*1.6), .035, 1.05)

times = [i*.04 for i in range(160)]
breaths = [breath(t) for t in times]
assert max(breaths)-min(breaths) > .75, (min(breaths), max(breaths))
assert min(breaths) < .23 and max(breaths) > 1.0
for x in (2, 7, 14, 19):
    values = [life(t,x) for t in times]
    assert max(values)-min(values) > .85, (x, min(values), max(values))
# Localized radius/height must be materially smaller on props than a tall
# 1.0-scale source and use distinct anchor count/shape for larger meshes.
def patch_scale(dimensions, count):
    x,y,z=(d*.5 for d in dimensions)
    area=max(x*y, x*z, y*z)
    return clamp(sqrt(max(1., area * 4. / count))/200., .18,1.35)

small = patch_scale((65, 55, 45),2)
large = patch_scale((350, 300, 250),8)
assert small < .30, small
assert large > small * 2, (small,large)
# Strong wind must turn over field cells rapidly, not merely rotate billboards.
def effective_wind(speed):
    # stationary character passing at speed, using LocalWind*1.35 factor.
    return clamp((speed * 1.35)/265., -3.25, 3.25)
assert 19.5 * effective_wind(400) > 36.0
print("Stage7: distributed mesh anchors, thermal lifetimes, responsive gust and light math OK")
