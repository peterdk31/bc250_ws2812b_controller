#!/usr/bin/env python3
"""Generate the BC-250 carrier board as a KiCad 7 project.

Everything about the board — parts, nets, placement, every track — lives in
this file, and running it rewrites bc250_carrier.kicad_sch / .kicad_pcb plus
the project-local symbol and footprint libraries. Stock KiCad symbols and
footprints are copied in from the system install (KICAD_SHARE), so the output
opens on any KiCad 7 without extra libraries.

Coordinates are millimetres, KiCad convention: y grows downwards on the board.
Run check.py afterwards — KiCad 7's command line has no DRC, so that script is
the clearance and connectivity check.

Usage:  python3 generate.py            (writes into this directory)
"""
import copy
import json
import os
import re
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
KICAD_SHARE = os.environ.get('KICAD_SHARE', '/usr/share/kicad')
PROJECT = 'bc250_carrier'
ROOT_UUID = '0f8c9b7e-4d21-4e3a-9c5a-bc250c0de001'


def U():
    return str(uuid.uuid4())


# ---------------------------------------------------------------- s-expr I/O

class Sym(str):
    """A bare (unquoted) atom."""


class Quoted(str):
    """A string that must be written quoted, even through S()."""


def parse(text):
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|\(|\)|[^\s()"]+', text)
    pos = 0

    def read():
        nonlocal pos
        tok = tokens[pos]
        pos += 1
        if tok == '(':
            lst = []
            while tokens[pos] != ')':
                lst.append(read())
            pos += 1
            return lst
        if tok.startswith('"'):
            return tok[1:-1].replace('\\"', '"').replace('\\\\', '\\')
        return Sym(tok)

    return read()


def q(s):
    return '"' + str(s).replace('\\', '\\\\').replace('"', '\\"') + '"'


def dump(node, indent=0):
    if isinstance(node, list):
        flat = all(not isinstance(c, list) for c in node)
        if flat:
            return '(' + ' '.join(dump(c) for c in node) + ')'
        parts = []
        head = []
        rest = []
        for c in node:
            if not rest and not isinstance(c, list):
                head.append(dump(c))
            else:
                rest.append(c)
        pad = '  ' * (indent + 1)
        body = '\n'.join(pad + dump(c, indent + 1) for c in rest)
        return '(' + ' '.join(head) + '\n' + body + '\n' + '  ' * indent + ')'
    if isinstance(node, Sym):
        return str(node)
    if isinstance(node, str):
        return q(node)
    if isinstance(node, float):
        return fnum(node)
    return str(node)


def S(*items):
    """Build a node: bare atoms for str, lists pass through."""
    out = []
    for it in items:
        if isinstance(it, str) and not isinstance(it, (Sym, Quoted)):
            out.append(Sym(it))
        else:
            out.append(it)
    return out


def Q(s):
    """A quoted string leaf."""
    return Quoted(s)


def find(node, key):
    for c in node:
        if isinstance(c, list) and c and c[0] == key:
            return c
    return None


def findall(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def fnum(v):
    return ('%.4f' % v).rstrip('0').rstrip('.') if isinstance(v, float) else str(v)


# ----------------------------------------------------------------- libraries

_lib_cache = {}


def load_lib_symbol(lib, name):
    path = os.path.join(KICAD_SHARE, 'symbols', lib + '.kicad_sym')
    if path not in _lib_cache:
        with open(path) as f:
            _lib_cache[path] = parse(f.read())
    for node in _lib_cache[path]:
        if isinstance(node, list) and node[0] == 'symbol' and node[1] == name:
            assert find(node, 'extends') is None, name
            return node
    raise KeyError(f'{lib}:{name}')


def load_footprint(lib, name):
    path = os.path.join(KICAD_SHARE, 'footprints', lib + '.pretty', name + '.kicad_mod')
    with open(path) as f:
        return parse(f.read())


def symbol_pins(sym):
    """[(number, x, y, angle)] in symbol units (y up)."""
    pins = []
    for unit in [sym] + findall(sym, 'symbol'):
        for p in findall(unit, 'pin'):
            at = find(p, 'at')
            num = find(p, 'number')[1]
            pins.append((num, float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0))
    return pins


# ------------------------------------------------------- the Super Mini parts

SM_ROW = 15.24          # pin row spacing — verify against the real module
SM_PITCH = 2.54
SM_W, SM_L = 18.0, 22.52
# Pin names by column, top to bottom, for the module lying COMPONENT SIDE UP
# with its USB-C at the top (the labels printed on the module's underside read
# mirrored to this). Swap the two lists if a module differs; the nets follow
# the names, but the tracks are laid out for this arrangement.
SM_LEFT = ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO8', 'GPIO9', 'GPIO10', 'GPIO20', 'GPIO21']
SM_RIGHT = ['5V', 'GND', '3V3', 'GPIO4', 'GPIO3', 'GPIO2', 'GPIO1', 'GPIO0']


def supermini_symbol():
    """Schematic symbol: box with 8 pins a side, pin 1 = 5V top-left."""
    name = 'ESP32-C3_SuperMini'
    body_w, body_h = 15.24, 12.7
    sym = S('symbol', Q(name), S('pin_names', S('offset', 1.016)), S('in_bom', 'yes'), S('on_board', 'yes'))
    sym += [
        S('property', Q('Reference'), Q('U'), S('at', 0, 14.605, 0), S('effects', S('font', S('size', 1.27, 1.27)))),
        S('property', Q('Value'), Q(name), S('at', 0, -14.605, 0), S('effects', S('font', S('size', 1.27, 1.27)))),
        S('property', Q('Footprint'), Q(f'{PROJECT}:{name}'), S('at', 0, 0, 0),
          S('effects', S('font', S('size', 1.27, 1.27)), 'hide')),
        S('property', Q('Datasheet'), Q('~'), S('at', 0, 0, 0), S('effects', S('font', S('size', 1.27, 1.27)), 'hide')),
        S('property', Q('ki_description'), Q('ESP32-C3 Super Mini dev board, soldered flat'), S('at', 0, 0, 0),
          S('effects', S('font', S('size', 1.27, 1.27)), 'hide')),
    ]
    g = S('symbol', Q(name + '_0_1'),
          S('rectangle', S('start', -body_w, body_h), S('end', body_w, -body_h),
            S('stroke', S('width', 0.254), S('type', 'default')), S('fill', S('type', 'background'))),
          S('text', Q('USB-C'), S('at', 0, 10.16, 0), S('effects', S('font', S('size', 1.27, 1.27)))),
          S('text', Q('antenna'), S('at', 0, -10.16, 0), S('effects', S('font', S('size', 1.27, 1.27)))))
    pins = S('symbol', Q(name + '_1_1'))
    def kind(nm):
        return 'passive' if nm in ('5V', 'GND') else 'power_out' if nm == '3V3' else 'bidirectional'
    for i, nm in enumerate(SM_LEFT):
        y = 8.89 - 2.54 * i
        pins.append(S('pin', kind(nm), 'line', S('at', -body_w - 2.54, y, 0), S('length', 2.54),
                      S('name', Q(nm), S('effects', S('font', S('size', 1.27, 1.27)))),
                      S('number', Q(str(i + 1)), S('effects', S('font', S('size', 1.27, 1.27))))))
    for i, nm in enumerate(SM_RIGHT):
        y = 8.89 - 2.54 * i
        pins.append(S('pin', kind(nm), 'line', S('at', body_w + 2.54, y, 180), S('length', 2.54),
                      S('name', Q(nm), S('effects', S('font', S('size', 1.27, 1.27)))),
                      S('number', Q(str(i + 9)), S('effects', S('font', S('size', 1.27, 1.27))))))
    sym += [g, pins]
    return sym


def supermini_footprint():
    """2x8 through-hole footprint, origin at the module centre, USB-C end up
    (-y). Pin names on the silkscreen sit outside the module outline so they
    stay readable after it is soldered on."""
    name = 'ESP32-C3_SuperMini'
    fp = S('footprint', Q(name), S('version', 20221018), S('generator', 'generate.py'), S('layer', Q('F.Cu')),
           S('descr', Q('ESP32-C3 Super Mini dev board, 2x8 2.54 mm through-hole, soldered flat; USB-C at the top edge')),
           S('tags', Q('ESP32-C3 SuperMini')), S('attr', 'through_hole'))
    fp.append(S('fp_text', 'reference', Q('REF**'), S('at', -6.5, -SM_L / 2 - 0.9), S('layer', Q('F.SilkS')),
                S('effects', S('font', S('size', 1, 1), S('thickness', 0.15)))))
    fp.append(S('fp_text', 'value', Q(name), S('at', 0, SM_L / 2 + 1.5), S('layer', Q('F.Fab')),
                S('effects', S('font', S('size', 1, 1), S('thickness', 0.15)))))
    hw, hl = SM_W / 2, SM_L / 2
    for layer, off in (('F.SilkS', 0.15), ('F.Fab', 0.0)):
        w = 0.12 if layer == 'F.SilkS' else 0.1
        fp.append(S('fp_rect', S('start', -hw - off, -hl - off), S('end', hw + off, hl + off),
                    S('stroke', S('width', w), S('type', 'default')), S('fill', 'none'), S('layer', Q(layer))))
    fp.append(S('fp_rect', S('start', -hw - 0.5, -hl - 0.5), S('end', hw + 0.5, hl + 0.5),
                S('stroke', S('width', 0.05), S('type', 'default')), S('fill', 'none'), S('layer', Q('F.CrtYd'))))
    # USB-C marker and the antenna zone, on the fab layer and the silkscreen
    fp.append(S('fp_rect', S('start', -4.5, -hl - 1.2), S('end', 4.5, -hl + 1.5),
                S('stroke', S('width', 0.1), S('type', 'default')), S('fill', 'none'), S('layer', Q('F.Fab'))))
    fp.append(S('fp_text', 'user', Q('USB-C'), S('at', 0, -hl + 3.2), S('layer', Q('F.Fab')),
                S('effects', S('font', S('size', 0.8, 0.8), S('thickness', 0.12)))))
    fp.append(S('fp_text', 'user', Q('antenna'), S('at', 0, hl - 2.2), S('layer', Q('F.Fab')),
                S('effects', S('font', S('size', 0.8, 0.8), S('thickness', 0.12)))))
    fp.append(S('fp_text', 'user', Q('USB'), S('at', 0, -hl - 0.9), S('layer', Q('F.SilkS')),
                S('effects', S('font', S('size', 0.8, 0.8), S('thickness', 0.12)))))
    y0 = -SM_PITCH * 3.5
    for i, nm in enumerate(SM_LEFT):
        y = y0 + SM_PITCH * i
        shape = 'rect' if i == 0 else 'circle'
        fp.append(S('pad', Q(str(i + 1)), 'thru_hole', shape, S('at', -SM_ROW / 2, y), S('size', 1.7, 1.7),
                    S('drill', 1.0), S('layers', Q('*.Cu'), Q('*.Mask'))))
        # no silk names on this side: the optional headers J11/J12 sit in the strip
        # beside these pads (the right column's names orient the module)
    for i, nm in enumerate(SM_RIGHT):
        y = y0 + SM_PITCH * i
        fp.append(S('pad', Q(str(i + 9)), 'thru_hole', 'circle', S('at', SM_ROW / 2, y), S('size', 1.7, 1.7),
                    S('drill', 1.0), S('layers', Q('*.Cu'), Q('*.Mask'))))
        fp.append(S('fp_text', 'user', Q(nm), S('at', SM_ROW / 2 + SM_LABEL_DX, y), S('layer', Q('F.SilkS')),
                    S('effects', S('font', S('size', SM_LABEL_SIZE, SM_LABEL_SIZE), S('thickness', 0.1)),
                      S('justify', 'left'))))
    return fp


# ------------------------------------------------------------------ the design
#
# Rev C layout, three columns on an 80 x 34.5 mm board (y grows downwards):
#
#   left    the Super Mini (USB-C at the top edge), the button and sense
#           XH connectors under its antenna end
#   middle  the PSU header: a RIGHT-ANGLE Mini-Fit Jr on the bottom edge
#           with its mating face pointing UP the board, so the PSU plug lies
#           flat over the board and its wires leave over the top edge.  The
#           whole column above the header is the "plug zone": tracks only,
#           nothing taller than the soldermask, because the plug body sits
#           ~1.3 mm above the board and the wires drape over the rest.
#   right   the strip VH, the four fan headers as a 2 x 2 block, Q1 and R1
#
# Board outline
BX0, BY0 = 100.0, 100.0
BX1, BY1 = 180.0, 134.5

# --- Super Mini: left column.  Module top edge 3.9 mm inside the board edge
# (the USB-C plug overhangs the board, that is fine; the mounting hole H1
# needs the corner).  1.5 mm right of centre so the optional GPIO header
# J11 fits between the left edge and the module's left pads.  Column names come from SM_LEFT / SM_RIGHT above (5V
# column on the RIGHT with the module component-side up, USB-C at the top).
SM_CX = BX0 + 16.3
SM_CY = BY0 + 3.9 + SM_L / 2
SM_Y0 = SM_CY - SM_PITCH * 3.5          # y of the first pin row
SML = SM_CX - SM_ROW / 2                # left pin column x  (GPIO5..GPIO21)
SMR = SM_CX + SM_ROW / 2                # right pin column x (5V..GPIO0)
SM_BOT = SM_CY + SM_L / 2               # module bottom edge (antenna end)
SM_LABEL_DX, SM_LABEL_SIZE = 1.6, 0.7   # stops short of J1's outline  # pin-name silk (right column): offset from the pad, text size


def sm_y(i):
    return SM_Y0 + SM_PITCH * i


# --- PSU header: Molex Mini-Fit Jr 5569-10A2 (2x5, right angle, snap-in
# pegs); the FSP500-30AS's own 10-pin plug mates with it.  Footprint at
# rotation 0: pins 1-5 in the FRONT row (nearest the body, y = MF_YF), pins
# 6-10 in the REAR row 5.5 mm behind it at the board's bottom edge, pin k+5
# behind pin k, pin 1/6 at the left (MF_X0).  The body extends 13.9 mm in
# front of the front row; the mating face points up the board (-y).
#
# Pin map from the FSP500-30AS pinout drawing (fsp500-30as.webp: "looking
# into the front face of the connector", latch up): latch-side row
# 3.3V GND PS_ON GND GND, other row 3.3V GND 5VSB 12V 12V.  In a right-angle
# Mini-Fit Jr the rear pin row feeds the upper contact row, and the latch
# ramp is on top (away from the board), so pins 6-10 are the latch row —
# the same rows the vertical 5566 footprint has (its ramp is drawn on the
# 6-10 row).  Looking into the header's face from the top edge of the board,
# +x is on the viewer's LEFT, so the header face reads 5 4 3 2 1 over
# 10 9 8 7 6; the plug face is its mirror image: 1 2 3 4 5 (lower row) and
# 6 7 8 9 10 (latch row), left to right, which lines up with the drawing:
MF_X0, MF_P = 131.9, 4.2
MF_YR = BY1 - 2.5                       # rear row (pins 6-10), pad edge 0.65 mm from the board edge
MF_YF = MF_YR - 5.5                     # front row (pins 1-5)
MF_NET = {
    1: '3.3V', 2: 'GND', 3: '5VSB', 4: '12V', 5: '12V',         # front row, left to right
    6: '3.3V', 7: 'GND', 8: 'PS_ON#', 9: 'GND', 10: 'GND',      # rear row (latch side), left to right
}
MF_FACE = MF_YF - 13.9                  # mating face; the plug body reaches ~6 mm further up
MF_GAP_Y = (MF_YF + MF_YR) / 2          # between the rows: 1.8 mm of board between the pad edges


def mf_xy(pin):
    return (MF_X0 + MF_P * ((pin - 1) % 5), MF_YF if pin <= 5 else MF_YR)


# --- Right column.  Strip output: JST-VH 3-pin (10 A contacts) at the top,
# pins along x: 3.3V, DIN, GND.
COLC_X = 153.4                          # right column starts here (header courtyard ends at 151.9)
VH_X0, VH_Y, VH_P = COLC_X + 2.75, 105.3, 3.96
VH_X = [VH_X0 + VH_P * k for k in range(3)]

# Fan headers, 2 x 2, rotated 180 so pin 4 (PWM) is the LEFT pin of each
# header and pin 1 (GND) the right one: PWM comes from the module on the
# left, 12 V and GND from the PSU header below-right.  FAN1 FAN2 in the top
# row, FAN3 FAN4 below; left to right, top to bottom = GPIO5, 6, 7, 10 =
# the firmware's FAN_PINS order.  The part is a KF2510 4-pin straight header
# (Ckmtw W-2510S04P): pads as the KiCad KK-254 footprint, but its body is
# 12.7 x 5.8 mm, the same as a PC fan plug, so the columns are 13.2 mm apart
# and the rows 9.6 (3.8 mm between bodies for the plugs' latches).  The
# stock footprint's 10.2 mm silk outline is dropped and the real body drawn.
FAN_P = 2.54
FAN_BODY_L, FAN_BODY_D = 12.7, 5.8
FAN_X1 = [COLC_X + FAN_BODY_L / 2 + 3.81, COLC_X + FAN_BODY_L * 1.5 + 0.5 + 3.81]   # pin 1 (GND) x of the two columns
FAN_Y = [114.4, 124.0]                          # pin y of the two rows
FAN_ORDER = ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO10']
FAN_POS = [(FAN_X1[k % 2], FAN_Y[k // 2]) for k in range(4)]   # (pin 1 x, y) for FAN1..FAN4


def fan_pin(k, pin):
    """Board (x, y) of pin `pin` of FANk+1 (rotated 180: pin n at x1 - (n-1)p)."""
    x1, y = FAN_POS[k]
    return (x1 - FAN_P * (pin - 1), y)


# Q1 (TO-92 inline, rotated 180: D G S left to right) and R1 (axial, 7.62 mm
# pitch, pin 1 = GATE, pin 2 = GND) in the bottom row of the right column,
# beside the header's rear pins.
Q1_Y = 131.5
Q1_S = 157.54
Q1_G, Q1_D = Q1_S - 1.27, Q1_S - 2.54
R1_X, R1_Y, R1_P = 160.2, 131.5, 7.62
R1_P1, R1_P2 = (R1_X, R1_Y), (R1_X + R1_P, R1_Y)

# Button and sense under the module, pins along x.  J9 (button) is a 4-pin
# JST-XH: 12V and GND for the button's ring LED, then the switch's NO and C
# (the firmware wants a contact that CLOSES on press; the NC terminal makes
# the PSU click, see the README).  J10 (sense) is a single 2.54 mm pin.
XH_Y, XH_P = 130.0, 2.5
J9_X0 = 109.2                            # housing edge 0.75 mm clear of a 5.6 mm screw head on H3
J9_X = [J9_X0 + XH_P * k for k in range(4)]      # 12V, GND, NO (BTN), C (GND)
J10_X = 121.6
# Optional breakout header J11 along the left edge, in the 7.8 mm strip
# between the board edge and the module's left pads: a 2x8 pin header.  The
# INNER column (even pins) is, top to bottom, the three rails 3.3V, 5VSB,
# 12V and then the module's five UNUSED GPIOs: GPIO8, GPIO9, GPIO20, GPIO21
# (left column, rows 3, 4, 6, 7 — 20 and 21 step up one row so GPIO0 can
# take the bottom row) and GPIO0 (right column, brought round under the
# module's end).  The OUTER column (odd pins) doubles each rail on the top
# three rows (two pins of 3.3V, two of 5VSB, two of 12V) and is GND beside
# each GPIO.  The rows sit half a pitch below the module's rows so the
# header's plastic clears the corner mounting screw's head; the GPIO stubs
# take a short diagonal.
J11_X0, J11_X1 = 102.4, 104.94          # outer (GND) / inner column x
J11_DY = 1.27                           # header rows = module rows + this
J11_INNER = ['3.3V', '5VSB', '12V', 'GPIO8', 'GPIO9', 'GPIO20', 'GPIO21', 'GPIO0']
J11_GPIO = J11_INNER[3:]
J11Y = [sm_y(k) + J11_DY for k in range(8)]

# footprint reference text moved off the stock spot: ref -> (x, y) relative
REF_POS = {f'J{5 + k}': (3.81, 4.0) for k in range(4)}
REF_POS['Q1'] = (5.0, 0)                 # left of the TO-92 (rotated 180: -x is +x on the board)
REF_POS['R1'] = (3.81, 2.4)
REF_POS['J1'] = (-2.4, 2.75)
REF_POS['J3'] = (3.96, -3.0)
REF_POS['J9'] = (3.75, 3.8)
# named by their legends instead (the stock spots collide with them); the holes need no name
HIDE_REF = {'J9', 'J10', 'J11', 'J5', 'J6', 'J7', 'J8', 'H1', 'H2', 'H3', 'H4'}
REF_POS['J10'] = (0, -2.6)
REF_POS['J11'] = (1.27, 19.9)            # under the header (above it is H1's screw head)

HOLES = [(103.2, 103.2), (BX1 - 3.2, 103.2), (103.2, BY1 - 3.2), (BX1 - 3.2, BY1 - 3.2)]

# Track widths
W_SIG, W_5V, W_12V, W_3A = 0.5, 1.5, 1.5, 3.0
# J11's rails: 12 V and 5VSB share the 2 mm strip between J11 and the module's
# pads (one per layer); 3.3V squeezes between the mounting holes and the board
# edge (1.6 mm, less the 0.25 hole and 0.3 edge rules) so it stops at 0.9
W_HDR_12V, W_HDR_5V, W_HDR_33 = 1.2, 1.0, 0.9
W_GND, W_GND3A = 1.0, 2.5
VIA_D, VIA_DRILL = 1.0, 0.5

# Net list — every pad connection in the design.  (ref, pad) -> net
NETS = {}


def net(name, *pads):
    for p in pads:
        assert p not in NETS, p
        NETS[p] = name


sm_pad = {nm: str(i + 1) for i, nm in enumerate(SM_LEFT)}
sm_pad.update({nm: str(i + 9) for i, nm in enumerate(SM_RIGHT)})

for pin, n in MF_NET.items():
    net(n, ('J1', str(pin)))
net('5VSB', ('U1', sm_pad['5V']))
net('GND', ('U1', sm_pad['GND']), ('J9', '2'), ('J9', '4'), ('Q1', '1'), ('R1', '2'),
    ('J3', '3'), ('J5', '1'), ('J6', '1'), ('J7', '1'), ('J8', '1'))
net('PS_ON#', ('Q1', '3'))
net('GATE', ('U1', sm_pad['GPIO3']), ('Q1', '2'), ('R1', '1'))
net('SENSE', ('J10', '1'), ('U1', sm_pad['GPIO2']))
net('BTN', ('J9', '3'), ('U1', sm_pad['GPIO1']))
net('DIN', ('U1', sm_pad['GPIO4']), ('J3', '2'))
net('3.3V', ('J3', '1'))
net('12V', ('J5', '2'), ('J6', '2'), ('J7', '2'), ('J8', '2'), ('J9', '1'))
for k, gp in enumerate(FAN_ORDER):
    net(f'FAN_{gp}', ('U1', sm_pad[gp]), (f'J{5 + k}', '4'))
for k, n in enumerate(J11_INNER):
    if n.startswith('GPIO'):
        NETS[('J11', str(2 * k + 1))] = 'GND'
        net(n, ('U1', sm_pad[n]), ('J11', str(2 * k + 2)))
    else:                                   # a rail: both pins of the row
        NETS[('J11', str(2 * k + 1))] = n
        NETS[('J11', str(2 * k + 2))] = n

NET_NAMES = ['GND', '5VSB', 'PS_ON#', 'GATE', 'SENSE', 'BTN', 'DIN', '3.3V', '12V'] + J11_GPIO + \
            [f'FAN_{g}' for g in ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO10']]
NET_ID = {n: i + 1 for i, n in enumerate(NET_NAMES)}

# Parts: ref -> (symbol lib, symbol, footprint lib, footprint, value, board (x, y, rot))
PARTS = {
    'U1': ('bc250_carrier', 'ESP32-C3_SuperMini', PROJECT, 'ESP32-C3_SuperMini', 'ESP32-C3 Super Mini',
           (SM_CX, SM_CY, 0)),
    'J1': ('Connector_Generic', 'Conn_01x10', 'Connector_Molex', 'Molex_Mini-Fit_Jr_5569-10A2_2x05_P4.20mm_Horizontal',
           'PSU Mini-Fit Jr 2x5 R/A', (MF_X0, MF_YF, 0)),
    'J3': ('Connector_Generic', 'Conn_01x03', 'Connector_JST', 'JST_VH_B3P-VH_1x03_P3.96mm_Vertical', 'STRIP',
           (VH_X0, VH_Y, 0)),
    'J9': ('Connector_Generic', 'Conn_01x04', 'Connector_JST', 'JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical', 'BUTTON',
           (J9_X0, XH_Y, 0)),
    'J10': ('Connector_Generic', 'Conn_01x01', 'Connector_PinHeader_2.54mm', 'PinHeader_1x01_P2.54mm_Vertical',
            'SENSE', (J10_X, XH_Y, 0)),
    'J11': ('Connector_Generic', 'Conn_02x08_Odd_Even', 'Connector_PinHeader_2.54mm',
            'PinHeader_2x08_P2.54mm_Vertical', 'PWR+GPIO/GND (optional)', (J11_X0, J11Y[0], 0)),
    'Q1': ('Transistor_FET', '2N7000', 'Package_TO_SOT_THT', 'TO-92_Inline', '2N7000', (Q1_S, Q1_Y, 180)),
    'R1': ('Device', 'R', 'Resistor_THT', 'R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal', '100k',
           (R1_X, R1_Y, 0)),
}
for k, (x1, y) in enumerate(FAN_POS):
    PARTS[f'J{5 + k}'] = ('Connector_Generic', 'Conn_01x04', 'Connector_Molex',
                          'Molex_KK-254_AE-6410-04A_1x04_P2.54mm_Vertical',
                          f'FAN{k + 1} {FAN_ORDER[k]}', (x1, y, 180))
for k, (hx, hy) in enumerate(HOLES):
    PARTS[f'H{k + 1}'] = ('Mechanical', 'MountingHole', 'MountingHole', 'MountingHole_3.2mm_M3', 'M3', (hx, hy, 0))

# ---------------------------------------------------------------- the tracks
# (net, layer, width, [points]) — polylines; THT pads join both layers, and
# the two vias in VIAS do too.
#
# Layer plan.  Across the plug zone the board is a grid: F.Cu carries the
# fat rails (3.3V, the 3 A GND return) up the header's left side and east to
# the VH, DIN along the very top, 12 V east along the header's front row,
# PS_ON# out through the row gap; B.Cu carries the module's 5VSB and GND
# straight down to the header, the four fan PWM lines along the top edge and
# down the right side of the zone, GATE through the row gap to Q1, and the
# XH ground.  GATE is the one net that has to cross both a B.Cu vertical
# (5VSB) and an F.Cu one (3.3V): two vias.

T = []
VIAS = []          # (net, x, y)


def trk(netname, layer, width, *pts):
    T.append((netname, layer, width, list(pts)))


def via(netname, x, y):
    VIAS.append((netname, x, y))


F, B = 'F.Cu', 'B.Cu'
V33_X = 128.6                            # 3.3V up the header's left side, F.Cu (3 mm: clears the peg by 0.3)
V33_Y = 105.5                            # 3.3V east to the VH, F.Cu
GND3A_X = mf_xy(2)[0]                    # GND return up from pin 2, F.Cu
GND3A_Y = 109.8                          # ... and east to the VH, F.Cu
DIN_X, DIN_Y = SMR + 2.2, 103.4          # DIN: up beside the module, east along the top, F.Cu
GATE_VIA1_X, GATE_X, GATE_VIA2_Y = 138.5, 146.4, 123.2
PSON_X = mf_xy(3)[0] + 2.1               # PS_ON# leaves pin 8 between the pad columns
V12B_Y = 128.0                           # 12 V to the right fan column, B.Cu
GNDB_Y = BY1 - 0.8                       # GND along the bottom edge, F.Cu
GNDR_X = BX1 - 0.8                       # GND up the right edge, F.Cu
PWM_ROW = [100.7, 101.5, 102.3, 103.1]   # PWM lines along the top edge, B.Cu (FAN1 top)
PWM_UP = [SML, SML + 1.52, SML + 2.32, SML + 3.12]      # ... rising from the module's left pads
PWM_DOWN = [153.9, 153.1, 152.3, 151.5]  # ... dropping down the right side of the plug zone
PWM_IN_Y = [FAN_Y[0], 118.9, FAN_Y[1], 126.5]          # ... and entering each fan's pin 4
XHG_Y, XHG_X = 128.55, SMR - 1.52        # J9 ground: east under J10, up the module's right side to its GND pad, B.Cu
V12J9_Y = 132.9                          # 12 V to the button LED: west through the row gap, then along the bottom edge under J10/J9, B.Cu
BTN_X, BTN_Y = SMR + 1.4, 128.55         # BTN / SENSE round the module's lower right, F.Cu
SENSE_X, SENSE_Y = SMR + 2.2, 129.3
GPIO0_Y = 127.8                          # GPIO0 west under the module's end to J11, F.Cu (above BTN/SENSE)
HDR_GND_Y, HDR_GND_X, HDR_GND_Y0 = 128.5, 105.6, 131.9   # J9 ground: below J9's pins, west past pin 1, up to J11's GND column, F.Cu
V12HDR_X, V12HDR_Y = 106.81, 126.5       # 12 V from J9 pin 1 up between J11 and the module to J11 row 2, B.Cu (centred in the strip)
V33HDR_Y, V33HDR_X = BY1 - 0.85, BX0 + 0.85     # 3.3V along the bottom edge, up the left edge, in over J11's top, F.Cu (0.3 off H3/H1, 0.4 off the edge)
V33HDR_TOP = 105.6                       # ... between the mounting hole and J11's top pads
V5HDR_X, V5HDR_Y, V5HDR_DROP = SMR + 1.03, 101.5, 106.81  # 5VSB over the top of the module, down the strip beside J11 into row 1, F.Cu
STUB_X = SML - 1.68                      # GPIO stubs: diagonal from the pad to here, then straight into J11

# --- 5VSB: front pin 3 straight up the plug zone, west to the module (B)
trk('5VSB', B, W_5V, mf_xy(3), (mf_xy(3)[0], sm_y(0)), (SMR, sm_y(0)))
# --- GND, module: west from a via on the 3 A return (B); the return itself
# runs up from pin 2 and east to the VH (F); pin 7 tied behind pin 2
trk('GND', B, W_GND, (SMR, sm_y(1)), (GND3A_X, sm_y(1)))
via('GND', GND3A_X, sm_y(1))
trk('GND', F, 2.7, mf_xy(7), mf_xy(2))
trk('GND', F, W_GND3A, mf_xy(2), (GND3A_X, sm_y(1)))
trk('GND', F, W_GND3A, (GND3A_X, GND3A_Y), (VH_X[2], GND3A_Y), (VH_X[2], VH_Y))
# GND, rear row: 9 <-> 10 tied (7 joins through pin 2, 9/10 through the
# bottom-edge run; the two halves meet at the left fan column)
trk('GND', B, W_GND, mf_xy(9), mf_xy(10))
# GND, right column: pin 10 along the bottom edge, up the right edge to the
# right fan column; Q1 source, R1 and the left fan column drop onto it, and
# the left fan column also ties up into the 3 A return
trk('GND', F, W_GND, mf_xy(10), (mf_xy(10)[0], GNDB_Y), (GNDR_X, GNDB_Y), (GNDR_X, FAN_Y[1]), fan_pin(3, 1),
    fan_pin(1, 1))
trk('GND', F, W_SIG, (Q1_S, GNDB_Y), (Q1_S, Q1_Y))
trk('GND', F, W_SIG, (R1_P2[0], GNDB_Y), R1_P2)
trk('GND', F, W_GND, (fan_pin(2, 1)[0], GNDB_Y), fan_pin(2, 1), fan_pin(0, 1), (fan_pin(0, 1)[0], GND3A_Y))
# --- 3.3V (3 A): rear pin 6 tied behind pin 1; pin 1 west, up the header's
# left side, east along the plug zone into the VH (F)
trk('3.3V', F, 2.7, mf_xy(6), mf_xy(1))
trk('3.3V', F, W_3A, mf_xy(1), (V33_X, mf_xy(1)[1]), (V33_X, V33_Y), (VH_X[0], V33_Y))   # ends inside the VH pad
# --- DIN: up beside the module, along the top edge, down into the VH (F)
trk('DIN', F, W_SIG, (SMR, sm_y(3)), (DIN_X, sm_y(3)), (DIN_X, DIN_Y), (VH_X[1], DIN_Y), (VH_X[1], VH_Y))
# --- 12 V: front pins 4 <-> 5 tied; east along the front row and up the
# left fan column (F); the right fan column fed under the header body (B)
trk('12V', F, W_12V, mf_xy(4), mf_xy(5), (fan_pin(2, 2)[0], mf_xy(5)[1]), fan_pin(2, 2), fan_pin(0, 2))
trk('12V', B, 1.2, mf_xy(5), (mf_xy(5)[0], V12B_Y), (fan_pin(3, 2)[0], V12B_Y), fan_pin(3, 2), fan_pin(1, 2))
# 12 V for the button's LED: pin 4 down into the row gap, west through it
# past pins 3/8, 2/7 and 1/6, then along the bottom edge under J10 and J9
# into J9 pin 1 from below (B)
trk('12V', B, W_HDR_12V, mf_xy(4), (mf_xy(4)[0], MF_GAP_Y), (127.5, MF_GAP_Y))
trk('12V', B, W_HDR_12V, (127.5, MF_GAP_Y), (127.5, V12J9_Y), (J9_X[0], V12J9_Y), (J9_X[0], XH_Y))
# --- power switch.  PS_ON#: rear pin 8 sideways, up between the pad
# columns, east through the row gap to Q1's drain (F)
trk('PS_ON#', F, W_SIG, mf_xy(8), (PSON_X, mf_xy(8)[1]), (PSON_X, MF_GAP_Y), (Q1_D, MF_GAP_Y), (Q1_D, Q1_Y))
# GATE: east from the module (B), via, on across the 5VSB vertical (F),
# down beside pin 4, via, on down through the row gap (B) to Q1's gate and R1
trk('GATE', B, W_SIG, (SMR, sm_y(4)), (GATE_VIA1_X, sm_y(4)))
via('GATE', GATE_VIA1_X, sm_y(4))
trk('GATE', F, W_SIG, (GATE_VIA1_X, sm_y(4)), (GATE_X, sm_y(4)), (GATE_X, GATE_VIA2_Y))
via('GATE', GATE_X, GATE_VIA2_Y)
trk('GATE', B, W_SIG, (GATE_X, GATE_VIA2_Y), (GATE_X, MF_GAP_Y), (R1_P1[0], MF_GAP_Y), R1_P1)
trk('GATE', B, W_SIG, (Q1_G, MF_GAP_Y), (Q1_G, Q1_Y))
# --- button and sense: right column pads, down the module's right side,
# west under the antenna end, into the XH pins (F).  BTN (the lower pad)
# takes the inner, upper path and the farther connector.
trk('BTN', F, W_SIG, (SMR, sm_y(6)), (BTN_X, sm_y(6)), (BTN_X, BTN_Y), (J9_X[2], BTN_Y), (J9_X[2], XH_Y))
trk('SENSE', F, W_SIG, (SMR, sm_y(5)), (SENSE_X, sm_y(5)), (SENSE_X, SENSE_Y), (J10_X, SENSE_Y), (J10_X, XH_Y))
# J9 grounds: pins 2 and 4 tied under the housing (round the NO pin between
# them), then east under J10 and up the module's right side, just outside the
# antenna zone, onto the module's GND pad (B)
trk('GND', B, W_SIG, (J9_X[1], XH_Y), (J9_X[1], XH_Y + 1.6), (J9_X[3], XH_Y + 1.6), (J9_X[3], XH_Y))
trk('GND', B, W_SIG, (J9_X[3], XH_Y), (J9_X[3], XHG_Y), (XHG_X, XHG_Y), (XHG_X, sm_y(1)), (SMR, sm_y(1)))
# --- optional header J11.  GPIO rows: GPIO8 and GPIO9 step half a pitch
# down, GPIO20 and GPIO21 half a pitch up (diagonal off the pad, then
# straight in), GPIO0 comes round under the module's end and up into the
# bottom row (all F); the GND column is one strip.
for gp, row in (('GPIO8', 3), ('GPIO9', 4), ('GPIO20', 5), ('GPIO21', 6)):
    i = SM_LEFT.index(gp)
    trk(gp, F, W_SIG, (SML, sm_y(i)), (STUB_X, J11Y[row]), (J11_X1, J11Y[row]))
trk('GPIO0', F, W_SIG, (SMR, sm_y(7)), (SMR, GPIO0_Y), (J11_X1, GPIO0_Y), (J11_X1, J11Y[7]))
trk('GND', F, W_SIG, (J11_X0, J11Y[3]), (J11_X0, J11Y[7]))       # GND column: GPIO rows only
for k in range(3):                                                # rail rows: outer pin bridged to the inner
    trk(J11_INNER[k], F, 1.2, (J11_X0, J11Y[k]), (J11_X1, J11Y[k]))
# J11 ground: from J9's pin 2 down and west below J9 pin 1 (F, between the
# 12 V run and the 3.3V run that are on the other layer / further down), up
# beside the mounting hole and into the GND column
trk('GND', F, W_SIG, (J9_X[1], XH_Y), (J9_X[1], HDR_GND_Y0), (HDR_GND_X, HDR_GND_Y0), (HDR_GND_X, HDR_GND_Y),
    (J11_X0, HDR_GND_Y), (J11_X0, J11Y[7]))
# J11 rails.  12 V: from J9 pin 1 up the strip between J11 and the module,
# in from the right into row 2 (B)
trk('12V', B, W_HDR_12V, (J9_X[0], XH_Y), (J9_X[0], V12HDR_Y), (V12HDR_X, V12HDR_Y), (V12HDR_X, J11Y[2]),
    (J11_X1, J11Y[2]))
# 3.3V: off the PSU header's pin 1/6 tie through the row gap, along the
# bottom edge under the connectors, up the left edge, in over the top of J11
# between the mounting hole and the pads, down into row 0 (F)
trk('3.3V', F, W_HDR_33, (mf_xy(1)[0], MF_GAP_Y), (127.5, MF_GAP_Y), (127.5, V33HDR_Y), (V33HDR_X, V33HDR_Y),
    (V33HDR_X, V33HDR_TOP), (J11_X1, V33HDR_TOP), (J11_X1, J11Y[0]))
# 5VSB: from the module's 5V pad, up beside the module, west above its USB
# end, down the strip beside J11's inner column, in from the right into row 1 (F)
trk('5VSB', F, W_HDR_5V, (SMR, sm_y(0)), (V5HDR_X, sm_y(0)), (V5HDR_X, V5HDR_Y), (V5HDR_DROP, V5HDR_Y),
    (V5HDR_DROP, J11Y[1]), (J11_X1, J11Y[1]))
# --- fan PWM (B): each left-column pad steps right and rises between the
# pads above it, runs east along the top edge, drops down the right side of
# the plug zone and enters its fan's pin 4 (the left pin) from the left.
# The lowest pad takes the inner drop and the lowest row; nothing crosses.
for k, gp in enumerate(FAN_ORDER):
    y = sm_y(SM_LEFT.index(gp))
    pts = [(SML, y), (PWM_UP[k], y), (PWM_UP[k], PWM_ROW[k]), (PWM_DOWN[k], PWM_ROW[k]),
           (PWM_DOWN[k], PWM_IN_Y[k])]
    px = fan_pin(k, 4)
    if PWM_IN_Y[k] != px[1]:
        pts += [(px[0], PWM_IN_Y[k])]
    pts += [px]
    trk(f'FAN_{gp}', B, W_SIG, *pts)

# ---------------------------------------------------------- parts to order
# Everything except the Super Mini, as LCSC stock numbers (LCSC ships together
# with a JLCPCB board order).  (refs, LCSC #, manufacturer part, description,
# per-board qty, LCSC minimum order).  Every number, name and minimum order
# checked against LCSC's product API on Sep 8 2026.
ORDER = [
    (['Q1'], 'C9114', 'JSCJ 2N7000', 'N-MOSFET TO-92, 60 V 200 mA (S G D)', 1, 10),
    (['R1'], 'C120103', 'CCO CF1/4W-100K 5%', '100 k axial 1/4 W carbon film, 2.3 x 6.5 mm body', 1, 100),
    (['J1'], 'C22365658', 'DLL DLL-5569-10AW',
     'Mini-Fit Jr 2x5 4.2 mm RIGHT-ANGLE header with snap-in pegs, 9 A (Molex 5569-10A2 clone)', 1, 5),
    (['J3'], 'C160316', 'JST B3P-VH(LF)(SN)', 'VH 3.96 mm 3-pin vertical header, 10 A', 1, 5),
    (['J9'], 'C594232', 'JST B4B-XH-A-G', 'XH 2.5 mm 4-pin vertical header (gold flash)', 1, 5),
    (['J10'], 'C2337', 'BOOMELE 2.54-1*40P', '2.54 mm 1x40 pin header strip: break off 1 pin for J10', 1, 5),
    # NOT C41927 (BOOMELE 2.54-4AS): that is a fully shrouded 2.54 mm wafer, a fan plug cannot enter it
    (['J5', 'J6', 'J7', 'J8'], 'C140769', 'Ckmtw W-2510S04P-0000',
     'KF2510 2.54 mm 4-pin straight header with friction-lock ramp, 12.7 x 5.8 mm body (PC fan plug mates)', 4, 10),
    # mating plugs: strip, button and sense.  J1 mates with the PSU's own plug,
    # the fan headers with the fans' plugs.  Contacts counted with spares.
    (['J3 plug'], 'C157899', 'JST VHR-3N', 'VH 3-way housing', 1, 10),
    (['J3 plug'], 'C160349', 'JST SVH-21T-P1.1', 'VH crimp contact, 18-22 AWG (3 used)', 5, 100),
    (['J9 plug'], 'C144403', 'JST XHP-4', 'XH 4-way housing', 1, 20),
    (['J9 plug'], 'C140573', 'JST SXH-001T-P0.6', 'XH crimp contact, 22-28 AWG (4 used)', 6, 100),
]
ORDER_OPTIONAL = [
    (['J11'], 'C124382', 'Ckmtw B-2100S32P-B110', '2.54 mm 2x16 pin header: cut to 2x8 for the optional rails + GPIO/GND breakout', 1, 5),
]


def write_parts_list(boards=1):
    """out/lcsc_parts.csv: upload to LCSC's BOM tool (map the columns), or
    search each LCSC # by hand.  Order qty = per-board qty x boards, rounded
    up to the minimum order."""
    import csv
    os.makedirs(os.path.join(HERE, 'out'), exist_ok=True)
    with open(os.path.join(HERE, 'out', 'lcsc_parts.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['LCSC Part Number', 'Manufacturer Part Number', 'Quantity', 'Description', 'Designators',
                    'Per board', 'Optional'])
        for optional, rows in ((False, ORDER), (True, ORDER_OPTIONAL)):
            for refs, lcsc, mpn, desc, per, moq in rows:
                qty = max(per * boards, moq)
                w.writerow([lcsc, mpn, qty, desc, ' '.join(refs), per, 'yes' if optional else ''])


# Antenna zone: no copper pour, no tracks, under the module's antenna end
KEEPOUT = (SM_CX - 5.5, SM_BOT - 7.0, SM_CX + 5.5, SM_BOT + 1.0)   # tracks are checked as copper, edge to edge

# Silkscreen legends: (text, x, y, rot, justify, size, layer)


def _lab(text, x, y, just, size=1.0, layer='F.SilkS'):
    return (text, x, y, 0, just, size, layer)


SILK = [
    _lab('BC-250 carrier  rev C', MF_X0 + 8.4, 111.2, None, 0.9),
    _lab('plug zone: keep clear', MF_X0 + 8.4, 112.8, None, 0.7),
    # button / sense pin names in the strip between the module and the XH housings
    # 0.85 mm between the module outline and the XH housing; 1 mm under the housing
    _lab('12V', J9_X[0], 127.1, None, 0.65), _lab('GND', J9_X[1], 127.1, None, 0.65),
    _lab('NO', J9_X[2], 127.1, None, 0.65), _lab('C', J9_X[3], 127.1, None, 0.65),
    _lab('SNS', J10_X, 127.1, None, 0.65),
    _lab('J9 BUTTON  LED | SW', J9_X0 + 3.75, BY1 - 0.58, None, 0.6),
    _lab('J10 SENSE', J10_X, BY1 - 0.58, None, 0.6),
    # strip: pin names under the VH housing
    _lab('STRIP', 169.3, 104.6, None, 0.8), _lab('3A', 169.3, 106.4, None, 0.8),
    # VH pin names above the housing (the fan headers sit right under it)
    _lab('3.3V', VH_X[0], 100.85, None, 0.7), _lab('DIN', VH_X[1], 100.85, None, 0.7),
    _lab('GND', VH_X[2], 100.85, None, 0.7),
    # PSU header: the rear (edge) row's names in the row gap on the front, the
    # front row's names in the same gap on the back
    _lab('PSU: row-gap names = EDGE row', MF_X0 + 8.4, 114.4, None, 0.8),
    ('PSU: row-gap names = INNER row', MF_X0 + 8.4, 114.4, 0, 'mirror', 0.8, 'B.SilkS'),
    ('serial_led_controller', MF_X0 + 8.4, 121.5, 0, 'mirror', 1.0, 'B.SilkS'),   # clear of the peg holes' mask openings
]
for pin in range(6, 11):
    x, y = mf_xy(pin)
    SILK.append(_lab(MF_NET[pin].replace('PS_ON#', 'PS_ON'), x, MF_GAP_Y, None, 0.7))
    SILK.append((MF_NET[pin - 5], x, MF_GAP_Y, 0, 'mirror', 0.7, 'B.SilkS'))
# J11's names on the back, beside each row (under the module, clear of its left
# pad column): the rail rows are both pins, the GPIO rows name the inner pin
# and the outer column's GND is said once.
SILK.append(_lab('J11 opt.', 103.6, 127.15, None, 0.8))
SILK.append(_lab('names: back', 103.4, 128.45, None, 0.6))
for k, n in enumerate(J11_INNER):
    SILK.append((n if n.startswith('GPIO') else f'2x {n}', 111.6, J11Y[k], 0, 'mirror', 0.7, 'B.SilkS'))
SILK.append(('J11: GND beside each GPIO', 112.0, 128.4, 0, 'mirror', 0.7, 'B.SilkS'))
for k, (x1, y) in enumerate(FAN_POS):
    SILK.append(_lab(f'^ J{5 + k} FAN{k + 1} {FAN_ORDER[k]}', x1 - 3.81, y + 4.1, None, 0.8))   # under its header
for x1 in FAN_X1:
    SILK.append(_lab('PWM  T  12V  G', x1 - 3.81, FAN_Y[1] + 5.3, None, 0.8))

# Silkscreen rectangles: (x0, y0, x1, y1, layer) — the fan headers' real bodies
SILK_RECTS = [(x1 - 3.81 - FAN_BODY_L / 2, y - FAN_BODY_D / 2, x1 - 3.81 + FAN_BODY_L / 2, y + FAN_BODY_D / 2, 'F.SilkS')
              for x1, y in FAN_POS]
NO_STOCK_SILK = {f'J{5 + k}' for k in range(4)}    # their stock outline is the smaller KK-254 body


# ------------------------------------------------------------ pad geometry

def rot_xy(x, y, deg):
    """Rotate a footprint-relative offset by the footprint angle (KiCad: CCW
    positive on a y-down board)."""
    import math
    a = math.radians(deg)
    return (x * math.cos(a) + y * math.sin(a), -x * math.sin(a) + y * math.cos(a))


def footprint_pads(fp):
    """[(number, rel_x, rel_y, size_x, size_y, drill)] from a footprint node."""
    out = []
    for p in findall(fp, 'pad'):
        at = find(p, 'at')
        size = find(p, 'size')
        drill = find(p, 'drill')
        try:
            dr = float(drill[1])
        except (TypeError, ValueError, IndexError):
            dr = 0.0
        out.append((p[1], float(at[1]), float(at[2]), float(size[1]), float(size[2]), dr))
    return out


def load_part_footprint(ref):
    slib, sname, flib, fname, value, place = PARTS[ref]
    if flib == PROJECT:
        return supermini_footprint()
    return load_footprint(flib, fname)


def abs_pads():
    """{(ref, pad): (x, y, half_w, half_h)} on the board, axis-aligned (every
    footprint sits at a multiple of 90 degrees).  Unnumbered pads (the
    header's pegs) get keys NP0, NP1, ..."""
    out = {}
    for ref, (slib, sname, flib, fname, value, (x, y, rot)) in PARTS.items():
        fp = load_part_footprint(ref)
        n_np = 0
        for num, px, py, sx, sy, drill in footprint_pads(fp):
            dx, dy = rot_xy(px, py, rot)
            if rot % 180:
                sx, sy = sy, sx
            if num == '':
                num, n_np = f'NP{n_np}', n_np + 1
            out[(ref, num)] = (x + dx, y + dy, sx / 2, sy / 2)
    for i, (n, vx, vy) in enumerate(VIAS):
        out[(f'V{i}', 'via')] = (vx, vy, VIA_D / 2, VIA_D / 2)
    return out


# ------------------------------------------------------------- write: board

def write_pcb():
    pcb = S('kicad_pcb', S('version', 20221018), S('generator', 'generate.py'))
    pcb.append(S('general', S('thickness', 1.6)))
    pcb.append(S('paper', Q('A4')))
    pcb.append(S('title_block', S('title', Q('BC-250 carrier')), S('rev', Q('C')),
                 S('comment', 1, Q('ESP32-C3 Super Mini carrier: power switch, WS2812B strip, 4 PWM fans'))))
    layers = S('layers')
    std = [(0, 'F.Cu', 'signal'), (31, 'B.Cu', 'signal'), (32, 'B.Adhes', 'user', 'B.Adhesive'),
           (33, 'F.Adhes', 'user', 'F.Adhesive'), (34, 'B.Paste', 'user'), (35, 'F.Paste', 'user'),
           (36, 'B.SilkS', 'user', 'B.Silkscreen'), (37, 'F.SilkS', 'user', 'F.Silkscreen'),
           (38, 'B.Mask', 'user'), (39, 'F.Mask', 'user'), (40, 'Dwgs.User', 'user', 'User.Drawings'),
           (41, 'Cmts.User', 'user', 'User.Comments'), (42, 'Eco1.User', 'user', 'User.Eco1'),
           (43, 'Eco2.User', 'user', 'User.Eco2'), (44, 'Edge.Cuts', 'user'), (45, 'Margin', 'user'),
           (46, 'B.CrtYd', 'user', 'B.Courtyard'), (47, 'F.CrtYd', 'user', 'F.Courtyard'),
           (48, 'B.Fab', 'user'), (49, 'F.Fab', 'user')]
    for row in std:
        n, name, kind = row[0], row[1], row[2]
        item = S(n, Q(name), kind)
        if len(row) > 3:
            item.append(Q(row[3]))
        layers.append(item)
    pcb.append(layers)
    pcb.append(S('setup', S('pad_to_mask_clearance', 0), S('grid_origin', BX0, BY0)))
    pcb.append(S('net', 0, Q('')))
    for name in NET_NAMES:
        pcb.append(S('net', NET_ID[name], Q(name)))

    for ref, (slib, sname, flib, fname, value, (x, y, rot)) in PARTS.items():
        fp = load_part_footprint(ref)
        node = S('footprint', Q(f'{flib}:{fname}'), S('layer', Q('F.Cu')), S('tstamp', Q(U())), S('at', x, y, rot))
        for c in fp[2:]:
            if not isinstance(c, list) or c[0] in ('version', 'generator', 'layer', 'tedit'):
                continue
            c = copy.deepcopy(c)
            if ref in NO_STOCK_SILK and c[0] in ('fp_line', 'fp_arc', 'fp_rect') and find(c, 'layer')[1] == 'F.SilkS':
                continue
            if c[0] == 'fp_text':
                if c[1] == 'reference':
                    c[2] = ref
                    if ref in REF_POS:
                        find(c, 'at')[:] = S('at', *REF_POS[ref])
                    if ref in HIDE_REF:
                        find(c, 'effects').append(Sym('hide'))
                elif c[1] == 'value':
                    c[2] = value
                at = find(c, 'at')
                if at is not None and rot:
                    a = float(at[3]) if len(at) > 3 else 0.0
                    at[:] = S('at', float(at[1]), float(at[2]), (a + rot) % 360)
            if c[0] == 'pad':
                at = find(c, 'at')
                if rot:
                    a = float(at[3]) if len(at) > 3 else 0.0
                    at[:] = S('at', float(at[1]), float(at[2]), (a + rot) % 360)
                key = (ref, c[1])
                if key in NETS:
                    c.append(S('net', NET_ID[NETS[key]], Q(NETS[key])))
            for t in findall(c, 'tstamp'):
                t[1] = U()
            if find(c, 'tstamp') is None and c[0] in ('fp_text', 'fp_line', 'fp_rect', 'fp_circle', 'fp_arc',
                                                     'fp_poly', 'pad'):
                c.append(S('tstamp', Q(U())))
            node.append(c)
        pcb.append(node)

    pcb.append(S('gr_rect', S('start', BX0, BY0), S('end', BX1, BY1),
                 S('stroke', S('width', 0.1), S('type', 'default')), S('fill', 'none'),
                 S('layer', Q('Edge.Cuts')), S('tstamp', Q(U()))))
    for x0, y0, x1, y1, layer in SILK_RECTS:
        pcb.append(S('gr_rect', S('start', x0, y0), S('end', x1, y1), S('stroke', S('width', 0.12), S('type', 'default')),
                     S('fill', 'none'), S('layer', Q(layer)), S('tstamp', Q(U()))))
    for text, x, y, rot, just, size, layer in SILK:
        eff = S('effects', S('font', S('size', size, size), S('thickness', 0.15 if size >= 1 else 0.12)))
        if just:
            eff.append(S('justify', just))
        pcb.append(S('gr_text', Q(text), S('at', x, y, rot), S('layer', Q(layer)), S('tstamp', Q(U())), eff))
    for netname, layer, width, pts in T:
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            pcb.append(S('segment', S('start', x0, y0), S('end', x1, y1), S('width', width), S('layer', Q(layer)),
                         S('net', NET_ID[netname]), S('tstamp', Q(U()))))
    for netname, vx, vy in VIAS:
        pcb.append(S('via', S('at', vx, vy), S('size', VIA_D), S('drill', VIA_DRILL), S('layers', Q('F.Cu'), Q('B.Cu')),
                     S('net', NET_ID[netname]), S('tstamp', Q(U()))))
    kx0, ky0, kx1, ky1 = KEEPOUT
    pcb.append(S('zone', S('net', 0), S('net_name', Q('')), S('layers', Q('F&B.Cu')), S('tstamp', Q(U())),
                 S('name', Q('antenna')), S('hatch', 'edge', 0.508), S('connect_pads', S('clearance', 0)),
                 S('min_thickness', 0.254), S('filled_areas_thickness', 'no'),
                 S('keepout', S('tracks', 'not_allowed'), S('vias', 'not_allowed'), S('pads', 'allowed'),
                   S('copperpour', 'not_allowed'), S('footprints', 'allowed')),
                 S('fill', S('thermal_gap', 0.508), S('thermal_bridge_width', 0.508)),
                 S('polygon', S('pts', S('xy', kx0, ky0), S('xy', kx1, ky0), S('xy', kx1, ky1), S('xy', kx0, ky1)))))
    with open(os.path.join(HERE, PROJECT + '.kicad_pcb'), 'w') as f:
        f.write(dump(pcb) + '\n')



# --------------------------------------------------------- write: schematic

def write_sch():
    sch = S('kicad_sch', S('version', 20230121), S('generator', 'generate.py'), S('uuid', Q(ROOT_UUID)),
            S('paper', Q('A4')),
            S('title_block', S('title', Q('BC-250 carrier')), S('rev', Q('C')),
              S('comment', 1, Q('ESP32-C3 Super Mini carrier: power switch, WS2812B strip, 4 PWM fans'))))
    libsyms = S('lib_symbols')
    seen = set()
    symnodes = {}
    for ref, (slib, sname, flib, fname, value, place) in PARTS.items():
        key = f'{slib}:{sname}'
        if key in seen:
            continue
        seen.add(key)
        node = supermini_symbol() if slib == 'bc250_carrier' else copy.deepcopy(load_lib_symbol(slib, sname))
        node[1] = key
        symnodes[key] = node
        libsyms.append(node)
    sch.append(libsyms)

    # schematic placement (sheet mm)
    SPOS = {
        'U1': (148.59, 96.52),
        'J1': (60.96, 60.96), 'J3': (60.96, 91.44), 'J9': (60.96, 111.76), 'J10': (60.96, 129.54),
        'J11': (27.94, 111.76),
        'Q1': (88.9, 147.32), 'R1': (71.12, 147.32),
        'J5': (236.22, 43.18), 'J6': (236.22, 66.04), 'J7': (236.22, 88.9), 'J8': (236.22, 111.76),
        'H1': (27.94, 175.26), 'H2': (43.18, 175.26), 'H3': (58.42, 175.26), 'H4': (73.66, 175.26),
    }
    STUB = 5.08
    items = []
    for ref, (slib, sname, flib, fname, value, place) in PARTS.items():
        key = f'{slib}:{sname}'
        sx, sy = SPOS[ref]
        node = S('symbol', S('lib_id', Q(key)), S('at', sx, sy, 0), S('unit', 1), S('in_bom', 'yes'),
                 S('on_board', 'yes'), S('dnp', 'no'), S('uuid', Q(U())))
        pins = symbol_pins(symnodes[key])
        # reference/value text just above and below the body
        ymin = min([p[2] for p in pins] + [0]) if pins else 0
        ymax = max([p[2] for p in pins] + [0]) if pins else 0
        if ref == 'U1':
            rpos, vpos = (sx, sy - 14.605), (sx, sy + 14.605)
        elif ref.startswith('H'):
            rpos, vpos = (sx, sy - 3.81), (sx, sy + 3.81)
        elif ref in ('Q1',):
            rpos, vpos = (sx + 5.08, sy - 1.27), (sx + 5.08, sy + 1.27)
        elif ref == 'R1':
            rpos, vpos = (sx + 2.54, sy - 1.27), (sx + 2.54, sy + 1.27)
        else:
            rpos, vpos = (sx + 1.27, sy - ymax - 2.54), (sx + 1.27, sy - ymin + 2.54)
        node.append(S('property', Q('Reference'), Q(ref), S('at', rpos[0], rpos[1], 0),
                      S('effects', S('font', S('size', 1.27, 1.27)))))
        node.append(S('property', Q('Value'), Q(value), S('at', vpos[0], vpos[1], 0),
                      S('effects', S('font', S('size', 1.27, 1.27)))))
        node.append(S('property', Q('Footprint'), Q(f'{flib}:{fname}'), S('at', sx, sy, 0),
                      S('effects', S('font', S('size', 1.27, 1.27)), 'hide')))
        node.append(S('property', Q('Datasheet'), Q('~'), S('at', sx, sy, 0),
                      S('effects', S('font', S('size', 1.27, 1.27)), 'hide')))
        for num, px, py, ang in pins:
            node.append(S('pin', Q(num), S('uuid', Q(U()))))
        node.append(S('instances', S('project', Q(PROJECT), S('path', Q('/' + ROOT_UUID), S('reference', Q(ref)),
                                                               S('unit', 1)))))
        items.append(node)
        # a stub wire and a net label on every connected pin; no-connect on the rest
        for num, px, py, ang in pins:
            x, y = sx + px, sy - py
            ang = int(round(ang)) % 360
            d = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[ang]
            netname = NETS.get((ref, num))
            if netname is None:
                items.append(S('no_connect', S('at', x, y), S('uuid', Q(U()))))
                continue
            ex, ey = x + d[0] * STUB, y + d[1] * STUB
            items.append(S('wire', S('pts', S('xy', x, y), S('xy', ex, ey)),
                           S('stroke', S('width', 0), S('type', 'default')), S('uuid', Q(U()))))
            if d == (-1, 0):
                lab_at, just = (ex, ey, 180), 'right bottom'
            elif d == (1, 0):
                lab_at, just = (ex, ey, 0), 'left bottom'
            elif d == (0, 1):
                lab_at, just = (ex, ey, 270), 'right bottom'
            else:
                lab_at, just = (ex, ey, 90), 'left bottom'
            items.append(S('label', Q(netname), S('at', *lab_at), S('fields_autoplaced'),
                           S('effects', S('font', S('size', 1.27, 1.27)), S('justify', *just.split())),
                           S('uuid', Q(U()))))
    notes = [
        (20.32, 20.32, 'ESP32-C3 Super Mini carrier for the BC-250 (serial_led_controller).'),
        (20.32, 24.13, 'The Super Mini brings its own USB-C, 3.3 V regulator, BOOT/RESET buttons and GPIO8 LED.'),
        (20.32, 27.94, 'Its USB VBUS is tied to the 5V pin: cut the red wire in the USB cable (README, Power switch).'),
        (20.32, 31.75, 'The switch common (J9 pin 4) is a real GND: power_switch.pins.button_gnd is null in the config.'),
        (20.32, 35.56, 'Fan header pin 1 = GND, 2 = +12 V, 3 = tach (unused), 4 = PWM. FAN1..4 = GPIO 5, 6, 7, 10 = fans.header1..4.'),
        (20.32, 39.37, 'J1 takes the FSP500-30AS 10-pin Mini-Fit Jr plug; pin map per its pinout drawing (see README).'),
        (20.32, 43.18, 'VIN is the strip supply rail (3.3 V from the PSU), separate from 5VSB; the strip path is sized for 3 A.'),
        (20.32, 163.83, 'Mounting holes, M3:'),
    ]
    for x, y, text in notes:
        items.append(S('text', Q(text), S('at', x, y, 0), S('effects', S('font', S('size', 1.27, 1.27)),
                                                            S('justify', 'left', 'bottom')), S('uuid', Q(U()))))
    sch += items
    sch.append(S('sheet_instances', S('path', Q('/'), S('page', Q('1')))))
    with open(os.path.join(HERE, PROJECT + '.kicad_sch'), 'w') as f:
        f.write(dump(sch) + '\n')


# ------------------------------------------------- write: project + libraries

def write_project():
    pro = {
        "board": {
            "design_settings": {
                "defaults": {"board_outline_line_width": 0.1, "copper_line_width": 0.2, "silk_line_width": 0.15,
                             "silk_text_size_h": 1.0, "silk_text_size_v": 1.0, "silk_text_thickness": 0.15},
                "rules": {"min_clearance": 0.2, "min_copper_edge_clearance": 0.3, "min_hole_clearance": 0.25,
                          "min_hole_to_hole": 0.25, "min_microvia_diameter": 0.2, "min_microvia_drill": 0.1,
                          "min_resolved_spokes": 2, "min_silk_clearance": 0.0, "min_text_height": 0.6,
                          "min_text_thickness": 0.08, "min_through_hole_diameter": 0.3, "min_track_width": 0.25,
                          "min_via_annular_width": 0.15, "min_via_diameter": 0.5, "solder_mask_clearance": 0.0,
                          "solder_mask_min_width": 0.0, "use_height_for_length_calcs": True},
                "track_widths": [0.0, 0.5, 0.9, 1.0, 1.2, 1.5, 2.5, 3.0],
                "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 1.0, "drill": 0.5}],
            },
            "layer_presets": [], "viewports": []
        },
        "boards": [], "cvpcb": {"equivalence_files": []}, "libraries": {"pinned_footprint_libs": [],
                                                                       "pinned_symbol_libs": []},
        "meta": {"filename": PROJECT + ".kicad_pro", "version": 1},
        "net_settings": {"classes": [{"bus_width": 12, "clearance": 0.2, "diff_pair_gap": 0.25,
                                      "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0,
                                      "microvia_diameter": 0.3, "microvia_drill": 0.1, "name": "Default",
                                      "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
                                      "track_width": 0.5, "via_diameter": 0.8, "via_drill": 0.4, "wire_width": 6}],
                         "meta": {"version": 3}, "net_colors": None, "netclass_assignments": None,
                         "netclass_patterns": []},
        "pcbnew": {"last_paths": {"gencad": "", "idf": "", "netlist": "", "specctra_dsn": "", "step": "", "vrml": ""},
                   "page_layout_descr_file": ""},
        "schematic": {"annotate_start_num": 0, "drawing": {"default_line_thickness": 6.0, "default_text_size": 50.0,
                                                           "field_names": [], "intersheets_ref_own_page": False,
                                                           "intersheets_ref_prefix": "", "intersheets_ref_short": False,
                                                           "intersheets_ref_show": False,
                                                           "intersheets_ref_suffix": "", "junction_size_choice": 3,
                                                           "label_size_ratio": 0.375, "pin_symbol_size": 25.0,
                                                           "text_offset_ratio": 0.15},
                      "legacy_lib_dir": "", "legacy_lib_list": [], "meta": {"version": 1}, "net_format_name": "",
                      "page_layout_descr_file": "", "plot_directory": "", "spice_current_sheet_as_root": False,
                      "spice_external_command": "spice \"%I\"", "spice_model_current_sheet_as_root": True,
                      "spice_save_all_currents": False, "spice_save_all_voltages": False,
                      "subpart_first_id": 65, "subpart_id_separator": 0},
        "sheets": [[ROOT_UUID, ""]], "text_variables": {}
    }
    with open(os.path.join(HERE, PROJECT + '.kicad_pro'), 'w') as f:
        json.dump(pro, f, indent=2)
    with open(os.path.join(HERE, 'sym-lib-table'), 'w') as f:
        f.write('(sym_lib_table\n  (version 7)\n'
                f'  (lib (name "bc250_carrier")(type "KiCad")(uri "${{KIPRJMOD}}/{PROJECT}.kicad_sym")'
                '(options "")(descr "Project symbols"))\n)\n')
    with open(os.path.join(HERE, 'fp-lib-table'), 'w') as f:
        f.write('(fp_lib_table\n  (version 7)\n'
                f'  (lib (name "{PROJECT}")(type "KiCad")(uri "${{KIPRJMOD}}/{PROJECT}.pretty")'
                '(options "")(descr "Project footprints"))\n)\n')
    lib = S('kicad_symbol_lib', S('version', 20211014), S('generator', 'generate.py'), supermini_symbol())
    with open(os.path.join(HERE, PROJECT + '.kicad_sym'), 'w') as f:
        f.write(dump(lib) + '\n')
    os.makedirs(os.path.join(HERE, PROJECT + '.pretty'), exist_ok=True)
    with open(os.path.join(HERE, PROJECT + '.pretty', 'ESP32-C3_SuperMini.kicad_mod'), 'w') as f:
        f.write(dump(supermini_footprint()) + '\n')


def design():
    """What check.py needs: pads, tracks, nets, outline, holes."""
    nets = dict(NETS)
    nets.update({(f'V{i}', 'via'): n for i, (n, vx, vy) in enumerate(VIAS)})
    return dict(pads=abs_pads(), nets=nets, tracks=T, vias=VIAS, outline=(BX0, BY0, BX1, BY1), net_names=NET_NAMES,
                parts=PARTS, keepout=KEEPOUT)


if __name__ == '__main__':
    write_project()
    write_sch()
    write_pcb()
    write_parts_list(int(os.environ.get('BOARDS', '1')))
    print('wrote', PROJECT, 'project into', HERE)
