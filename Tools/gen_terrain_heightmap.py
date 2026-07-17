# Gera heightmap de teste 2048^2 u16 RAW (.r16) para o terreno da SmileEngine — F2.5:
# vale plano no centro (a Bistro senta nele) + CORDILHEIRA ridged na borda (~140 m
# com heightScale 170 / originY -21).
import numpy as np

SIZE = 2048
rng = np.random.default_rng(42)

def value_noise(size, cells):
    g = rng.random((cells + 1, cells + 1)).astype(np.float32)
    x = np.linspace(0, cells, size, endpoint=False, dtype=np.float32)
    xi = x.astype(np.int32); xf = x - xi
    sx = xf * xf * (3 - 2 * xf)
    a = g[np.ix_(xi, xi)]; b = g[np.ix_(xi, xi + 1)]
    c = g[np.ix_(xi + 1, xi)]; d = g[np.ix_(xi + 1, xi + 1)]
    top = a + (b - a) * sx[None, :]
    bot = c + (d - c) * sx[None, :]
    return top + (bot - top) * sx[:, None]

def fbm(octaves, cells0, gain=0.5):
    h = np.zeros((SIZE, SIZE), np.float32)
    amp, cells, total = 1.0, cells0, 0.0
    for _ in range(octaves):
        h += amp * value_noise(SIZE, cells)
        total += amp
        amp *= gain
        cells *= 2
    return h / total

base = fbm(7, 4)

# Ridged FBM pros picos: 1 - |2n - 1| realca cristas
ridged = np.zeros((SIZE, SIZE), np.float32)
amp, cells, total = 1.0, 6, 0.0
for _ in range(5):
    n = value_noise(SIZE, cells)
    ridged += amp * (1.0 - np.abs(2.0 * n - 1.0))
    total += amp
    amp *= 0.55
    cells *= 2
ridged /= total

yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
r = np.sqrt((xx / SIZE - 0.5) ** 2 + (yy / SIZE - 0.5) ** 2) * 2.0
ring = np.clip((r - 0.28) / 0.72, 0.0, 1.0)

# Vale quase plano no centro; a cordilheira entra com ring^1.4 e os picos ridged por cima
h = 0.10 + base * 0.25 * np.maximum(ring, 0.12) + ring ** 1.4 * (0.42 + 0.38 * ridged)
h = np.clip(h, 0.0, 1.0)

out = (h * 65535.0 + 0.5).astype("<u2")
path = r"D:\Engines\SmileEngine\Assets\Scenes\Bistro\terrain_test_2048.r16"
out.tofile(path)
c = h[SIZE // 2, SIZE // 2]
print("ok:", path, "| centro h =", round(float(c), 3), "| max h =", round(float(h.max()), 3))
print("com heightScale 170 / originY -21: centro =", round(c * 170 - 21, 1), "m | pico =",
      round(float(h.max()) * 170 - 21, 1), "m")
