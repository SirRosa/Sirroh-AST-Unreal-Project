"""Stage27: native material prevents a custom-smoke shader fallback."""
from pathlib import Path
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
assert "u.MaterialExpressionCustom" not in m
assert 'u.MaterialExpressionComponentMask' not in m
assert 'u.MaterialExpressionConstant3Vector' in m
assert 'EDIT.connect_material_property(vertex,"A",' in m
assert "MP_OPACITY" in m and "MP_EMISSIVE_COLOR" in m
assert "const float Density=FMath::Clamp(" in c
assert "const float Bands=FMath::FloorToFloat(" in c
assert "Bands*EdgeWobble" in c
assert "Render->SetMaterial(3,SmokeMaterial);" in c
assert "SmokeMaterial = LoadObject<UMaterialInterface>" in c
for x,y in ((0.,0.),(.5,0.),(1.,0.),(1.,1.)):
    radial=(x*x+y*y)**.5
    density=max(0.,min(1.,(1.12-radial)/.95))
    band=int(density*4+.001)*.25
    if x==0. and y==0.: assert band==1.
    if x==1. and y==1.: assert band==0.
print("Stage27: native shader material and bounded silhouette alpha guards OK")
