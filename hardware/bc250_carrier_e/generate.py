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
PROJECT = 'bc250_carrier_e'
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
# Pin names by column, top to bottom, for the module COMPONENT SIDE UP with
# its USB-C at the top (the labels printed on the module's underside read
# mirrored to this). Swap the two lists if a module differs; the nets follow
# the names, but the tracks are laid out for this arrangement.
#
# The module is not soldered: it plugs, male pins down, into two 1x8 female
# headers J12 (left column) and J13 (right column), 5.7 mm low-profile.  U1
# exists in the schematic only; on the board the sockets carry its nets.
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
        S('property', Q('Footprint'), Q(''), S('at', 0, 0, 0),
          S('effects', S('font', S('size', 1.27, 1.27)), 'hide')),
        S('property', Q('Datasheet'), Q('~'), S('at', 0, 0, 0), S('effects', S('font', S('size', 1.27, 1.27)), 'hide')),
        S('property', Q('ki_description'), Q('ESP32-C3 Super Mini dev board, plugs into the J12/J13 sockets'), S('at', 0, 0, 0),
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


# ------------------------------------------------------------------ the design
#
# Rev E layout: every plug on the left, the module right beside the PSU
# header at the right-hand end.  On a 76.8 x 34.5 mm board (y grows
# downwards), left to right:
#
#   fans    the four fan headers stacked down the left edge, pins left to
#           right (G 12V T PWM), their 12 V and GND as two bars down the
#           column
#   col C   the power breakout J14 at the top, the button J9, R1, Q1 and
#           the strip VH J3 at the bottom
#   J11     sense + spare GPIOs, right beside the module's left pad column
#   module  the Super Mini (USB-C at the top edge)
#   right   the PSU header: a RIGHT-ANGLE Mini-Fit Jr on the bottom edge
#           with its mating face pointing UP the board, so the PSU plug lies
#           flat over the board and its wires leave over the top edge.  The
#           whole column above the header is the "plug zone": tracks only,
#           nothing taller than the soldermask, because the plug body sits
#           ~1.3 mm above the board and the wires drape over the rest.
#
# The module's right pads (DIN, GATE, BTN, GPIO0) face the PSU header, so
# their lines go round the module: down the gap between it and the header
# (or down inside its right pad column) and west along the bottom edge under
# its antenna end, where the strip's 3 A feed runs too.
#
# Board outline (the right edge follows the PSU header, see MF_X0)
BX0, BY0 = 100.0, 100.0
BY1 = 134.5

# --- Fan headers: stacked down the left edge, pins left to right: pin 1
# (G) at the edge, then 12V, T, pin 4 (PWM) facing column C.  Top to bottom
# FAN1..FAN4 = GPIO5, 6, 7, 10 = the firmware's FAN_PINS order.  The 12 V
# and GND of all four are two straight bars down the column (pin 2s on the
# front, pin 1s on the back), fed along the top edge from the PSU header.
# 9 mm apart: a PC fan plug is 12.7 x 5.8 mm, so 3.2 mm between the plugs.
# The part is the Molex 47053-1000, the KK 254 4-circuit header the 4-wire
# PC fan spec names: friction-lock ramp over circuits 1-3 only (a 3-pin fan
# fits too); unrotated like this the ramps face down the board.  Pads are
# the KiCad KK-254 footprint's.
FAN_P = 2.54
FAN_PLUG_L = 12.7
FAN_X1 = 103.0                                     # pin 1 (GND) x; pin 4 (PWM) is 7.62 right of it
FAN_YS = [104.0, 113.0, 122.0, 131.0]              # pin row y, FAN1..FAN4
FAN_ORDER = ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO10']
FAN_POS = [(FAN_X1, y) for y in FAN_YS]            # footprint origin (pin 1) of FAN1..FAN4
# the PWM lines' drops between the fans and column C (B): FAN1 up, FAN2 and
# FAN3 down from the lanes under J14, FAN4 down from its lane over J3
PWM_X = [FAN_X1 + 9.24, FAN_X1 + 9.24, FAN_X1 + 10.04, FAN_X1 + 9.24]
V33_X = FAN_X1 + 9.6                     # J14's 3.3V: up the same gap on the front


def fan_pin(k, pin):
    """Board (x, y) of pin `pin` of FANk+1 (unrotated: pin n at x1 + (n-1)p)."""
    x1, y = FAN_POS[k]
    return (x1 + FAN_P * (pin - 1), y)


# --- Column C, between the fans and J11, top to bottom: the power breakout
# J14 (a 2x3 pin header turned 90: three columns of two, 3V3 | 12V | 5VSB,
# each column one rail on both pins), under it the three fan PWM lanes, the
# button J9 (a 4-pin JST-XH: 12V, GND, NO, C left to right), R1, Q1 and the
# strip VH J3 (turned 180: GND, DIN, 3.3V left to right, so its 3.3V pin
# meets the 3 A feed from the PSU header first).  The firmware wants the
# button contact that CLOSES on press (the NC terminal makes the PSU click,
# see the README).
CC_X = BX0 + 15.0                        # column C's left pad column
J14_X0, J14_YB = CC_X, BY0 + 6.6         # J14 pin 1 (bottom of the 3V3 column)
J14_NET = {1: '3.3V', 2: '3.3V', 3: '12V', 4: '12V', 5: '5VSB', 6: '5VSB'}


def j14_xy(pin):
    col, row = (pin - 1) // 2, (pin - 1) % 2
    return (J14_X0 + 2.54 * col, J14_YB - 2.54 * row)


FAN_LANES = [108.2, 109.0, 109.8]        # FAN1..FAN3 PWM west under J14 (B)
J9_X1, J9_Y, XH_P = CC_X + 0.4, 111.6, 2.5      # 0.4 in: its courtyard clears FAN2's
J9_X = [J9_X1 + XH_P * k for k in range(4)]      # pins 1..4: 12V, GND, NO (BTN), C (GND)

# R1 (axial, 7.62 mm pitch, turned 180: pin 1 = GATE on the right, pin 2 =
# GND) and under it Q1 (TO-92 inline WIDE, 2.54 mm between the legs: S G D
# left to right).  The wide footprint is for hand soldering: the stock 1.27
# mm one leaves 0.3 mm between the gate and drain pads, and a bridge there
# puts PS_ON# straight on GPIO3 (the Sep 2026 board 2 fault: the PSU stuck
# on, then clicked).  Bend the legs out.
R1_P = 7.62
R1_X, R1_Y = CC_X + R1_P, 117.0
R1_P1, R1_P2 = (R1_X, R1_Y), (R1_X - R1_P, R1_Y)
Q1_Y = 121.3
Q1_S = CC_X
Q1_G, Q1_D = Q1_S + 2.54, Q1_S + 5.08

VH_P = 3.96
VH_X1, VH_Y = CC_X + 2 * VH_P, 129.2     # J3 pin 1 (3.3V), the right-hand pin
VH_X = [VH_X1 - VH_P * k for k in range(3)]      # pins 1..3: 3.3V, DIN, GND

# the three lines that come west along the bottom and up into column C, in
# the gap between it and J11: PS_ON# (to Q1's drain), GATE (to R1 and Q1's
# gate), BTN (to J9)
GAP_PSON_X, GAP_GATE_X, GAP_BTN_X = CC_X + 10.05, CC_X + 10.85, CC_X + 11.65

# Sense + GPIO breakout J11 between column C and the module: a 2x6 pin
# header.  The INNER column (even pins, facing the module) is, top to
# bottom, SENSE (GPIO2, the BC-250's TPMS1 pin 9) and the module's five
# UNUSED GPIOs: GPIO8, GPIO9, GPIO20, GPIO21 (left column) and GPIO0 (right
# column, brought round under the module's end); the OUTER column (odd pins)
# is GND beside each.  The rows sit half a pitch below the module's rows (the
# GPIO stubs take a short diagonal).  FAN4's PWM line runs down between it
# and the module.
J11_X0 = CC_X + 13.25                    # outer (GND) column x
J11_X1 = J11_X0 + 2.54                   # inner column x
J11_DY = 1.27                            # header rows = module rows + this
J11_GPIO = ['GPIO8', 'GPIO9', 'GPIO20', 'GPIO21', 'GPIO0']
J11_ROWS = ['SENSE'] + J11_GPIO

# --- Super Mini.  Module top edge 3.9 mm inside the board edge (the USB-C
# plug overhangs the board, that is fine).  Its left pads (the fan PWMs and
# J11's GPIOs) face J11, its right pads (5V, GND, DIN, GATE, BTN) the PSU
# header.  Column names come from SM_LEFT / SM_RIGHT above (5V column on the
# RIGHT with the module component-side up, USB-C at the top).
SML = J11_X1 + 3.66                     # left pin column x  (GPIO5..GPIO21)
SM_CX = SML + SM_ROW / 2
SM_CY = BY0 + 3.9 + SM_L / 2
SM_Y0 = SM_CY - SM_PITCH * 3.5          # y of the first pin row
SMR = SM_CX + SM_ROW / 2                # right pin column x (5V..GPIO0)
SM_BOT = SM_CY + SM_L / 2               # module bottom edge (antenna end)
SM_LABEL_DX, SM_LABEL_SIZE = -1.4, 0.6  # pin-name silk (right column): INSIDE the module outline
SM_RIGHT_SILK = ['5V', 'GND', '3V3', 'IO4', 'IO3', 'IO2', 'IO1', 'IO0']
# the two sockets the module plugs into: ref -> (pin 1 x, pin names top to bottom); pin 1 is the top row
SM_SOCKET = {'J12': (SML, SM_LEFT), 'J13': (SMR, SM_RIGHT)}


def sm_y(i):
    return SM_Y0 + SM_PITCH * i


J11Y = [sm_y(k) + J11_DY for k in range(2, 8)]

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
# 6 7 8 9 10 (latch row), left to right, which lines up with the drawing.
# 3.3V comes out at the header's left end, west along the bottom edge to
# the strip VH; 12 V at the right end, up through the plug zone and west
# along the top edge to the fans.
MF_X0, MF_P = SMR + 6.65, 4.2           # the gap from the module: BTN, GATE, the GND spine, PS_ON#'s via
BX1 = MF_X0 + 20.5                      # the header's courtyard ends 0.5 mm inside the right edge
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


# footprint reference text moved off the stock spot: ref -> (x, y) relative
REF_POS = {f'J{5 + k}': (3.81, 4.0) for k in range(4)}
REF_POS['Q1'] = (7.4, 0.0)               # right of the drain pad
REF_POS['R1'] = (3.81, 0.0)              # on its body (turned 180)
REF_POS['J1'] = (-2.4, 2.75)
REF_POS['J3'] = (3.96, 3.6)              # turned 180: below its pins
# named by their legends instead (the stock spots collide with them); the holes need no name
HIDE_REF = {'J9', 'J11', 'J14', 'J5', 'J6', 'J7', 'J8', 'H1', 'H2'}

# M3 mounting holes: a few, where the parts leave room (none fit this layout
# yet: the plug zone must stay flat, and every other spot is spoken for)
HOLES = {}

# Track widths
W_SIG, W_5V, W_12V, W_3A = 0.5, 1.0, 2.0, 2.5
W_PWR = 1.0                              # J14's rails and the button LED feed: about 2 A
W_GND, W_GND3A = 2.0, 1.5
VIA_D, VIA_DRILL = 1.0, 0.5

# Net list — every pad connection in the design.  (ref, pad) -> net
NETS = {}


def net(name, *pads):
    for p in pads:
        assert p not in NETS, p
        NETS[p] = name


# module pin name -> (socket ref, socket pin)
sm_pad = {nm: (ref, str(i + 1)) for ref, (x, names) in SM_SOCKET.items() for i, nm in enumerate(names)}

for pin, n in MF_NET.items():
    net(n, ('J1', str(pin)))
net('5VSB', sm_pad['5V'])
net('GND', sm_pad['GND'], ('J9', '2'), ('J9', '4'), ('Q1', '1'), ('R1', '2'),
    ('J3', '3'), ('J5', '1'), ('J6', '1'), ('J7', '1'), ('J8', '1'))
net('PS_ON#', ('Q1', '3'))
net('GATE', sm_pad['GPIO3'], ('Q1', '2'), ('R1', '1'))
net('BTN', ('J9', '3'), sm_pad['GPIO1'])
net('DIN', sm_pad['GPIO4'], ('J3', '2'))
net('3.3V', ('J3', '1'))
net('12V', ('J5', '2'), ('J6', '2'), ('J7', '2'), ('J8', '2'), ('J9', '1'))
for k, gp in enumerate(FAN_ORDER):
    net(f'FAN_{gp}', sm_pad[gp], (f'J{5 + k}', '4'))
for k, n in enumerate(J11_ROWS):
    NETS[('J11', str(2 * k + 1))] = 'GND'
    net(n, sm_pad['GPIO2' if n == 'SENSE' else n], ('J11', str(2 * k + 2)))
for pin, n in J14_NET.items():
    net(n, ('J14', str(pin)))

NET_NAMES = ['GND', '5VSB', 'PS_ON#', 'GATE', 'SENSE', 'BTN', 'DIN', '3.3V', '12V'] + J11_GPIO + \
            [f'FAN_{g}' for g in ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO10']]
NET_ID = {n: i + 1 for i, n in enumerate(NET_NAMES)}

# Parts: ref -> (symbol lib, symbol, footprint lib, footprint, value, board (x, y, rot))
# Schematic-only parts (no footprint, not in the BOM): the module, drawn wired
# to the nets its socket pins carry.
SCH_ONLY = {'U1': ('bc250_carrier', 'ESP32-C3_SuperMini', 'ESP32-C3 Super Mini: plugs into J12 + J13')}
PARTS = {
    'J12': ('Connector_Generic', 'Conn_01x08', 'Connector_PinSocket_2.54mm', 'PinSocket_1x08_P2.54mm_Vertical',
            'U1 socket L', (SML, SM_Y0, 0)),
    'J13': ('Connector_Generic', 'Conn_01x08', 'Connector_PinSocket_2.54mm', 'PinSocket_1x08_P2.54mm_Vertical',
            'U1 socket R', (SMR, SM_Y0, 0)),
    'J1': ('Connector_Generic', 'Conn_01x10', 'Connector_Molex', 'Molex_Mini-Fit_Jr_5569-10A2_2x05_P4.20mm_Horizontal',
           'PSU Mini-Fit Jr 2x5 R/A', (MF_X0, MF_YF, 0)),
    'J3': ('Connector_Generic', 'Conn_01x03', 'Connector_JST', 'JST_VH_B3P-VH_1x03_P3.96mm_Vertical', 'STRIP',
           (VH_X1, VH_Y, 180)),
    'J9': ('Connector_Generic', 'Conn_01x04', 'Connector_JST', 'JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical', 'BUTTON',
           (J9_X1, J9_Y, 0)),
    'J11': ('Connector_Generic', 'Conn_02x06_Odd_Even', 'Connector_PinHeader_2.54mm',
            'PinHeader_2x06_P2.54mm_Vertical', 'SENSE+GPIO/GND', (J11_X0, J11Y[0], 0)),
    'J14': ('Connector_Generic', 'Conn_02x03_Odd_Even', 'Connector_PinHeader_2.54mm',
            'PinHeader_2x03_P2.54mm_Vertical', 'PWR 3.3V/12V/5VSB (optional)', (J14_X0, J14_YB, 90)),
    'Q1': ('Transistor_FET', '2N7000', 'Package_TO_SOT_THT', 'TO-92_Inline_Wide', '2N7000', (Q1_S, Q1_Y, 0)),
    'R1': ('Device', 'R', 'Resistor_THT', 'R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal', '100k',
           (R1_X, R1_Y, 180)),
}
for k, (x1, y) in enumerate(FAN_POS):
    PARTS[f'J{5 + k}'] = ('Connector_Generic', 'Conn_01x04', 'Connector_Molex',
                          'Molex_KK-254_AE-6410-04A_1x04_P2.54mm_Vertical',
                          f'FAN{k + 1} {FAN_ORDER[k]}', (x1, y, 0))
for name, (hx, hy) in HOLES.items():
    PARTS[name] = ('Mechanical', 'MountingHole', 'MountingHole', 'MountingHole_3.2mm_M3', 'M3', (hx, hy, 0))

# ---------------------------------------------------------------- the tracks
# (net, layer, width, [points]) — polylines; THT pads join both layers, and
# the vias in VIAS do too (fill.py adds the ground stitching vias on top).
#
# Layer plan.  The rails leave the PSU header westward: 12 V (F) up through
# the plug zone and along the top edge to the fans' 12 V bar, with the fans'
# ground (B) right under it back to the header, so the fan current's loop is
# a single strip; 5VSB (F) up the plug zone into the module's 5V pad and on
# over the module's top to J14; 3.3V (F, 3 A) west along the bottom edge
# into the VH, on to the gap by the fans and up it to J14; its return (B)
# right under it, east into a ground spine up the gap between the module and
# the header.  From the module: the fan PWM lines west on the back, under
# J14 (FAN4's round J11's module side and over the VH); DIN (B) down inside
# the module's right pad column and west along the bottom into the VH; BTN
# and GATE (F) down the gap by the header, west along the bottom and up
# beside J11; SENSE (F) over the module's top into J11; PS_ON# (B) through
# the header's row gap, a via, then (F) with BTN and GATE.
#
# The rule behind it: the back is where every joint is soldered, so on B.Cu
# 12 V never runs past a pad of 5VSB, GATE or a GPIO (a bridge there burns
# the module); nets whose short only turns the PSU on or off, or trips it,
# may.  12 V runs on the back only as the header's pin 4-5 tie.

T = []
VIAS = []          # (net, x, y)


def trk(netname, layer, width, *pts):
    T.append((netname, layer, width, list(pts)))


def via(netname, x, y):
    VIAS.append((netname, x, y))


F, B = 'F.Cu', 'B.Cu'
TOP_Y = BY0 + 1.3                        # the top edge: 12 V (F) over the fans' ground (B), both 2.0 mm (~3.5 A)
V5_TOP_Y = TOP_Y + 1.8                   # 5VSB over the module's top to J14 (F), under the 12 V
SENSE_TOP_Y = V5_TOP_Y + 1.1             # SENSE over the module's top into J11 (F), under the 5VSB
IN_X = SMR - 1.62                        # the lane inside the module's right pad column (0.5 clear of the
                                         # pads and of the antenna zone): SENSE up (F), DIN down (B)
EX_BTN_X, EX_GATE_X = SMR + 1.6, SMR + 2.4   # BTN and GATE down the gap by the header (F)
SPINE_X = SMR + 2.3                      # the ground spine up that gap (B)
PSON_VIA_X = SMR + 4.3                   # PS_ON# comes out of the header's row gap here, onto the front
PSON_X = mf_xy(7)[0] + 2.1               # PS_ON#: off pin 8 between the pad columns, up into the row gap,
PSON_GAP_Y = MF_GAP_Y + 0.1              # ... west through it (0.55 clear of pins 6/1, 7/2) to the via (B)
# the lanes west along the bottom edge under the module's antenna end (F), top to bottom
L_GPIO0, L_BTN, L_GATE, L_PSON = 127.75, 128.55, 129.35, 130.15
V33_Y = BY1 - 1.75                       # the 3 A feed (F), lowest
DIN_Y = 131.6                            # DIN west under the VH's 3.3V pin and up into its pin 2 (B)
GND3A_Y = BY1 - 1.25                     # the strip's return, east along the bottom edge (B)
GND_PIN2_Y = MF_YF - 3.5                 # ... and from the spine into the header's pin 2 over pin 1 (B)
GND_WEB_Y = 119.0                        # column C's grounds: J11's GND column west to R1 and Q1 (B)
BTN_UNDER_Y = J9_Y + 1.8                 # BTN under J9 into its pin 3 (F); J9's GNDs tied the same way (B)
FAN4_X, FAN4_Y = (J11_X1 + SML) / 2, 127.0   # FAN4's PWM: down between J11 and the module, west over the VH (B)
STUB_X = SML - 1.68                      # GPIO stubs: diagonal from the pad to here, then straight into J11
FAN_DROP_X = SML - 2.25                  # FAN1's PWM steps down to its lane here, clear of GPIO6's pad

# --- fan PWM (B): FAN1..FAN3 west off the module's top three left pads,
# into three lanes under J14 and through column C to the gap by the fans:
# FAN1's up into its pin 4, FAN2's and FAN3's down (FAN3's the further east,
# so nothing crosses).  FAN4's down between J11 and the module, west under
# J11 and over the VH's pins, down the gap into pin 4.
trk('FAN_GPIO5', B, W_SIG, (SML, sm_y(0)), (FAN_DROP_X, sm_y(0)), (FAN_DROP_X, FAN_LANES[0]),
    (PWM_X[0], FAN_LANES[0]), (PWM_X[0], FAN_YS[0]), fan_pin(0, 4))
trk('FAN_GPIO6', B, W_SIG, (SML, sm_y(1)), (SML - 1.2, FAN_LANES[1]), (PWM_X[1], FAN_LANES[1]),
    (PWM_X[1], FAN_YS[1]), fan_pin(1, 4))
trk('FAN_GPIO7', B, W_SIG, (SML, sm_y(2)), (SML - 1.9, sm_y(2)), (SML - 1.9, FAN_LANES[2]), (PWM_X[2], FAN_LANES[2]),
    (PWM_X[2], FAN_YS[2]), fan_pin(2, 4))
trk('FAN_GPIO10', B, W_SIG, (SML, sm_y(5)), (FAN4_X, sm_y(5)), (FAN4_X, FAN4_Y), (PWM_X[3], FAN4_Y),
    (PWM_X[3], FAN_YS[3]), fan_pin(3, 4))
# --- 12 V: front pins 4 <-> 5 tied (B); up out of pin 4 through the plug
# zone, west along the top edge and down the fans' pin 2s (F); J14's 12 V
# column off it, and on from that column into J9's LED pin (F)
trk('12V', B, W_12V, mf_xy(4), mf_xy(5))
trk('12V', F, W_12V, mf_xy(4), (mf_xy(4)[0], TOP_Y), (fan_pin(0, 2)[0], TOP_Y), fan_pin(3, 2))
trk('12V', F, W_PWR, (j14_xy(4)[0], TOP_Y), j14_xy(4))
trk('12V', F, W_PWR, j14_xy(3), (j14_xy(3)[0], j14_xy(3)[1] + 1.4), (J9_X[0], J9_Y - 1.6), (J9_X[0], J9_Y))
# --- GND: along the top edge right under the 12 V (B), down the fans' pin 1s
# and, at the other end, down into the header's pin 2; the spine down the
# gap by the header, off the top edge, past the module's GND pad (joined)
# and on down to the bottom edge, where the strip's return comes east along
# the edge from the VH (B); a branch off the spine over pin 1 into pin 2
# (B); pin 7 tied behind pin 2 (F), pins 9/10 tied (B) and west to the 7-2
# tie through the row gap (F).
trk('GND', B, W_GND, mf_xy(2), (mf_xy(2)[0], TOP_Y), (fan_pin(0, 1)[0], TOP_Y), fan_pin(3, 1))
trk('GND', B, W_GND3A, (SPINE_X, TOP_Y), (SPINE_X, GND3A_Y))
trk('GND', B, 0.6, (SMR, sm_y(1)), (SPINE_X, sm_y(1)))
trk('GND', B, W_GND3A, (VH_X[2], VH_Y), (VH_X[2], GND3A_Y), (SPINE_X, GND3A_Y))
trk('GND', B, W_GND3A, (SPINE_X, GND_PIN2_Y), (mf_xy(2)[0], GND_PIN2_Y))
trk('GND', F, 2.7, mf_xy(7), mf_xy(2))
trk('GND', B, W_GND, mf_xy(9), mf_xy(10))
trk('GND', F, 0.8, mf_xy(9), (mf_xy(9)[0], MF_GAP_Y), (mf_xy(7)[0], MF_GAP_Y))
# column C's grounds: J11's GND column tied down its pads (B), west to the
# column's left edge, up into R1's pin 2 and down into Q1's source (B); J9's
# pins 2 and 4 tied under the housing, round the NO pin, and pin 2 down onto
# that web (B); Q1's source on down into the VH's GND pin (F)
trk('GND', B, W_SIG, (J11_X0, J11Y[0]), (J11_X0, J11Y[5]))
trk('GND', B, W_SIG, (J11_X0, GND_WEB_Y), (Q1_S, GND_WEB_Y))
trk('GND', B, W_SIG, R1_P2, (Q1_S, Q1_Y))
trk('GND', B, W_SIG, (J9_X[3], J9_Y), (J9_X[3], BTN_UNDER_Y), (J9_X[1], BTN_UNDER_Y))
trk('GND', B, W_SIG, (J9_X[1], J9_Y), (J9_X[1], GND_WEB_Y))
trk('GND', F, W_SIG, (Q1_S, Q1_Y), (VH_X[2], VH_Y))
# --- 3.3V (3 A): rear pin 6 tied behind pin 1 (F); west along the bottom
# edge and up into the VH's pin 1 (F); on from there, narrower, under the
# VH's other pins to the gap by the fans, up it and into J14's 3V3 column (F)
trk('3.3V', F, 2.7, mf_xy(6), mf_xy(1))
trk('3.3V', F, W_3A, mf_xy(6), (mf_xy(6)[0], V33_Y), (VH_X[0], V33_Y), (VH_X[0], VH_Y))
trk('3.3V', F, W_PWR, (VH_X[0], V33_Y), (V33_X, V33_Y), (V33_X, j14_xy(1)[1]), j14_xy(1))
# --- 5VSB: up out of pin 3 through the plug zone and west into the module's
# 5V pad (F); on up over the module's top and west to J14's 5VSB column (F)
trk('5VSB', F, W_5V, mf_xy(3), (mf_xy(3)[0], sm_y(0)), (SMR, sm_y(0)))
trk('5VSB', F, W_PWR, (SMR, sm_y(0)), (SMR, V5_TOP_Y), (j14_xy(6)[0], V5_TOP_Y), j14_xy(6))
# --- J14: each column's two pins tied (F)
for pin in (1, 3, 5):
    trk(J14_NET[pin], F, 1.2, j14_xy(pin), j14_xy(pin + 1))
# --- DIN: out of GPIO4 INTO the module's right column, down it and west
# along the bottom under the VH's 3.3V pin, up into its pin 2 (B)
trk('DIN', B, W_SIG, (SMR, sm_y(3)), (IN_X, sm_y(3)), (IN_X, DIN_Y), (VH_X[1], DIN_Y), (VH_X[1], VH_Y))
# --- BTN: out of GPIO1 east, down the gap by the header, west along the
# bottom, up beside J11 and west under J9 into its pin 3 (F)
trk('BTN', F, W_SIG, (SMR, sm_y(6)), (EX_BTN_X, sm_y(6)), (EX_BTN_X, L_BTN), (GAP_BTN_X, L_BTN),
    (GAP_BTN_X, BTN_UNDER_Y), (J9_X[2], BTN_UNDER_Y), (J9_X[2], J9_Y))
# --- power switch.  GATE: out of GPIO3 east, down the gap by the header,
# west along the bottom, up beside J11 into R1's pin 1 and on down to Q1's
# gate (F).  PS_ON#: off rear pin 8 between the pad columns, up into the row
# gap, west through it and out past the header's left end to a via (B),
# then down to the lowest lane, west along the bottom and up into Q1's
# drain (F): a bridge from it to its neighbours only turns the PSU on or
# off.
trk('GATE', F, W_SIG, (SMR, sm_y(4)), (EX_GATE_X, sm_y(4)), (EX_GATE_X, L_GATE), (GAP_GATE_X, L_GATE),
    (GAP_GATE_X, R1_Y), R1_P1, (Q1_G, Q1_Y - 1.5), (Q1_G, Q1_Y))
trk('PS_ON#', B, W_SIG, mf_xy(8), (PSON_X, mf_xy(8)[1]), (PSON_X, PSON_GAP_Y), (PSON_VIA_X, PSON_GAP_Y))
via('PS_ON#', PSON_VIA_X, PSON_GAP_Y)
trk('PS_ON#', F, W_SIG, (PSON_VIA_X, PSON_GAP_Y), (PSON_VIA_X, L_PSON), (GAP_PSON_X, L_PSON),
    (GAP_PSON_X, Q1_Y), (Q1_D, Q1_Y))
# --- SENSE: off its pad INTO the column, up past the module's top pads, west
# above both sockets and down into J11's top row (F; a slow 1 Hz ADC input,
# the length does not matter)
trk('SENSE', F, W_SIG, (SMR, sm_y(5)), (IN_X, sm_y(5)), (IN_X, SENSE_TOP_Y), (J11_X1, SENSE_TOP_Y),
    (J11_X1, J11Y[0]))
# --- J11's GPIO rows: stubs off the module's left pads, diagonal to STUB_X,
# then straight in; GPIO0 comes down off its pad, west under the module's
# end and up into the bottom row (all F)
for gp, row in (('GPIO8', 1), ('GPIO9', 2), ('GPIO20', 3), ('GPIO21', 4)):
    i = SM_LEFT.index(gp)
    trk(gp, F, W_SIG, (SML, sm_y(i)), (STUB_X, J11Y[row]), (J11_X1, J11Y[row]))
trk('GPIO0', F, W_SIG, (SMR, sm_y(7)), (SMR, L_GPIO0), (J11_X1, L_GPIO0), (J11_X1, J11Y[5]))

# ---------------------------------------------------------- parts to order
# Everything on the board, as LCSC stock numbers (LCSC ships together
# with a JLCPCB board order, and fab.py turns the board parts into each fab's
# assembly BOM).  (refs, LCSC #, "manufacturer part" (one space between the
# two), description, per-board qty, LCSC minimum order).  Every number, name
# and minimum order checked against LCSC's product API on Sep 8 2026; Sep 18
# 2026: the single sense pin and J11 re-picked as one-piece parts an assembly line can fit,
# the fan headers swapped for the Molex part, the module sockets added (all
# lines are in JLCPCB's assembly library, with stock, that day).
ORDER = [
    (['Q1'], 'C9114', 'JSCJ 2N7000', 'N-MOSFET TO-92, 60 V 200 mA (S G D)', 1, 10),
    (['R1'], 'C120103', 'CCO CF1/4W-100K 5%', '100 k axial 1/4 W carbon film, 2.3 x 6.5 mm body', 1, 100),
    (['J1'], 'C22365658', 'DLL DLL-5569-10AW',
     'Mini-Fit Jr 2x5 4.2 mm RIGHT-ANGLE header with snap-in pegs, 9 A (Molex 5569-10A2 clone)', 1, 5),
    (['J3'], 'C160316', 'JST B3P-VH(LF)(SN)', 'VH 3.96 mm 3-pin vertical header, 10 A', 1, 5),
    (['J9'], 'C594232', 'JST B4B-XH-A-G', 'XH 2.5 mm 4-pin vertical header (gold flash)', 1, 5),
    # NOT C41927 (BOOMELE 2.54-4AS): that is a fully shrouded 2.54 mm wafer, a fan plug cannot enter it
    # NOT C140769 (Ckmtw W-2510S04P): its ramp spans all four positions and a PC fan
    # plug's latch wall lands on it -- the Sep 2026 boards needed it cut down
    (['J5', 'J6', 'J7', 'J8'], 'C240840', 'MOLEX 47053-1000',
     'KK 254 2.54 mm 4-circuit fan header, friction-lock ramp over circuits 1-3 (the 4-wire PC fan header; 3- and 4-pin fan plugs mate)', 4, 5),
    (['J12', 'J13'], 'C55218878', 'ShouHan PM2.54-1x8PZZ-H5.7',
     '2.54 mm 1x8 female header, 5.7 mm low profile: the Super Mini plugs into the pair, pins down', 2, 5),
    # two 2x8 headers, cut: a 2x6 for J11 (sense + GPIO, GND beside each) and a 2x3 for J14
    # (rails, optional).  Soldered by hand, so NOT_ASSEMBLED keeps both off an assembly order
    (['J11 + J14'], 'C68234', 'BOOMELE 2.54-2*8P', '2.54 mm 2x8 pin header, cut: a 2x6 (J11 sense/GPIO/GND) and a 2x3 (J14 rails, optional)', 2, 5),
    # mating plugs: strip and button.  J1 mates with the PSU's own plug,
    # the fan headers with the fans' plugs.  Contacts counted with spares.
    (['J3 plug'], 'C157899', 'JST VHR-3N', 'VH 3-way housing', 1, 10),
    (['J3 plug'], 'C160349', 'JST SVH-21T-P1.1', 'VH crimp contact, 18-22 AWG (3 used)', 5, 100),
    (['J9 plug'], 'C144403', 'JST XHP-4', 'XH 4-way housing', 1, 20),
    (['J9 plug'], 'C140573', 'JST SXH-001T-P0.6', 'XH crimp contact, 22-28 AWG (4 used)', 6, 100),
]
NOT_ASSEMBLED = {'J11', 'J14'}     # headers you cut from 2x8s and fit yourself
ORDER_OPTIONAL = [
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


# Ground fill (fill.py fills it): 0.5 mm from every other net, like the
# tracks, 0.5 mm in from the board edge, thermal spokes on every ground pad so
# the pads still take solder, no slivers thinner than GND_MIN_W.
GND_CLEAR, GND_EDGE, GND_SPOKE, GND_MIN_W = 0.5, 0.5, 0.5, 0.5

# Antenna zone: no copper pour, no tracks, under the module's antenna end
KEEPOUT = (SM_CX - 5.5, SM_BOT - 7.0, SM_CX + 5.5, SM_BOT + 1.0)   # tracks are checked as copper, edge to edge

# Silkscreen legends: (text, x, y, rot, justify, size, layer)


def _lab(text, x, y, just, size=1.0, layer='F.SilkS'):
    return (text, x, y, 0, just, size, layer)


SILK = [
    _lab('BC-250 carrier  rev E', MF_X0 + 8.4, 106.2, None, 0.9),
    _lab('plug zone: keep clear', MF_X0 + 8.4, 107.8, None, 0.7),
    # PSU header: the rear (edge) row's names in the row gap on the front, the
    # front row's names in the same gap on the back
    _lab('PSU: row-gap names = EDGE row', MF_X0 + 8.4, 116.6, None, 0.7),        # clear of the peg holes
    ('PSU: row-gap names = INNER row', MF_X0 + 8.4, 116.6, 0, 'mirror', 0.7, 'B.SilkS'),
    ('serial_led_controller', MF_X0 + 8.4, 110.0, 0, 'mirror', 1.0, 'B.SilkS'),
]
for pin in range(6, 11):
    x, y = mf_xy(pin)
    SILK.append(_lab(MF_NET[pin].replace('PS_ON#', 'PS_ON'), x, MF_GAP_Y, None, 0.7))
    SILK.append((MF_NET[pin - 5], x, MF_GAP_Y, 0, 'mirror', 0.7, 'B.SilkS'))
# fans: each one's name above it, FAN1's with the pin legend (left to right)
for k, (x1, y) in enumerate(FAN_POS):
    SILK.append(_lab(f'FAN{k + 1}' + ('  G 12V T PWM' if k == 0 else ''), x1 + 3.81, y - 3.6, None, 0.5))
# J9: pin names under the housing
for k, name in enumerate(('12V', 'GND', 'NO', 'C')):
    SILK.append(_lab(name, J9_X[k], J9_Y + 2.3, None, 0.55))
# J14: each column's rail above it, the name beside it
for pin in (2, 4, 6):
    x, y = j14_xy(pin)
    SILK.append(_lab({'3.3V': '3V3'}.get(J14_NET[pin], J14_NET[pin]), x, y - 2.45, None, 0.5))   # above its outline
# the VH: pin names above its pins, inside the housing outline (turned 180)
for k, name in enumerate(('3.3V', 'DIN', 'GND')):
    SILK.append(_lab(name, VH_X[k], VH_Y - 2.3, None, 0.55))
# J11: its names on the back beside each row (under the module); the front
# says where they are
SILK.append(_lab('J11 SNS/IO', (J11_X0 + J11_X1) / 2, J11Y[0] - 2.35, None, 0.55))
for k, n in enumerate(J11_ROWS):
    SILK.append((n, SML + 3.2, J11Y[k], 0, 'mirror', 0.6, 'B.SilkS'))
SILK.append(('J11: GND outside', SML + 3.2, J11Y[5] + 1.6, 0, 'mirror', 0.6, 'B.SilkS'))

# Silkscreen rectangles: (x0, y0, x1, y1, layer).  The fan headers' bodies
# and 3-position ramps, in KK-254 footprint coordinates (pin 1 at the
# origin, pins along +x, body x -1.38..9.0, y -3.03..2.99), unrotated: the
# ramp (the footprint's +y edge, 1.53 mm deep, over circuits 1-3) faces down.
SILK_RECTS = []
for x1, y in FAN_POS:
    SILK_RECTS.append((x1 - 1.38, y - 3.03, x1 + 9.0, y + 2.99, 'F.SilkS'))                 # body
    SILK_RECTS.append((x1 - 1.38, y + 1.46, x1 + FAN_P * 2.5, y + 2.99, 'F.SilkS'))         # ramp: pins 1-3
NO_STOCK_SILK = {f'J{5 + k}' for k in range(4)}    # the stock outline draws the ramp across all four pins
# the module: a ghost of its outline on the fab layer, what it is and which
# way in on the silk between the sockets, and its right column's pin names
# just inside it.
SILK_RECTS.append((SM_CX - SM_W / 2, SM_CY - SM_L / 2, SM_CX + SM_W / 2, SM_CY + SM_L / 2, 'F.Fab'))
SILK += [
    _lab('USB', SM_CX, SM_CY - SM_L / 2 - 0.9, None, 0.8),
    _lab('U1 ESP32-C3', SM_CX, SM_CY - 6.6, None, 0.8),
    _lab('Super Mini', SM_CX, SM_CY - 5.1, None, 0.8),
    _lab('plugs in here', SM_CX, SM_CY - 3.3, None, 0.7),
    _lab('pins down, USB-C ^', SM_CX, SM_CY - 1.9, None, 0.7),
    _lab('antenna v', SM_CX, SM_BOT - 5.2, None, 0.7),
]
SILK += [_lab(nm, SMR + SM_LABEL_DX, sm_y(i), 'right', SM_LABEL_SIZE) for i, nm in enumerate(SM_RIGHT_SILK)]
# Q1's legs by name, so a reversed part is spotted before power-up (fitted the
# wrong way round, its body diode holds PS_ON# low and the PSU stays on)
SILK += [_lab('S', Q1_S, Q1_Y - 1.75, None, 0.6), _lab('G', Q1_G, Q1_Y - 1.35, None, 0.6), _lab('D', Q1_D, Q1_Y - 1.75, None, 0.6)]


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
    pcb.append(S('title_block', S('title', Q('BC-250 carrier')), S('rev', Q('E')),
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
    # the ground fill: one zone per copper layer over the whole board, filled
    # and stitched by fill.py (KiCad's own filler)
    for layer in ('F.Cu', 'B.Cu'):
        gx0, gy0, gx1, gy1 = BX0 + GND_EDGE, BY0 + GND_EDGE, BX1 - GND_EDGE, BY1 - GND_EDGE
        pcb.append(S('zone', S('net', NET_ID['GND']), S('net_name', Q('GND')), S('layer', Q(layer)),
                     S('tstamp', Q(U())), S('name', Q('GND fill ' + layer[0])), S('hatch', 'edge', 0.508),
                     S('connect_pads', S('clearance', GND_CLEAR)), S('min_thickness', GND_MIN_W),
                     S('filled_areas_thickness', 'no'),
                     S('fill', 'yes', S('thermal_gap', GND_CLEAR), S('thermal_bridge_width', GND_SPOKE),
                       S('island_removal_mode', 0)),
                     S('polygon', S('pts', S('xy', gx0, gy0), S('xy', gx1, gy0), S('xy', gx1, gy1), S('xy', gx0, gy1)))))
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
            S('title_block', S('title', Q('BC-250 carrier')), S('rev', Q('E')),
              S('comment', 1, Q('ESP32-C3 Super Mini carrier: power switch, WS2812B strip, 4 PWM fans'))))
    libsyms = S('lib_symbols')
    seen = set()
    symnodes = {}
    # every part plus the schematic-only ones (no footprint: flib/fname/place None)
    allparts = dict(PARTS)
    allparts.update({ref: (slib, sname, None, None, value, None) for ref, (slib, sname, value) in SCH_ONLY.items()})
    for ref, (slib, sname, flib, fname, value, place) in allparts.items():
        key = f'{slib}:{sname}'
        if key in seen:
            continue
        seen.add(key)
        node = supermini_symbol() if slib == 'bc250_carrier' else copy.deepcopy(load_lib_symbol(slib, sname))
        node[1] = key
        symnodes[key] = node
        libsyms.append(node)
    sch.append(libsyms)
    # U1's pins carry whatever the socket pin they plug into carries
    sch_nets = dict(NETS)
    for i, nm in enumerate(SM_LEFT):
        sch_nets[('U1', str(i + 1))] = NETS.get(sm_pad[nm])
    for i, nm in enumerate(SM_RIGHT):
        sch_nets[('U1', str(i + 9))] = NETS.get(sm_pad[nm])

    # schematic placement (sheet mm)
    SPOS = {
        'U1': (148.59, 96.52), 'J12': (105.41, 96.52), 'J13': (200.66, 96.52),
        'J1': (60.96, 60.96), 'J3': (60.96, 91.44), 'J9': (60.96, 111.76),
        'J11': (27.94, 111.76), 'J14': (27.94, 137.16),
        'Q1': (88.9, 147.32), 'R1': (71.12, 147.32),
        'J5': (236.22, 43.18), 'J6': (236.22, 66.04), 'J7': (236.22, 88.9), 'J8': (236.22, 111.76),
        'H1': (27.94, 175.26), 'H2': (43.18, 175.26),
    }
    STUB = 5.08
    items = []
    for ref, (slib, sname, flib, fname, value, place) in allparts.items():
        key = f'{slib}:{sname}'
        sx, sy = SPOS[ref]
        yn = 'no' if place is None else 'yes'
        node = S('symbol', S('lib_id', Q(key)), S('at', sx, sy, 0), S('unit', 1), S('in_bom', yn),
                 S('on_board', yn), S('dnp', 'no'), S('uuid', Q(U())))
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
        node.append(S('property', Q('Footprint'), Q(f'{flib}:{fname}' if flib else ''), S('at', sx, sy, 0),
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
            netname = sch_nets.get((ref, num))
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
        (20.32, 46.99, 'U1 is not soldered: it plugs, pins down, into the 1x8 sockets J12 (left column) and J13 (right), USB-C at the board edge.'),
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
                "rules": {"min_clearance": 0.3, "min_copper_edge_clearance": 0.3, "min_hole_clearance": 0.25,
                          "min_hole_to_hole": 0.25, "min_microvia_diameter": 0.2, "min_microvia_drill": 0.1,
                          "min_resolved_spokes": 1, "min_silk_clearance": 0.0, "min_text_height": 0.6,
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
        "net_settings": {"classes": [{"bus_width": 12, "clearance": 0.3, "diff_pair_gap": 0.25,
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
    lib = S('kicad_symbol_lib', S('version', 20211014), S('generator', 'generate.py'), supermini_symbol())
    with open(os.path.join(HERE, PROJECT + '.kicad_sym'), 'w') as f:
        f.write(dump(lib) + '\n')


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
