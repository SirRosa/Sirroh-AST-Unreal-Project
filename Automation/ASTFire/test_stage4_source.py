"""Stage 5 static checks: heat advection, geometry, renderer and ownership."""
from pathlib import Path

root = Path("Plugins/ASTFireRuntime")
header = (root / "Source/ASTFireRuntime/Public/ASTFireReactiveComponent.h").read_text()
runtime = (root / "Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp").read_text()
api = (root / "Source/ASTFireRuntime/Private/ASTFireBlueprintLibrary.cpp").read_text()
plugin = (root / "ASTFireRuntime.uplugin").read_text()
script = Path("Automation/ASTFire/setup_stage4_volume.py").read_text()

for token in (
    "ASTHeat::Prewarm", "ASTHeat::Read", "ASTHeat::Source",
    "Field.Temperature", "Field.Work", "Swap(Field.Temperature, Field.Work)",
    "const float Shear",
    "Fragment.Life", "Fragment.VY", "Semi-Lagrangian advection",
    "CreateMeshSection_LinearColor", "UpdateMeshSection_LinearColor",
    "SmoothedFlow", "SmoothedAngularVelocity", "Acceleration",
    "AmbientWind", "AnchorVelocity", "VolumeUpdateDebt",
    "DOREPLIFETIME(UASTFireReactiveComponent, ExtraLocalPoints)",
    "GetNetMode() == NM_DedicatedServer",
):
    assert token in runtime, f"Missing heat field feature: {token}"

for token in ("FASTLocalFlameField", "FASTFlameFragment", "LocalFields",
              "ReplicatedUsing=OnRep_Burning", "ReplicatedUsing=OnRep_BurnAnchors"):
    assert token in header, f"Missing state: {token}"

assert "constexpr int32 Tongues" not in runtime, "Stage4 geometric tentacles remain!"
assert "Taper" not in runtime, "Old shrinking tube radius logic remains!"
assert "GetNetMode() == NM_Client" in api
assert '"ProceduralMeshComponent", "Enabled": true' in plugin
assert "BP_Torch_U" not in script and "GrimFX" not in script
assert "M_AST_FirePixel_Volume" in script
assert 'return color*0.82;' in script
assert 'return step(.21,C.r);' in script
assert 'return a*2.5;' not in script, "Stage4 over-bright emissive remains!"
assert 'set_editor_property("light_power", 450.0)' in script
assert 'set_editor_property("auto_activate", False)' in script
assert 'set_editor_property("visible", False)' in script
assert '("color", col, "", "C")' in script
assert "MP_EMISSIVE_COLOR" in script
print("AST Fire Stage5 source/ownership/renderer checks: OK")
