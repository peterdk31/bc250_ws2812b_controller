#!/usr/bin/env python3
"""Render an annotated pinout of the carrier (pinout.png, tracked in git so
GitHub shows it in the README): the board as
preview.py draws it, with every pad labelled by what it carries — rail
voltage, GPIO number and the role on this board — plus the connector tables
and the OpenPuck wake wiring in the margins.  Pillow only, straight from
generate.py's geometry, so it can never disagree with the board.

Usage:  python3 pinout.py [pinout.png] [px_per_mm]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

import generate as g

SCALE = float(sys.argv[2]) if len(sys.argv) > 2 else 26.0
ML, MR, MT, MB = 42.0, 3.0, 9.0, 30.0        # margins, mm: labels live left and below

FONT_DIR = '/usr/share/fonts/truetype/dejavu/'

# colours by what a pad carries
C_12V = (255, 96, 96)
C_5VSB = (255, 176, 64)
C_33 = (250, 232, 90)
C_GND = (215, 215, 215)
C_PSON = (255, 130, 255)
C_GPIO = (120, 225, 255)
C_DIN = (130, 255, 170)
C_DIM = (170, 170, 170)
C_TXT = (240, 240, 240)
BOARD = (18, 58, 30)
BG = (24, 24, 26)


def net_color(net):
    if net is None:
        return C_DIM
    if net == '12V':
        return C_12V
    if net == '5VSB':
        return C_5VSB
    if net == '3.3V':
        return C_33
    if net == 'GND':
        return C_GND
    if net in ('PS_ON#', 'GATE'):
        return C_PSON
    if net == 'DIN':
        return C_DIN
    return C_GPIO


def main():
    d = g.design()
    bx0, by0, bx1, by1 = d['outline']
    W = int((bx1 - bx0 + ML + MR) * SCALE)
    H = int((by1 - by0 + MT + MB) * SCALE)
    img = Image.new('RGB', (W, H), BG)
    dr = ImageDraw.Draw(img, 'RGBA')

    def P(x, y):
        return ((x - bx0 + ML) * SCALE, (y - by0 + MT) * SCALE)

    fonts = {}

    def font(mm, bold=False):
        key = (round(mm, 2), bold)
        if key not in fonts:
            name = 'DejaVuSans-Bold.ttf' if bold else 'DejaVuSans.ttf'
            try:
                fonts[key] = ImageFont.truetype(FONT_DIR + name, max(7, int(mm * SCALE)))
            except OSError:
                fonts[key] = ImageFont.load_default()
        return fonts[key]

    def text(x, y, s, mm=1.0, fill=C_TXT, anchor='mm', bold=False, stroke=True):
        """Board-mm coordinates; stroked so it stays legible over copper."""
        dr.text(P(x, y), s, fill=fill, font=font(mm, bold), anchor=anchor,
                stroke_width=int(0.12 * SCALE) if stroke else 0, stroke_fill=(0, 0, 0, 230))

    def vtext(x, y, s, mm=1.0, fill=C_TXT, top=True, bold=False):
        """Text rotated 90° (reads bottom-to-top), its top (or bottom) end at (x, y)."""
        f = font(mm, bold)
        w = int(f.getlength(s)) + int(0.5 * SCALE)
        h = int(mm * SCALE * 1.6)
        tmp = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(tmp).text((int(0.25 * SCALE), h // 2), s, fill=fill, font=f, anchor='lm',
                                 stroke_width=int(0.12 * SCALE), stroke_fill=(0, 0, 0, 230))
        tmp = tmp.rotate(90, expand=True)
        px, py = P(x, y)
        ox = int(px - tmp.width / 2)
        oy = int(py) if top else int(py - tmp.height)
        img.paste(tmp, (ox, oy), tmp)

    # ---- the board -------------------------------------------------------
    dr.rectangle([P(bx0, by0), P(bx1, by1)], fill=BOARD, outline=(240, 220, 60), width=3)
    kx0, ky0, kx1, ky1 = d['keepout']
    dr.rectangle([P(kx0, ky0), P(kx1, ky1)], outline=(220, 80, 220, 160), width=2)

    # tracks, muted so the labels win
    col = {'B.Cu': (60, 110, 255, 95), 'F.Cu': (255, 70, 70, 105)}
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

    # footprint outlines and pads
    for ref, (slib, sname, flib, fname, value, (x, y, rot)) in d['parts'].items():
        fp = g.load_part_footprint(ref)
        for c in fp:
            if not isinstance(c, list):
                continue
            lay = g.find(c, 'layer')
            if lay is None or lay[1] not in ('F.CrtYd', 'F.Fab', 'F.SilkS'):
                continue
            if ref in g.NO_STOCK_SILK and lay[1] == 'F.SilkS':
                continue                      # the board drops it and draws g.SILK_RECTS instead
            color = {'F.CrtYd': (255, 255, 255, 60), 'F.Fab': (200, 200, 120, 140),
                     'F.SilkS': (235, 235, 235, 230)}[lay[1]]
            if c[0] == 'fp_line':
                s, e = g.find(c, 'start'), g.find(c, 'end')
                a = g.rot_xy(float(s[1]), float(s[2]), rot)
                b = g.rot_xy(float(e[1]), float(e[2]), rot)
                dr.line([P(x + a[0], y + a[1]), P(x + b[0], y + b[1])], fill=color, width=2)
            elif c[0] == 'fp_rect':
                s, e = g.find(c, 'start'), g.find(c, 'end')
                pts = [(float(s[1]), float(s[2])), (float(e[1]), float(s[2])), (float(e[1]), float(e[2])),
                       (float(s[1]), float(e[2]))]
                pts = [g.rot_xy(px, py, rot) for px, py in pts]
                dr.polygon([P(x + px, y + py) for px, py in pts], outline=color)
        for num, px, py, sx, sy, drill in g.footprint_pads(fp):
            dx, dy = g.rot_xy(px, py, rot)
            cx, cy = P(x + dx, y + dy)
            a = abs(rot % 180) == 90
            hw, hh = (sy if a else sx) / 2 * SCALE, (sx if a else sy) / 2 * SCALE
            net = g.NETS.get((ref, num))
            fill = (205, 205, 205) if num else (90, 90, 90)
            dr.rounded_rectangle([cx - hw, cy - hh, cx + hw, cy + hh], radius=min(hw, hh) * 0.6, fill=fill,
                                 outline=net_color(net) if num else None, width=2)
            if drill:
                r = drill / 2 * SCALE
                dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(30, 30, 30))
    for n, vx, vy in d.get('vias', []):
        cx, cy = P(vx, vy)
        r = g.VIA_D / 2 * SCALE
        dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(180, 120, 255))
        r = g.VIA_DRILL / 2 * SCALE
        dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(30, 30, 30))

    # mounting holes
    for k, (hx, hy) in enumerate(g.HOLES):
        text(hx, hy, f'H{k + 1}', 0.9, C_DIM, stroke=False)

    # ---- labels ----------------------------------------------------------
    pads = d['pads']

    def pad(ref, num):
        return pads[(ref, str(num))][:2]

    # U1: a dark wash over the module body so its pin labels read, short pin
    # names beside the pads, the full roles in the table below the board
    sml, smr = pad('J12', 1)[0], pad('J13', 1)[0]
    top, bot = g.SM_CY - g.SM_L / 2, g.SM_BOT
    dr.rectangle([P(sml + 1.3, top), P(smr - 1.3, bot)], fill=(0, 0, 0, 120))
    short_left = {'GPIO5': 'IO5  FAN1', 'GPIO6': 'IO6  FAN2', 'GPIO7': 'IO7  FAN3', 'GPIO8': 'IO8  J11',
                  'GPIO9': 'IO9  J11', 'GPIO10': 'IO10 FAN4', 'GPIO20': 'IO20 WAKE', 'GPIO21': 'IO21 J11'}
    short_right = {'5V': '5VSB', 'GND': 'GND', '3V3': 'n/c 3V3', 'GPIO4': 'DIN  IO4', 'GPIO3': 'GATE IO3',
                   'GPIO2': 'SNS  IO2', 'GPIO1': 'BTN  IO1', 'GPIO0': 'J11  IO0'}
    for i, nm in enumerate(g.SM_LEFT):
        x, y = pad('J12', i + 1)
        text(x + 1.5, y, short_left[nm], 0.72, net_color(g.NETS.get(('J12', str(i + 1)))), 'lm', True)
    for i, nm in enumerate(g.SM_RIGHT):
        x, y = pad('J13', i + 1)
        text(x - 1.5, y, short_right[nm], 0.72, net_color(g.NETS.get(('J13', str(i + 1)))), 'rm', True)
    text(g.SM_CX, (top + bot) / 2, 'U1', 1.4, C_TXT, 'mm', True)
    text(g.SM_CX, top - 0.9, 'U1 Super Mini plugs into J12 | J13 · pins down · USB-C ↑', 0.68, C_TXT)
    text(g.SM_CX, bot - 1.0, 'antenna end ↓  keepout', 0.65, (225, 130, 225))

    # the module's roles, tabulated under the board's left end
    ty = by1 + 1.4
    text(bx0 - ML + 1.0, ty, 'U1 left column, top → bottom', 0.85, C_TXT, 'lm', True, stroke=False)
    text(bx0 - ML + 23.5, ty, 'U1 right column, top → bottom', 0.85, C_TXT, 'lm', True, stroke=False)
    role_left = ['GPIO5 → FAN1 PWM (J5)', 'GPIO6 → FAN2 PWM (J6)', 'GPIO7 → FAN3 PWM (J7)',
                 'GPIO8 → J11 p8 · blue LED · strap', 'GPIO9 → J11 p10 · BOOT strap',
                 'GPIO10 → FAN4 PWM (J8)', 'GPIO20 → J11 p12 · wake (OpenPuck)', 'GPIO21 → J11 p14 · U0TXD']
    role_right = ['5V ← 5VSB (standby, always on)', 'GND', '3V3: module output, unused',
                  'GPIO4 → DIN, strip data (J3)', 'GPIO3 → GATE of Q1 → PS_ON#',
                  'GPIO2 ← SENSE, TPMS1 pin 9 (J10)', 'GPIO1 ← button NO (J9 pin 3)', 'GPIO0 → J11 p16']
    for i in range(8):
        cl = net_color(g.NETS.get(('J12', str(i + 1))))
        cr = net_color(g.NETS.get(('J13', str(i + 1))))
        text(bx0 - ML + 1.0, ty + 1.6 + i * 1.45, role_left[i], 0.8, cl, 'lm', stroke=False)
        text(bx0 - ML + 23.5, ty + 1.6 + i * 1.45, role_right[i], 0.8, cr, 'lm', stroke=False)

    # J11: the optional breakout — one line per row in the left margin
    text(bx0 - 1.0, g.J11Y[0] - 2.4, 'J11  optional 2×8 breakout  ·  odd = outer column, even = inner', 0.9,
         C_TXT, 'rm', True)
    text(bx0 - 1.0, g.J11Y[0] - 1.3, 'pin 1 = top outer (square pad); pin names are printed on the BACK', 0.7,
         C_DIM, 'rm')
    for k, inner in enumerate(g.J11_INNER):
        y = g.J11Y[k]
        outer = g.NETS[('J11', str(2 * k + 1))]
        s_in = f'{2 * k + 2}: {inner}'
        s_out = f'{2 * k + 1}: {outer}'
        f = font(0.95)
        text(bx0 - 1.0, y, s_in, 0.95, net_color(inner), 'rm')
        text(bx0 - 1.0 - f.getlength(s_in) / SCALE - 2.0, y, s_out, 0.95, net_color(outer), 'rm')
        dr.line([P(bx0 - 0.6, y), P(g.J11_X0 - 0.9, y)], fill=(120, 120, 120, 160), width=1)
    oy = g.J11Y[7] + 2.7
    text(bx0 - 1.0, oy, 'OpenPuck:  017 → pin 12 (GPIO20)', 0.9, C_GPIO, 'rm', True)
    text(bx0 - 1.0, oy + 1.5, 'GND → pin 11 (GND)', 0.9, C_GND, 'rm', True)
    text(bx0 - 1.0, oy + 3.0, 'BAT → pin 3 or 4 (5VSB)', 0.9, C_5VSB, 'rm', True)
    text(bx0 - 1.0, oy + 4.4, "the puck's USB cable stays intact", 0.75, C_DIM, 'rm')

    # J1: the PSU header — front row labelled above (the plug zone is tracks
    # only), rear row below the board edge
    for pin in range(1, 6):
        x, y = pad('J1', pin)
        n = g.MF_NET[pin]
        text(x, y - 2.45, n, 0.85, net_color(n), 'mm', True)
        text(x, y - 3.6, f'pin {pin}' + (' ■' if pin == 1 else ''), 0.65, C_DIM)
    for pin in range(6, 11):
        x, y = pad('J1', pin)
        n = g.MF_NET[pin]
        vtext(x, by1 + 0.6, f'{pin}: {n}', 0.9, net_color(n), top=True, bold=True)
    cx = (g.mf_xy(1)[0] + g.mf_xy(5)[0]) / 2
    text(cx, g.MF_YF - 6.3, 'J1  PSU in — FSP500-30AS 10-pin Mini-Fit Jr', 0.8, C_TXT, 'mm', True)
    text(cx, g.MF_YF - 5.3, 'right-angle: the plug lies flat over the zone above', 0.68, C_DIM)
    text(cx, g.MF_YF - 4.5, 'front row 1–5 here · rear row 6–10 at the edge = LATCH side', 0.68, C_DIM)

    # the board's own silk rectangles: the fan headers' bodies and 3-pin ramps, the module ghost
    for x0, y0, x1, y1, layer in g.SILK_RECTS:
        dr.rectangle([P(x0, y0), P(x1, y1)], outline=(235, 235, 235, 230) if layer == 'F.SilkS' else (200, 200, 120, 140),
                     width=2)

    # J3: strip
    for pin, lab in ((1, '3.3V'), (2, 'DIN (GPIO4)'), (3, 'GND')):
        x, y = pad('J3', pin)
        text(x, y - 2.75, lab, 0.72, net_color(g.NETS[('J3', str(pin))]), 'mm', True)
    text(g.VH_X[1], g.VH_Y - 4.15, 'J3  STRIP  JST-VH · pin 1 ▷ = 3.3V · 3 A', 0.72, C_TXT, 'mm', True)

    # fans: pin roles above each header, the header's name and GPIO below it
    for k in range(4):
        gp = g.FAN_ORDER[k]
        for pin, lab in ((4, 'PWM'), (3, 'T'), (2, '12V'), (1, 'G')):
            x, y = g.fan_pin(k, pin)
            n = g.NETS.get((f'J{5 + k}', str(pin)))
            text(x, y - 3.55, lab, 0.75, net_color(n) if n else C_DIM, 'mm', True)
        x1, y = g.FAN_POS[k]
        text(x1 - 1.5 * g.FAN_P, y + 3.9, f'J{5 + k} FAN{k + 1} · {gp} · header{k + 1}', 0.68, C_GPIO, 'mm')

    # J9 button and J10 sense: below the board edge
    for pin, lab in ((1, '1 ▷ 12V  ring LED +'), (2, '2: GND  ring LED −'), (3, '3: NO → GPIO1'), (4, '4: C = GND')):
        x, y = pad('J9', pin)
        vtext(x, by1 + 0.6, lab, 0.85, net_color(g.NETS.get(('J9', str(pin)))), top=True, bold=True)
    x, y = pad('J10', 1)
    vtext(x, by1 + 0.6, 'SENSE ← TPMS1 pin 9', 0.85, C_GPIO, top=True, bold=True)
    cy = by1 + 13.4   # group captions under the vertical pad labels
    text(pad('J9', 2)[0] + 1.25, cy, 'J9 BUTTON', 0.8, C_TXT, 'mm', True, stroke=False)
    text(x, cy, 'J10 SENSE → GPIO2', 0.8, C_TXT, 'mm', True, stroke=False)
    text((g.mf_xy(6)[0] + g.mf_xy(10)[0]) / 2, cy, 'J1 rear row · at the board edge · latch side', 0.8, C_TXT,
         'mm', True, stroke=False)
    text(g.Q1_G, cy, 'Q1 2N7000', 0.8, C_TXT, 'mm', True, stroke=False)

    # Q1 / R1
    for pin, lab in ((3, 'S: GND'), (2, 'G ← GPIO3'), (1, 'D: PS_ON#')):
        x, y = pad('Q1', pin)
        vtext(x, by1 + 0.6, lab, 0.8, net_color(g.NETS.get(('Q1', str(pin)))), top=True, bold=True)
    text((g.R1_P1[0] + g.R1_P2[0]) / 2, g.R1_Y - 1.9, 'R1 100 kΩ', 0.72, C_PSON)
    text(g.Q1_G, g.Q1_Y - 2.4, 'Q1', 0.72, C_PSON)

    # ---- title, legend, notes ----------------------------------------------
    text(bx0, by0 - 6.2, 'BC-250 carrier  rev C  —  pinout, top view', 2.2, C_TXT, 'lm', True, stroke=False)
    text(bx0, by0 - 3.2, f'{bx1 - bx0:g} × {by1 - by0:g} mm.  ▷ / ■ on the silkscreen = pin 1.  '
         'Red = front copper, blue = back copper.', 1.0, C_DIM, 'lm', stroke=False)

    lx, ly = bx0 + 9.0, by1 + 16.4
    text(lx, ly, 'colours:', 0.95, C_TXT, 'lm', True, stroke=False)
    off = 7.5
    for n, c in (('12V', C_12V), ('5VSB', C_5VSB), ('3.3V', C_33), ('GND', C_GND), ('PS_ON# / gate', C_PSON),
                 ('strip DIN', C_DIN), ('GPIO / signal', C_GPIO)):
        text(lx + off, ly, n, 0.95, c, 'lm', True, stroke=False)
        off += font(0.95, True).getlength(n) / SCALE + 3.0
    dr.line([P(bx1 - 10, ly), P(bx1, ly)], fill=(255, 255, 255), width=3)
    text(bx1 - 5, ly + 1.3, '10 mm', 0.8, C_TXT, 'mm', stroke=False)

    notes = [
        'Rails: 12V and 3.3V are PSU main rails, dead while the machine is off. 5VSB is standby and feeds the '
        'module (and an OpenPuck on J11) at all times.',
        "The C3 module's USB VBUS is tied to its 5V pin, so cut the red wire in the MODULE's USB cable (README, "
        "Power switch). The puck's cable stays whole.",
        'Q1: flat face per the silkscreen outline, pins D G S left to right; R1 is its 100 kΩ gate pull-down.',
        'Fan headers J5–J8: lock ramp (over G · 12V · T) toward the TOP edge, pin 1 (G) is the RIGHT pin, tach unconnected.',
        'config.json  power_switch.pins: ps_on 3 · button 1 · button_gnd null · sense 2 · led 8 · wake 20 (null '
        'without a puck).  fans.header1..4 = FAN1..4.  strip.pin 4.',
    ]
    for i, s in enumerate(notes):
        text(lx, ly + 3.0 + i * 1.7, s, 0.85, C_DIM, 'lm', stroke=False)

    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(g.HERE, 'pinout.png')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print('wrote', out, f'{W}x{H}')


if __name__ == '__main__':
    main()
