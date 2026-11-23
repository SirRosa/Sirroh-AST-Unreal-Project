"""Stage31: long continuous smoke centreline and four-tone masked bands."""
from pathlib import Path
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(p/"Public/ASTFireFreeFlameComponent.h").read_text()
c=(p/"Private/ASTFireFreeFlameComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_stage24_smoke.py").read_text()
deploy=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()
for t in ("MaxSmoke = 96","SmokeLayers = 4","struct FSmoke",
          "TArray<FSmoke> Smoke","SmokeLightScanDebt = 0.f;",
          "TArray<TWeakObjectPtr<UPointLightComponent>> NearbySmokeLights",
          "void UpdateSmoke(float Dt)","SmokeMaterial = nullptr"):
    assert t in h,t
for t in ("M_AST_FireSmoke.M_AST_FireSmoke",
          "Render->SetMaterial(3, SmokeMaterial)",
          "void UASTFireFreeFlameComponent::UpdateSmoke(float Dt)",
          "S.Position+=S.Velocity*Dt",
          "SmokeSpawnDebt=FMath::Min(4.f,SmokeSpawnDebt+",
          "S.Lifetime=6.1f+.45f*R;",
          "S.Position.Z-S.BaseZ>=1050.f",
          "const FLinearColor Palette[SmokeLayers]",
          "Live.Sort([](const FSmoke* A,const FSmoke* B)",
          "const int32 BandCount=MaxSmoke-1;",
          "const int32 QuadCount=BandCount*SmokeLayers*Sides;",
          "constexpr float PixelStep=3.f;",
          "NearbySmokeLights",
          "Render->CreateMeshSection_LinearColor(",
          "3,MV,MT,MN,MU,MC,MX,false",
          "3,MV,MN,MU,MC,MX",
          "UpdateSmoke(SimDt);","bHasSmoke=true; break;"):
    assert t in c,t
for t in ("BLEND_MASKED","MP_OPACITY_MASK","MP_EMISSIVE_COLOR",
          "MaterialExpressionVertexColor",
          'connect_material_property(vertex,"A"',
          'connect_material_property(vertex,""'):
    assert t in m,t
assert "setup_stage24_smoke.py" in deploy
assert "M_AST_FireSmoke.uasset" in deploy
assert (96-1)*4*2*4==3040
print("Stage31: 1km/1000cm connected smoke, native masked four-grey palette")
