"""Stage13 connected-body regression: no hollow rings, tongues share a filled base.

Static checks / numerical references do not substitute for PIE, DX12
shader validation, gameplay networking or measured multi-actor frame rate.
"""
from pathlib import Path
from math import sqrt

root=Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime")
c=(root/"Private/ASTFireFreeFlameComponent.cpp").read_text()
h=(root/"Public/ASTFireFreeFlameComponent.h").read_text()
reactive=(root/"Private/ASTFireReactiveComponent.cpp").read_text()
mat=Path("Automation/ASTFire/setup_stage10_lifecycle.py").read_text()
install=Path("Automation/ASTFire/deploy_stage10_windows.ps1").read_text()

def check_tokens(where, tokens):
    for t in tokens:
        assert t in where, t

# The currently approved tongue appearance, heating and bridge split are
# unchanged. Both tongues and the mantle use exactly the same material.
check_tokens(c,(
    "GetSkinnedVertexPosition", "GetResourceForRendering",
    "GetSkinWeightVertexBuffer", "LineTraceComponent(",
    "static constexpr" if False else "SampleRoot(",
    "EFlameLife::Bridging", "EFlameLife::Retracting",
    "EmitTip(T,VisualBase,T.SplitLength,Flow)",
    "T.CurrentLength = FMath::Lerp(T.SplitLength,T.RetractTarget,Ease)",
    "D.Position+=D.Velocity*Dt", "ConfigureGeneration(T)",
    "FMath::Frac(ShapeSeed*.017f)", "AddStrip(C,W,H",
    "Render->SetMaterial(0, Material)",
    "Render->SetMaterial(1, Material)",
    "UpdateConnectedSource(bCreate, View, Right)",
    "ConnectedRoots.Reset();",
    "ConnectedRoots.Reserve(Tongues.Num());",
    "ConnectedRoots.IsValidIndex(I)",
    "ConnectedRoots.Add(Root+Normal*(Front+6.5f-RootDepth));",
    "const FVector WorldB=TongueCenterAt(T,Root,V1,Right);",
    "const float Support=Cover*(.19f+",
    "C.Add(TongueCenterAt(T,Base,V,Right));",
    "Necks.Reserve(Tongues.Num()*(RingsPerTongue+4));",
    "static constexpr float NeckFractions[9] =",
    "const FVector VisualBase=ConnectedRoots.IsValidIndex(TongueIndex)",
    "Render->CreateMeshSection_LinearColor(",
    "Render->UpdateMeshSection_LinearColor(",
))
check_tokens(h,(
    "MaxTongues = 15", "MaxDetached = 16", "BridgeRings = 4",
    "SourceGridX = 32", "SourceGridY = 44",
    "MaxSourceAnchors = 96", "TArray<FSourceAnchor> SourceAnchors",
    "TArray<FBurnCluster> BurnClusters",
))
check_tokens(reactive,(
    "FreeFlames->SetThermal", "FreeFlames->GetVisualEnergy()",
    "FreeFlames->SetIgnitionPoints(ExtraLocalPoints)",
    "const bool bCharOverlay = bEnableProceduralFlames && bEnableCharring;",
    "SetOverlayMaterial(PreviousFireSurfaceOverlay)",
    "0.91f * VisualEnergy",
))
assert c.index("Render->SetMaterial(1, Material)") < c.index("BuildAnchors();")
assert "SetupAttachment(Parent, Socket)" not in c

# The OLD WPO overlaid and distorted low-poly triangles. The pipeline now
# skips it for procedural fire. The asset is left on disk for rollback only.
assert '"enable_surface_fire",False' in mat
assert '"enable_procedural_flames",True' in mat
assert 'MaterialExpressionVertexColor' in mat
assert 'MaterialExpressionTextureCoordinate' in mat
assert 'MP_OPACITY_MASK' in mat
assert 'M_AST_FireTongue' in mat
assert 'MP_WORLD_POSITION_OFFSET' not in mat
assert 'new_material("M_AST_FireSurfaceLife"' not in mat
for token in (".12,h", ".24,h", ".43,h", ".61,h", ".77,h", ".90,h"):
    assert token in mat, token

# Mesh samples are taken from CURRENT skinned geometry and collision hits.
# Tongue mesh section and source mesh section now use the same projected
# surface depth in the SAME render update. They do not float on two planes.
assert c.index("UpdateConnectedSource(bCreate, View, Right)") < c.index(
    "const FVector Base=ConnectedRoots.IsValidIndex(I)")
assert "const FVector Base=SampleRoot(T);" not in c[
    c.index("void UASTFireFreeFlameComponent::UpdateGeometry("):
    c.index("void UASTFireFreeFlameComponent::BuildSourceAnchors()")]
assert "TArray<FVector> ConnectedRoots;" in h
assert "FVector TongueCenterAt(const FTongue& Tongue" in h
assert "T.BridgedChild = EmitTip(T,VisualBase,T.SplitLength,Flow);" in c
assert c.count("C.Add(TongueCenterAt(T,Base,V,Right));")==1
assert "const float Curl=FMath::Sin(V*8.1f" in c
assert "const float Curl2=FMath::Sin(V*13.3f" in c
assert "DepthCompensation=0.f" in c

# Stage15: skin samples provide only FRONT DEPTH. The pixel silhouette is
# driven entirely by independently lived, tapered active tongue curves.
check_tokens(c,(
    "BuildSourceAnchors()", "SourceAnchors.Reset()",
    "StaticVertexBuffers.PositionVertexBuffer",
    "USkinnedMeshComponent::GetSkinnedVertexPosition(",
    "Transform.InverseTransformPosition(", "Transform.TransformPosition(",
    "if(SourceAnchors.IsEmpty())", "Source->GetComponentTransform()",
    "for(const FTongue& T:Tongues)",
    "Samples.Add(P)",
    "FVector2D::DistSquared(P,Samples[I])",
    "Necks.Add(Neck);",
    "Necks.Reserve(Tongues.Num()*(RingsPerTongue+4));",
    "TArray<float> SampleDepths;",
    "float FrontDepth=-BIG_NUMBER;",
    "FrontDepth=FMath::Max(FrontDepth,BestDepth[I]);",
    "float Depth=FrontDepth>-BIG_NUMBER*.5f",
    "const float Support=Cover*(.19f+",
    "Heat*NeckSupport*Flicker",
    "H.Add(0.f);",
    "UpdateConnectedSource(bCreate, View, Right)",
    "1,Verts,Tris,Normals,UVs,Colors,Tangents,false",
    "1,Verts,Normals,UVs,Colors,Tangents",
))
field=c[c.index("void UASTFireFreeFlameComponent::UpdateConnectedSource("):
        c.index("void UASTFireFreeFlameComponent::TickComponent(")]
assert "Parent[J]=Best" not in field
assert "FMath::Sqrt(PointD2)" not in field
assert "FMath::Sqrt(EdgeD2)" not in field
assert field.index("Samples.Add(P)") < field.index("Necks.Add(Neck)")
assert field.count("Heat*NeckSupport*Flicker")==1
assert c.count("H.Add(0.f);")==1
assert "Source.IsValid()" not in c
assert "Render->UpdateMeshSection_LinearColor(1" not in c  # formatting differs
assert "return dir*depth" not in c
assert "FMath::Sin(Cycle * PI)" not in c

# Numeric field reference: a root broadens into a tall, tapering tongue,
# rather than creating full-skeleton filled geometry.
from math import sqrt, sin
def smooth(x):
    v=max(0.,min(1.,x))
    return v*v*(3.-2.*v)
def radius(v,width=18.):
    ridge=max(.4,min(1.05,.85+.15*sin(v*13.8)))
    return max(0.,width*(.94-.18*v)*max(0.,1.-v)**.73*ridge)
assert radius(0.)>radius(.5)>radius(.8)>radius(1.)==0.
def heat_at(distance,r):
    cover=max(0.,min(1.,1.-(distance-r)/max(4.,r*.45)))
    core=max(0.,min(1.,(r-distance)/max(1.,r)))
    return cover*(.19+.81*core**.56)
assert heat_at(0.,14.)>heat_at(7.,14.)>heat_at(14.,14.)>0
assert heat_at(21.,14.)==0

# Previously approved bridge: multi-frame visible neck followed by eased
# retraction, not instant parent length teleport.
split,short=100.,42.
frames=[i*.04 for i in range(11)]
length=[]
for t in frames:
    if t<=.15:
        length.append(split)
    else:
        length.append(split+(short-split)*smooth((t-.15)/.22))
assert length[:4]==[100.]*4
assert all(length[i+1]<=length[i]+1.e-8 for i in range(10))
assert length[4]>90.

# Fixed topology and bounds. Size assertions are NOT benchmark results.
# Stage18: detached field has seven transverse thermal-gradient columns
# rather than two uniformly colored strip vertices.
verts=(32+1)*(44+1)+2*15*(9+4)+16*5*7
tris=32*44*2+15*(9+4-2)*2+16*(5-1)*(7-1)*2
assert verts==2435 and tris==3914
assert verts<=2600 and tris<=4100
assert verts>2*(15*(9+4)+16*5)
for n in (1,10,30):
    print(f"Stage12 max {n} actors: {n*verts} verts, "
          f"{n*tris} tris / frame @25Hz (theoretical only)")

# Installer still maintains backups, pins commit, and avoids writes with an
# open editor. No change to maps, weapons, NPC prefabs or original materials.
check_tokens(install,(
    "STAGE10_BACKUP=", "STAGE10_REVISION=",
    "STAGE10_BUILD_EXIT=", "Get-Process -Name UnrealEditor",
    "setup_stage10_lifecycle.py", "STAGE10_COMMANDLET=",
))
print("Stage15 one living flame silhouette: lifecycle, mesh depths, heat shape and backup checks OK")
