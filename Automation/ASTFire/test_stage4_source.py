"""Static source/scope checks for AST Fire Stage4. No Unreal engine required."""
from pathlib import Path

root = Path("Plugins/ASTFireRuntime")
header = (root / "Source/ASTFireRuntime/Public/ASTFireReactiveComponent.h").read_text()
runtime = (root / "Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp").read_text()
api = (root / "Source/ASTFireRuntime/Private/ASTFireBlueprintLibrary.cpp").read_text()
plugin = (root / "ASTFireRuntime.uplugin").read_text()
script = Path("Automation/ASTFire/setup_stage4_volume.py").read_text()
for token in (
    "CreateMeshSection_LinearColor",
    "UpdateMeshSection_LinearColor",
    "SmoothedFlow", "SmoothedAngularVelocity",
    "Acceleration", "AmbientWind", "TangentialVelocity",
    "FMath::VInterpTo", "SetCastShadow(false)",
    "GetNetMode() == NM_DedicatedServer",
    "VolumeUpdateDebt", "Taper", "BurnScale", "Eddy",
    "DOREPLIFETIME(UASTFireReactiveComponent, ExtraLocalPoints)",
):
    assert token in runtime, f"Required stage4 geometry/flow/replication: {token}"
assert "class FLifetimeProperty;" in header
assert "double RotationAngle" in runtime
assert "ReplicatedUsing=OnRep_Burning" in header
assert "ReplicatedUsing=OnRep_BurnAnchors" in header
assert "GetNetMode() == NM_Client" in api
assert '"ProceduralMeshComponent", "Enabled": true' in plugin
assert "BP_Torch_U" not in script and "GrimFX" not in script
assert "M_AST_FirePixel_Volume" in script
assert 'set_editor_property("auto_activate", False)' in script
assert 'set_editor_property("visible", False)' in script
assert "MP_EMISSIVE_COLOR" in script
print("AST Fire Stage4 structure and boundaries: OK")
