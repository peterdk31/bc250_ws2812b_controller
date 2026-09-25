#!/usr/bin/env python3
"""Encode the receiver's fan settings into the blob the firmware reads from
the `fancfg` flash partition at boot. Invoked by the Makefile (`make flash`,
`make flash-fan`); not normally run by hand.

The settings come from the daemon config's "fans" block (--config) — the same
list the daemon reads at runtime — so there is one place to describe the
fans, and the receiver runs the standalone part of it without any daemon at
all: every fan on a receiver output (a header of the board, or a raw GPIO)
with its fallback duty, boost and boost length, its ramp, and for a fan whose
input is "gpio:N" the curve itself — the receiver samples the PWM signal on
that GPIO and follows the curve on its own. Curves on host inputs
(temperatures, loads, hwmon pwm outputs), the hysteresis, and every fan on a
HOST output (a hwmon pwmN the daemon drives) are daemon-only and are not
encoded, beyond being validated here like the daemon validates them.

The fans on receiver outputs take the receiver's slots in list order — the
daemon (daemon/fans.hpp) assigns them the same way, so the partition and the
daemon's push agree. The board's header → GPIO map is the board's
(pincheck.FAN_PINS); it is written into the partition, since the records
name headers by number and the receiver resolves them.

The layout must match Config::decode in firmware/main/fan.cpp:

    "FAN5" magic, then enabled(1) headerPin[6], then per slot the record
    CMD_FAN_STANDALONE carries (common/protocol.hpp, common/fanwire.hpp):
    fallback(1) boost(1) boost_secs(1) ramp(1) kind(1) gpio(1) npts(1) pts[8][2]
    out_kind(1) out(1)

enabled = the config has a fans block (an empty list included: the phone can
add fans to a receiver with none). headerPin[n-1] is header n's GPIO, 0xFF =
no such header. A slot no fan uses is a record of 0xFF throughout (the
receiver leaves its pin undriven — a 4-pin fan plugged into it runs full, per
the fan spec's floating-PWM rule). out_kind is 1 (header: out = the header
number) or 2 (gpio: out = the GPIO); fallback the resting duty percent; boost
its boost percent, 0xFF = sits the boost out, boost_secs how long it runs;
ramp the slow-down rate in whole percent per second; kind the input's kind
(protocol.hpp FAN_KIND_*: 0 fallback, 1 gpio, 2 host); gpio the input pin of
a gpio fan (0xFF otherwise); the points are (input percent, duty percent)
pairs, npts of them, sorted. Every tuning is the fan's own; there is nothing
global. Older layouts are not read: `make flash` writes the firmware and this
blob together.

The firmware has no console, so this encoder is where wiring mistakes are
caught before they reach the board: it validates the block the way the
daemon does, and the wiring against the target chip and the pins other
features claim (--strip-pin, --avoid) — outputs and inputs both. (The
receiver checks every pin again at runtime, since the phone can move them;
it refuses a bad one rather than drive it.)

--list-pins prints the GPIOs the receiver fans drive and read (outputs, then
gpio inputs; comma-separated) instead of writing anything, for the
Makefile's cross-feature collision checks.
"""
import argparse
import json
import re
import struct
import sys

# the per-chip pin facts (flash pads, straps, reserved pins, ...) are shared
# with the power-switch encoder — see tools/pincheck.py
from pincheck import CHIPS, FAN_PINS, MAX_PIN, parse_avoid, pin_byte

MAX_FANS = 6  # LEDC channels on the smallest target (ESP32-C3) = the receiver's slots
MAX_POINTS = 8  # common/fancurve.hpp MAX_POINTS = protocol.hpp FAN_CURVE_POINTS
MAX_GPIO = 48  # daemon/fans.hpp MAX_GPIO
NONE = 0xFF
KIND_FALLBACK, KIND_GPIO, KIND_HOST = 0, 1, 2  # protocol.hpp FAN_KIND_*
OUT_HEADER, OUT_GPIO = 1, 2  # protocol.hpp FAN_OUT_*
FAN_KEYS = ('name', 'output', 'input', 'curve', 'fallback', 'boost')
RECORD_LEN = 9 + 2 * MAX_POINTS  # protocol.hpp FAN_HEADER_LEN
# the fan's tunings, as daemon/fans.hpp TUNINGS has them: key, what a bad
# value is told, range, which fans it applies to (f -> bool), what it is for,
# default (protocol.hpp FAN_DEFAULT_*, fans.hpp DEFAULT_HYSTERESIS)
TUNINGS = (
    ('hysteresis', 'a number of °C, 0 or more', 0, float('inf'),
     lambda f: f['temp'], 'a temperature input', 3),
    ('ramp', 'percent per second, 0..255 (0 = at once)', 0, 255,
     lambda f: f['curve_kind'], 'an input with a curve', 5),
    ('boost_seconds', 'seconds, 0..255', 0, 255,
     lambda f: f['boost'] != NONE, 'a fan with a boost', 5),
)
INPUT_HELP = ('expected fallback, gpio:N, temp, cpu_load, gpu_load, a hwmon chip:label / '
              'chip:pwmN, pmbus:CPU VRM / pmbus:GPU VRM, smu:VRAM hotspot / smu:VRAM 0..7, '
              'file:/path, or (for a host output) "" for the board\'s own curve')
OUTPUT_HELP = ('expected headerN (the receiver\'s header N), gpio:N (a receiver GPIO), '
               'chip:pwmN (a pwm output of this host, e.g. nct6686:pwm2), or "" for none')

p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--out', help='output file (required unless --list-pins)')
p.add_argument('--config', required=True,
               help='the daemon config (its "fans" block is what gets encoded; '
                    'no block = the feature is written off)')
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
               help='print the receiver fans\' GPIOs and exit')
a = p.parse_args()

errors = []
warnings = []


def err(where, what):
    errors.append(f'{where}: {what}')


number = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool)
is_pwm_label = lambda label: re.fullmatch(r'pwm\d+', label) is not None

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
enabled = block is not None
if isinstance(block, dict):
    err('fans', 'the fans block is a list now, each fan naming its output and its input '
                '("source" became "input"; README "Fans") — `led --check` prints the '
                'converted list to paste in')
    block = []
elif block is not None and not isinstance(block, list):
    err('fans', 'expected a list of fans [ { "name", "output", "input", ... }, ... ]')
    block = []
block = block or []


def parse_output(o, where):
    """Mirror of fans::Controller::parseOutput: (kind, num, spec) with kind
    'parked' | 'header' | 'gpio' | 'host', or None after an error."""
    if not isinstance(o, str):
        return err(where, OUTPUT_HELP)
    if o == '':
        return 'parked', None, None
    m = re.fullmatch(r'header([1-9]\d*)', o)
    if o.startswith('header'):
        if not m or int(m.group(1)) > MAX_FANS:
            return err(where, f'headerN names the receiver board\'s header N, header1..header{MAX_FANS} '
                              '(the board has as many as tools/pincheck.py FAN_PINS lists)')
        return 'header', int(m.group(1)), None
    chip, sep, label = o.partition(':')
    if not sep:
        return err(where, OUTPUT_HELP)
    if chip == 'gpio':
        if not re.fullmatch(r'0|[1-9]\d*', label) or int(label) > MAX_GPIO:
            return err(where, f'gpio:N names a receiver GPIO, 0..{MAX_GPIO}')
        return 'gpio', int(label), None
    if not chip or chip in ('file', 'pmbus', 'smu') or not is_pwm_label(label):
        return err(where, OUTPUT_HELP)
    return 'host', None, o


def parse_input(src, out, where):
    """Mirror of fans::Controller::parseInput: (kind, gpio, is_temperature,
    board) or None after an error."""
    if not isinstance(src, str):
        return err(where, INPUT_HELP)
    if not src:
        if out[0] != 'host':
            return err(where, "a blank input is the board's own curve, which only a host output "
                              'has — a fixed speed is input "fallback"')
        return KIND_HOST, None, False, True
    if src == 'constant':
        return err(where, 'renamed: a fixed speed is input "fallback" — the fan runs its '
                          'fallback value and takes no curve (move the constant into fallback)')
    if out[0] == 'host' and src == out[2]:
        return err(where, 'renamed: the board\'s own curve is a blank input now — write "input": ""')
    if src == 'fallback':
        return KIND_FALLBACK, None, False, False
    if src == 'temp':
        return KIND_HOST, None, True, False
    if src in ('cpu_load', 'gpu_load'):
        return KIND_HOST, None, False, False
    chip, sep, label = src.partition(':')
    if not sep:
        return err(where, INPUT_HELP)
    if chip == 'gpio':
        if not re.fullmatch(r'0|[1-9]\d*', label) or int(label) > MAX_GPIO:
            return err(where, f'gpio:N names a receiver GPIO, 0..{MAX_GPIO}')
        return KIND_GPIO, int(label), False, False
    # chip:label / chip:pwmN — the daemon's; a pwmN label is a percent, the rest °C
    return KIND_HOST, None, not is_pwm_label(label), False


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
            return err(where, f'"{t}": expected input:percent points, e.g. '
                              '"45:35 60:55 75:100" (a fixed speed is input '
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


fans = []  # every fan parsed, in list order
for i, v in enumerate(block):
    where = f'fans[{i}]'
    if not isinstance(v, dict):
        err(where, 'expected an object { "name", "output", "input", ... }')
        continue
    before = len(errors)
    for kk in v:
        if kk == 'source':
            err(f'{where}.source', 'renamed: this is "input" now (what the curve reads; '
                                   '"output" is what the fan drives)')
        elif kk not in FAN_KEYS and kk not in [t[0] for t in TUNINGS]:
            err(f'{where}.{kk}', 'unknown key')
    missing = [kk for kk in ('name', 'output', 'input', 'fallback') if kk not in v]
    if missing:
        err(where, f'missing "{missing[0]}" (every fan has a name, an output, an input and a '
                   'fallback, and a curve unless the input is fallback)')
        continue
    if not isinstance(v['name'], str):
        err(f'{where}.name', 'expected a string')
    out = parse_output(v['output'], f'{where}.output')
    if not out:
        continue
    parsed = parse_input(v['input'], out, f'{where}.input')
    if not parsed:
        continue
    kind, gpio, temp, board = parsed
    if kind == KIND_GPIO and out[0] == 'host':
        err(f'{where}.input', 'a gpio input is read by the receiver, which can\'t drive a host '
                              'output — put the fan on a receiver output (headerN / gpio:N), or '
                              'give it a host input')
    if kind == KIND_GPIO and out[0] == 'gpio' and gpio == out[1]:
        err(f'{where}.input', f'GPIO{gpio} can\'t be the fan\'s input and its output at once')
    curve_kind = kind != KIND_FALLBACK and not board
    pts = []
    if not curve_kind:
        if 'curve' in v:
            err(f'{where}.curve', 'the board\'s own curve runs this output — delete this key' if board
                else 'a fallback input takes no curve — the fan runs its fallback value; delete this key')
    elif 'curve' not in v:
        err(where, 'missing "curve" (input:percent points, e.g. "45:35 60:55 75:100")')
    else:
        pts = parse_curve(v['curve'], kind, f'{where}.curve') or []
    b = v.get('boost')
    if b is not None and not (number(b) and 0 <= b <= 100):
        err(f'{where}.boost', 'expected a percent 0..100, or null for no boost')
    elif b is not None and out[0] == 'host':
        err(f'{where}.boost', 'a boost is the receiver\'s, run the moment the host powers on — '
                              'before the daemon exists to drive a host output; write null')
    fb = v['fallback']
    if not (number(fb) and 0 <= fb <= 100):
        err(f'{where}.fallback', 'expected a percent 0..100')
    f = dict(name=v['name'], out=out, kind=kind, gpio=gpio, temp=temp, curve_kind=curve_kind,
             boost=NONE if b is None or not number(b) else int(round(b)))
    tunings = {}
    for key, rng, lo, hi, applies, only_for, dflt in TUNINGS:
        tunings[key] = dflt
        if key not in v:
            continue
        tv = v[key]
        if not number(tv) or not lo <= tv <= hi:
            err(f'{where}.{key}', f'expected {rng}')
        elif not applies(f):
            err(f'{where}.{key}', f'{key} only applies to {only_for} — delete this key')
        else:
            tunings[key] = tv
    if len(errors) == before:
        f.update(pts=[(int(x), int(round(y))) for x, y in pts] if kind == KIND_GPIO else [],
                 boost_secs=int(round(tunings['boost_seconds'])),
                 ramp=int(round(tunings['ramp'])),
                 fallback=int(round(fb)), output=v['output'])
        fans.append((i, f))

# ---- the list: what only the whole list can say, and the slots ----

seen_out = {}
for i, f in fans:
    o = f['output']
    if o and o in seen_out:
        err(f'fans[{i}].output', f'"{o}" is fans[{seen_out[o]}]\'s output already — one fan per output')
    seen_out.setdefault(o, i)

slots = {}  # slot -> (list index, fan)
for i, f in fans:
    if f['out'][0] in ('header', 'gpio'):
        if len(slots) >= MAX_FANS:
            err(f'fans[{i}].output', f'more than {MAX_FANS} fans on receiver outputs — it has '
                                     f'{MAX_FANS} PWM channels')
            continue
        slots[len(slots)] = (i, f)

# ---- the wiring ----

try:
    avoid = parse_avoid(a.avoid)
except ValueError as e:
    errors.append(f'--avoid {e}: expected GPIO:label, e.g. "3:--ps-on '
                  '(power switch)"')
    avoid = {}

board_pins = FAN_PINS.get(a.target, [])
out_pins = {}  # slot -> GPIO
for slot, (i, f) in slots.items():
    kind, n, _ = f['out']
    if kind == 'header':
        if n > len(board_pins):
            err(f'fans[{i}].output', f'the {a.target or "target"} board has no header{n}'
                + (f' (it has header1..header{len(board_pins)})' if board_pins else ' (no header map for it)')
                + ' — use gpio:N for a hand-wired build')
            continue
        out_pins[slot] = board_pins[n - 1]
    else:
        out_pins[slot] = n

if not errors and slots:
    chip = CHIPS.get(a.target)
    top = chip['max_pin'] if chip else MAX_PIN.get(a.target)

    seen = {}
    for slot in sorted(out_pins):
        i, f = slots[slot]
        v = out_pins[slot]
        flag = f'fans[{i}] ({f["name"]}, {f["output"]} = GPIO{v})'
        if v in seen:
            err(flag, f'drives the same pin as {seen[v]}')
            continue
        seen[v] = f'fans[{i}]'

        if a.strip_pin >= 0 and v == a.strip_pin:
            err(flag, 'this is the LED data pin (strip.pin in the daemon config)')
        if v in avoid:
            (errors if a.avoid_hard else warnings).append(
                f'{flag}: GPIO{v} is already {avoid[v]}')

        if top is not None and v > top:
            err(flag, f'{a.target} has no GPIO{v} (0..{top}) — the receiver would refuse to drive it')
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
        elif v in chip['strap'] and f['out'][0] == 'gpio':
            # a raw pin the receiver refuses (the board's header pins are its
            # maker's call, checked below as a warning)
            err(flag, f'GPIO{v} selects the boot mode on {a.target} at reset, and a fan\'s PWM '
                      'input has its own pull-up — the receiver won\'t drive a raw strap pin. '
                      'Pick another')
        elif v in chip['strap']:
            warnings.append(
                f'{flag}: GPIO{v} selects the boot mode on {a.target} at '
                "reset, and a fan's PWM input has its own pull-up — a reset "
                'with the fan connected can drop the chip into the wrong '
                'boot mode. Prefer a non-strap pin')

    # the gpio inputs' pins. Two fans may read the same signal, but an input
    # can't be an output, a header's wire, the strip, or another feature's
    # pin; and a strap pin is a hard error here — a PWM input at 0 % duty is a
    # pin held LOW, exactly what selects the wrong boot mode at reset
    for slot in sorted(slots):
        i, f = slots[slot]
        if f['kind'] != KIND_GPIO:
            continue
        v = f['gpio']
        flag = f'fans[{i}] ({f["name"]}, input gpio:{v})'
        if v in seen:
            err(flag, f'GPIO{v} is {seen[v]}\'s PWM output — an input needs a pin of '
                      'its own (the carrier board breaks out GPIO0, 20 and 21 on J11)')
            continue
        if v in board_pins:
            err(flag, f'GPIO{v} is header{board_pins.index(v) + 1}\'s PWM wire on this board')
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
    outs = [str(out_pins[s]) for s in sorted(out_pins)]
    ins = sorted({str(slots[s][1]['gpio']) for s in slots if slots[s][1]['kind'] == KIND_GPIO})
    print(','.join(outs + ins))
    sys.exit(0)

if not a.out:
    print('fancfg: --out is required', file=sys.stderr)
    sys.exit(1)

header_map = [pin_byte(board_pins[n]) if n < len(board_pins) else NONE for n in range(MAX_FANS)]
blob = b'FAN5' + struct.pack('<B6B', 1 if enabled else 0, *header_map)

for s in range(MAX_FANS):
    if s not in slots:
        blob += bytes([NONE] * RECORD_LEN)  # unused: the receiver drives nothing there
        continue
    f = slots[s][1]
    pts = f['pts']
    flat = [c for pt in pts for c in pt] + [0] * (2 * (MAX_POINTS - len(pts)))
    kind, n, _ = f['out']
    blob += bytes([f['fallback'], f['boost'], f['boost_secs'], f['ramp'], f['kind'],
                   NONE if f['gpio'] is None else f['gpio'], len(pts)] + flat +
                  [OUT_HEADER if kind == 'header' else OUT_GPIO, n])
assert len(blob) == 4 + 1 + MAX_FANS + MAX_FANS * RECORD_LEN

with open(a.out, 'wb') as f:
    f.write(blob)
