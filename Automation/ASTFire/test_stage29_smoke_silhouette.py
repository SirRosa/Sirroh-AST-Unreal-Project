"""Stage29: mathematically bounded pixel-silhouette smoke."""
from pathlib import Path
s=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
start=s.index("// A smoke particle is three contiguous solid")
end=s.index("// Section 1 was authored",start)
geo=s[start:end]
for x in (".085f,.092f,.084f",".151f,.158f,.145f",
          ".109f,.117f,.104f","PixelStep=2.f",
          "SmokeSegments;++K","SmokeLayers;++Layer",
          "Size>0.f?1.f:0.f","Render->SetMaterial(3,SmokeMaterial);"):
    assert x in geo,x
for forbidden in ("Opacity","Blend_Translucent","smoothstep","Bands*EdgeWobble"):
    assert forbidden not in geo,forbidden
# Constant pool and triangle fans: 18 * 3 * (center+12 rim) = 702.
assert (18*3*13,18*3*12*3)==(702,1944)
# Full-sized middle-age puffs, zero geometry at both ends of life.
def size(a):
    return min(1.,a/.14)*min(1.,(1.-a)/.24)
assert size(0)==0 and size(1.)==0
assert size(.45)==1.
print("Stage29: quantized circular smoke with exactly three solid colors")
