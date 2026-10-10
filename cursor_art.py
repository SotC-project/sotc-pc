# Builds the menu cursor as a C array for the runtime: a knapped stone arrowhead, lit from the
# upper left, with a soft shadow under it. Everything is drawn here; no picture is read.
# Usage: python cursor_art.py <header path>   (also writes <header path>.png and a preview)
import random
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S, K = 48, 8  # final size, supersample
random.seed(11)
big = S * K


def scaled(points):
    return [(x * K, y * K) for x, y in points]


# Outline: tip at the top left (the hotspot), a broad blade and a broken-off tail.
outline = [(2, 2), (30, 14), (21.5, 18.5), (29, 33), (22.5, 37), (15, 22.5), (8.5, 31)]
# The ridge the blade was struck along runs from the tip to the notch; it splits the lit
# face from the shaded one. The tail has its own two faces.
ridge = [(2, 2), (18.3, 20.5)]
faces = [
    ([(2, 2), (30, 14), (21.5, 18.5), (18.3, 20.5)], 0.92),      # upper face, catches the light
    ([(2, 2), (18.3, 20.5), (15, 22.5), (8.5, 31)], 0.46),       # lower face, turned away
    ([(21.5, 18.5), (29, 33), (25.5, 35), (18.3, 20.5)], 0.66),  # tail, lit side
    ([(18.3, 20.5), (25.5, 35), (22.5, 37), (15, 22.5)], 0.32),  # tail, dark side
]

mask = Image.new("L", (big, big), 0)
ImageDraw.Draw(mask).polygon(scaled(outline), fill=255)

# Base tone per face.
tone = Image.new("L", (big, big), 0)
draw = ImageDraw.Draw(tone)
for points, light in faces:
    draw.polygon(scaled(points), fill=int(255 * light))
tone = tone.filter(ImageFilter.GaussianBlur(K * 0.45))  # faces meet in a soft arris, not a line


def noise(scale, strength):
    small = Image.effect_noise((max(big // scale, 2), max(big // scale, 2)), 64)
    return small.resize((big, big), Image.BICUBIC), strength


# Stone surface: broad mottling, medium grain and fine grit, plus pits and pale flecks.
surface = Image.new("L", (big, big), 128)
for layer, strength in (noise(40, 0.42), noise(14, 0.40), noise(5, 0.34), noise(2, 0.22)):
    surface = Image.blend(surface, layer, strength)
marks = ImageDraw.Draw(surface)
for _ in range(260):
    x, y = random.uniform(0, big), random.uniform(0, big)
    r = random.uniform(K * 0.12, K * 0.55)
    marks.ellipse((x - r, y - r, x + r, y + r), fill=random.choice((30, 44, 60, 72, 196, 214)))
surface = surface.filter(ImageFilter.GaussianBlur(K * 0.07))

# Conchoidal flake scars: shallow curved steps fanning out from the ridge.
scars = Image.new("L", (big, big), 128)
sd = ImageDraw.Draw(scars)
for cx, cy, radius, start in ((20, 6, 15, 110), (26, 9, 12, 120), (11, 22, 10, 300),
                              (9, 17, 8, 310), (27, 26, 9, 100), (20, 31, 8, 290)):
    box = [(cx - radius) * K, (cy - radius) * K, (cx + radius) * K, (cy + radius) * K]
    sd.arc(box, start, start + 62, fill=168, width=max(K // 3, 1))
    box = [v + K * 0.45 for v in box]
    sd.arc(box, start, start + 62, fill=92, width=max(K // 3, 1))
scars = scars.filter(ImageFilter.GaussianBlur(K * 0.22))

# Put tone, surface and scars together: grey flint, slightly warm in the light, cool in shade.
stone = Image.new("RGB", (big, big))
tone_px, surf_px, scar_px, out_px = tone.load(), surface.load(), scars.load(), stone.load()
for y in range(big):
    for x in range(big):
        light = tone_px[x, y] / 255.0
        value = light * (0.30 + 1.40 * surf_px[x, y] / 255.0) + (scar_px[x, y] - 128) / 255.0 * 0.85
        value = max(0.0, min(1.0, value))
        warm = 0.5 + (light - 0.5) * 0.9
        out_px[x, y] = (int(24 + 176 * value + 12 * warm), int(24 + 172 * value + 5 * warm),
                        int(26 + 160 * value - 8 * warm))

# Edges: a thin bright chamfer where the upper left edges catch the light, a dark one opposite.
inner = mask.filter(ImageFilter.MinFilter(2 * (K // 2) + 1))
rim = ImageChops.subtract(mask, inner)
lit = ImageChops.multiply(rim, ImageChops.offset(inner, K // 2, K // 2).point(lambda v: 255 - v))
dark = ImageChops.multiply(rim, ImageChops.offset(inner, -K // 2, -K // 2).point(lambda v: 255 - v))
stone = Image.composite(Image.new("RGB", (big, big), (244, 242, 232)), stone,
                        lit.filter(ImageFilter.GaussianBlur(K * 0.12)).point(lambda v: int(v * 0.70)))
stone = Image.composite(Image.new("RGB", (big, big), (26, 26, 28)), stone,
                        dark.filter(ImageFilter.GaussianBlur(K * 0.12)).point(lambda v: int(v * 0.75)))
# The ridge itself, and one hairline crack across the blade.
sd = ImageDraw.Draw(stone)
sd.line(scaled(ridge), fill=(236, 234, 224), width=max(K // 4, 1))
crack = [(12.5, 6.4), (14.2, 9.6), (13.4, 12.2), (15.6, 15.4)]
sd.line(scaled(crack), fill=(44, 44, 46), width=max(K // 4, 1))
sd.line(scaled([(x + 0.3, y + 0.3) for x, y in crack]), fill=(208, 206, 196), width=max(K // 6, 1))

# A little moss in the notch and along the shaded edge, as on old masonry.
moss = Image.new("L", (big, big), 0)
md = ImageDraw.Draw(moss)
for cx, cy, count, spread in ((17.6, 21.4, 60, 1.5), (11.5, 26.0, 34, 1.2), (23.5, 35.0, 26, 1.0)):
    for _ in range(count):
        x = cx * K + random.gauss(0, K * spread)
        y = cy * K + random.gauss(0, K * spread * 0.8)
        r = random.uniform(K * 0.22, K * 0.62)
        md.ellipse((x - r, y - r, x + r, y + r), fill=random.randint(150, 255))
moss = moss.filter(ImageFilter.GaussianBlur(K * 0.18))
green = Image.new("RGB", (big, big))
gp = green.load()
for y in range(big):
    for x in range(big):
        v = random.randint(-20, 20)
        gp[x, y] = (62 + v, 100 + v, 40 + v // 2)
stone = Image.composite(green, stone, ImageChops.multiply(moss, mask))

# Shadow thrown down and to the right, then a dark outline so the stone reads on bright sky,
# and the stone on top.
out = Image.new("RGBA", (big, big), (0, 0, 0, 0))
shadow = ImageChops.offset(mask, int(K * 2.6), int(K * 3.2)).filter(ImageFilter.GaussianBlur(K * 1.7))
out.paste((0, 0, 0, 255), mask=shadow.point(lambda v: int(v * 0.62)))
edge = mask.filter(ImageFilter.MaxFilter(2 * int(K * 0.9) + 1)).filter(ImageFilter.GaussianBlur(K * 0.2))
out.paste((16, 16, 18, 255), mask=edge)
out.paste(stone, mask=mask)
out = out.resize((S, S), Image.LANCZOS)
out.save(sys.argv[1] + ".png")

# Preview: the cursor on a bright sky tone, mid grey and the dark menu tone, at 1x and 3x.
preview = Image.new("RGBA", (S * 4 * 3, S * 3), (0, 0, 0, 255))
for i, back in enumerate(((206, 212, 214), (112, 116, 112), (22, 22, 20))):
    tile = Image.new("RGBA", (S * 4, S * 3), back + (255,))
    tile.alpha_composite(out.resize((S * 3, S * 3), Image.NEAREST), (0, 0))
    tile.alpha_composite(out, (S * 3, S))
    preview.paste(tile, (i * S * 4, 0))
preview.save(sys.argv[1] + ".preview.png")

data = out.tobytes()
with open(sys.argv[1], "w", newline="\n") as f:
    f.write("// Generated by cursor_art.py: menu cursor, RGBA8.\n#pragma once\n\n")
    f.write(f"inline constexpr int SotcCursorSize = {S};\n")
    f.write("inline constexpr unsigned char SotcCursorPixels[] = {\n")
    for i in range(0, len(data), 20):
        f.write("    " + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
    f.write("};\n")
