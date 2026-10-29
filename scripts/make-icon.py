#!/usr/bin/env python3
# Usage: scripts/make-icon.py webos   (needs Pillow: pip install pillow)
"""Draws the SonicWebMania app icon as original pixel art: a checkered loop on a
checkered hill under a banded 16-bit sky, with speed streaks, and a red ribbon
across the hill carrying the name ("SONIC" over "WEBMANIA") so the app is easy
to spot in the webOS app list. No characters, logos or game artwork are used;
the letters are font8x8 (public domain, the same font the app draws its own
text with, read from src/port/BitmapFont.cpp). The scene is rendered on a cell
grid and upscaled 2x with nearest-neighbour; the ribbon and letters are drawn
at the final size, so they stay crisp on the TV."""
import math, os, re, sys
from PIL import Image

SKY = [(28, 44, 170), (36, 78, 210), (48, 120, 235), (80, 170, 250), (130, 210, 255)]
CHECK_A, CHECK_B = (196, 92, 28), (238, 142, 46)
CHECK_EDGE = (120, 50, 18)
GRASS, GRASS_DARK = (70, 205, 60), (28, 140, 40)
CLOUD = (245, 250, 255)
STREAK = [(255, 255, 255), (255, 224, 72), (255, 255, 255)]
BG_EDGE = (10, 18, 48)

# Vertical layout (fractions of the icon): the scene sits high, so the name
# ribbon fits on the ground under the loop and a band of checkered ground
# still shows beneath it.
GROUND = 0.58      # Where the hill's grass starts (it waves a little around this).
LOOP_Y = 0.35      # Centre of the loop.
RIBBON_TOP = 0.635 # Top of the name ribbon, just under the loop.

def pixel(u, v):
    """Colour at normalised position (u, v) in [0,1), or None for transparent."""
    # Rounded-square icon mask.
    r = 0.18
    cx = min(max(u, r), 1 - r); cy = min(max(v, r), 1 - r)
    if (u - cx) ** 2 + (v - cy) ** 2 > r * r:
        return None

    # Loop: checkered ring with a grass lip on the inside.
    lx, ly, outer, inner = 0.56, LOOP_Y, 0.27, 0.18
    d = math.hypot(u - lx, v - ly)
    ground_top = GROUND + 0.03 * math.sin(u * 7.0)
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
    for i, (sy, length) in enumerate([(0.41, 0.30), (0.475, 0.40), (0.54, 0.26)]):
        x0 = 0.06 + (0.40 - length)
        if x0 <= u <= 0.40 and abs(v - sy) < 0.016:
            return STREAK[i]

    # Clouds.
    for (cx2, cy2, rad) in [(0.17, 0.15, 0.06), (0.24, 0.12, 0.07), (0.31, 0.16, 0.05), (0.82, 0.10, 0.045), (0.88, 0.12, 0.04)]:
        if (u - cx2) ** 2 + ((v - cy2) * 1.6) ** 2 < rad * rad:
            return CLOUD

    # Banded sky.
    return SKY[min(len(SKY) - 1, int(v / GROUND * len(SKY)))]

# ---- The name ribbon ----------------------------------------------------------

RIBBON = (206, 32, 44)
RIBBON_LIGHT = (240, 78, 76)
RIBBON_DARK = (130, 14, 30)
RIBBON_FOLD = (96, 8, 22)
TITLE_FILL = [(255, 236, 92), (255, 206, 40), (248, 150, 24)]  # top to bottom
SUBTITLE_FILL = (255, 255, 255)
OUTLINE = (24, 14, 50)


def load_font():
    """Glyphs of font8x8 from the app's BitmapFont.cpp: {char: 8 row bytes, bit 0 = leftmost}."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source = open(os.path.join(root, "src", "port", "BitmapFont.cpp"), encoding="utf-8").read()
    rows = re.findall(r"\{ (0x[0-9A-F]{2}(?:, 0x[0-9A-F]{2}){7}) \}", source)
    return {chr(0x20 + i): [int(v, 16) for v in row.split(", ")] for i, row in enumerate(rows)}


FONT = load_font()


def text_mask(text, scale, bold):
    """Set of (x, y) pixels of @text at @scale (bold: each glyph pixel also one to the right)."""
    pixels = set()
    advance = 8 * scale
    for i, ch in enumerate(text):
        for gy, bits in enumerate(FONT[ch]):
            for gx in range(8):
                if bits >> gx & 1:
                    for sy in range(scale):
                        for sx in range(scale + (1 if bold else 0)):
                            pixels.add((i * advance + gx * scale + sx, gy * scale + sy))
    return pixels


def bounds(mask):
    xs = [x for x, _ in mask]; ys = [y for _, y in mask]
    return min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def draw_text(img, mask, left, top, fill):
    """Draws @mask at (left, top) with a 1-pixel outline; @fill is a colour or a top-to-bottom list."""
    x0, y0, w, h = bounds(mask)
    outline = {(x + dx, y + dy) for x, y in mask for dx in (-1, 0, 1) for dy in (-1, 0, 1)} - mask
    for x, y in outline:
        img.putpixel((left + x - x0, top + y - y0), OUTLINE + (255,))
    for x, y in mask:
        colour = fill[min(len(fill) - 1, (y - y0) * len(fill) // h)] if isinstance(fill, list) else fill
        img.putpixel((left + x - x0, top + y - y0), colour + (255,))


def draw_ribbon(img, top, height):
    """A banner across the icon: lit top edge, shaded bottom edge, folded ends that hang lower."""
    size = img.width
    fold_w, drop = max(4, size // 12), max(2, height // 5)
    for y in range(top, top + height + drop):
        for x in range(size):
            if img.getpixel((x, y))[3] == 0:
                continue  # Outside the rounded icon.
            in_band = top <= y < top + height and fold_w <= x < size - fold_w
            in_fold = (x < fold_w or x >= size - fold_w) and top + drop <= y < top + height + drop
            if in_band:
                rel = y - top
                colour = RIBBON_LIGHT if rel < max(1, height // 10) else RIBBON_DARK if rel >= height - max(1, height // 8) else RIBBON
            elif in_fold:
                # The folded-back ends, darker, with a notch cut into the outer edge.
                mid = top + drop + height // 2
                notch = (x < fold_w and x < abs(y - mid) * fold_w // (height // 2 + 1) - fold_w // 2 + 2) or \
                        (x >= size - fold_w and size - 1 - x < abs(y - mid) * fold_w // (height // 2 + 1) - fold_w // 2 + 2)
                if notch:
                    continue
                colour = RIBBON_FOLD
            else:
                continue
            img.putpixel((x, y), colour + (255,))


def add_name(img, title_scale, subtitle_scale):
    """The ribbon with "SONIC" over "WEBMANIA", centred horizontally on the lower part of the icon."""
    size = img.width
    # Bold from 2x up: at 1x the extra column would close the gap between letters.
    title    = text_mask("SONIC", title_scale, title_scale >= 2)
    subtitle = text_mask("WEBMANIA", subtitle_scale, subtitle_scale >= 2)
    _, _, tw, th = bounds(title)
    _, _, sw, sh = bounds(subtitle)
    gap = max(1, title_scale)
    pad = max(2, title_scale + 1)
    height = th + gap + sh + 2 * pad
    top = int(size * RIBBON_TOP)
    draw_ribbon(img, top, height)
    draw_text(img, title, (size - tw) // 2, top + pad, TITLE_FILL)
    draw_text(img, subtitle, (size - sw) // 2, top + pad + th + gap, SUBTITLE_FILL)


def render(cells, scale, title_scale, subtitle_scale, path):
    img = Image.new("RGBA", (cells, cells), (0, 0, 0, 0))
    for y in range(cells):
        for x in range(cells):
            c = pixel((x + 0.5) / cells, (y + 0.5) / cells)
            if c is not None:
                img.putpixel((x, y), c + (255,))
    img = img.resize((cells * scale, cells * scale), Image.NEAREST)
    add_name(img, title_scale, subtitle_scale)
    img.save(path)


out = sys.argv[1]
render(40, 2, 1, 1, f"{out}/icon.png")         # 80x80
render(65, 2, 2, 1, f"{out}/largeIcon.png")    # 130x130
if len(sys.argv) > 2:
    render(128, 2, 4, 2, sys.argv[2])          # 256x256, the README logo

