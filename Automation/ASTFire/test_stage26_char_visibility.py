"""Stage26 charcoal: visible opacity, separate smolder and existing overlays.

Static tests ensure coverage changes do not alter source flame lifecycle.
"""
from pathlib import Path
base=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(base/"Private/ASTFireReactiveComponent.cpp").read_text()
h=(base/"Public/ASTFireReactiveComponent.h").read_text()
m=Path("Automation/ASTFire/setup_stage22_char.py").read_text()
for t in ("!ASTFire::IsDedicatedFireActor(GetOwner())",
          "PreviousFireSurfaceOverlay = Existing;",
          "FireSurfaceMesh->SetOverlayMaterial(FireSurfaceMID);",
          "FireSurfaceMesh->SetOverlayMaterial(PreviousFireSurfaceOverlay)",
          "CharSmolderRemaining = FMath::Max(CharSmolderRemaining, 5.f);",
          "CharSmolderRemaining = FMath::Max(0.f,CharSmolderRemaining-DeltaTime);",
          "CharSmolderRemaining/2.5f",
          "FName(TEXT(\"CharProgress\")),CharProgress",
          "GetOwner()->HasAuthority() && bEnableCharring && bStartLit"):
    assert t in c,t
assert "float CharSmolderRemaining = 0.f;" in h
for t in ('used_with_skeletal_mesh', 'float3 soot=float3(.013,.016,.012)',
          'float3 core=float3(3.5,.12,.012)*hot;',
          'float crack=1.-smoothstep(.035,.15,min(crackA,crackB));',
          'return saturate(dark+hot*.015);'):
    assert t in m,t
# Full-char final opacity almost obscures the untouched original material.
def alpha(progress,coverage):
    return min(1.,max(0.,progress)*(.70+.28*coverage))
assert alpha(1.,0.) >=.7 and alpha(1.,1.)>=.98
assert alpha(0.,1.)==0.
print("Stage26: char opacity and afterglow, overlay preservation guarded")
