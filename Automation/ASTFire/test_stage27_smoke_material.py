"""Regression: no Custom HLSL or translucent quads for smoke."""
from pathlib import Path
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
assert "u.MaterialExpressionCustom" not in m
assert "BLEND_TRANSLUCENT" not in m
assert "u.BlendMode.BLEND_MASKED" in m
assert "MP_OPACITY_MASK" in m
assert 'EDIT.connect_material_property(vertex,"A",' in m
assert 'EDIT.connect_material_property(vertex,"",' in m
assert "const FLinearColor Palette[SmokeLayers]" in c
assert "Palette[Layer]" in c
assert "const float Size=S.bAlive ? Birth*Exit : 0.f;" in c
assert "const float Radius" not in c[c.index("const FLinearColor Palette[SmokeLayers]"):c.index("    // Section 1 was authored")]
print("Stage27->29: native masked smoke graph, no alpha blending and RGB palette")
