#!/usr/bin/env python3
"""Encode the receiver's fan settings into the blob the firmware reads from
the `fancfg` flash partition at boot. Invoked by the Makefile (`make flash`,
`make flash-fan`); not normally run by hand.

The settings come from the daemon config's "fans" block (--config) — the same
block the daemon reads at runtime — so there is one place to describe the
fans, and the receiver runs the standalone part of it without any daemon at
all: every header's fallback duty, boost and boost length, its ramp, and for
a header whose source is "gpio:N" the curve itself — the receiver samples the
PWM signal on that GPIO and follows the curve on its own. Curves on host
sources (temperatures, loads, hwmon pwm outputs) and the hysteresis are
daemon-only and are not encoded. The header → GPIO map is the board's
(pincheck.FAN_PINS, overridable with "pins" in the block).

The layout must match Config::decode in firmware/main/fan.cpp:

    "FAN4" magic, then enabled(1) pin[6], then per header the record
    CMD_FAN_STANDALONE carries (common/protocol.hpp, common/fanwire.hpp):
    fallback(1) boost(1) boost_secs(1) ramp(1) kind(1) gpio(1) npts(1) pts[8][2]

Slot i is header i+1. Pins are GPIO numbers, 0xFF = header not wired (a
header the block doesn't list — the firmware never configures its output, and
a 4-pin fan plugged into it runs full, per the fan spec's floating-PWM rule;
its record is all 0xFF, which the firmware leaves alone). fallback is the
header's resting duty percent; boost its boost percent, 0xFF = sits the boost
out, boost_secs how long it runs; ramp the slow-down rate in whole percent
per second; kind the source's kind (protocol.hpp FAN_KIND_*: 0 fallback, 1
gpio, 2 host); gpio the input pin of a gpio header (0xFF otherwise); the
points are (input percent, duty percent) pairs, npts of them, sorted. Every
tuning is the header's own; there is nothing global. Older layouts are not
read: `make flash` writes the firmware and this blob together.

The firmware has no console and quietly drops anything it can't use (a pin
past GPIO_NUM_MAX decodes as "not wired", so a typo'd pin silently becomes a
fan that never spins), so this encoder is where mistakes have to be caught:
it validates the block the way the daemon does (daemon/fans.hpp), and the
wiring against the target chip and the pins other features claim
(--strip-pin, --avoid) — outputs and inputs both.

--list-pins prints the listed headers' GPIOs (outputs, then any gpio-source
inputs; comma-separated) instead of writing anything, for the Makefile's
cross-feature collision checks.
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
MAX_POINTS = 8  # common/fancurve.hpp MAX_POINTS = protocol.hpp FAN_CURVE_POINTS
NONE = 0xFF
KIND_FALLBACK, KIND_GPIO, KIND_HOST = 0, 1, 2  # protocol.hpp FAN_KIND_*
HEADER_KEYS = ('name', 'source', 'curve', 'boost', 'fallback')
RECORD_LEN = 7 + 2 * MAX_POINTS  # protocol.hpp FAN_HEADER_LEN
# the header's tunings, as daemon/fans.hpp TUNINGS has them: key, what a bad
# value is told, range, which headers it applies to (kind, has_boost), what
# it is for, default (protocol.hpp FAN_DEFAULT_*, fans.hpp DEFAULT_HYSTERESIS)
TUNINGS = (
    ('hysteresis', 'a number of °C, 0 or more', 0, float('inf'),
     lambda kind, boost, temp: temp, 'a temperature source', 3),
    ('ramp', 'percent per second, 0..255 (0 = at once)', 0, 255,
     lambda kind, boost, temp: kind != KIND_FALLBACK, 'a source with a curve', 5),
    ('boost_seconds', 'seconds, 0..255', 0, 255,
     lambda kind, boost, temp: boost is not None, 'a header with a boost', 5),
)
SOURCE_HELP = ('expected fallback, gpio:N, temp, cpu_load, gpu_load, or a hwmon '
               'chip:label / chip:pwmN')

p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--out', help='output file (required unless --list-pins)')
p.add_argument('--config', required=True,
               help='the daemon config (its "fans" block is what gets encoded; '
                    'no block, or no header = the feature is written off)')
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
               help='print the listed headers\' GPIOs and exit')
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

pins_override = None
headers = {}  # slot -> dict

number = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool)


def parse_source(src, where):
    """Mirror of fans::Controller::parseSource: (kind, gpio, is_temperature)
    or None after an error."""
    if not isinstance(src, str) or not src:
        return err(where, SOURCE_HELP)
    if src == 'constant':
        return err(where, 'renamed: a fixed speed is source "fallback" — the header '
                          'runs its fallback value and takes no curve (move the '
                          'constant into fallback)')
    if src == 'fallback':
        return KIND_FALLBACK, None, False
    if src == 'temp':
        return KIND_HOST, None, True
    if src in ('cpu_load', 'gpu_load'):
        return KIND_HOST, None, False
    chip, sep, label = src.partition(':')
    if not sep:
        return err(where, SOURCE_HELP)
    if chip == 'gpio':
        if not re.fullmatch(r'\d+', label) or int(label) > 48:
            return err(where, 'gpio:N names a receiver GPIO, 0..48')
        return KIND_GPIO, int(label), False
    # chip:label / chip:pwmN — the daemon's; a pwmN label is a percent, the rest °C
    return KIND_HOST, None, not re.fullmatch(r'pwm\d+', label)


def parse_curve(text, kind, where):
    """Mirror of fans::Controller::parseCurve — sorted (x, y) points, or None
    after an error."""
    if not isinstance(text, str):
        return err(where, 'expected a string like "45:35 60:55 75:100"')
    toks = [t for t in re.split(r'[ \t,]+', text.strip()) if t]
    if not toks:
        return err(where, 'empty curve')
    if len(toks) > MAX_POINTS:
        return err(where, f'at most {MAX_POINTS} points')
    pts = []
    for t in toks:
        m = re.fullmatch(r'(-?\d+(?:\.\d+)?):(-?\d+(?:\.\d+)?)', t)
        if not m:
            return err(where, f'"{t}": expected source:percent points, e.g. '
                              '"45:35 60:55 75:100" (a fixed speed is source '
                              '"fallback" with no curve)')
        x, y = float(m.group(1)), float(m.group(2))
        if not 0 <= y <= 100:
            return err(where, f'"{t}": percent must be 0..100')
        if kind == KIND_GPIO and not (0 <= x <= 100 and x == int(x)):
            return err(where, f'"{t}": a gpio curve\'s input is whole percents 0..100 '
                              '(it travels to the receiver as bytes)')
        if any(px == x for px, _ in pts):
            return err(where, f'two points at {m.group(1)}')
        pts.append((x, y))
    return sorted(pts)


for k, v in block.items():
    where = f'fans.{k}'
    if k in ('hysteresis', 'ramp', 'boost_seconds'):
        err(where, "moved: this is each header's own setting now — put it in the "
                   'fans.headerN block it belongs to (README "Fans")')
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
            if kk == 'enabled':
                err(f'{where}.enabled', 'retired — a header listed here is always '
                    'driven; delete the header\'s block to drop it, or give it '
                    'source "fallback" with a fallback of 0 to stop the fan')
            elif kk not in HEADER_KEYS and kk not in [t[0] for t in TUNINGS]:
                err(f'{where}.{kk}', 'unknown key')
        missing = [kk for kk in HEADER_KEYS if kk != 'curve' and kk not in v]
        if missing:
            err(where, f'missing "{missing[0]}" (every header has name, source, '
                       'boost, fallback, and a curve unless the source is fallback)')
            continue
        if not isinstance(v['name'], str):
            err(f'{where}.name', 'expected a string')
        parsed = parse_source(v['source'], f'{where}.source')
        kind, gpio, temp = parsed if parsed else (KIND_HOST, None, False)
        pts = []
        if kind == KIND_FALLBACK:
            if 'curve' in v:
                err(f'{where}.curve', 'a fallback source takes no curve — the header '
                                      'runs its fallback value; delete this key')
        elif 'curve' not in v:
            err(where, 'missing "curve" (source:percent points, e.g. '
                       '"45:35 60:55 75:100")')
        else:
            pts = parse_curve(v['curve'], kind, f'{where}.curve') or []
        b = v['boost']
        if b is not None and not (number(b) and 0 <= b <= 100):
            err(f'{where}.boost', 'expected a percent 0..100, or null for no boost')
        fb = v['fallback']
        if not (number(fb) and 0 <= fb <= 100):
            err(f'{where}.fallback', 'expected a percent 0..100')
        tunings = {}
        for key, rng, lo, hi, applies, only_for, dflt in TUNINGS:
            tunings[key] = dflt
            if key not in v:
                continue
            tv = v[key]
            if not number(tv) or not lo <= tv <= hi:
                err(f'{where}.{key}', f'expected {rng}')
            elif not applies(kind, b, temp):
                err(f'{where}.{key}', f'{key} only applies to {only_for} — delete this key')
            else:
                tunings[key] = tv
        if not errors:
            headers[n - 1] = dict(name=v['name'], kind=kind, gpio=gpio,
                                  pts=[(int(x), int(round(y))) for x, y in pts]
                                  if kind == KIND_GPIO else [],
                                  boost=NONE if b is None else int(round(b)),
                                  boost_secs=int(round(tunings['boost_seconds'])),
                                  ramp=int(round(tunings['ramp'])),
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

for slot in sorted(headers):
    if slot >= len(board_pins):
        err(f'fans.header{slot + 1}',
            f'no GPIO for this header on {a.target or "this target"}'
            + (f' (the board has header1..header{len(board_pins)})' if board_pins else '')
            + ' — add "pins": "5,6,7,10,..." to the fans block for a hand-wired build')

if not errors and headers:
    chip = CHIPS.get(a.target)
    top = chip['max_pin'] if chip else MAX_PIN.get(a.target)

    seen = {}
    for slot in sorted(headers):
        v = board_pins[slot]
        flag = f'fans.header{slot + 1} ({headers[slot]["name"]}, GPIO{v})'
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

    # the gpio sources' INPUT pins. Two headers may read the same signal, but
    # an input can't be an output, the strip, or another feature's pin; and a
    # strap pin is a hard error here — a PWM input at 0 % duty is a pin held
    # LOW, exactly what selects the wrong boot mode at reset
    for slot in sorted(headers):
        h = headers[slot]
        if h['kind'] != KIND_GPIO:
            continue
        v = h['gpio']
        flag = f'fans.header{slot + 1} ({h["name"]}, source gpio:{v})'
        if v in seen:
            err(flag, f'GPIO{v} is {seen[v]}\'s PWM output — an input needs a pin of '
                      'its own (the carrier board breaks out GPIO0, 20 and 21 on J11)')
            continue
        if a.strip_pin >= 0 and v == a.strip_pin:
            err(flag, 'this is the LED data pin (strip.pin in the daemon config)')
            continue
        if v in avoid:
            (errors if a.avoid_hard else warnings).append(
                f'{flag}: GPIO{v} is already {avoid[v]}')
        if top is not None and v > top:
            err(flag, f'{a.target} has no GPIO{v} (0..{top})')
            continue
        if not chip:
            continue
        if v in chip['flash']:
            err(flag, f'GPIO{v} is wired to the SPI flash on {a.target}')
        elif v in chip['reserved']:
            err(flag, f'GPIO{v} is {chip["reserved"][v]} on {a.target}')
        elif v in chip['strap']:
            err(flag, f'GPIO{v} selects the boot mode on {a.target} at reset, and a '
                      'PWM signal at 0 % is a pin held low — the receiver would boot '
                      'into the wrong mode whenever the board asks for 0 %. Use another '
                      'pin')
        elif v in chip['input_only']:
            warnings.append(
                f'{flag}: GPIO{v} has no internal pull-up on {a.target}; a fan '
                "header's PWM is open-drain, so add an external pull-up to 3.3 V "
                'or the input floats')

if errors:
    for e in errors:
        print(f'fancfg: {e}', file=sys.stderr)
    sys.exit(1)
for w in warnings:
    print(f'fancfg warning: {w}', file=sys.stderr)

if a.list_pins:
    outs = [str(board_pins[s]) for s in sorted(headers)]
    ins = sorted({str(headers[s]['gpio']) for s in headers if headers[s]['kind'] == KIND_GPIO})
    print(','.join(outs + ins))
    sys.exit(0)

if not a.out:
    print('fancfg: --out is required', file=sys.stderr)
    sys.exit(1)

wire_pins = [pin_byte(board_pins[i]) if i in headers else NONE
             for i in range(MAX_FANS)]

blob = b'FAN4' + struct.pack('<B6B', 1 if headers else 0, *wire_pins)

for i in range(MAX_FANS):
    h = headers.get(i)
    if not h:
        blob += bytes([NONE] * RECORD_LEN)  # not listed: the firmware leaves it alone
        continue
    pts = h['pts']
    flat = [c for pt in pts for c in pt] + [0] * (2 * (MAX_POINTS - len(pts)))
    blob += bytes([h['fallback'], h['boost'], h['boost_secs'], h['ramp'], h['kind'],
                   NONE if h['gpio'] is None else h['gpio'], len(pts)] + flat)
assert len(blob) == 4 + 1 + MAX_FANS + MAX_FANS * RECORD_LEN

with open(a.out, 'wb') as f:
    f.write(blob)
