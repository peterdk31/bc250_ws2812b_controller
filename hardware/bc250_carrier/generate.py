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
    fp.append(S('fp_text', 'reference', Q('REF**'), S('at', 0, -SM_L / 2 - 1.5), S('layer', Q('F.SilkS')),
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
        fp.append(S('fp_text', 'user', Q(nm), S('at', -SM_ROW / 2 - 3.6, y), S('layer', Q('F.SilkS')),
                    S('effects', S('font', S('size', 0.8, 0.8), S('thickness', 0.12)), S('justify', 'right'))))
    for i, nm in enumerate(SM_RIGHT):
        y = y0 + SM_PITCH * i
        fp.append(S('pad', Q(str(i + 9)), 'thru_hole', 'circle', S('at', SM_ROW / 2, y), S('size', 1.7, 1.7),
                    S('drill', 1.0), S('layers', Q('*.Cu'), Q('*.Mask'))))
        fp.append(S('fp_text', 'user', Q(nm), S('at', SM_ROW / 2 + 3.6, y), S('layer', Q('F.SilkS')),
                    S('effects', S('font', S('size', 0.8, 0.8), S('thickness', 0.12)), S('justify', 'left'))))
    return fp


# ------------------------------------------------------------------ the design

# Board outline: 80 x 55 mm
BX0, BY0, BX1, BY1 = 100.0, 100.0, 180.0, 155.0

# Super Mini placement: centre x, module top edge 1.24 mm inside the board edge.
# Column names come from SM_LEFT / SM_RIGHT above (rev B: 5V column on the
# RIGHT when the module sits component-side up with its USB-C at the top).
SM_CX = 140.0
SM_CY = BY0 + 1.24 + SM_L / 2
SM_Y0 = SM_CY - SM_PITCH * 3.5          # y of the first pin row
SML = SM_CX - SM_ROW / 2                # left pin column x  (GPIO5..GPIO21)
SMR = SM_CX + SM_ROW / 2                # right pin column x (5V..GPIO0)


def sm_y(i):
    return SM_Y0 + SM_PITCH * i


# --- PSU header: Molex Mini-Fit Jr 5566-10A (2x5); the FSP500-30AS's own
# 10-pin plug mates with it.  Rotated 270: two pin columns run down the right
# edge, pins 1-5 in the column nearest the board edge (x=MF_EDGE), pins 6-10
# in the inner column (x=MF_IN), pin k beside pin k+5; the latch ramp is on
# the inner side.
#
# Pin map from the FSP500-30AS pinout drawing (fsp500-30as.webp: "looking
# into the front face of the connector", latch up): latch-side row
# 3.3V GND PS_ON GND GND, other row 3.3V GND 5VSB 12V 12V.  KiCad's footprint
# has the ramp on the pin 6-10 row, and a header seen from above is the
# mirror image of the plug seen face-on, which gives:
MF_EDGE, MF_IN = 176.0, 170.5
MF_Y0, MF_P = 110.5, 4.2
MF_NET = {
    1: 'VIN', 2: 'GND', 3: '5VSB', 4: '12V', 5: '12V',          # edge column, top to bottom
    6: 'VIN', 7: 'GND', 8: 'PS_ON#', 9: 'GND', 10: 'GND',       # inner column, top to bottom
}


def mf_xy(pin):
    col = MF_EDGE if pin <= 5 else MF_IN
    return (col, MF_Y0 + MF_P * ((pin - 1) % 5))


# Strip output: JST-VH 3-pin (10 A contacts), below the PSU header, pins
# down the right edge: VIN, DIN, GND
VH_X, VH_Y0, VH_P = 174.5, 134.5, 3.96
VH_Y = [VH_Y0 + VH_P * k for k in range(3)]

# Button and sense: JST-XH 2-pin on the left edge, pins downwards
LX, XH = 104.5, 2.5


def col(y0, n):
    return [y0 + XH * k for k in range(n)]


J9_Y = col(110.0, 2)     # BTN: BTN, GND
J10_Y = col(119.0, 2)    # SENSE: GND, SENSE

# Q1 (TO-92 inline, rotated 180): D G S left to right, on the PS_ON# pin's row;
# PS_ON# steps over the gate and source pads to reach the drain
Q1_X, Q1_Y = 160.5, mf_xy(8)[1]
Q1_S, Q1_G, Q1_D = Q1_X, Q1_X - 1.27, Q1_X - 2.54
# R1 axial, vertical (rot 270): pin 1 (gate) up, pin 2 (GND) down, straight above FAN4's GND pin
R1_X, R1_Y = 152.5, 129.0
R1_P1, R1_P2 = (R1_X, R1_Y), (R1_X, R1_Y + 10.16)

# Fan headers along the bottom, pin 1 (GND) leftmost; PWM on pin 4.
# Left to right FAN1..FAN4 = GPIO5, 6, 7, 10 = FAN_PINS order.
FAN_Y = 151.5
FAN_X0 = [113.5, 126.5, 139.5, 152.5]
FAN_ORDER = ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO10']

# footprint reference text moved off the stock spot: ref -> (x, y) relative
REF_POS = {f'J{5 + k}': (3.81, -2.2) for k in range(4)}
REF_POS['Q1'] = (1.27, -3.2)   # below the body (rot 180 flips it)
REF_POS['J1'] = (-2.3, 2.75)
REF_POS['J3'] = (-2.7, 2.2)

HOLES = [(105.7, 103.5), (174.3, 103.5), (105.7, 149.5), (174.3, 149.5)]

# Track widths
W_SIG, W_5V, W_12V, W_3A = 0.5, 1.0, 1.5, 3.0
W_GND, W_GND3A = 1.0, 2.5

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
net('GND', ('U1', sm_pad['GND']), ('J10', '1'), ('J9', '2'), ('Q1', '1'), ('R1', '2'),
    ('J3', '3'), ('J5', '1'), ('J6', '1'), ('J7', '1'), ('J8', '1'))
net('PS_ON#', ('Q1', '3'))
net('GATE', ('U1', sm_pad['GPIO3']), ('Q1', '2'), ('R1', '1'))
net('SENSE', ('J10', '2'), ('U1', sm_pad['GPIO2']))
net('BTN', ('J9', '1'), ('U1', sm_pad['GPIO1']))
net('DIN', ('U1', sm_pad['GPIO4']), ('J3', '2'))
net('VIN', ('J3', '1'))
net('12V', ('J5', '2'), ('J6', '2'), ('J7', '2'), ('J8', '2'))
for k, gp in enumerate(FAN_ORDER):
    net(f'FAN_{gp}', ('U1', sm_pad[gp]), (f'J{5 + k}', '4'))

NET_NAMES = ['GND', '5VSB', 'PS_ON#', 'GATE', 'SENSE', 'BTN', 'DIN', 'VIN', '12V'] + \
            [f'FAN_{g}' for g in ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO10']]
NET_ID = {n: i + 1 for i, n in enumerate(NET_NAMES)}

# Parts: ref -> (symbol lib, symbol, footprint lib, footprint, value, board (x, y, rot))
PARTS = {
    'U1': ('bc250_carrier', 'ESP32-C3_SuperMini', PROJECT, 'ESP32-C3_SuperMini', 'ESP32-C3 Super Mini',
           (SM_CX, SM_CY, 0)),
    'J1': ('Connector_Generic', 'Conn_01x10', 'Connector_Molex', 'Molex_Mini-Fit_Jr_5566-10A_2x05_P4.20mm_Vertical',
           'PSU Mini-Fit Jr 2x5', (MF_EDGE, MF_Y0, 270)),
    'J3': ('Connector_Generic', 'Conn_01x03', 'Connector_JST', 'JST_VH_B3P-VH_1x03_P3.96mm_Vertical', 'STRIP',
           (VH_X, VH_Y0, 270)),
    'J9': ('Connector_Generic', 'Conn_01x02', 'Connector_JST', 'JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical', 'BTN',
           (LX, J9_Y[0], 270)),
    'J10': ('Connector_Generic', 'Conn_01x02', 'Connector_JST', 'JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical', 'SENSE',
            (LX, J10_Y[0], 270)),
    'Q1': ('Transistor_FET', '2N7000', 'Package_TO_SOT_THT', 'TO-92_Inline', '2N7000', (Q1_X, Q1_Y, 180)),
    'R1': ('Device', 'R', 'Resistor_THT', 'R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal', '100k',
           (R1_X, R1_Y, 270)),
}
for k, x0 in enumerate(FAN_X0):
    PARTS[f'J{5 + k}'] = ('Connector_Generic', 'Conn_01x04', 'Connector_Molex',
                          'Molex_KK-254_AE-6410-04A_1x04_P2.54mm_Vertical',
                          f'FAN{k + 1} {FAN_ORDER[k]}', (x0, FAN_Y, 0))
for k, (hx, hy) in enumerate(HOLES):
    PARTS[f'H{k + 1}'] = ('Mechanical', 'MountingHole', 'MountingHole', 'MountingHole_3.2mm_M3', 'M3', (hx, hy, 0))

# ---------------------------------------------------------------- the tracks
# (net, layer, width, [points]) — polylines; THT pads join both layers.
# Layer plan: F.Cu carries 5VSB, PS_ON#, the fan PWM runs, the 12 V drop and
# its bus along the bottom edge, the left GND link; B.Cu carries the module
# GND, GATE, DIN, VIN, the BTN/SENSE cross-unders below the module, and the
# GND ring (right trunk + bottom run).

T = []


def trk(netname, layer, width, *pts):
    T.append((netname, layer, width, list(pts)))


F, B = 'F.Cu', 'B.Cu'
STRIP_G, STRIP_12V = 178.3, 178.4        # the 2.35 mm strip right of the header: GND (B.Cu) under 12 V (F.Cu)
STRIP_G0 = 178.6                         # module-GND link up the strip to pin 2, B.Cu (thinner: passes pin 1)
MID_X = (MF_EDGE + MF_IN) / 2            # between the header columns: 5VSB up (F.Cu), GND link down (B.Cu)
GRING_Y = 154.0                          # GND ring along the bottom edge, B.Cu
V12_Y = 153.8                            # 12 V bus along the bottom edge, F.Cu
VIN_X = 166.3                            # VIN drop left of the header, B.Cu
DIN_X = 164.0                            # DIN drop, B.Cu
BAND_BTN, BAND_SENSE = 125.5, 126.5      # cross-unders below the module, B.Cu
LRING_X = 106.9                          # left GND link, F.Cu
TRUNK_Y = 130.6                          # GND: pin 10 across to the strip, B.Cu

# --- 5VSB: edge pin 3, in between the columns, up over the header, along the top to the module
trk('5VSB', F, W_5V, mf_xy(3), (MID_X, mf_xy(3)[1]), (MID_X, 107.5), (166.0, 107.5), (166.0, sm_y(0)),
    (SMR, sm_y(0)))
# --- GND: module pin -> up the strip to edge pin 2; pins 2<->7 across the row; 7 -> 9 between the
# columns; 9 -> 10 down the column; 10 across to the strip and down it (3 A return for the strip)
trk('GND', B, W_GND, (SMR, sm_y(1)), (STRIP_G0, sm_y(1)), (STRIP_G0, mf_xy(2)[1]), mf_xy(2))
trk('GND', B, W_GND, mf_xy(7), mf_xy(2))
trk('GND', B, W_GND, (MID_X, mf_xy(7)[1]), (MID_X, mf_xy(9)[1]), mf_xy(9))
trk('GND', B, W_GND, mf_xy(9), mf_xy(10))
trk('GND', B, W_GND3A, mf_xy(10), (MF_IN, TRUNK_Y), (STRIP_G, TRUNK_Y), (STRIP_G, VH_Y[2]))
trk('GND', B, W_GND3A, (VH_X, VH_Y[2]), (STRIP_G, VH_Y[2]))
trk('GND', B, W_GND, (STRIP_G, VH_Y[2]), (STRIP_G, GRING_Y), (FAN_X0[0], GRING_Y))
# --- 12 V: edge pins 4+5, down the strip, along the bottom edge to every fan pin 2
trk('12V', F, W_12V, mf_xy(4), mf_xy(5), (STRIP_12V, mf_xy(5)[1]), (STRIP_12V, V12_Y), (FAN_X0[0] + 2.54, V12_Y))
for x0 in FAN_X0:
    trk('12V', F, W_12V, (x0 + 2.54, V12_Y), (x0 + 2.54, FAN_Y))
    trk('GND', B, W_GND, (x0, FAN_Y), (x0, GRING_Y))
# --- VIN (3 A): inner pin 6 (edge pin 1 tied across the row), west, down, east into the VH
trk('VIN', B, W_12V, mf_xy(1), mf_xy(6))
trk('VIN', B, W_3A, mf_xy(6), (VIN_X, mf_xy(6)[1]), (VIN_X, VH_Y[0]), (VH_X, VH_Y[0]))
trk('DIN', B, W_SIG, (SMR, sm_y(3)), (DIN_X, sm_y(3)), (DIN_X, VH_Y[1]), (VH_X, VH_Y[1]))
# --- power switch
trk('PS_ON#', F, W_SIG, mf_xy(8), (162.5, Q1_Y), (162.5, Q1_Y - 3.4), (Q1_D, Q1_Y - 3.4), (Q1_D, Q1_Y))
trk('GATE', B, W_SIG, (SMR, sm_y(4)), (Q1_G, sm_y(4)), (Q1_G, R1_Y), R1_P1)
trk('GND', B, W_SIG, (Q1_S, Q1_Y), (Q1_S, Q1_Y + 1.4), (Q1_S + 0.7, Q1_Y + 1.4), (Q1_S + 0.7, R1_P2[1]), R1_P2)
trk('GND', F, W_SIG, R1_P2, (R1_X, FAN_Y))                      # R1 pin 2 straight down onto FAN4 pin 1
# --- button and sense: right column pads, drop, run left below the module, up the left edge
trk('BTN', B, W_SIG, (SMR, sm_y(6)), (149.5, sm_y(6)), (149.5, BAND_BTN), (109.5, BAND_BTN), (109.5, J9_Y[0]),
    (LX, J9_Y[0]))
trk('SENSE', B, W_SIG, (SMR, sm_y(5)), (150.5, sm_y(5)), (150.5, BAND_SENSE), (108.5, BAND_SENSE),
    (108.5, J10_Y[1]), (LX, J10_Y[1]))
trk('GND', F, W_GND, (LX, J9_Y[1]), (LRING_X, J9_Y[1]), (LRING_X, 145.0), (FAN_X0[0], 145.0), (FAN_X0[0], FAN_Y))
trk('GND', F, W_GND, (LX, J10_Y[0]), (LRING_X, J10_Y[0]))
# --- fan PWM: left column pads exit west, drop, and run to their header's pin 4.
# The lowest pad takes the innermost drop and the highest turn, and runs the
# farthest, so nothing crosses.
PWM_DROP = {'GPIO10': (130.0, 127.0), 'GPIO7': (128.0, 129.5), 'GPIO6': (126.0, 132.0), 'GPIO5': (124.0, 134.5)}
for k, gp in enumerate(FAN_ORDER):
    dx, ty = PWM_DROP[gp]
    px = FAN_X0[k] + 7.62
    y = sm_y(SM_LEFT.index(gp))
    trk(f'FAN_{gp}', F, W_SIG, (SML, y), (dx, y), (dx, ty), (px, ty), (px, FAN_Y))

# ---------------------------------------------------------- parts to order
# Everything except the Super Mini, as LCSC stock numbers (LCSC ships together
# with a JLCPCB board order).  (refs, LCSC #, manufacturer part, description,
# per-board qty, LCSC minimum order).  Verified on lcsc.com Sep 2026.
ORDER = [
    (['Q1'], 'C9114', 'JSCJ 2N7000', 'N-MOSFET TO-92, 60 V 200 mA (S G D)', 1, 10),
    (['R1'], 'C120103', 'CCO CF1/4W-100K 5%', '100 k axial 1/4 W carbon film, 2.3 x 6.5 mm body', 1, 100),
    (['J1'], 'C22365703', 'DLL DLL-5566-10A', 'Mini-Fit Jr 2x5 4.2 mm vertical header, 9 A (Molex 5566-10A clone)', 1, 5),
    (['J3'], 'C160316', 'JST B3P-VH(LF)(SN)', 'VH 3.96 mm 3-pin vertical header, 10 A', 1, 5),
    (['J9', 'J10'], 'C158012', 'JST B2B-XH-A(LF)(SN)', 'XH 2.5 mm 2-pin vertical header', 2, 20),
    (['J5', 'J6', 'J7', 'J8'], 'C41927', 'BOOMELE 2.54-4AS',
     'KF2510-style 2.54 mm 4-pin header with lock (PC fan plug mates)', 4, 5),
    # mating plugs: strip, button and sense.  J1 mates with the PSU's own plug,
    # the fan headers with the fans' plugs.  Contacts counted with spares.
    (['J3 plug'], 'C157899', 'JST VHR-3N', 'VH 3-way housing', 1, 5),
    (['J3 plug'], 'C160349', 'JST SVH-21T-P1.1', 'VH crimp contact, 18-22 AWG (3 used)', 5, 10),
    (['J9/J10 plug'], 'C144401', 'JST XHP-2', 'XH 2-way housing', 2, 10),
    (['J9/J10 plug'], 'C140573', 'JST SXH-001T-P0.6', 'XH crimp contact, 22-28 AWG (4 used)', 6, 10),
]
ORDER_OPTIONAL = []


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
KEEPOUT = (SM_CX - 5.5, 116.5, SM_CX + 5.5, 125.0)

# Silkscreen legends: (text, x, y, rot, justify, size, layer)
LL = 107.6               # label x just outside the left connector housings
ML = MF_IN - 3.2         # labels left of the PSU header's inner column


def _lab(text, x, y, just, size=1.0, layer='F.SilkS'):
    return (text, x, y, 0, just, size, layer)


SILK = [
    _lab('BC-250 carrier  rev B', SM_CX, 128.5, None, 0.9),
    _lab('BUTTON', 109.5, J9_Y[0] - 1.7, 'left'),
    _lab('BTN', LL, J9_Y[0], 'left'), _lab('GND', LL, J9_Y[1], 'left'),
    _lab('SENSE', 109.5, J10_Y[0] - 1.7, 'left'),
    _lab('GND', LL, J10_Y[0], 'left'), _lab('TPMS1.9', LL, J10_Y[1], 'left'),
    _lab('PSU 10p', ML, MF_Y0 - 2.2, 'right'),
    _lab('STRIP  3A', VH_X - 6.0, VH_Y0 - 2.0, 'right'),
    _lab('VIN', VH_X - 6.0, VH_Y[0], 'right'), _lab('DIN', VH_X - 6.0, VH_Y[1], 'right'),
    _lab('GND', VH_X - 6.0, VH_Y[2], 'right'),
    ('serial_led_controller  BC-250 carrier  rev B', SM_CX, 141.0, 0, 'mirror', 1.0, 'B.SilkS'),
]
for pin in range(6, 11):
    x, y = mf_xy(pin)
    SILK.append(_lab(MF_NET.get(pin, 'nc'), ML, y, 'right'))
for pin in range(1, 6):
    x, y = mf_xy(pin)
    SILK.append(_lab(MF_NET.get(pin, 'nc'), (MF_EDGE + MF_IN) / 2, y, None, 0.7))
for k, x0 in enumerate(FAN_X0):
    SILK.append(_lab(f'FAN{k + 1}  {FAN_ORDER[k]}', x0 + 3.81, FAN_Y - 4.4, None, 0.9))
    SILK.append(_lab('G 12V T PWM', x0 + 3.81, FAN_Y + 2.6, None, 0.8))


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
    """{(ref, pad): (x, y, r)} on the board."""
    out = {}
    for ref, (slib, sname, flib, fname, value, (x, y, rot)) in PARTS.items():
        fp = load_part_footprint(ref)
        for num, px, py, sx, sy, drill in footprint_pads(fp):
            dx, dy = rot_xy(px, py, rot)
            out[(ref, num)] = (x + dx, y + dy, max(sx, sy) / 2)
    return out


# ------------------------------------------------------------- write: board

def write_pcb():
    pcb = S('kicad_pcb', S('version', 20221018), S('generator', 'generate.py'))
    pcb.append(S('general', S('thickness', 1.6)))
    pcb.append(S('paper', Q('A4')))
    pcb.append(S('title_block', S('title', Q('BC-250 carrier')), S('rev', Q('A')),
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
            if c[0] == 'fp_text':
                if c[1] == 'reference':
                    c[2] = ref
                    if ref in REF_POS:
                        find(c, 'at')[:] = S('at', *REF_POS[ref])
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
    for text, x, y, rot, just, size, layer in SILK:
        eff = S('effects', S('font', S('size', size, size), S('thickness', 0.15 if size >= 1 else 0.12)))
        if just:
            eff.append(S('justify', just))
        pcb.append(S('gr_text', Q(text), S('at', x, y, rot), S('layer', Q(layer)), S('tstamp', Q(U())), eff))
    for netname, layer, width, pts in T:
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            pcb.append(S('segment', S('start', x0, y0), S('end', x1, y1), S('width', width), S('layer', Q(layer)),
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
            S('title_block', S('title', Q('BC-250 carrier')), S('rev', Q('A')),
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
        'J1': (60.96, 60.96), 'J3': (60.96, 91.44), 'J9': (60.96, 109.22), 'J10': (60.96, 124.46),
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
        (20.32, 31.75, 'Button pin 2 is a real GND: flash the power config with PWR_BUTTON_GND=-1.'),
        (20.32, 35.56, 'Fan header pin 1 = GND, 2 = +12 V, 3 = tach (unused), 4 = PWM. FAN1..4 = FAN_PINS=5,6,7,10.'),
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
                          "min_resolved_spokes": 2, "min_silk_clearance": 0.0, "min_text_height": 0.8,
                          "min_text_thickness": 0.08, "min_through_hole_diameter": 0.3, "min_track_width": 0.25,
                          "min_via_annular_width": 0.15, "min_via_diameter": 0.5, "solder_mask_clearance": 0.0,
                          "solder_mask_min_width": 0.0, "use_height_for_length_calcs": True},
                "track_widths": [0.0, 0.5, 1.0, 1.5],
                "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.8, "drill": 0.4}],
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
    return dict(pads=abs_pads(), nets=NETS, tracks=T, outline=(BX0, BY0, BX1, BY1), net_names=NET_NAMES,
                parts=PARTS, keepout=KEEPOUT)


if __name__ == '__main__':
    write_project()
    write_sch()
    write_pcb()
    write_parts_list(int(os.environ.get('BOARDS', '1')))
    print('wrote', PROJECT, 'project into', HERE)
