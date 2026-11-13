#!/usr/bin/env python3
"""Offline approximation of the device page with the new build: bg.bin (decoded RGB565) +
cards/rows at the exact geometry and alpha values used in screen.c. Not a device screenshot."""
import struct
from PIL import Image, ImageDraw, ImageFont
W, H = 320, 432
raw = open('bg.bin', 'rb').read()
base = Image.new('RGBA', (W, 480), (8, 20, 40, 255))
px = base.load()
for i in range(W * H):
    v = struct.unpack_from('<H', raw, 2 * i)[0]
    px[i % W, i // W] = ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)
F = '/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc'
def font(s): return ImageFont.truetype(F, s)
def rect(x, y, w, h, rgb, a, r, border=True):
    ov = Image.new('RGBA', base.size, (0, 0, 0, 0)); d = ImageDraw.Draw(ov)
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1], r, fill=rgb + (a,))
    if border: d.rounded_rectangle([x, y, x + w - 1, y + h - 1], r, outline=(200, 225, 255, 56), width=1)
    base.alpha_composite(ov)
def text(x, y, s, size, rgb):
    ImageDraw.Draw(base).text((x, y), s, font=font(size), fill=rgb + (255,))
text(16, 8, '代理', 22, (232, 241, 242))
text(16, 38, '运行中 · 透明代理开', 14, (154, 176, 204))
for i, (x, name) in enumerate([(12, '规则'), (112, '全局'), (212, '直连')]):
    sel = i == 0
    rect(x, 64, 96, 36, (47, 128, 237) if sel else (16, 36, 62), 105 if sel else 66, 10)
    text(x + 8 + 30, 64 + 7, name, 16, (255, 255, 255) if sel else (154, 176, 204))
text(16, 106, '东京 01', 16, (232, 241, 242))
rect(8, 136, 304, 240, (10, 28, 48), 31, 12)
names = ['香港 01', '香港 02', '东京 01', '新加坡 01', '美国 洛杉矶', '台湾 01', '韩国 首尔']
for i, n in enumerate(names):
    y = 136 + 6 + i * 36
    if y + 32 > 136 + 240: break
    sel = i == 2
    rect(8 + 6, y, 292, 32, (47, 128, 237) if sel else ((20, 48, 79) if i % 2 else (14, 32, 54)), 102 if sel else (56 if i % 2 else 38), 8, border=False)
    text(8 + 6 + 10, y + 4, n, 16, (255, 255, 255) if sel else (232, 241, 242))
rect(12, 384, 296, 40, (12, 110, 190), 87, 12)
text(20, 384 + 9, '透明代理    已开启', 16, (255, 255, 255))
for i in range(3):
    a = i == 2
    d = ImageDraw.Draw(base); x = 140 + 16 * i; y = 439 if a else 440; s = 8 if a else 6
    d.rounded_rectangle([x, y, x + s, y + s], s // 2, fill=(232, 241, 242) if a else (58, 76, 102))
base.convert('RGB').save('mock-device-proxy.png')
print('mock-device-proxy.png')
