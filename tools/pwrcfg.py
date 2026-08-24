#!/usr/bin/env python3
"""Encode the receiver's power-switch settings into the blob the firmware
reads from the `pwrcfg` flash partition at boot. Invoked by the Makefile
(`make flash PWR=on ...`, `make flash-pwr`); not normally run by hand.

The layout must match Config::decode in firmware/main/power_switch.cpp:

    "PWR1" magic, then
    enabled(1) button_pin(1) ps_on_pin(1) button_gnd_pin(1) sense_pin(1)
    hold_ms(2) boot_timeout_ms(2) sense_low_mv(2) sense_high_mv(2)
    led_pin(1)

u16s little-endian; pins are GPIO numbers, 0xFF = not wired. Fields are only
ever APPENDED (firmware older than a field reads a shorter blob and ignores
the tail; firmware newer than a blob reads erased flash, 0xFF = not wired).
The defaults here are this project's BC-250 hookup (ESP32-C3).

The firmware has no console and quietly drops anything it can't use (a pin
past GPIO_NUM_MAX decodes as "not wired" — for PS_ON that means the whole
feature is off), so this encoder is where mistakes have to be caught: it
validates the wiring against the target chip and refuses to write a config
that would fail silently on the board whose power this partition controls.
"""
import argparse
import struct
import sys

# the per-chip pin facts (flash pads, ADC pins, straps, ...) are shared with
# the fan encoder — see tools/pincheck.py, the single source of truth
from pincheck import CHIPS, MAX_PIN, parse_avoid, pin_byte as pin


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', required=True, help='output file')
p.add_argument('--disabled', action='store_true',
               help='write the config with the feature switched off')
p.add_argument('--target', default='',
               help='chip the config is for (validates pins, e.g. esp32c3)')
p.add_argument('--strip-pin', type=int, default=-1,
               help='the LED data pin (strip.pin), to catch collisions')
p.add_argument('--ps-on', type=int, default=3,
               help='pin driving the PS_ON# MOSFET gate (see README)')
p.add_argument('--button', type=int, default=1, help='momentary button pin')
p.add_argument('--button-gnd', type=int, default=21,
               help="local ground for the button's second terminal, -1 = real GND")
p.add_argument('--sense', type=int, default=2,
               help='board-power sense pin (ADC), -1 = not wired')
p.add_argument('--led', type=int, default=8,
               help='feedback LED pin, blinks while the button reads pressed '
                    '(8 = the plain onboard LED on common C3 dev boards), '
                    '-1 = none')
p.add_argument('--hold', type=float, default=2,
               help='seconds to hold the button to force off')
p.add_argument('--boot-timeout', type=float, default=10,
               help='seconds to wait for sense after power-on (sense only)')
p.add_argument('--sense-low', type=int, default=800,
               help='hysteresis: below this many mV the board is down')
p.add_argument('--sense-high', type=int, default=2000,
               help='hysteresis: above this many mV the board is up')
p.add_argument('--avoid', action='append', metavar='GPIO:label', default=[],
               help='a pin some other feature claims (e.g. "5:--pins (fans)"),'
                    ' to catch cross-feature collisions; repeatable')
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
    errors.append(f'--avoid {e}: expected GPIO:label, e.g. "5:--pins (fans)"')
    avoid = {}

hold_ms = round(a.hold * 1000)
boot_timeout_ms = round(a.boot_timeout * 1000)
if not 100 <= hold_ms <= 65535:
    errors.append(f'--hold {a.hold}: must be 0.1..65.535 seconds')
if not 1000 <= boot_timeout_ms <= 65535:
    errors.append(f'--boot-timeout {a.boot_timeout}: must be 1..65.535 seconds')
if not 0 <= a.sense_low <= 65535 or not 0 <= a.sense_high <= 65535:
    errors.append('--sense-low/--sense-high: must be 0..65535 mV')
elif a.sense_low >= a.sense_high:
    errors.append(
        f'--sense-low {a.sense_low} must be below --sense-high {a.sense_high}: '
        'inverted hysteresis never settles, so the boot timeout would cut '
        'the PSU shortly after every power-on')

pins = {'--ps-on': a.ps_on, '--button': a.button,
        '--button-gnd': a.button_gnd, '--sense': a.sense, '--led': a.led}
for flag, v in pins.items():
    if not -1 <= v <= 0xFE:
        errors.append(f'{flag} {v}: not a GPIO number (-1 = not wired)')

# The wiring only matters when the feature is on: PWR=off must never be
# refused over pins the firmware will ignore (they may be another chip's).
if not a.disabled and not errors:
    chip = CHIPS.get(a.target)
    top = chip['max_pin'] if chip else MAX_PIN.get(a.target)

    if a.ps_on < 0:
        errors.append('--ps-on is required: without the PS_ON# wire there '
                      'is no power switch')
    if a.button < 0:
        errors.append('--button is required: enabled but buttonless, the '
                      'machine could never be powered on — worse than the '
                      'jumper this replaces')

    wired = {flag: v for flag, v in pins.items() if v >= 0}

    seen = {}
    for flag, v in wired.items():
        if v in seen:
            errors.append(f'{flag} and {seen[v]} are both GPIO{v}')
        else:
            seen[v] = flag
    if a.strip_pin >= 0 and a.strip_pin in seen:
        errors.append(f'{seen[a.strip_pin]} {a.strip_pin}: GPIO{a.strip_pin} '
                      'is the LED data pin (strip.pin in the daemon config)')
    for v, label in avoid.items():
        if v in seen:
            (errors if a.avoid_hard else warnings).append(
                f'{seen[v]} {v}: GPIO{v} is already {label}')

    for flag, v in wired.items():
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
        elif v in chip['input_only'] and flag != '--sense':
            errors.append(f'{flag} {v}: GPIO{v} is input-only on {a.target} '
                          '(and has no internal pull-up, so not even the '
                          'button works there)')
        if flag == '--sense' and v not in chip['adc']:
            errors.append(f'--sense {v}: GPIO{v} is not ADC-capable on '
                          f'{a.target} — the firmware would silently run '
                          'without sense (no shutdown follow-down, no boot '
                          'timeout)')
        elif flag == '--sense' and v in chip['strap']:
            warnings.append(
                f'--sense {v}: GPIO{v} selects the boot mode on {a.target} at '
                'reset, and the sense line sits at 0 V whenever the machine is '
                'off (on the BC-250 it is a dead 3.3 V rail) — so a reset while '
                'the machine is down can drop the chip into the wrong boot '
                'mode. Prefer an ADC pin that is not a boot-mode strap')
        if flag == '--ps-on' and chip['hold'] is not None \
                and v not in chip['hold']:
            warnings.append(
                f'--ps-on {v}: gpio_hold cannot latch GPIO{v} through a '
                f'reset on {a.target} — a crash briefly floats PS_ON#; an '
                'RTC pin (0, 2, 4, 12-15, 25-27, 32, 33) rides it out (see '
                'README, Power switch)')

if errors:
    for e in errors:
        print(f'pwrcfg: {e}', file=sys.stderr)
    sys.exit(1)
for w in warnings:
    print(f'pwrcfg warning: {w}', file=sys.stderr)

blob = b'PWR1' + struct.pack(
    '<5B4HB',
    0 if a.disabled else 1,
    pin(a.button), pin(a.ps_on), pin(a.button_gnd), pin(a.sense),
    hold_ms, boot_timeout_ms,
    a.sense_low, a.sense_high,
    pin(a.led))

with open(a.out, 'wb') as f:
    f.write(blob)
