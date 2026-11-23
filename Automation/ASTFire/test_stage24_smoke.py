"""Stage29 smoke: opaque three-tone CPU silhouette, fixed topology."""
from pathlib import Path
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
ps=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()
for t in ("MaxSmoke = 18","SmokeSegments = 12","SmokeLayers = 3",
          "struct FSmoke","TArray<FSmoke> Smoke",
          "void UpdateSmoke(float Dt)","SmokeMaterial = nullptr"):
    assert t in h,t
for t in ("M_AST_FireSmoke.M_AST_FireSmoke",
          "Render->SetMaterial(3, SmokeMaterial)",
          "void UASTFireFreeFlameComponent::UpdateSmoke(float Dt)",
          "S.Position+=S.Velocity*Dt",
          "T.WorldVelocity*.22f+Wind*.045f",
          "Heat<.10f",
          "S.Lifetime=1.05f+1.3f*J;",
          "constexpr int32 VerticesPerLayer = SmokeSegments + 1;",
          "const FLinearColor Palette[SmokeLayers]",
          "const float LayerScale[SmokeLayers]",
          "constexpr float PixelStep=2.f;",
          "const float Size=S.bAlive ? Birth*Exit : 0.f;",
          "Render->CreateMeshSection_LinearColor(",
          "3,MV,MT,MN,MU,MC,MX,false",
          "3,MV,MN,MU,MC,MX",
          "UpdateSmoke(SimDt);",
          "bHasSmoke=true; break;"):
    assert t in c,t
for t in ("BLEND_MASKED","MP_OPACITY_MASK","MP_EMISSIVE_COLOR",
          "MaterialExpressionVertexColor",
          'connect_material_property(vertex,"A"',
          'connect_material_property(vertex,""'):
    assert t in m,t
for forbidden in ("BLEND_TRANSLUCENT","MaterialExpressionCustom",
                  "MaterialExpressionTextureSample","MP_OPACITY),"):
    assert forbidden not in m,forbidden
assert "setup_stage24_smoke.py" in ps
assert "M_AST_FireSmoke.uasset" in ps
assert "STAGE24_SMOKE_MATERIAL_READY_VISUAL_TEST_PENDING" in ps
assert 18*3*(12+1)==702 and 18*3*12==648
print("Stage29: solid three-tone, pixel-quantized smoke mesh and masked material")
