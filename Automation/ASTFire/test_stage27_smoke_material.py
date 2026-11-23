"""Stage31: four colors and binary mask instead of translucent dust clouds."""
from pathlib import Path
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
assert "u.MaterialExpressionCustom" not in m
assert "u.BlendMode.BLEND_MASKED" in m
assert "MP_OPACITY_MASK" in m
assert 'EDIT.connect_material_property(vertex,"A",' in m
assert "const FLinearColor Palette[SmokeLayers]" in c
for grey in (".56f,.56f,.57f",".39f,.39f,.40f",
             ".24f,.24f,.26f",".105f,.105f,.12f"):
    assert grey in c,grey
assert "bConnected?1.f:0.f" in c
assert "const int32 Tone=Shade>.88f?0:" in c
print("Stage31: four discrete smoke grays, with light-reactive bands")
