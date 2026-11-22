"""Stage8 integration guardrails. Unreal build and visual PIE remain mandatory."""
from pathlib import Path
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(root/"Public/ASTFireReactiveComponent.h").read_text()
c=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
material=Path("Automation/ASTFire/setup_stage8_surface.py").read_text()
volume=Path("Automation/ASTFire/setup_stage4_volume.py").read_text()
deploy=Path("Automation/ASTFire/deploy_stage8_windows.ps1").read_text()
for x in ("bEnableSurfaceFire = true", "SurfaceMaterial",
          "FireSurfaceMID", "PreviousFireSurfaceOverlay",
          "SetupSurfaceFire", "ReleaseSurfaceFire",
          "UpdateSurfaceVisibility"):
    assert x in h or x in c, x
for x in (
    "SetupSurfaceFire(Parent)", "ReleaseSurfaceFire();",
    "Mesh->GetOverlayMaterial(false, 0)",
    "Mesh->IsVisible()", "FireSurfaceMesh->SetOverlayMaterial(FireSurfaceMID)",
    "FireSurfaceMesh->SetOverlayMaterial(PreviousFireSurfaceOverlay)",
    "SurfaceMaterial.LoadSynchronous()",
    'FName(TEXT("FireIntensity"))', 'FName(TEXT("FireEnergy"))',
    'FName(TEXT("FireMotion"))',
    "Field.Temperature", "Field.VisualEnergy",
    "Field.LastAnchorVelocity", "LocalLife", "LocalAge",
    "ASTHeat::ShapedCoverage", "SmoothedFlow", "FMath::Pow",
):
    assert x in c, x
assert "FragmentCount = 4" in c
assert "Scale * FlameSize *" in c
assert "0.46f" in c
assert "return color*1.13;" in volume
for x in (
    "MaterialExpressionPreSkinnedPosition",
    "BLEND_MASKED", "MSM_UNLIT",
    "FireIntensity", "FireMotion", "FireEnergy", "FireSeed",
    "MP_OPACITY_MASK", "MP_EMISSIVE_COLOR",
    "BP_AST_FireSource_Igniter",
    "SURFACE_FIRE_CONFIGURED_REQUIRES_VISUAL_TEST",
):
    assert x in material, x
assert "BP_Torch_U" not in material
assert "setup_stage8_surface.py" in deploy
assert "M_AST_FireSurface.uasset" in deploy
assert "STAGE8_" in deploy
assert "Stage6\\stage6_result.json" in deploy
assert "Stage8\\stage8_result.json" in deploy
print("Stage8 coupled skin-surface heat / buoyant tongues / light and install scope OK")
