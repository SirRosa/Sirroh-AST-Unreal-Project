"""Historical Stage9 contract: maintained only as an architecture regression test.

Stage10 replaced per-vertex fixed-wave quads; do not demand removed machinery.
"""
from pathlib import Path

root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
parent=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
header=(root/"Public/ASTFireReactiveComponent.h").read_text()
free=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
freeh=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
editor=Path("Automation/ASTFire/setup_stage8_surface.py").read_text()
for token in (
    "UASTFireFreeFlameComponent", "GetSkinnedVertexPosition",
    "GetResourceForRendering",
    "StaticVertexBuffers.PositionVertexBuffer.VertexPosition",
    "SampleRoot(", "LineTraceComponent(",
    "Render->UpdateMeshSection_LinearColor",
):
    assert token in free or token in freeh, token
assert "bUseLegacyHeatGrid = false;" in header
assert "bEnableProceduralFlames && bUseLegacyHeatGrid" in parent
assert "FreeFlames->SetThermal" in parent
assert "FreeFlames->GetVisualEnergy()" in parent
assert "IsValid(FreeFlames) && !bEnableFlameCards && !bEnableNiagara" in parent
assert '"use_legacy_heat_grid", False' in editor
assert "GetBoneLocation(" not in free
assert "SetupAttachment(Parent, Socket)" not in free
assert "Width = 22" not in free
assert "Height = 30" not in free
assert "ShapedCoverage" not in free
# Lifecycle replaces infinite repeated sprites; separated heads are in WORLD
# space and never clamped by a shared grid extent.
assert "D.Position+=D.Velocity*Dt" in free
assert "EmitTip(" in free
print("Stage9 regression: no finite heat grid, correct skin roots and world detach OK")
