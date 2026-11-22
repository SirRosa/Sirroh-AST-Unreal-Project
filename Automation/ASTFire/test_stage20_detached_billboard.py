"""Stage20 regression: perspective-correct detached billboard, without changing Stage18 fire.

Numerical tests are independent reference math; not Unreal rendering/PIE tests.
"""
from pathlib import Path
from math import sin, cos, sqrt

source = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireFreeFlameComponent.cpp").read_text()
geometry = source[source.index("void UASTFireFreeFlameComponent::UpdateGeometry("):
                  source.index("void UASTFireFreeFlameComponent::BuildSourceAnchors()")]
fragment = geometry[geometry.index("for (const FDetached& D:Detached)"):
                    geometry.index("TArray<int32> Triangles;")]

for required in (
    "CameraLocation=Cam->GetCameraLocation();",
    "CameraRight=Cam->GetCameraRotation().RotateVector(",
    "const FVector ToCamera=(CameraLocation-D.Position).GetSafeNormal();",
    "const FVector BillboardView=ToCamera.IsNearlyZero()?View:ToCamera;",
    "const FVector ProjectedRight=CameraRight-BillboardView*",
    "FVector::DotProduct(CameraRight,BillboardView);",
    "const FVector BillboardUp=FVector::CrossProduct(",
    "BillboardView,BillboardRight).GetSafeNormal();",
    "BillboardUp*FMath::Cos(Angle)+",
    "BillboardRight*FMath::Sin(Angle)",
    "BillboardRight*FMath::Cos(Angle)-",
    "BillboardUp*FMath::Sin(Angle)",
):
    assert required in geometry, required
assert "FVector::UpVector*FMath::Cos(Angle)" not in fragment
assert "const FVector LocalSide=(Right*FMath::Cos(Angle)-" not in fragment

def dot(a,b):
    return sum(x*y for x,y in zip(a,b))
def cross(a,b):
    return (a[1]*b[2]-a[2]*b[1],
            a[2]*b[0]-a[0]*b[2],
            a[0]*b[1]-a[1]*b[0])
def add(a,b):
    return tuple(x+y for x,y in zip(a,b))
def sub(a,b):
    return tuple(x-y for x,y in zip(a,b))
def mul(a,k):
    return tuple(x*k for x in a)
def unit(a):
    n=sqrt(dot(a,a))
    return mul(a,1/n) if n>1e-8 else (0.,0.,0.)

UP=(0.,0.,1.)
FORWARD=(1.,0.,0.)
RIGHT=(0.,1.,0.)

def basis(camera,fragment_position,camera_right,root_view):
    ray=unit(sub(camera,fragment_position))
    view=ray if dot(ray,ray)>1e-8 else root_view
    projected=sub(camera_right,mul(view,dot(camera_right,view)))
    right=unit(projected)
    if dot(right,right)<1e-8: right=unit(cross(UP,view))
    if dot(right,right)<1e-8: right=unit(cross(FORWARD,view))
    if dot(right,right)<1e-8: right=unit(cross(RIGHT,view))
    up=unit(cross(view,right))
    return view,right,up

# Camera elevated, exactly above, near top-down, and multiple offset fragments.
cases = [
    ((180.,20.,90.),(0.,0.,0.),(0.,1.,0.)),
    ((180.,20.,90.),(110.,-30.,15.),(0.,1.,0.)),
    ((0.,0.,200.),(0.,0.,0.),(0.,1.,0.)),
    ((0.0001,0.,200.),(0.,0.,0.),(0.,1.,0.)),
    ((15.,-20.,900.),(0.,0.,0.),(0.,1.,0.)),
    ((0.,0.,200.),(0.,0.,0.),(0.,0.,1.)), # camera-right degenerate
]
for camera,fragment_pos,camera_right in cases:
    view,right,up=basis(camera,fragment_pos,camera_right,FORWARD)
    assert abs(dot(right,view))<1e-6, (camera,fragment_pos)
    assert abs(dot(up,view))<1e-6
    assert abs(dot(right,up))<1e-6
    assert abs(dot(right,right)-1.)<1e-6
    assert abs(dot(up,up)-1.)<1e-6
    for spin in (0.,-.75,.37,.75):
        rotated_up=unit(add(mul(up,cos(spin)),mul(right,sin(spin))))
        rotated_side=unit(sub(mul(right,cos(spin)),mul(up,sin(spin))))
        normal=unit(cross(rotated_side,rotated_up))
        assert dot(normal,view)>.999999, (camera,fragment_pos,spin)

# The same camera yields distinct perspective normals for distinct children.
a=basis((100.,30.,60.),(0.,0.,0.),RIGHT,FORWARD)[0]
b=basis((100.,30.,60.),(40.,-25.,12.),RIGHT,FORWARD)[0]
assert dot(a,b)<.999

# Zero camera distance uses the parent view and remains nondegenerate.
view,right,up=basis((0.,0.,0.),(0.,0.,0.),RIGHT,FORWARD)
assert dot(cross(right,up),view)>.999999

# The 7-column heat palette, geometry budget, and physics must remain intact.
for token in (
    "static constexpr int32 DetachedColumns=7;",
    "Heat*D.Temperature*LocalHeat*",
    "FMath::Pow(Fade,.66f)",
    "const float Angle=FMath::Sin(Spin)*.75f;",
    "LocalUp*D.Length*V*Fade",
    "FMath::Sin(V*14.2f+Spin+D.Seed)",
    "const FVector World=C[K]+LocalSide*",
    "Triangles.Add(A+DetachedColumns+1);",
):
    assert token in geometry, token
for token in (
    "D.Position+=D.Velocity*Dt;",
    "UpdateDetachedVortices(SimDt);",
    "EFlameLife::Bridging",
    "EFlameLife::Retracting",
):
    assert token in source, token
print("Stage20: fragment-position billboard, overhead/oblique camera, spin, Stage18 palette guarded")
