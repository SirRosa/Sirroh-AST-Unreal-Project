"""Stage21: bounded fuel lifetime, 7 second reference, re-ignition and server authority.

Numerical and source guards; CI does not claim a runtime PIE result.
"""
from pathlib import Path
from math import pow
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(root/"Public/ASTFireReactiveComponent.h").read_text()
c=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
flame=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
for token in (
    "bool bAutoExtinguish = true;",
    "float BaseBurnSeconds = 7.0f;",
    "float BurnFadeSeconds = 0.8f;",
    "float ReferenceSizeCm = 180.0f;",
    "float MaxBurnSizeMultiplier = 3.5f;",
    "float BurnDurationSeconds = 0.0f;",
): assert token in h, token
for token in (
    "GetOwner()->HasAuthority() && bAutoExtinguish &&",
    "!ASTFire::IsDedicatedFireActor(GetOwner()) && bStartLit",
    "BurnElapsedSeconds += DeltaTime;",
    "BurnDurationSeconds = ComputeBurnDuration();",
    "InitialBurnIntensity * FMath::Pow(Fade, 1.2f)",
    "bStartLit = false;",
    "const bool bAfterglow = bPlaying && SmoothIntensity > .012f;",
    "FreeFlames->SetFlamesVisible(bShowFire);",
    "const bool bWasLit = bStartLit && Intensity > 0.005f;",
    "if (bStartLit && !bWasLit)",
    "PrimaryComponentTick.TickInterval = 0.04f;",
): assert token in c, token
assert "D.Position+=D.Velocity*Dt;" in flame
assert "T.Life = EFlameLife::Growing;" in flame
assert "T.RestartLength=FMath::Min(FMath::Min(Remaining,ResetCap),T.MaxLength*.18f);" in flame
assert "T.Life = EFlameLife::Bridging;" in flame
def duration(size,base=7.,ref=180.,cap=3.5):
    factor=max(3/7,min(cap,(max(10.,size)/max(10.,ref))**.66))
    return max(.1,base)*factor
assert abs(duration(180.)-7.)<1.e-10
assert abs(duration(50.)-3.)<.12
assert duration(50.)<duration(100.)<duration(180.)<duration(300.)<duration(600.)
assert duration(1e9)<=24.5
assert abs(duration(1.)-3.)<1.e-9
assert "Extent.GetAbs()*Primitive->GetComponentScale().GetAbs()*2.f" in c
assert "Asset->GetBounds().BoxExtent" in c
assert "RunBurnDurationAudit(" in c
for seconds in (3.,7.,24.5):
    fade_start=max(0.,seconds-.8)
    levels=[max(0.,min(1.,(seconds-i*.01)/max(.01,seconds-fade_start)))**1.2
            for i in range(int(seconds/.01)+2) if i*.01>=fade_start]
    assert all(a>=b for a,b in zip(levels,levels[1:]))
    assert levels[-1]<.02
print("Stage21: authority, bounded duration, smooth fade, refuel/reignite guards OK")
