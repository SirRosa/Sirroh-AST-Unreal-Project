"""Independent numerical smoke-check for Stage5's visual advection equations.

Reference math, not a UE runtime/PIE/visual validation. No nonstdlib packages.
"""
from math import sin, cos, exp
from pathlib import Path

W, H, DT, STEPS = 22, 30, 0.04, 90
SEED = 1729
PHASE = (SEED % 1009) * .0137
code = Path("Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp").read_text()
for marker in ("constexpr int32 Width = 22;", "constexpr int32 Height = 30;",
               "(0.21f + 0.36f * V)", "6.2f * FMath::Sin",
               "4.1f * FMath::Sin", "FMath::Exp(-Dt * 1.4f * V * Shear)",
               "Fragment.Y = float(ASTHeat::Height) * (0.72f + 0.15f *"):
    assert marker in code, f"Reference drifted from C++: {marker}"


def clamp(x):
    return max(0.0, min(1.0, x))


def prewarm():
    out = [0.0] * (W * H)
    for y in range(H):
        v = (y + .5) / H
        for x in range(W):
            u = (x + .5) / W * 2 - 1
            drift = .105 * sin(v * 11 + PHASE) + .048 * sin(v * 26 - PHASE)
            width = .76 * max(0.0, 1 - v / .77) ** .55
            base = clamp((width - abs(u - drift)) * 7) * (.94 - v * .49)
            tips = 0.0
            for b, height, middle in ((0, .96, -.09), (1, .74, -.36), (2, .86, .33)):
                center = middle + .15 * sin(v * (9 + b) + PHASE + b * 1.8)
                edge = clamp((.19 * (1 - .68 * v) - abs(u - center)) * 11)
                cap = clamp((height - v) * 11)
                tips = max(tips, edge * cap * (.72 - v * .31))
            out[y * W + x] = max(base, tips)
    return out


def sample(arr, x, y):
    if not (0 <= x < W - 1 and 0 <= y < H - 1):
        return 0.0
    xi, yi = int(x), int(y)
    ax, ay = x - xi, y - yi
    i = yi * W + xi
    a = arr[i] * (1 - ax) + arr[i + 1] * ax
    b = arr[i + W] * (1 - ax) + arr[i + W + 1] * ax
    return a * (1 - ay) + b * ay


def source(x, t):
    u = (x + .5) / W * 2 - 1
    center = .09 * sin(t * 2.3 + PHASE)
    left = -.36 + .12 * sin(t * 3.1 + PHASE * 1.37)
    right = .32 + .14 * sin(t * 2.7 + PHASE * .79)
    return clamp(max(
        exp(-((u - center) / .49) ** 2),
        .82 * exp(-((u - left) / .22) ** 2),
        .73 * exp(-((u - right) / .23) ** 2),
    ))


def run(wind):
    field = prewarm()
    first = tuple(field)
    t = 0.0
    for _ in range(STEPS):
        t += DT
        output = []
        for y in range(H):
            v = (y + .5) / H
            cooling = exp(-DT * (.21 + .36 * v))
            for x in range(W):
                u = (x + .5) / W * 2 - 1
                old = field[y * W + x]
                eddies = (
                    6.2 * sin(u * 7 + v * 14 + t * 3.2 + PHASE)
                    + 4.1 * sin(u * 13 - v * 8 - t * 2.7 + PHASE * .6)
                )
                rise = (10 + 3.8 * old + 2.3) * DT
                shift = (wind * 9.2 + eddies) * DT
                new = sample(field, x - shift, y - rise) * cooling
                if y < 5:
                    fallback = max(0.0, 1 - y / 5)
                    wave = .55 * sin((t * 2.1 + u * 3 + PHASE) * 1.932)
                    wave += .30 * sin((t * 2.1 + u * 3 + PHASE) * 3.117 + 1.4)
                    new = max(new, source(x, t) * (.82 + .18 * wave) * fallback)
                shear = max(0, sin(u * 15 - v * 23 + t * 4.6 + PHASE)
                            + .45 * cos(u * 9 + v * 17 - t * 3.0))
                new *= exp(-DT * 1.4 * v * shear)
                if 14 < y < H - 2:
                    new += .11 * max(0, sin(t * 9.7 + x * 13 + y * 7 + PHASE)) * DT
                output.append(clamp(new))
        field = output

    active = sum(value >= .21 for value in field)
    high = [(x, field[y * W + x]) for y in range(12, H) for x in range(W)]
    total = sum(v for _, v in high)
    centroid = sum(x * v for x, v in high) / total
    return active, max(field), centroid, field != list(first)


still = run(0)
left = run(-1)
right = run(1)
assert 100 < still[0] < W * H * .85, still
assert still[1] > .72, still
assert still[3], "Temperature must evolve after startup"
assert right[2] - left[2] > 6.0, (left, right)
print("Stage5 heat reference OK: living field, varied coverage, wind-dependent shift")
