"""Fire v4 current visual contract; replace obsolete v3 geometry snapshots.
CPU/reference math and static regression locks are not GPU/PIE tests.
"""
from pathlib import Path
from math import sin, pi, ceil, log2
root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
r=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
m=Path("Automation/ASTFire/setup_fire_v4.py").read_text()
d=Path("Automation/ASTFire/deploy_fire_v4_windows.ps1").read_text()
def fnv(s):
    v=2166136261
    for x in s: v=((v^ord(x))*16777619)&0xffffffff
    return f"{v:08x}"
def region(a,b): return c[c.index(a):c.index(b,c.index(a))]
# The hashes are pinned to accepted v3 7d0eee1049ffbcca7f25ed39811901667a362ba5, NOT recomputed from v4.
for a,b,expected in [["void UASTFireFreeFlameComponent::UpdateSparks(","void UASTFireFreeFlameComponent::UpdateSmoke(","1408cade"],["    // Collision volume is also the exact support of visible density.","    const bool bSmolder=","413ed2d7"],["// Perspective-correct density raster, with per-cell collision-safe depth.","FString UASTFireFreeFlameComponent::RunSmokeAudit(","ad9ffb29"]]:
    assert fnv(region(a,b))==expected, a
# Approved lifetime change only; visual source remains pinned to accepted main.
# Pin all non-light runtime code to the deployed stage2 baseline (ec2a067).
light_start=r.index("void UASTFireReactiveComponent::ConfigureFireLight(")
light_end=r.index("void UASTFireReactiveComponent::RemoveGeneratedVisuals(",light_start)
assert fnv(r[:light_start]+r[light_end:])=="bbea9c56","unexpected non-light runtime change after approved poseable target adaptation"
assert fnv(c[:c.index("\nbool UASTFireFreeFlameComponent::GetFireLightCenter(")])=="94a582ea","accepted fire rendering changed after approved skinned-type adaptation"
for token in ("GetTongueCount()", "SetSizeScale(", "RunSmokeAudit(", "RunFireV4Audit("):
    assert token in h,token
for token in ("GetSkinnedVertexPosition","LineTraceComponent(","ConnectedRoots.Add(",
              "AddFuel(W,A.RestPosition,INDEX_NONE)",
              "EFlameLife::Bridging","D.Position+=D.Velocity*Dt;",
              "const float PixelPitch=bPointField?UpdatePointField",
              "const float Pitch=FMath::Max(.1875f,PixelPitch);",
              "const float Edge=F.Strength", "Cell.Temperature=Hot*Heat",
              "Colors.Add(FLinearColor(ASTFireLife::ToneCode(Cell.Temperature),0.f,0.f,1.f))",
              "Projection.Unproject(P,Cell.Depth)", "RenderFieldFragments(Pitch,Verts,UVs,Colors,Triangles)",
              "MaxXCells=144", "RunFireV4Audit("):
    assert token in c,token
assert "floor(UV" not in m
assert "MP_WORLD_POSITION_OFFSET" not in m
assert 'connect_material_property(vertex,"A",u.MaterialProperty.MP_OPACITY_MASK)' in m
for threshold in ("1.,h","2.,h","3.,h","4.,h","5.,h","6.,h"):
    assert threshold in m
assert "float h=floor(saturate(C.r)*7.)" in m
assert m.count("save_loaded_asset(")==2
assert "run_smoke_audit(smoke)" in m and "run_fire_v4_audit(mat)" in m
for token in ("FIRE_V4_ABI_IMPORTS_CHECKED", "FIRE_V4_ROLLBACK_COMPLETE",
              "Assert-EditorClosed", "Local source changed", "FIRE_V4_COMMANDLET_EXIT"):
    assert token in d,token
assert "M_AST_FireSmoke.uasset" not in d
# Raster pitch is square, power-of-two bounded and independent of UV stretch.
def pitch(scale,w,h):
    b=.1875
    return b*2**ceil(log2(max(b,w/70,h/102)/b))
for scale in (.15,1,2):
    for w,height in ((12,22),(100,170),(350,700)):
        p=pitch(scale,w,height)
        assert ceil(w/p)+1<=72 and ceil(height/p)+1<=104
# Real lifecycle contract: the original arc is transferred at an exact
# sampled cut ring. There is no independent upper sprite or retraction.
assert "EFlameLife::Retracting" not in c
assert "BodyCenter(" not in c
assert "Child.Arc[K]=(bSnapshot?Tongue.DisplayArc[Cut+K]" in c
assert "Child.Radius[K]=bSnapshot?Tongue.DisplayRadius[Cut+K]" in c
assert "Child.BirthPhase=Tongue.Seed+Tongue.Phase;" in c
assert "T.RestartLength=FMath::Min(FMath::Min(Remaining,ResetCap),T.MaxLength*.18f);" in c
assert "D.Temperature=FMath::Pow(Fade,.88f);" in c
assert "MaxTransferError<.05f" in c and "MaxResetRatio<=.151f" in c
assert "bCycle && bFragment" in c and "bMesh && bWick" in c
assert "World->SendAllEndOfFrameUpdates();" in c
for cut in (2,3):
    vs=[k/8 for k in range(cut,9)]
    assert vs[0] in (.25,.375) and vs[-1]==1
    assert 1-vs[0]>.60
    for i in range(1,20):
        a,b=1-(i-1)/20,1-i/20
        assert b**.72<a**.72 and b**.88<a**.88
print("Fire lifecycle: original upper arc, atomic rebirth, cooling, preserved smoke/sparks")

assert 'T.PhaseAge>=T.BridgeSeconds && bCutReady' in c
assert 'const FVector Expected=Old.DisplayArc[Ring];' in c

assert "Root.Outward=Radial;" in c
assert "T.Age-T.RebirthDelay" in c
assert "Exposure>=.50f && FanRate>=.60f" in c
assert "bSparkGrid && bSparkFade" in c
assert "bInFront?Fade:0.f" in c
assert "u.BlendMode.BLEND_TRANSLUCENT" in m
assert 'u.MaterialProperty.MP_OPACITY)' in m
assert "M_AST_FireSpark.uasset" in d

# Emission changes authorized; collision transport, density raster and spark simulation remain locked.
assert "Tongue.MaxLength*=Compactness;" in c
assert "MassRatio<2.f" in c
assert "ActiveArea+=T.CurrentWidth*T.CurrentLength;" in c
assert "FMath::Lerp(1.f,1.38f,Abundance)" in c

# Surface is rendered as fuel, not merely sampled for depth. The native
# skeletal fixture must pass before the installer accepts the update.
assert "Fuel.Add({Q,Depth,Radius,Influence,Owner});" in c
assert "Surface.SetNum(NX*NY);Gas.SetNum(NX*NY)" in c
assert "RunFireSurfaceAudit(" in c and "Coverage>=.85f" in c
assert "run_fire_surface_audit" in m
assert "ProfileSamples[9]={.58f,.91f,1.f,.96f,.84f,.70f,.50f,.28f,0.f}" in c

assert "AppendNeck(" not in region("float UASTFireFreeFlameComponent::UpdateConnectedSource(","void UASTFireFreeFlameComponent::TickComponent")
assert "Lost[C.Owner].Add(C)" in c and "MeshTransferred+=Group.Num()" in c
assert "MeshCuts>=2 && MeshTransferred>=4" in c

# Point fire is a shared evolving front: no legacy arcs in its render path.
assert "bPointField?UpdatePointField" in c
assert "RenderFieldFragments(Pitch,Verts,UVs,Colors,Triangles);" in c
assert "if(!PointField[I].bLit || Next[I].bLit)continue;" in c
assert "FieldCellsTransferred>=15" in c
assert "Ridge=FMath::Max(Ridge,Heights[I]*Shape)" in c

assert 'OldField[J].bLit && !Single->PointField[J].bLit' in c
assert 'FVector::DistSquared(OldField[J].World,Restored)<.0001f' in c

assert 'FirePixelPitch=Desired;' in c
assert 'const float Bottom=-2.5f+6.0f' in c
assert 'const FVector2D Rotated(' in c

# Accepted point effect is locked, including its actual transferred cells.
assert fnv(region("float UASTFireFreeFlameComponent::UpdatePointField(","void UASTFireFreeFlameComponent::RenderFieldFragments("))=="82461965"
assert "GetSkinnedVertexPosition(Skel,A.SkinnedVertexIndex" in c
assert "if(X>=0 && X<NX && Y>=0 && Y<NY && Gas[Y*NX+X].Owner!=INDEX_NONE)continue;" in c
assert "Group.Num()<2" in c

assert 'L.IndexBuffer.GetArrayView()' in c
assert 'Fire->SourceAnchors.Num()>=30' in c
assert 'CrateSaved>=4 && CrateAnchors>=30 && CrateCuts>=2' in c

assert 'WorldRequired>FireVisualWidth' in c
assert 'MeshPitchChanges<=4' in c

# Light changes cannot alter the accepted simulation or its envelope.
assert "Light->SetUseInverseSquaredFalloff(false);" in r
assert "Light->SetCastShadows(true);" in r
assert "ELightUnits::Unitless" in r
assert "RangeDrift<.1f" in r
assert "FMath::PerlinNoise1D" in r
assert "0.09f + 0.91f * VisualEnergy + 0.04f" in r
assert "RunFireLightAudit()" in r

# Manual-exposure regression: a bounded smooth kernel, not an unbounded hot spot.
assert "Light->SetLightFalloffExponent(4.5f)" in r
assert "FMath::Min(1.65f" in r
assert "ASTFireLightCameraOffset" not in r and "ASTFireCharacterFill" not in r
assert "Light->SetWorldLocation(FireLightCenter)" in r
assert "Light->ComponentTags.AddUnique(ASTFire::FireLightTag)" in r
assert "Radius>Fire->LightRadius*1.8f" in r
assert "ClipRate<.01f" in r and "Near>Far+1.f" in r
assert "RunFireLightCapture(UClass* TorchClass)" in r
assert "PP.AutoExposureApplyPhysicalCameraExposure=false" in r
assert "Light->SetSpecularScale(.12f)" in r
assert "Light->SetVolumetricScatteringIntensity(.025f)" in r
for size in (.05,.15,.25,1,4,8):
    for power in (0,450,1300,50000):
        peak=min(1.65,1.35*min(1.6,max(0,power/450))*size**(1.35*.25)*1.52)
        assert 0<=peak<=1.65
for dist in (.0,.25,.5,.75,.95,1.):
    assert 0<=(1-dist*dist)**4.5<=1

assert 'Sirroh3DVisualMesh' in r and 'FindTargetComponent()==Rendered' in r
assert 'Cast<USkinnedMeshComponent>(Source)' in c
zone=(root/"Private/ASTFireIgnitionZoneComponent.cpp").read_text()
assert "OverlapMultiByObjectType" in zone and "AllObjects" in zone
assert "bNoDuplicate" in zone and "bCarriedReentry" in zone

assert "SirrohCarryProp" in zone and "Separated(FVector::CrossProduct" in zone

# User-requested burn polish: palette indices must never be sRGB-encoded.
assert "UpdateMeshSection_LinearColor(1,Verts,Normals,UVs,Colors,Tangents,false)" in c
assert "UpdateMeshSection_LinearColor(0,Verts,Normals,UVs,Colors,Tangents,false)" in c
assert "SmokeAfterglowSeconds=3.f" in c and "bSmolder" in c
assert "return 2.f*FMath::Max(.1f,BaseBurnSeconds)*Factor;" in r
assert "RunCarryVisualAudit" in c and "Low<Peak" in c
assert "SmokeGridMaxX = 128" in h and "SmokeGridMaxY = 256" in h
assert "const float Dying=FMath::SmoothStep" in c

assert "ASTPropCharGlow" in r and "RunPropCharAudit" in r and "PreviousCharPropMaterial" in r
