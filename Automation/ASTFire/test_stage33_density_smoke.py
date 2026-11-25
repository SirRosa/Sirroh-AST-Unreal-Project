"""Stage33 numerical/reference and source checks (not an Unreal runtime test)."""
from pathlib import Path
from math import hypot
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(p/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(p/"Public/ASTFireFreeFlameComponent.h").read_text()
def params(size,j=.5,r=.5):
    scale=max(0,min(1,(size-16)/220))
    height=(125+875*scale**1.25)*(.83+.17*j)
    speed=(35+50*scale)*(.94+.12*r)
    radius=(10+36*scale)*(.9+.2*r)
    life=min(14,(height-radius)/speed*(1.04+.08*j))
    return height,speed,radius,life
small=params(16); mid=params(110); large=params(280,1,1)
assert 100<small[0]<150 and small[1]<40
assert small[0]<mid[0]<large[0]<=1000
assert small[2]<mid[2]<large[2]
assert large[1]<100 and large[3]>small[3]
for size in (8,16,30,100,200,280):
    H,v,r,t=params(size)
    assert 0<r<H<=1000 and 0<v<100 and t<=14
    # Average carrier spacing is smaller than the diameter at full heat.
    rate=4.2+1.1*max(0,min(1,(size-16)/220))
    assert v/rate < 2*r
    assert rate*t<=72
def alpha(age,life):
    return max(0,min(1,(life-age)/(life*.24)))
assert alpha(0,5)==1 and alpha(3.5,5)==1
assert .49<alpha(4.4,5)<.51 and alpha(5,5)==0
# Every rendered square corner stays within the swept support radius.
for radius in (8,20,60):
    for pitch in (2.5,5,9):
        safe=radius-pitch*.72
        if safe>0: assert safe+hypot(pitch/2,pitch/2)<=radius
assert "SmokeGridMaxX = 64" in h and "SmokeGridMaxY = 112" in h
assert "SmokeSimDebt<.079f" in c and "S.Velocity.GetClampedToMaxSize(135.f)" in c
assert "S.WaveAmplitude=5.f+9.f*FMath::Abs(Random)" in c
# Palette has five genuinely separated linear neutral shades.
palette=(.018,.065,.17,.36,.64)
assert len(palette)==5 and all(a<b for a,b in zip(palette,palette[1:]))
print("Stage33: proportional scale, bounded pool, overlap, terminal fade and swept support reference")

# Test the actual native audit is invoked by the material commandlet.
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
assert "run_smoke_audit(m)" in m and 'audit.startswith("PASS:")' in m
assert "UWorld::CreateWorld(EWorldType::Game,false)" in c
assert "Trigger->SetCollisionResponseToAllChannels(ECR_Overlap)" in c
assert "bMeshClip" in c and "bStayedAlive" in c
assert "OverlapBlockingTestByChannel" in c
assert "SmokePixelPitch" in h
# Representative lighting inputs span all five palette indices.
def tone(height,light,facing,core):
    shade=max(0,min(1,.39-.32*height+.43*light+.42*facing-.32*core))
    return min(4,int(shade*5))
assert {tone(h,l,f,k) for h in (0,.25,.5,.75,1) for l in (0,.3,.6,1) for f in (0,.5,1) for k in (0,.5,1)}==set(range(5))
print("Stage33 audit: collision fixtures and five achievable light bands")
