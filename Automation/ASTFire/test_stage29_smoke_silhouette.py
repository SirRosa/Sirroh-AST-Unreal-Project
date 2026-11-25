"""Smoke geometry is a unified sampled field, not independent discs/ribbons."""
from pathlib import Path
c=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
g=c[c.index("void UASTFireFreeFlameComponent::UpdateSmokeGeometry()"):c.index("// Applied AFTER the approved life simulation.")]
for k in ("Cell.Density+=Mass","Cell.Carrier=I","Cell.Density<.19f",
          "SafeRadius=S.Radius-Pitch*.72f","SmokeGridMaxX","SmokeGridMaxY",
          "const FVector2D Gradient","S.LightDir","SmokeRender->CreateMeshSection_LinearColor"):
    assert k in g,k
assert "Live.Sort" not in g and "LayerScale" not in g
print("Unified density field and bounded support geometry")
