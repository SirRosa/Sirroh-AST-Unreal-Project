"""Stage9 architecture invariant: no box-bound flames or bone-bound default cards."""
from pathlib import Path
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
parent=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
header=(root/"Public/ASTFireReactiveComponent.h").read_text()
f=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
editor=Path("Automation/ASTFire/setup_stage8_surface.py").read_text()
deploy=Path("Automation/ASTFire/deploy_stage9_windows.ps1").read_text()
for token in (
    "UASTFireFreeFlameComponent",
    "GetSkinnedVertexPosition",
    "GetResourceForRendering",
    "StaticVertexBuffers.PositionVertexBuffer.VertexPosition",
    "SampleRoot(Root)",
    "LineTraceComponent(",
    "Ember.Position += Ember.Velocity * DeltaTime",
    "Render->UpdateMeshSection_LinearColor",
    "FMath::Sin(Cycle * PI)",
):
    assert token in f or token in h, token
assert "bUseLegacyHeatGrid = false;" in header
assert "bEnableProceduralFlames && bUseLegacyHeatGrid" in parent
assert "FreeFlames->SetThermal" in parent
assert "FreeFlames->GetVisualEnergy()" in parent
assert "IsValid(FreeFlames) && !bEnableFlameCards && !bEnableNiagara" in parent
assert '"use_legacy_heat_grid", False' in editor
assert "ASTFireFreeFlameComponent.cpp" in deploy
assert "ASTFireFreeFlameComponent.h" in deploy
assert "M_AST_FirePixel_Volume.uasset" in deploy
assert "Stage9" in deploy
# All active roots are true skinned verts or STATIC hit surfaces; no bone
# or collision-proxy attachment is allowed in the default renderer.
assert "GetBoneLocation(" not in f
assert "SetupAttachment(Parent, Socket)" not in f
assert "Width = 22" not in f
assert "Height = 30" not in f
assert "ShapedCoverage" not in f
assert "Bounds" not in f or "BoxExtent" in f
assert "MaxRoots = 18" in h
assert "Segments = 7" in h
assert "MaxEmbers = 12" in h
# Previous field remains only as explicit backward-compat migration option.
print("Stage9 unbounded skin-surface roots + no bone-grid default: OK")
