#!/usr/bin/env python3
"""Encode the receiver's power-switch settings into the blob the firmware
reads from the `pwrcfg` flash partition at boot. Invoked by the Makefile
(`make flash PWR=on ...`, `make flash-pwr`); not normally run by hand.

The layout must match Config::decode in firmware/main/power_switch.cpp:

    "PWR1" magic, then
    enabled(1) button_pin(1) ps_on_pin(1) button_gnd_pin(1) sense_pin(1)
    hold_ms(2) boot_timeout_ms(2) sense_low_mv(2) sense_high_mv(2)

u16s little-endian; pins are GPIO numbers, 0xFF = not wired. The defaults
here are this project's BC-250 hookup (ESP32-C3).
"""
import argparse
import struct


def pin(v):
    return 0xFF if v < 0 else v


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', required=True, help='output file')
p.add_argument('--disabled', action='store_true',
               help='write the config with the feature switched off')
p.add_argument('--ps-on', type=int, default=3, help='PS_ON# pin (open-drain)')
p.add_argument('--button', type=int, default=2, help='momentary button pin')
p.add_argument('--button-gnd', type=int, default=1,
               help="local ground for the button's second terminal, -1 = real GND")
p.add_argument('--sense', type=int, default=0,
               help='board-power sense pin (ADC), -1 = not wired')
p.add_argument('--hold', type=float, default=5,
               help='seconds to hold the button to force off')
p.add_argument('--boot-timeout', type=float, default=10,
               help='seconds to wait for sense after power-on (sense only)')
p.add_argument('--sense-low', type=int, default=800,
               help='hysteresis: below this many mV the board is down')
p.add_argument('--sense-high', type=int, default=2000,
               help='hysteresis: above this many mV the board is up')
a = p.parse_args()

blob = b'PWR1' + struct.pack(
    '<5B4H',
    0 if a.disabled else 1,
    pin(a.button), pin(a.ps_on), pin(a.button_gnd), pin(a.sense),
    round(a.hold * 1000), round(a.boot_timeout * 1000),
    a.sense_low, a.sense_high)

with open(a.out, 'wb') as f:
    f.write(blob)
