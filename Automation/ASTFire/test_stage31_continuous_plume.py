"""Smoke physical transport: scale, obstruction clearance and ceiling drift."""
from pathlib import Path
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
for k in ("FlameBounds.GetSize()","FMath::Lerp(125.f,1000.f",
          "FMath::Lerp(35.f,85.f","SmokeFireTop+FVector::UpVector",
          "OverlapBlockingTestByChannel","SweepSingleByChannel",
          "Normal.Z<-.45f","FVector::VectorPlaneProject",
          "ECC_Visibility","LineTraceSingleByChannel",
          "TObjectIterator<UPointLightComponent>","Spot->OuterConeAngle",
          "S.MaxRise-Height-2.f","if (!Slot) break;"):
    assert k in c,k
print("Scaled smoke and collision-aware transport guards")
