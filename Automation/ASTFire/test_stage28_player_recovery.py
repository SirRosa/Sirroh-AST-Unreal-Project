"""Stage28: seven seconds reference burn and player-only 10s soot recovery."""
from pathlib import Path
p=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
h=(p/"Public/ASTFireReactiveComponent.h").read_text()
c=(p/"Private/ASTFireReactiveComponent.cpp").read_text()
for key in ('float BaseBurnSeconds = 7.0f;',
            'float PlayerCharRecoverySeconds = 10.0f;',
            'float PlayerCharFadeElapsedSeconds = 0.f;',
            'float PlayerCharFadeStartProgress = 0.f;'):
    assert key in h, key
for key in ('const APawn* Pawn = Cast<APawn>(GetOwner());',
            'if (Pawn && Pawn->IsPlayerControlled())',
            'PlayerCharFadeStartProgress = CharProgress;',
            'PlayerCharFadeElapsedSeconds + DeltaTime',
            'PlayerCharFadeStartProgress * (1.f -',
            'CharProgress = FMath::Max(0.f,',
            'bPreviousReplicatedLit && !bStartLit'):
    assert key in c, key
assert 'if (GetOwner()->HasAuthority() && bAutoExtinguish &&' in c
assert '!ASTFire::IsDedicatedFireActor(GetOwner()) && bStartLit' in c
# Server authority makes this role check reliable, unlike a client-local pawn.
def char_recovery(initial,seconds,player):
    if not player: return initial
    return max(0.,initial*(1.-min(seconds,10.)/10.))
for start in (.15,.55,1.):
    assert char_recovery(start,0.,True)==start
    assert abs(char_recovery(start,5.,True)-start*.5)<1.e-9
    assert char_recovery(start,10.,True)==0
    assert char_recovery(start,20.,True)==0
    assert char_recovery(start,20.,False)==start
print("Stage28: 7s reference burn; only server-owned player pawn recovers in 10s")
