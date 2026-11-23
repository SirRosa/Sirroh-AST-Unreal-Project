"""Stage22: procedural UV-attached charring with preserved base materials."""
from pathlib import Path
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(root/"Public/ASTFireReactiveComponent.h").read_text()
c=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
mat=Path("Automation/ASTFire/setup_stage22_char.py").read_text()
install=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()
for s in (
    "bEnableCharring = true;",
    "CharProgress = 0.0f;",
    "float CharringRate = 1.5f;",
    "M_AST_FireChar",
    "DOREPLIFETIME(UASTFireReactiveComponent, CharProgress)",
    "GetOwner()->HasAuthority() && bEnableCharring && bStartLit",
    "DeltaTime*FMath::Max(0.f,Intensity)",
    "FName(TEXT(\"CharProgress\")),CharProgress",
    "FName(TEXT(\"CharGlow\"))",
    "if (bCharOverlay)",
    "CharProgress > .001f",
    "GetOverlayMaterial(false, 0)",
    "FireSurfaceMesh->SetOverlayMaterial(PreviousFireSurfaceOverlay)",
):
    assert s in h+c, s
assert "if (bEnableProceduralFlames) return;" not in c[c.index(
    "void UASTFireReactiveComponent::SetupSurfaceFire("):c.index(
    "void UASTFireReactiveComponent::UpdateSurfaceVisibility()")]
for s in (
    "BLEND_TRANSLUCENT","MP_OPACITY","MP_EMISSIVE_COLOR",
    'set_editor_property("used_with_skeletal_mesh",True)',
    "MaterialExpressionTextureCoordinate","CharProgress","CharGlow",
    "CharSeed","CharTiling","crack*=coverage;",
    "float3 soot=float3(.013,.016,.012)",
    "float3 core=float3(3.5,.12,.012)*hot;",
    "float dark=saturate(Progress)*(.70+.28*coverage);",
    "return saturate(dark+hot*.015);",
):
    assert s in mat,s
assert "PreSkinnedPosition" not in mat.split("    field=(")[1]
assert "MP_WORLD_POSITION_OFFSET" not in mat
assert "M_AST_FireChar.uasset" in install
assert "setup_stage22_char.py" in install
assert "STAGE22_CHAR_MATERIAL_READY_VISUAL_TEST_PENDING" in install
# Server burn intensity drives accumulated marks; extinguishing never sets
# CharProgress back to zero, and the overlay remains on the mesh afterward.
ext=c[c.index("void UASTFireReactiveComponent::Extinguish()"):
      c.index("void UASTFireReactiveComponent::SetFireCoverage(")]
assert "CharProgress" not in ext
print("Stage22: UV-locked soot/emissive cracks and material preservation guards OK")
