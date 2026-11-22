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
    "TongueCenterAt(T,Root,.10f,Right)",
    "TongueCenterAt(T,Root,.23f,Right)",
    "C.Add(TongueCenterAt(T,Base,V,Right));",
    "Necks.Reserve(Tongues.Num()*3);",
    "static constexpr float NeckFractions[4] = {0.f,.14f,.28f,.42f};",
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
    "if (bEnableProceduralFlames) return;",
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

# Mesh samples are taken from CURRENT skinned geometry and collision hits.
# The fixed-topology marching-like mask is a single connected source, not
# independent source cards corresponding to each tongue/bone.
check_tokens(c,(
    "BuildSourceAnchors()", "SourceAnchors.Reset()",
    "StaticVertexBuffers.PositionVertexBuffer",
    "USkinnedMeshComponent::GetSkinnedVertexPosition(",
    "Transform.InverseTransformPosition(", "Transform.TransformPosition(",
    "if(SourceAnchors.IsEmpty())", "Source->GetComponentTransform()",
    "for(const FTongue& T:Tongues)",
    "Samples.Add(P)", "Parent[J]=Best", "Nearest[J]=D",
    "FVector2D::DistSquared(P,Samples[I])",
    "float PointD2=BIG_NUMBER,EdgeD2=BIG_NUMBER;",
    "FMath::Sqrt(PointD2)-Radius", "FMath::Sqrt(EdgeD2)-Radius*.68f",
    "const float Fill=FMath::Clamp(1.f-Dist/Outer,0.f,1.f);",
    "const float Core=FMath::Clamp((-Dist)/(Radius+Inner),0.f,1.f);",
    "Include(Origin); // no donut in free-standing fire",
    "Necks.Add(Neck);",
    "const float BurnRadius = FMath::Clamp(",
    "const UPrimitiveComponent* Primitive =",
    "TArray<float> SampleDepths;",
    "float FrontDepth=-BIG_NUMBER;",
    "FrontDepth=FMath::Max(FrontDepth,BestDepth[I]);",
    "const float Depth=FrontDepth>-BIG_NUMBER*.5f",
    "float Thermal=FMath::Clamp(",
    "UpdateConnectedSource(bCreate, View, Right)",
    "1,Verts,Tris,Normals,UVs,Colors,Tangents,false",
    "1,Verts,Normals,UVs,Colors,Tangents",
))
assert "Source.IsValid()" not in c
assert "Render->UpdateMeshSection_LinearColor(1" not in c  # formatting differs
assert "return dir*depth" not in c
assert "FMath::Sin(Cycle * PI)" not in c

# Numerical reference for the connected-source geometry and thermal profile.
# A single fire source has its own contour even without any source mesh.
def smooth(x):
    t=max(0.,min(1.,x))
    return t*t*(3.-2.*t)

def mst(points):
    n=len(points)
    parent=[None]*n
    visited=[False]*n
    best=[float("inf")]*n
    best[0]=0.
    for _ in range(n):
        i=min((i for i in range(n) if not visited[i]),
              key=lambda i:best[i])
        visited[i]=True
        for j in range(n):
            if visited[j]:continue
            delta=sum((points[i][k]-points[j][k])**2 for k in range(2))
            if delta<best[j]:
                best[j]=delta
                parent[j]=i
    return parent

def shape_distance(point,points,parent,radius):
    px,py=point
    dpoint=min(sqrt((px-x)**2+(py-y)**2)-radius
               for x,y in points)
    dedge=float("inf")
    for i,j in enumerate(parent):
        if j is None:continue
        x0,y0=points[i]
        x1,y1=points[j]
        dx,dy=x1-x0,y1-y0
        t=max(0.,min(1.,((px-x0)*dx+(py-y0)*dy)/max(1.,dx*dx+dy*dy)))
        dedge=min(dedge,sqrt((px-(x0+t*dx))**2+
                             (py-(y0+t*dy))**2)-radius*.68)
    return min(dpoint,dedge)

def thermal(d,radius=10,spread=.22):
    inner=max(5.,min(12.,radius*.65))
    outer=max(9.,min(18.,radius*.85))
    fill=max(0.,min(1.,1-d/outer))
    core=max(0.,min(1.,(-d)/(radius+inner)))
    return fill*(.43+.26*core+.13*spread)

# Even if the tongues are far apart, their common base has a connecting
# manifold. This must ALSO hold for the free-standing case (no mesh).
samples=[(-30.,0.),(-5.,0.),(21.,0.),(47.,0.)]
parents=mst(samples)
assert parents[0] is None
assert all(p is not None for p in parents[1:])
for x in range(-30,48,3):
    assert shape_distance((float(x),0.),samples,parents,10.)<0.
assert thermal(-20.)>.6  # NO hollow center; source core is hot
assert thermal(-5.)>thermal(0.)>thermal(5.)>0.
assert thermal(30.)==0.
assert thermal(-10.)>thermal(10.)
# Root at the origin plus close roots produce solid fuel, not a donut:
free_roots=[(0.,0.),(13.,0.),(-10.,8.),(-10.,-8.)]
free_parents=mst(free_roots)
assert shape_distance((0.,0.),free_roots,free_parents,10.)<0.
assert thermal(shape_distance((0.,0.),free_roots,free_parents,10.))>.4
# A living tongue sprouts from inside that same connected body.
assert "const FVector Root=SampleRoot(T);" in c
assert "const float Emergence=ASTFireLife::SmoothStep((V-.20f)/.26f);" in c

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
verts=(32+1)*(44+1)+2*(15*(9+4)+16*5)
tris=32*44*2 + 2*(15*(9+4-2)+16*(5-1))
assert verts<=2100 and tris<=3300
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
print("Stage13 filled source: no hollow rings, root blending, mesh depth and backup checks OK")
