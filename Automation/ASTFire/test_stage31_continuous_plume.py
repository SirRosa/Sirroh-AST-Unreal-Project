"""Stage31: live four-band lighting and long plume smoke invariants."""
from pathlib import Path
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
h=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireFreeFlameComponent.h").read_text()
for v in ("#include \"Components/PointLightComponent.h\"",
          "#include \"Engine/PointLight.h\"","#include \"EngineUtils.h\"",
          "TActorIterator<APointLight> It(World)",
          "Light->GetIntensity()/1700.f",
          "Light->AttenuationRadius",
          "Live.Sort([](const FSmoke* A,const FSmoke* B)",
          "S.Position.Z-S.BaseZ>=1050.f",
          "FMath::VInterpTo(S.Velocity,Target,Dt,1.85f)",
          "S.Lifetime=6.1f+.45f*R;"):
    assert v in p,v
assert "MaxSmoke = 96" in h
assert "SmokeLayers = 4" in h
assert "FLinearColor(.56f,.56f,.57f" in p
assert "FLinearColor(.105f,.105f,.12f" in p
print("Stage31: connected, illuminated, long-lived smoke invariants")
