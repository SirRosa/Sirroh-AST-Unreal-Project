"""Smoke graph: native vertex palette, true alpha only at end of life."""
from pathlib import Path
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
assert "u.MaterialExpressionCustom" not in m
assert "u.BlendMode.BLEND_TRANSLUCENT" in m
assert 'EDIT.connect_material_property(vertex,"A",' in m
assert "u.MaterialProperty.MP_OPACITY)" in m
assert "const float Palette[SmokeTones]={.018f,.065f,.17f,.36f,.64f};" in c
assert "S.Lifetime*.24f" in c
assert "FLinearColor Color(Grey,Grey,Grey,Cell.Alpha)" in c
print("Five neutral grays and final-life alpha")
