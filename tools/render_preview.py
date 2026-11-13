#!/usr/bin/env python3
"""Render scene dumps (tests/preview/*.json, written by tests/test_ui.c through the recording
fake LVGL) into PNG previews. Geometry, colours, opacity, text, hidden flags and z-order come
from the exact LVGL calls screen.c makes; fonts are approximated with Noto Sans CJK and
labels use LV_LABEL_LONG_DOT semantics (wrap, clip to height, '…' on the last line).
Offline approximation, not a device screenshot."""
import json, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'
W, H = 320, 480
_fc = {}
def font(sz):
    sz = sz or 14
    if sz not in _fc: _fc[sz] = ImageFont.truetype(FONT, sz)
    return _fc[sz]

def load_bg():
    raw = open(os.path.join(ROOT, 'bg.bin'), 'rb').read()
    im = Image.new('RGBA', (320, 432)); px = im.load()
    for i in range(320 * 432):
        v = struct.unpack_from('<H', raw, 2 * i)[0]
        px[i % 320, i // 320] = ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)
    return im

def wrap(s, f, w):
    lines = []
    for para in s.split('\n'):
        cur = ''
        for ch in para:
            if f.getlength(cur + ch) <= w or not cur: cur += ch
            else: lines.append(cur); cur = ch
        lines.append(cur)
    return lines

def render(scene, bg, out):
    objs = {o['i']: o for o in scene['objects']}
    kids = {}
    for o in scene['objects']: kids.setdefault(o['p'], []).append(o)
    img = Image.new('RGBA', (W, H), (0, 0, 0, 255))
    off = scene['page_x']
    def draw(o, ox, oy, clip):
        if o['hidden']: return
        x, y = ox + o['x'], oy + o['y']
        if o['p'] == 0: x -= off            # children of the page scroller: shift to the visible page
        box = (x, y, x + o['w'], y + o['h'])
        c = (max(box[0], clip[0]), max(box[1], clip[1]), min(box[2], clip[2]), min(box[3], clip[3]))
        if c[0] >= c[2] or c[1] >= c[3]: return
        layer = Image.new('RGBA', (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(layer)
        if o['img'] and bg is not None and scene.get('bg_on', 1):
            layer.paste(bg, (x, y))
        elif o['bg'] is not None and o['bg'][3] > 0:
            d.rounded_rectangle([x, y, box[2] - 1, box[3] - 1], o['r'], fill=tuple(o['bg']))
        if o['bw'] and o['bo']:
            d.rounded_rectangle([x, y, box[2] - 1, box[3] - 1], o['r'], outline=tuple(o['bc']) + (o['bo'],), width=o['bw'])
        if o['k'] == 1 and o['text']:
            f = font(o['font']); lh = int(f.size * 1.3)
            lines = wrap(o['text'], f, o['w'])
            maxl = max(1, o['h'] // lh)
            if len(lines) > maxl:
                lines = lines[:maxl]; last = lines[-1]
                while last and f.getlength(last + '…') > o['w']: last = last[:-1]
                lines[-1] = last + '…'
            for i, ln in enumerate(lines):
                d.text((x, y + i * lh), ln, font=f, fill=tuple(o['tc']) + (255,))
        mask = Image.new('L', (W, H), 0); ImageDraw.Draw(mask).rectangle([c[0], c[1], c[2] - 1, c[3] - 1], fill=255)
        clipped = Image.new('RGBA', (W, H), (0, 0, 0, 0)); clipped.paste(layer, (0, 0), mask)
        img.alpha_composite(clipped)
        for k in kids.get(o['i'], []): draw(k, x, y, c)
    for o in kids.get(-1, []): draw(o, 0, 0, (0, 0, W, H))
    img.convert('RGB').save(out)

def main():
    src = os.path.join(ROOT, 'tests', 'preview'); dst = os.path.join(ROOT, 'preview')
    os.makedirs(dst, exist_ok=True); bg = load_bg()
    for n in sorted(os.listdir(src)):
        if not n.endswith('.json'): continue
        scene = json.load(open(os.path.join(src, n)))
        out = os.path.join(dst, n[:-5] + '.png'); render(scene, bg, out); print(out)
if __name__ == '__main__': main()
