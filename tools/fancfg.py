#!/usr/bin/env python3
"""Encode the receiver's fan settings into the blob the firmware reads from
the `fancfg` flash partition at boot. Invoked by the Makefile (`make flash`,
`make flash-fan`); not normally run by hand.

The settings come from the daemon config's "fans" block (--config) — the same
block the daemon reads at runtime — so there is one place to describe the
fans, and the receiver runs the standalone part of it (fallback duty, boost,
boost length) without any daemon at all. Curves and sources are daemon-only
and are not encoded. The header → GPIO map is the board's (pincheck.FAN_PINS,
overridable with "pins" in the block).

The layout must match Config::decode in firmware/main/fan.cpp:

    "FAN2" magic, then
    enabled(1) boost_secs(1) pin[6] duty[6] boost[6]

Slot i is header i+1. Pins are GPIO numbers, 0xFF = header not wired (a
disabled header — the firmware never configures its output, and a 4-pin fan
plugged into it runs full, per the fan spec's floating-PWM rule). duty is the
header's fallback percent; boost its boost percent, 0xFF = sits the boost out.
Fields are only ever APPENDED (firmware newer than a blob reads erased flash
for the tail: 0xFF pins = not wired, duty bytes past 100 clamp to 100). The
firmware also still reads the original "FAN1" layout.

The firmware has no console and quietly drops anything it can't use (a pin
past GPIO_NUM_MAX decodes as "not wired", so a typo'd pin silently becomes a
fan that never spins), so this encoder is where mistakes have to be caught:
it validates the block the way the daemon does (daemon/fans.hpp), and the
wiring against the target chip and the pins other features claim
(--strip-pin, --avoid).

--list-pins prints the enabled headers' GPIOs (comma-separated) instead of
writing anything, for the Makefile's cross-feature collision checks.
"""
import argparse
import json
import re
import struct
import sys

# the per-chip pin facts (flash pads, straps, reserved pins, ...) are shared
# with the power-switch encoder — see tools/pincheck.py
from pincheck import CHIPS, FAN_PINS, MAX_PIN, parse_avoid, pin_byte

MAX_FANS = 6  # LEDC channels on the smallest target (ESP32-C3)
NONE = 0xFF
HEADER_KEYS = ('enabled', 'name', 'source', 'curve', 'boost', 'fallback')
SOURCE_WORDS = ('constant', 'temp', 'cpu_load', 'gpu_load')

p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--out', help='output file (required unless --list-pins)')
p.add_argument('--config', required=True,
               help='the daemon config (its "fans" block is what gets encoded; '
                    'no block, or no enabled header = the feature is written off)')
p.add_argument('--target', default='',
               help='chip the config is for (validates pins, e.g. esp32c3)')
p.add_argument('--strip-pin', type=int, default=-1,
               help='the LED data pin (strip.pin), to catch collisions')
p.add_argument('--avoid', action='append', metavar='GPIO:label', default=[],
               help='a pin some other feature claims (e.g. "3:--ps-on (power '
                    'switch)"), to catch cross-feature collisions; repeatable')
p.add_argument('--avoid-hard', action='store_true',
               help='make an --avoid collision an error instead of a warning '
                    '(when the other feature is being flashed alongside, so '
                    'its pins are known-true)')
p.add_argument('--list-pins', action='store_true',
               help='print the enabled headers\' GPIOs and exit')
a = p.parse_args()

errors = []
warnings = []


def err(where, what):
    errors.append(f'{where}: {what}')


# ---- the config block ----

try:
    with open(a.config) as f:
        root = json.load(f)
except OSError as e:
    print(f'fancfg: {a.config}: {e.strerror}', file=sys.stderr)
    sys.exit(1)
except ValueError as e:
    print(f'fancfg: {a.config}: {e}', file=sys.stderr)
    sys.exit(1)

block = root.get('fans') if isinstance(root, dict) else None
if block is not None and not isinstance(block, dict):
    err('fans', 'expected an object')
    block = None
block = block or {}

boost_secs = 5
pins_override = None
headers = {}  # slot -> dict

number = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool)


def parse_curve(text, constant, where):
    """Mirror of fans::Controller::parseCurve — only the shape matters here."""
    toks = [t for t in re.split(r'[ \t,]+', str(text).strip()) if t]
    if not toks:
        return err(where, 'empty curve')
    if constant:
        if len(toks) != 1 or not re.fullmatch(r'-?\d+(\.\d+)?', toks[0]):
            return err(where, 'a constant source takes one value, e.g. "65"')
        if not 0 <= float(toks[0]) <= 100:
            return err(where, 'percent must be 0..100')
        return
    xs = set()
    for t in toks:
        m = re.fullmatch(r'(-?\d+(?:\.\d+)?):(-?\d+(?:\.\d+)?)', t)
        if not m:
            return err(where, f'"{t}": expected source:percent points, e.g. '
                              '"45:35 60:55 75:100" (a bare value needs source '
                              '"constant")')
        x, y = float(m.group(1)), float(m.group(2))
        if not 0 <= y <= 100:
            return err(where, f'"{t}": percent must be 0..100')
        if x in xs:
            return err(where, f'two points at {m.group(1)}')
        xs.add(x)


for k, v in block.items():
    where = f'fans.{k}'
    if k == 'hysteresis':
        if not number(v) or v < 0:
            err(where, 'expected a number of °C, 0 or more')
    elif k == 'ramp':
        if not number(v) or v < 0:
            err(where, 'expected percent per second, 0 or more')
    elif k == 'boost_seconds':
        if not number(v) or not 0 <= v <= 255:
            err(where, 'expected seconds, 0..255')
        else:
            boost_secs = int(v)
    elif k == 'pins':
        try:
            vals = v if isinstance(v, list) else str(v).split(',')
            pins_override = [int(str(x).strip()) for x in vals if str(x).strip()]
        except ValueError:
            err(where, 'expected "5,6,7,10" (or an array of GPIO numbers)')
    elif re.fullmatch(r'header[1-9]\d*', k):
        n = int(k[6:])
        if not 1 <= n <= MAX_FANS:
            err(where, f'no such header (header1..header{MAX_FANS})')
            continue
        if not isinstance(v, dict):
            err(where, 'expected an object')
            continue
        for kk in v:
            if kk not in HEADER_KEYS:
                err(f'{where}.{kk}', 'unknown key')
        missing = [kk for kk in HEADER_KEYS if kk not in v]
        if missing:
            err(where, f'missing "{missing[0]}" (every header has '
                       + ', '.join(HEADER_KEYS) + ')')
            continue
        if not isinstance(v['enabled'], bool):
            err(f'{where}.enabled', 'expected true or false')
        if not isinstance(v['name'], str):
            err(f'{where}.name', 'expected a string')
        src = v['source']
        if not isinstance(src, str) or not src:
            err(f'{where}.source', 'expected constant, temp, cpu_load, gpu_load, '
                                   'or a hwmon chip:label / chip:pwmN')
            src = ''
        if not (isinstance(v['curve'], str) or number(v['curve'])):
            err(f'{where}.curve', 'expected a string like "45:35 60:55 75:100"')
        else:
            parse_curve(v['curve'], src == 'constant', f'{where}.curve')
        b = v['boost']
        if b is not None and not (number(b) and 0 <= b <= 100):
            err(f'{where}.boost', 'expected a percent 0..100, or null for no boost')
        fb = v['fallback']
        if not (number(fb) and 0 <= fb <= 100):
            err(f'{where}.fallback', 'expected a percent 0..100')
        if not errors:
            headers[n - 1] = dict(enabled=v['enabled'], name=v['name'],
                                  boost=NONE if b is None else int(round(b)),
                                  fallback=int(round(fb)))
    else:
        err(where, 'unknown key')

# ---- the wiring ----

try:
    avoid = parse_avoid(a.avoid)
except ValueError as e:
    errors.append(f'--avoid {e}: expected GPIO:label, e.g. "3:--ps-on '
                  '(power switch)"')
    avoid = {}

board_pins = pins_override if pins_override is not None else FAN_PINS.get(a.target, [])
if pins_override is not None and len(pins_override) > MAX_FANS:
    err('fans.pins', f'{len(pins_override)} pins, but the firmware drives at '
                     f'most {MAX_FANS} headers')

enabled = {slot: h for slot, h in headers.items() if h['enabled']}

for slot in sorted(enabled):
    if slot >= len(board_pins):
        err(f'fans.header{slot + 1}',
            f'no GPIO for this header on {a.target or "this target"}'
            + (f' (the board has header1..header{len(board_pins)})' if board_pins else '')
            + ' — add "pins": "5,6,7,10,..." to the fans block for a hand-wired build')

if not errors and enabled:
    chip = CHIPS.get(a.target)
    top = chip['max_pin'] if chip else MAX_PIN.get(a.target)

    seen = {}
    for slot in sorted(enabled):
        v = board_pins[slot]
        flag = f'fans.header{slot + 1} ({enabled[slot]["name"]}, GPIO{v})'
        if v in seen:
            err(flag, f'shares its pin with {seen[v]} — check "pins"')
            continue
        seen[v] = f'header{slot + 1}'

        if a.strip_pin >= 0 and v == a.strip_pin:
            err(flag, 'this is the LED data pin (strip.pin in the daemon config)')
        if v in avoid:
            (errors if a.avoid_hard else warnings).append(
                f'{flag}: GPIO{v} is already {avoid[v]}')

        if top is not None and v > top:
            err(flag, f'{a.target} has no GPIO{v} (0..{top}) — the firmware '
                      'would silently treat it as not wired')
            continue
        if not chip:
            continue
        if v in chip['flash']:
            err(flag, f'GPIO{v} is wired to the SPI flash on {a.target}')
        elif v in chip['reserved']:
            err(flag, f'GPIO{v} is {chip["reserved"][v]} on {a.target}')
        elif v in chip['input_only']:
            err(flag, f'GPIO{v} is input-only on {a.target} — it cannot drive '
                      'a PWM wire')
        elif v in chip['strap']:
            warnings.append(
                f'{flag}: GPIO{v} selects the boot mode on {a.target} at '
                "reset, and a fan's PWM input has its own pull-up — a reset "
                'with the fan connected can drop the chip into the wrong '
                'boot mode. Prefer a non-strap pin')

if errors:
    for e in errors:
        print(f'fancfg: {e}', file=sys.stderr)
    sys.exit(1)
for w in warnings:
    print(f'fancfg warning: {w}', file=sys.stderr)

if a.list_pins:
    print(','.join(str(board_pins[s]) for s in sorted(enabled)))
    sys.exit(0)

if not a.out:
    print('fancfg: --out is required', file=sys.stderr)
    sys.exit(1)

wire_pins = [pin_byte(board_pins[i]) if i in enabled else NONE
             for i in range(MAX_FANS)]
wire_duty = [enabled[i]['fallback'] if i in enabled else 100
             for i in range(MAX_FANS)]
wire_boost = [enabled[i]['boost'] if i in enabled else NONE
              for i in range(MAX_FANS)]

blob = b'FAN2' + struct.pack(
    '<BB6B6B6B',
    1 if enabled else 0,
    boost_secs,
    *wire_pins,
    *wire_duty,
    *wire_boost)

with open(a.out, 'wb') as f:
    f.write(blob)
