#!/usr/bin/env python3
"""Encode the receiver's PWM fan settings into the blob the firmware reads
from the `fancfg` flash partition at boot. Invoked by the Makefile
(`make flash FAN=on ...`, `make flash-fan`); not normally run by hand.

The layout must match Config::decode in firmware/main/fan.cpp:

    "FAN1" magic, then
    enabled(1) pin[6] duty[6] boost_duty(1) boost_secs(1)

Pins are GPIO numbers, 0xFF = channel not wired — channel order is the wire
order, and the daemon's CMD_FAN_DUTY percents map to the wired channels in
that order. Duties are percent (0-100). boost_secs is how long every channel
runs at boost_duty after the host powers on (0 = no boost). Fields are only
ever APPENDED (firmware older than a field reads a shorter blob and ignores
the tail; firmware newer than a blob reads erased flash — 0xFF pins = not
wired, and duty bytes past 100 clamp to 100).

The firmware has no console and quietly drops anything it can't use (a pin
past GPIO_NUM_MAX decodes as "not wired", so a typo'd pin silently becomes a
fan that never spins), so this encoder is where mistakes have to be caught:
it validates the wiring against the target chip and against the pins other
features claim (--strip-pin, --avoid).
"""
import argparse
import struct
import sys

# the per-chip pin facts (flash pads, straps, reserved pins, ...) are shared
# with the power-switch encoder — see tools/pincheck.py
from pincheck import CHIPS, MAX_PIN, parse_avoid, pin_byte

MAX_FANS = 6  # LEDC channels on the smallest target (ESP32-C3)


def int_list(s):
    return [int(v.strip()) for v in s.split(',') if v.strip() != '']


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', required=True, help='output file')
p.add_argument('--disabled', action='store_true',
               help='write the config with the feature switched off')
p.add_argument('--target', default='',
               help='chip the config is for (validates pins, e.g. esp32c3)')
p.add_argument('--strip-pin', type=int, default=-1,
               help='the LED data pin (strip.pin), to catch collisions')
p.add_argument('--pins', default='5,6,7,10',
               help='comma-separated GPIO numbers, one per fan PWM wire, in '
                    'channel order (max %d)' % MAX_FANS)
p.add_argument('--duty', default='100',
               help='duty percent per channel (comma-separated, or one value '
                    'for all). 100 is the safe cooling default; the daemon '
                    'config ("fans.duty") can lower it at runtime. Keep an '
                    'AIO pump high — 100 ideal, never below ~30')
p.add_argument('--boost-duty', type=int, default=100,
               help='duty percent every channel runs at right after the host '
                    'powers on (primes an AIO pump)')
p.add_argument('--boost-secs', type=int, default=5,
               help='how long the boost lasts, seconds (0 = no boost)')
p.add_argument('--avoid', action='append', metavar='GPIO:label', default=[],
               help='a pin some other feature claims (e.g. "3:--ps-on (power '
                    'switch)"), to catch cross-feature collisions; repeatable')
p.add_argument('--avoid-hard', action='store_true',
               help='make an --avoid collision an error instead of a warning '
                    '(when the other feature is being flashed alongside, so '
                    'its pins are known-true)')
a = p.parse_args()

errors = []
warnings = []

try:
    avoid = parse_avoid(a.avoid)
except ValueError as e:
    errors.append(f'--avoid {e}: expected GPIO:label, e.g. "3:--ps-on '
                  '(power switch)"')
    avoid = {}

try:
    pins = int_list(a.pins)
except ValueError:
    errors.append(f'--pins {a.pins}: expected comma-separated GPIO numbers')
    pins = []

try:
    duty = int_list(a.duty)
except ValueError:
    errors.append(f'--duty {a.duty}: expected comma-separated percents')
    duty = [100]

if len(duty) == 1:
    duty = duty * max(len(pins), 1)
if pins and len(duty) != len(pins):
    errors.append(f'--duty: {len(duty)} values for {len(pins)} pins — give '
                  'one per pin, or a single value for all')
for v in duty:
    if not 0 <= v <= 100:
        errors.append(f'--duty {v}: must be 0..100 percent')
if not 0 <= a.boost_duty <= 100:
    errors.append(f'--boost-duty {a.boost_duty}: must be 0..100 percent')
if not 0 <= a.boost_secs <= 255:
    errors.append(f'--boost-secs {a.boost_secs}: must be 0..255 seconds')

if len(pins) > MAX_FANS:
    errors.append(f'--pins: {len(pins)} pins, but the firmware drives at '
                  f'most {MAX_FANS} fan channels (LEDC on the C3)')
for v in pins:
    if not 0 <= v <= 0xFE:
        errors.append(f'--pins {v}: not a GPIO number')

# The wiring only matters when the feature is on: FAN=off must never be
# refused over pins the firmware will ignore (they may be another chip's).
if not a.disabled and not errors:
    chip = CHIPS.get(a.target)
    top = chip['max_pin'] if chip else MAX_PIN.get(a.target)

    if not pins:
        errors.append('--pins is required: enabled with no fan wired, the '
                      'feature would silently do nothing')

    seen = {}
    for i, v in enumerate(pins):
        flag = f'--pins[{i}]'
        if v in seen:
            errors.append(f'{flag} and {seen[v]} are both GPIO{v}')
            continue
        seen[v] = flag

        if a.strip_pin >= 0 and v == a.strip_pin:
            errors.append(f'{flag} {v}: GPIO{v} is the LED data pin '
                          '(strip.pin in the daemon config)')
        if v in avoid:
            (errors if a.avoid_hard else warnings).append(
                f'{flag} {v}: GPIO{v} is already {avoid[v]}')

        if top is not None and v > top:
            errors.append(f'{flag} {v}: {a.target} has no GPIO{v} '
                          f'(0..{top}) — the firmware would silently treat '
                          'it as not wired')
            continue
        if not chip:
            continue
        if v in chip['flash']:
            errors.append(f'{flag} {v}: GPIO{v} is wired to the SPI flash '
                          f'on {a.target}')
        elif v in chip['reserved']:
            errors.append(f'{flag} {v}: GPIO{v} is {chip["reserved"][v]} '
                          f'on {a.target}')
        elif v in chip['input_only']:
            errors.append(f'{flag} {v}: GPIO{v} is input-only on {a.target} '
                          '— it cannot drive a PWM wire')
        elif v in chip['strap']:
            warnings.append(
                f'{flag} {v}: GPIO{v} selects the boot mode on {a.target} at '
                "reset, and a fan's PWM input has its own pull-up — a reset "
                'with the fan connected can drop the chip into the wrong '
                'boot mode. Prefer a non-strap pin')

if errors:
    for e in errors:
        print(f'fancfg: {e}', file=sys.stderr)
    sys.exit(1)
for w in warnings:
    print(f'fancfg warning: {w}', file=sys.stderr)

wire_pins = [pin_byte(pins[i]) if i < len(pins) else 0xFF
             for i in range(MAX_FANS)]
wire_duty = [duty[i] if i < len(duty) else 100 for i in range(MAX_FANS)]

blob = b'FAN1' + struct.pack(
    '<B6B6BBB',
    0 if a.disabled else 1,
    *wire_pins,
    *wire_duty,
    a.boost_duty, a.boost_secs)

with open(a.out, 'wb') as f:
    f.write(blob)
