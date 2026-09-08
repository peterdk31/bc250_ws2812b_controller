#!/usr/bin/env python3
"""Render the generated board to a PNG (out/preview.png) straight from
generate.py's geometry — outline, courtyards, pads, tracks on both layers,
the antenna keepout and the silkscreen legends — so a layout can be looked
at without KiCad.  Pillow only.  Not a substitute for the KiCad render, just
fast.

Usage:  python3 preview.py [out/preview.png] [px_per_mm]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

import generate as g

SCALE = float(sys.argv[2]) if len(sys.argv) > 2 else 14.0
MARGIN = 3.0


def main():
    d = g.design()
    bx0, by0, bx1, by1 = d['outline']
    w = int((bx1 - bx0 + 2 * MARGIN) * SCALE)
    h = int((by1 - by0 + 2 * MARGIN) * SCALE)
    img = Image.new('RGB', (w, h), (30, 30, 30))
    dr = ImageDraw.Draw(img, 'RGBA')

    def P(x, y):
        return ((x - bx0 + MARGIN) * SCALE, (y - by0 + MARGIN) * SCALE)

    def font(mm):
        try:
            return ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', max(6, int(mm * SCALE)))
        except OSError:
            return ImageFont.load_default()

    # board
    dr.rectangle([P(bx0, by0), P(bx1, by1)], fill=(20, 60, 30), outline=(240, 220, 60), width=2)

    # keepout
    kx0, ky0, kx1, ky1 = d['keepout']
    dr.rectangle([P(kx0, ky0), P(kx1, ky1)], outline=(220, 80, 220), width=1)

    # tracks: B.Cu first (blue), then F.Cu (red)
    col = {'B.Cu': (60, 110, 255, 200), 'F.Cu': (255, 70, 70, 200)}
    for layer in ('B.Cu', 'F.Cu'):
        for n, lay, width, pts in d['tracks']:
            if lay != layer:
                continue
            for a, b in zip(pts, pts[1:]):
                dr.line([P(*a), P(*b)], fill=col[layer], width=max(1, int(width * SCALE)))
                for x, y in (a, b):
                    r = width / 2 * SCALE
                    px, py = P(x, y)
                    dr.ellipse([px - r, py - r, px + r, py + r], fill=col[layer])

    # courtyards and pads per part
    for ref, (slib, sname, flib, fname, value, (x, y, rot)) in d['parts'].items():
        fp = g.load_part_footprint(ref)
        for c in fp:
            if not isinstance(c, list):
                continue
            lay = g.find(c, 'layer')
            if lay is None or lay[1] not in ('F.CrtYd', 'F.Fab', 'F.SilkS'):
                continue
            color = {'F.CrtYd': (255, 255, 255, 90), 'F.Fab': (200, 200, 120, 160), 'F.SilkS': (230, 230, 230, 220)}[lay[1]]
            if c[0] == 'fp_line':
                s, e = g.find(c, 'start'), g.find(c, 'end')
                a = g.rot_xy(float(s[1]), float(s[2]), rot)
                b = g.rot_xy(float(e[1]), float(e[2]), rot)
                dr.line([P(x + a[0], y + a[1]), P(x + b[0], y + b[1])], fill=color, width=1)
            elif c[0] == 'fp_rect':
                s, e = g.find(c, 'start'), g.find(c, 'end')
                pts = [(float(s[1]), float(s[2])), (float(e[1]), float(s[2])), (float(e[1]), float(e[2])),
                       (float(s[1]), float(e[2]))]
                pts = [g.rot_xy(px, py, rot) for px, py in pts]
                dr.polygon([P(x + px, y + py) for px, py in pts], outline=color)
            elif c[0] == 'fp_text' and lay[1] == 'F.SilkS' and c[1] == 'user':
                at = g.find(c, 'at')
                px, py = g.rot_xy(float(at[1]), float(at[2]), rot)
                size = float(g.find(g.find(g.find(c, 'effects'), 'font'), 'size')[1])
                just = g.find(g.find(c, 'effects'), 'justify')
                anchor = {'right': 'rm', 'left': 'lm'}.get(just[1] if just else None, 'mm')
                dr.text(P(x + px, y + py), c[2], fill=(230, 230, 230), font=font(size * 0.9), anchor=anchor)
        for num, px, py, sx, sy, drill in g.footprint_pads(fp):
            dx, dy = g.rot_xy(px, py, rot)
            cx, cy = P(x + dx, y + dy)
            a = abs(rot % 180) == 90
            hw, hh = (sy if a else sx) / 2 * SCALE, (sx if a else sy) / 2 * SCALE
            fill = (200, 200, 200) if num else (90, 90, 90)
            dr.rounded_rectangle([cx - hw, cy - hh, cx + hw, cy + hh], radius=min(hw, hh) * 0.6, fill=fill)
            if drill:
                r = drill / 2 * SCALE
                dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(30, 30, 30))
        dr.text(P(x, y - 0.1), ref, fill=(255, 255, 120), font=font(1.0), anchor='mm')
    for n, vx, vy in d.get('vias', []):
        cx, cy = P(vx, vy)
        r = g.VIA_D / 2 * SCALE
        dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(180, 120, 255))
        r = g.VIA_DRILL / 2 * SCALE
        dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(30, 30, 30))

    # silkscreen legends
    for text, x, y, rot, just, size, layer in g.SILK:
        if layer != 'F.SilkS':
            continue
        anchor = {'right': 'rm', 'left': 'lm'}.get(just, 'mm')
        dr.text(P(x, y), text, fill=(240, 240, 240), font=font(size * 0.9), anchor=anchor)

    # scale bar
    dr.line([P(bx0, by1 + 1.5), P(bx0 + 10, by1 + 1.5)], fill=(255, 255, 255), width=2)
    dr.text(P(bx0 + 11, by1 + 1.5), f'10 mm   board {bx1 - bx0:g} x {by1 - by0:g} mm', fill=(255, 255, 255),
            font=font(1.2), anchor='lm')

    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(g.HERE, 'out', 'preview.png')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()
