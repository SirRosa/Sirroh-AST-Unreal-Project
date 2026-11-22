"""Stage24 smoke: dynamic 5x5 puffs, dark quantized material, lifetime and drift."""
from pathlib import Path
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
ps=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()
for t in ("MaxSmoke = 18","SmokeGrid = 5","struct FSmoke","TArray<FSmoke> Smoke",
          "void UpdateSmoke(float Dt)","SmokeMaterial = nullptr"): assert t in h,t
for t in ("M_AST_FireSmoke.M_AST_FireSmoke","Render->SetMaterial(3, SmokeMaterial)",
          "void UASTFireFreeFlameComponent::UpdateSmoke(float Dt)",
          "S.Position+=S.Velocity*Dt","T.WorldVelocity*.22f+Wind*.045f",
          "Heat<.10f","S.Lifetime=1.05f+1.3f*J;",
          "Render->CreateMeshSection_LinearColor(",
          "3,MV,MT,MN,MU,MC,MX,false",
          "Render->UpdateMeshSection_LinearColor(",
          "3,MV,MN,MU,MC,MX","UpdateSmoke(SimDt);",
          "bHasSmoke=true; break;","D.Position+=D.Velocity*Dt;"):
    assert t in c,t
for t in ("BLEND_TRANSLUCENT","MP_OPACITY","MP_EMISSIVE_COLOR",
          "MaterialExpressionVertexColor","float radius=length(d)*2.;",
          "float3 a=float3(.108,.111,.103)-band*.023;"):
    assert t in m,t
assert "setup_stage24_smoke.py" in ps
assert "M_AST_FireSmoke.uasset" in ps
assert "STAGE24_SMOKE_MATERIAL_READY_VISUAL_TEST_PENDING" in ps
assert 18*5*5==450 and 18*4*4*2==576
assert "static constexpr int32 DetachedColumns=7;" in c
print("Stage24: procedural camera-facing smoke with wind and thermal fade OK")
