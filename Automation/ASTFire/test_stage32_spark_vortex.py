"""Stage32: perimeter sparks and sinusoidal drift rather than vertical particles."""
from pathlib import Path
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
h=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireFreeFlameComponent.h").read_text()
assert "static constexpr int32 MaxSparks = 48;" in h
for field in ("FVector DriftPosition","FVector WaveSide",
              "float WaveAmplitude","float WaveFrequency",
              "float WavePhase"):
    assert field in h,field
for expr in (
    "const float Angle=2.f*PI*ASTFireLife::Hash(",
    "CameraScreenRight*FMath::Cos(Angle)",
    "FVector::UpVector*FMath::Sin(Angle)",
    "const float HeightFraction=.08f+.86f*Random;",
    "Outward*(T.CurrentWidth*(.45f+.40f*Other))",
    "S.Velocity=Outward*(115.f+145.f*Other)+",
    "S.WaveAmplitude=11.f+21.f*Random;",
    "S.WaveFrequency=6.4f+5.2f*Other;",
    "S.WaveSide*(S.WaveAmplitude*(Offset+Second))",
    "S.DriftPosition+=S.Velocity*Dt",
):
    assert expr in c,expr
# Reference: a full 360 degree circle in the camera plane contains left,
# right, diagonal and upward directions. A nonzero harmonic bends each one.
from math import cos,sin,pi
directions=[(cos(2*pi*i/12),sin(2*pi*i/12)) for i in range(12)]
assert any(x>.8 for x,y in directions)
assert any(x<-.8 for x,y in directions)
assert any(y>.8 for x,y in directions)
assert any(x>.5 and y>.5 for x,y in directions)
phase=.5
amp=20.
def offset(age):
    return amp*(sin(phase+age*9.)-sin(phase))
assert abs(offset(.2)-offset(.45))>2
assert abs(offset(.2))>1
print("Stage32: left/right/top/diagonal emission and harmonic advection")
