# Cozinha o catalogo de estrelas do ceu noturno (Assets/Sky/stars.sstars).
#
# Fonte: HYG Database v3.5 (https://github.com/astronexus/HYG-Database), filtrada as linhas
# com numero HR — ou seja, exatamente o subconjunto do Yale Bright Star Catalog (dominio
# publico, ~9k estrelas visiveis a olho nu). Campos usados: rarad, decrad, mag, ci (B-V).
#
# Formato de saida (little-endian):
#   u32 magic 'SSTR' (0x52545353) | u32 version=1 | u32 count | u32 pad
#   por estrela (20 bytes): float dirX,dirY,dirZ (frame equatorial, polo celeste = +Y),
#                           float brightness (10^(-0.4*mag), mag 0 -> 1.0),
#                           u32 colorRGBA8 (cor linear do B-V, A livre)
#
# Uso: python Tools/CookStars.py <hyg_v35.csv> [mag_max=6.5]

import csv
import math
import struct
import sys


def bv_to_rgb(bv):
    """B-V -> RGB linear aproximado (curva classica de temperatura de cor, Dan Bruton)."""
    bv = max(-0.4, min(2.0, bv))
    t = (bv + 0.4) / 2.4
    # temperatura ~ 10000K (azul) .. 2500K (vermelho); rampa empirica em 3 trechos
    if t < 0.4:
        r = 0.62 + 0.95 * t
        g = 0.70 + 0.60 * t
        b = 1.00
    elif t < 0.7:
        r = 1.00
        g = 0.94 - 0.30 * (t - 0.4)
        b = 1.00 - 1.60 * (t - 0.4)
    else:
        r = 1.00
        g = 0.85 - 0.80 * (t - 0.7)
        b = 0.52 - 0.90 * (t - 0.7)
    clamp = lambda x: max(0.0, min(1.0, x))
    return clamp(r), clamp(g), clamp(b)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    mag_max = float(sys.argv[2]) if len(sys.argv) > 2 else 6.5

    stars = []
    with open(src, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if not row.get("hr"):
                continue  # so o subconjunto Yale BSC (dominio publico)
            try:
                mag = float(row["mag"])
                ra = float(row["rarad"])
                dec = float(row["decrad"])
            except (ValueError, KeyError):
                continue
            if mag > mag_max:
                continue
            ci = 0.5
            try:
                if row.get("ci"):
                    ci = float(row["ci"])
            except ValueError:
                pass

            # Frame do catalogo: polo celeste (dec=+90) = +Y; RA gira no plano XZ.
            cd = math.cos(dec)
            x = cd * math.cos(ra)
            z = cd * math.sin(ra)
            y = math.sin(dec)
            brightness = 10.0 ** (-0.4 * mag)
            r, g, b = bv_to_rgb(ci)
            stars.append((x, y, z, brightness, r, g, b))

    stars.sort(key=lambda s: -s[3])  # mais brilhantes primeiro (debug/lod futuro)

    out = "Assets/Sky/stars.sstars"
    with open(out, "wb") as f:
        f.write(struct.pack("<4sIII", b"SSTR", 1, len(stars), 0))
        for x, y, z, br, r, g, b in stars:
            rgba = (int(r * 255) | (int(g * 255) << 8) | (int(b * 255) << 16) | (255 << 24))
            f.write(struct.pack("<ffffI", x, y, z, br, rgba))

    print(f"{out}: {len(stars)} estrelas (mag <= {mag_max})")


if __name__ == "__main__":
    main()
