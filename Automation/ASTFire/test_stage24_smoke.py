"""Smoke lifecycle/material integration, upgraded to Stage33 density transport."""
from pathlib import Path
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(p/"Public/ASTFireFreeFlameComponent.h").read_text()
c=(p/"Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
d=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()
for key in ("MaxSmoke = 72","SmokeTones = 5","SmokeRender = nullptr",
            "void UpdateSmokeGeometry();","TArray<FSmoke> Smoke"):
    assert key in h,key
for key in ("M_AST_FireSmoke.M_AST_FireSmoke","SmokeRender->SetAbsolute(true,true,true)",
            "SmokeRender->SetMaterial(0,SmokeMaterial)","UpdateSmoke(SimDt);",
            "UpdateSmokeGeometry();","bHasSmoke=true; break;"):
    assert key in c,key
for key in ("BLEND_TRANSLUCENT","MP_OPACITY","MP_EMISSIVE_COLOR",
            'connect_material_property(vertex,"A"', "DENSITY_FIVE_TONE_TERMINAL_ALPHA"):
    assert key in m,key
assert "MaterialExpressionCustom" not in m
assert "M_AST_FireSmoke.uasset" in d and "setup_stage24_smoke.py" in d
print("Smoke lifecycle and material integration OK")
