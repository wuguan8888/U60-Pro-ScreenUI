#!/usr/bin/env python3
"""Make bg.bin for the U60 screen extension.
Output: 320x432 LVGL LV_IMG_CF_TRUE_COLOR pixels, RGB565 little-endian (LV_COLOR_DEPTH 16, no swap),
276480 bytes, no header. Pre-darkened with the clear-design overlay (+5% because the device has no text shadow),
4x4 ordered dithering to avoid banding in the night sky.
"""
import sys, struct
from PIL import Image
src = sys.argv[1] if len(sys.argv) > 1 else '/workspace/u60/screen-design/bg/anime-bg-640x960.png'
out = sys.argv[2] if len(sys.argv) > 2 else 'bg.bin'
W, H, FULL_H = 320, 432, 480
im = Image.open(src).convert('RGB').resize((W, FULL_H), Image.LANCZOS)
px = im.load()
EXTRA = 0.05
def overlay(y):
    t = y / (FULL_H - 1)
    # design: rgba(0,0,0,.05) 0% -> rgba(4,10,24,.10) 45% -> rgba(0,0,0,.20) 100%
    if t <= 0.45:
        k = t / 0.45; a = 0.05 + (0.10 - 0.05) * k; c = (4 * k, 10 * k, 24 * k)
    else:
        k = (t - 0.45) / 0.55; a = 0.10 + (0.20 - 0.10) * k; c = (4 * (1 - k), 10 * (1 - k), 24 * (1 - k))
    return a + EXTRA, c
B = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
buf = bytearray()
prev = Image.new('RGB', (W, H))
pp = prev.load()
for y in range(H):
    a, c = overlay(y)
    for x in range(W):
        r, g, b = px[x, y]
        r = r * (1 - a) + c[0] * a; g = g * (1 - a) + c[1] * a; b = b * (1 - a) + c[2] * a
        d = (B[y & 3][x & 3] + 0.5) / 16.0
        R = min(31, int(r / 255 * 31 + d)); G = min(63, int(g / 255 * 63 + d)); Bl = min(31, int(b / 255 * 31 + d))
        v = (R << 11) | (G << 5) | Bl
        buf += struct.pack('<H', v)
        pp[x, y] = (R * 255 // 31, G * 255 // 63, Bl * 255 // 31)
assert len(buf) == W * H * 2 == 276480
open(out, 'wb').write(buf)
prev.save(out.rsplit('.', 1)[0] + '-preview.png')
print(out, len(buf))
