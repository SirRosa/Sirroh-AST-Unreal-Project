"""Stage19: isolate ONLY live RadiusAt slimming; ensure NO regression in Stage18 detached fire.

The tests are deterministic/structural, not a substitute for user PIE visual inspection.
"""
from pathlib import Path
from math import exp, sin
s=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
source_start="void UASTFireFreeFlameComponent::UpdateConnectedSource("
source_end="void UASTFireFreeFlameComponent::TickComponent("
radius_start="        auto RadiusAt=[&](float V)"
radius_end="        FVector WorldA=TongueCenterAt("
def fnv(text):
    h=2166136261
    for char in text:
        h=((h^ord(char))*16777619)&0xffffffff
    return f"{h:08x}"
start,begin,end,stop=(s.index(source_start),s.index(radius_start),
                      s.index(radius_end,s.index(radius_start)),s.index(source_end))
assert start<begin<end<stop
# Byte-level guards captured from deployed Stage18 f36fa0e: no other
# changes to source root gathering, fork generation, mesh depth and thermal logic.
assert fnv(s[start:begin])=="fbf2d826"
assert fnv(s[end:stop])=="30489c64"
# Stage18 detached 7-column radial gradient and split bridge remain identical.
geo=s[s.index("void UASTFireFreeFlameComponent::UpdateGeometry("):
      s.index("void UASTFireFreeFlameComponent::BuildSourceAnchors()")]
assert fnv(geo)=="94d99736"
life=s[s.index("void UASTFireFreeFlameComponent::AdvanceSimulation("):
       s.index("void UASTFireFreeFlameComponent::UpdateSurfaceMap(")]
assert fnv(life)=="99c50fdc"
# No new geometry, no altered materials/particle systems, no reset lifetimes.
assert "static constexpr int32 DetachedColumns=7;" in geo
assert "Heat*D.Temperature*LocalHeat*" in geo
assert "UpdateDetachedVortices(SimDt);" in geo or "UpdateDetachedVortices(SimDt);" in s
assert s.count("H.Add(0.f);")==1

radius=s[begin:end]
assert "T.CurrentLength*(.058f+.010f*" in radius
assert "const float BulgeA=.26f*FMath::Exp(" in radius
assert "const float BulgeB=.12f*FMath::Exp(" in radius
assert "const float Waist=.14f*FMath::Exp(" in radius
assert "const float UpperTaper=FMath::Lerp(1.f,.55f," in radius
assert "(.88f+BulgeA+BulgeB-Waist+MovingRim)*UpperTaper" in radius
assert ".42f,1.28f" in radius
assert ".098f+.017f" not in radius
assert ".46f*FMath::Exp" not in radius
assert ".55f*FMath::Exp" not in radius

# Geometrical reference: lower body stays wide, upper half tapers, true tip
# tends to zero. Test several seeds/time phases to avoid one lucky silhouette.
def smooth(x):
    t=max(0.,min(1.,x))
    return t*t*(3.-2.*t)
def profile(v,base_width=8.,length=170.,seed=.5,elapsed=0.):
    width=max(base_width,length*(.058+.010*seed))
    lower=.26*exp(-((v-(.28+.04*seed))/.18)**2)
    mid=.12*exp(-((v-(.46+.03*seed))/.16)**2)
    waist=.14*exp(-((v-.58)/.09)**2)
    upper=1+(.55-1)*smooth((v-.42)/.40)
    moving=.10*sin(v*18.+seed+elapsed*3.2)+.06*sin(v*31.-seed-elapsed*4.8)
    q=max(.42,min(1.28,(.88+lower+mid-waist+moving)*upper))
    return width*max(0.,1.-v)**.69*q
for seed in (.05,.3,.55,.85):
    for elapsed in (.0,.4,.8):
        low=profile(.28,seed=seed,elapsed=elapsed)
        high=profile(.80,seed=seed,elapsed=elapsed)
        assert low>high*3.,(seed,elapsed,low,high)
        assert profile(.95,seed=seed,elapsed=elapsed)<low*.18
        assert profile(1.,seed=seed,elapsed=elapsed)==0.
        assert low>5.
print("Stage19: lower-weight plume, tapered top; Stage18 palette, detached and lifecycle hashes unchanged")
