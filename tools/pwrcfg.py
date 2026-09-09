#!/usr/bin/env python3
"""Encode the receiver's power-switch settings into the blob the firmware
reads from the `pwrcfg` flash partition at boot. Invoked by the Makefile
(`make flash`, `make flash-pwr`); not normally run by hand.

The settings come from the daemon config's "power_switch" block (--config):

    "power_switch": {
        "enabled": true,
        "hold_seconds": 2, "boot_timeout_seconds": 10,
        "sense_low_mv": 800, "sense_high_mv": 2000,
        "pins": { "ps_on": 3, "button": 1, "button_gnd": null, "sense": 2, "led": 8 },
        "short_press": "systemctl poweroff"     // the daemon's; null = ignore
    }

A pin of null is "not wired" (button_gnd: the button goes to a real GND;
sense: no sense wire; led: no feedback LED). With NO block at all this exits
3 and writes nothing — the Makefile then leaves whatever is on the chip
alone. That is deliberately not the fans' rule (no block = written off):
writing the power switch off releases PS_ON# once the receiver reboots, i.e.
it cuts the machine's power, so only an explicit "enabled": false may do it.

The layout must match Config::decode in firmware/main/power_switch.cpp:

    "PWR1" magic, then
    enabled(1) button_pin(1) ps_on_pin(1) button_gnd_pin(1) sense_pin(1)
    hold_ms(2) boot_timeout_ms(2) sense_low_mv(2) sense_high_mv(2)
    led_pin(1)

u16s little-endian; pins are GPIO numbers, 0xFF = not wired. Fields are only
ever APPENDED (firmware older than a field reads a shorter blob and ignores
the tail; firmware newer than a blob reads erased flash, 0xFF = not wired).

The firmware has no console and quietly drops anything it can't use (a pin
past GPIO_NUM_MAX decodes as "not wired" — for PS_ON that means the whole
feature is off), so this encoder is where mistakes have to be caught: it
validates the wiring against the target chip and refuses to write a config
that would fail silently on the board whose power this partition controls.

--list-pins prints the wired pins (comma-separated) instead of writing, for
the Makefile's cross-feature collision checks; nothing when the block is
absent or disabled.
"""
import argparse
import json
import struct
import sys

# the per-chip pin facts (flash pads, ADC pins, straps, ...) are shared with
# the fan encoder — see tools/pincheck.py, the single source of truth
from pincheck import CHIPS, MAX_PIN, parse_avoid, pin_byte as pin

KEYS = ('enabled', 'hold_seconds', 'boot_timeout_seconds', 'sense_low_mv',
        'sense_high_mv', 'pins', 'short_press')
PIN_KEYS = ('ps_on', 'button', 'button_gnd', 'sense', 'led')
NO_BLOCK = 3  # exit code: no power_switch block, leave the chip alone

p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--out', help='output file (required unless --list-pins)')
p.add_argument('--config', required=True,
               help='the daemon config (its "power_switch" block is what gets '
                    'encoded)')
p.add_argument('--target', default='',
               help='chip the config is for (validates pins, e.g. esp32c3)')
p.add_argument('--strip-pin', type=int, default=-1,
               help='the LED data pin (strip.pin), to catch collisions')
p.add_argument('--avoid', action='append', metavar='GPIO:label', default=[],
               help='a pin some other feature claims (e.g. "5:a fan header"), '
                    'to catch cross-feature collisions; repeatable')
p.add_argument('--avoid-hard', action='store_true',
               help='make an --avoid collision an error instead of a warning '
                    '(when the other feature is being flashed alongside, so '
                    'its pins are known-true)')
p.add_argument('--list-pins', action='store_true',
               help='print the wired GPIOs and exit')
a = p.parse_args()

errors = []
warnings = []


def err(where, what):
    errors.append(f'{where}: {what}')


number = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool)

# ---- the config block ----

try:
    with open(a.config) as f:
        root = json.load(f)
except OSError as e:
    print(f'pwrcfg: {a.config}: {e.strerror}', file=sys.stderr)
    sys.exit(1)
except ValueError as e:
    print(f'pwrcfg: {a.config}: {e}', file=sys.stderr)
    sys.exit(1)

block = root.get('power_switch') if isinstance(root, dict) else None
if block is None:
    if a.list_pins:
        print('')
        sys.exit(0)
    print('pwrcfg: no "power_switch" block in the config — leaving the '
          "chip's power-switch settings alone", file=sys.stderr)
    sys.exit(NO_BLOCK)
if not isinstance(block, dict):
    print('pwrcfg: power_switch: expected an object', file=sys.stderr)
    sys.exit(1)

for k in block:
    if k not in KEYS:
        err(f'power_switch.{k}', 'unknown key')
for k in KEYS:
    if k not in block:
        err('power_switch', f'missing "{k}" (the block has ' + ', '.join(KEYS) + ')')
if errors:
    for e in errors:
        print(f'pwrcfg: {e}', file=sys.stderr)
    sys.exit(1)

enabled = block['enabled']
if not isinstance(enabled, bool):
    err('power_switch.enabled', 'expected true or false')
    enabled = False

hold_ms = boot_timeout_ms = 0
if not number(block['hold_seconds']):
    err('power_switch.hold_seconds', 'expected seconds')
else:
    hold_ms = round(block['hold_seconds'] * 1000)
    if not 100 <= hold_ms <= 65535:
        err('power_switch.hold_seconds', 'must be 0.1..65.535 seconds')
if not number(block['boot_timeout_seconds']):
    err('power_switch.boot_timeout_seconds', 'expected seconds')
else:
    boot_timeout_ms = round(block['boot_timeout_seconds'] * 1000)
    if not 1000 <= boot_timeout_ms <= 65535:
        err('power_switch.boot_timeout_seconds', 'must be 1..65.535 seconds')

sense_low = block['sense_low_mv']
sense_high = block['sense_high_mv']
if not (number(sense_low) and number(sense_high)):
    err('power_switch.sense_low_mv/sense_high_mv', 'expected millivolts')
    sense_low, sense_high = 0, 1
elif not 0 <= sense_low <= 65535 or not 0 <= sense_high <= 65535:
    err('power_switch.sense_low_mv/sense_high_mv', 'must be 0..65535 mV')
elif sense_low >= sense_high:
    err('power_switch.sense_low_mv',
        f'{sense_low} must be below sense_high_mv {sense_high}: inverted '
        'hysteresis never settles, so the boot timeout would cut the PSU '
        'shortly after every power-on')
sense_low, sense_high = int(sense_low), int(sense_high)

pins = {k: -1 for k in PIN_KEYS}
pb = block['pins']
if not isinstance(pb, dict):
    err('power_switch.pins', 'expected { "ps_on": 3, "button": 1, "button_gnd": null, '
                             '"sense": 2, "led": 8 }')
else:
    for k in pb:
        if k not in PIN_KEYS:
            err(f'power_switch.pins.{k}', 'unknown pin (' + ', '.join(PIN_KEYS) + ')')
    for k in PIN_KEYS:
        if k not in pb:
            err('power_switch.pins', f'missing "{k}" (null = not wired)')
            continue
        v = pb[k]
        if v is None:
            pins[k] = -1
        elif number(v) and 0 <= v <= 0xFE and int(v) == v:
            pins[k] = int(v)
        else:
            err(f'power_switch.pins.{k}', f'{v!r}: not a GPIO number (null = not wired)')

sp = block['short_press']
if sp is not None and not (isinstance(sp, str) and sp.strip()):
    err('power_switch.short_press', 'expected a command string, or null to ignore '
                                    'a short press')

# ---- the wiring ----

try:
    avoid = parse_avoid(a.avoid)
except ValueError as e:
    errors.append(f'--avoid {e}: expected GPIO:label, e.g. "5:a fan header"')
    avoid = {}

# The wiring only matters when the feature is on: "enabled": false must never
# be refused over pins the firmware will ignore (they may be another chip's).
if enabled and not errors:
    chip = CHIPS.get(a.target)
    top = chip['max_pin'] if chip else MAX_PIN.get(a.target)

    if pins['ps_on'] < 0:
        err('power_switch.pins.ps_on', 'required: without the PS_ON# wire there '
                                       'is no power switch')
    if pins['button'] < 0:
        err('power_switch.pins.button', 'required: enabled but buttonless, the '
                                        'machine could never be powered on — '
                                        'worse than the jumper this replaces')

    wired = {k: v for k, v in pins.items() if v >= 0}
    where = lambda k: f'power_switch.pins.{k}'

    seen = {}
    for k, v in wired.items():
        if v in seen:
            err(where(k), f'{k} and {seen[v]} are both GPIO{v}')
        else:
            seen[v] = k
    if a.strip_pin >= 0 and a.strip_pin in seen:
        err(where(seen[a.strip_pin]), f'GPIO{a.strip_pin} is the LED data pin '
                                      '(strip.pin in the daemon config)')
    for v, label in avoid.items():
        if v in seen:
            (errors if a.avoid_hard else warnings).append(
                f'{where(seen[v])}: GPIO{v} is already {label}')

    for k, v in wired.items():
        if top is not None and v > top:
            err(where(k), f'{a.target} has no GPIO{v} (0..{top}) — the firmware '
                          'would silently treat it as not wired')
            continue
        if not chip:
            continue
        if v in chip['flash']:
            err(where(k), f'GPIO{v} is wired to the SPI flash on {a.target}')
        elif v in chip['reserved']:
            err(where(k), f'GPIO{v} is {chip["reserved"][v]} on {a.target}')
        elif v in chip['input_only'] and k != 'sense':
            err(where(k), f'GPIO{v} is input-only on {a.target} (and has no '
                          'internal pull-up, so not even the button works there)')
        if k == 'sense' and v not in chip['adc']:
            err(where(k), f'GPIO{v} is not ADC-capable on {a.target} — the '
                          'firmware would silently run without sense (no '
                          'shutdown follow-down, no boot timeout)')
        elif k == 'sense' and v in chip['strap']:
            warnings.append(
                f'{where(k)}: GPIO{v} selects the boot mode on {a.target} at '
                'reset, and the sense line sits at 0 V whenever the machine is '
                'off (on the BC-250 it is a dead 3.3 V rail) — so a reset while '
                'the machine is down can drop the chip into the wrong boot '
                'mode. Prefer an ADC pin that is not a boot-mode strap')
        if k == 'ps_on' and chip['hold'] is not None and v not in chip['hold']:
            warnings.append(
                f'{where(k)}: gpio_hold cannot latch GPIO{v} through a reset on '
                f'{a.target} — a crash briefly floats PS_ON#; an RTC pin (0, 2, '
                '4, 12-15, 25-27, 32, 33) rides it out (see README, Power switch)')

if errors:
    for e in errors:
        print(f'pwrcfg: {e}', file=sys.stderr)
    sys.exit(1)
for w in warnings:
    print(f'pwrcfg warning: {w}', file=sys.stderr)

if a.list_pins:
    print(','.join(str(v) for v in pins.values() if v >= 0) if enabled else '')
    sys.exit(0)

if not a.out:
    print('pwrcfg: --out is required', file=sys.stderr)
    sys.exit(1)

blob = b'PWR1' + struct.pack(
    '<5B4HB',
    1 if enabled else 0,
    pin(pins['button']), pin(pins['ps_on']), pin(pins['button_gnd']), pin(pins['sense']),
    hold_ms, boot_timeout_ms,
    sense_low, sense_high,
    pin(pins['led']))

with open(a.out, 'wb') as f:
    f.write(blob)
