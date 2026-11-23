"""Stage30: sparks use a dedicated high-contrast masked material and active tongues."""
from pathlib import Path
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
m=Path("Automation/ASTFire/setup_stage30_sparks.py").read_text()
deploy=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()
assert "SparkMaterial = nullptr;" in h and "EmissionHeat = 1.f;" in h
for x in ('M_AST_FireSpark.M_AST_FireSpark','Render->SetMaterial(2, SparkMaterial)',
          'Render->SetMaterial(2,SparkMaterial)',
          'if (Tongues[Candidate].bActive && Tongues[Candidate].Age>=0.f)',
          'ConnectedRoots.IsValidIndex(Index)',
          'S.Size=2.8f+2.9f*Other;',
          'S.Lifetime=.35f+.50f*Random;',
          'const float Temperature=S.EmissionHeat*Fade;',
          'FLinearColor(1.f,.84f,.28f,1.f)',
          'FLinearColor(1.f,.40f,.055f,1.f)',
          'FLinearColor(.80f,.115f,.019f,1.f)',
          'SparkSpawnDebt=FMath::Min(4.f,',
          'S.EmissionHeat=FMath::Clamp(Heat*'):
    assert x in c,x
for x in ('BLEND_MASKED','MP_OPACITY_MASK','MP_EMISSIVE_COLOR',
          'MaterialExpressionVertexColor','MaterialExpressionMultiply',
          'boost.set_editor_property("r",2.8)'):
    assert x in m,x
assert "BLEND_TRANSLUCENT" not in m
assert "setup_stage30_sparks.py" in deploy
assert "M_AST_FireSpark.uasset" in deploy
assert "STAGE30_SPARK_MATERIAL_READY_VISUAL_TEST_PENDING" in deploy
print("Stage30: dedicated palette sparks, active tongue sampling and HDR gain")
