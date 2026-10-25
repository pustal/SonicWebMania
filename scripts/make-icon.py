#!/usr/bin/env python3
# Usage: scripts/make-icon.py webos   (needs Pillow: pip install pillow)
"""Draws the SonicWebMania app icon as original pixel art: a checkered loop on a
checkered hill under a banded 16-bit sky, with speed streaks. No characters,
logos or game artwork are used. Rendered on a cell grid and upscaled 2x with
nearest-neighbour so it stays crisp on the TV."""
import math, sys
from PIL import Image

SKY = [(28, 44, 170), (36, 78, 210), (48, 120, 235), (80, 170, 250), (130, 210, 255)]
CHECK_A, CHECK_B = (196, 92, 28), (238, 142, 46)
CHECK_EDGE = (120, 50, 18)
GRASS, GRASS_DARK = (70, 205, 60), (28, 140, 40)
CLOUD = (245, 250, 255)
STREAK = [(255, 255, 255), (255, 224, 72), (255, 255, 255)]
BG_EDGE = (10, 18, 48)

def pixel(u, v):
    """Colour at normalised position (u, v) in [0,1), or None for transparent."""
    # Rounded-square icon mask.
    r = 0.18
    cx = min(max(u, r), 1 - r); cy = min(max(v, r), 1 - r)
    if (u - cx) ** 2 + (v - cy) ** 2 > r * r:
        return None

    # Loop: checkered ring with a grass lip on the inside.
    lx, ly, outer, inner = 0.56, 0.47, 0.30, 0.20
    d = math.hypot(u - lx, v - ly)
    ground_top = 0.70 + 0.035 * math.sin(u * 7.0)
    if inner <= d <= outer and v < ground_top + 0.02:
        if d > outer - 0.025:
            return CHECK_EDGE
        if d < inner + 0.03:
            return GRASS if d > inner + 0.012 else GRASS_DARK
        ang = math.atan2(v - ly, u - lx)
        seg = int((ang + math.pi) / (2 * math.pi) * 16)
        band = int((d - inner) / ((outer - inner) / 2))
        return CHECK_A if (seg + band) % 2 == 0 else CHECK_B

    # Hill: grass edge over a checkerboard.
    if v >= ground_top:
        if v < ground_top + 0.035:
            return GRASS
        if v < ground_top + 0.055:
            return GRASS_DARK
        cell = 0.1
        return CHECK_A if (int(u / cell) + int((v - ground_top) / cell)) % 2 == 0 else CHECK_B

    # Speed streaks entering the loop from the left.
    for i, (sy, length) in enumerate([(0.52, 0.30), (0.585, 0.40), (0.65, 0.26)]):
        x0 = 0.06 + (0.40 - length)
        if x0 <= u <= 0.40 and abs(v - sy) < 0.016:
            return STREAK[i]

    # Clouds.
    for (cx2, cy2, rad) in [(0.20, 0.20, 0.07), (0.28, 0.17, 0.08), (0.36, 0.21, 0.06), (0.80, 0.13, 0.05), (0.87, 0.15, 0.045)]:
        if (u - cx2) ** 2 + ((v - cy2) * 1.6) ** 2 < rad * rad:
            return CLOUD

    # Banded sky.
    return SKY[min(len(SKY) - 1, int(v / 0.70 * len(SKY)))]

def render(cells, scale, path):
    img = Image.new("RGBA", (cells, cells), (0, 0, 0, 0))
    for y in range(cells):
        for x in range(cells):
            c = pixel((x + 0.5) / cells, (y + 0.5) / cells)
            if c is not None:
                img.putpixel((x, y), c + (255,))
    img.resize((cells * scale, cells * scale), Image.NEAREST).save(path)

out = sys.argv[1]
render(40, 2, f"{out}/icon.png")        # 80x80
render(65, 2, f"{out}/largeIcon.png")   # 130x130

