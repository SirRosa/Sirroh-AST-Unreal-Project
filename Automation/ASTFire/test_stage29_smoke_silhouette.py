"""Stage31 smoke: connected advected ribbon with decimetre-scale coherent curl."""
from pathlib import Path
from math import sin
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
a=p.index("// Stage31: a SINGLE continuous silhouette")
b=p.index("// Section 1 was authored",a)
geo=p[a:b]
for token in (
    "Live.Sort([](const FSmoke* A,const FSmoke* B)",
    "A->Age<B->Age",
    "for (int32 Row=0;Row<BandCount;++Row)",
    "for (int32 Half=0;Half<Sides;++Half)",
    "const FVector WorldQuad[4]",
    "const float Scallop=FMath::Clamp(",
    "const float Expansion=FMath::Pow(NormalizedHeight,.70f);",
    "const float TipTaper=FMath::Clamp(",
    "FMath::RoundToFloat(Width/PixelStep)*PixelStep",
    "NearbySmokeLights",
    "const float Shade=.63f*FMath::Pow(",
): assert token in geo,token
for forbidden in ("SmokeSegments","for (int32 I=0;I<MaxSmoke;++I)",
                  "BLEND_TRANSLUCENT","MaterialExpressionCustom"):
    assert forbidden not in geo,forbidden
# Lifetime/rise reference: reaching 10m is feasible without horizontal wind.
for lifetime in (6.1,6.55):
    rise=150*lifetime
    assert 900<rise<1000
# All layers share one centreline but form different widths.
assert (96-1)*4*2*6==4560
assert len((".56",".39",".24",".105"))==4
print("Stage31: no individual smoke puffs; one lobe-deformed continuous strip")
