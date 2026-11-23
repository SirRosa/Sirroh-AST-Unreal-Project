"""Regression: map-authored permanent sources vs finite spawned/ignited objects.

Code review/static and duration reference only; no gameplay rendering.
"""
from pathlib import Path
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp").read_text()
template=Path("Automation/ASTFire/setup_stage10_lifecycle.py").read_text()
assert 'GetName().StartsWith(TEXT("BP_AST_FireSource"))' in root
assert "if (ASTFire::IsDedicatedFireActor(GetOwner()))\n        bAutoExtinguish = false;" in root
assert "GetOwner()->HasAuthority() && bAutoExtinguish &&" in root
assert "!ASTFire::IsDedicatedFireActor(GetOwner()) && bStartLit" in root
assert 'component.set_editor_property("auto_extinguish",False)' in template
assert 'for name in ("BP_AST_FireSource","BP_AST_FireSource_Igniter"):' in template
assert 'float UASTFireReactiveComponent::ComputeBurnDuration() const' in root
assert "BurnElapsedSeconds += DeltaTime;" in root
assert "BurnDurationSeconds = ComputeBurnDuration();" in root
assert "void UASTFireReactiveComponent::IgniteAtWorldPosition" in root
assert "if (!bStartLit) Ignite(1.0f);" in root
# Protected source status and target timer are independent of bStartLit.
def would_consume_fuel(is_source,auto_extinguish,authority,lit):
    return authority and auto_extinguish and not is_source and lit
assert not would_consume_fuel(True,True,True,True)
assert not would_consume_fuel(True,False,True,True)
assert would_consume_fuel(False,True,True,True)
assert not would_consume_fuel(False,True,False,True)
assert not would_consume_fuel(False,True,True,False)
print("Stage25: only ignited targets consume the finite fuel budget")
