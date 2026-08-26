#!/usr/bin/env python3
"""Encode the receiver's BLE power-remote settings into the blob the firmware
reads from the `blecfg` flash partition at boot. Invoked by the Makefile
(`make flash BLE=on ...`, `make flash-ble`); not normally run by hand.

The layout must match Config::decode in firmware/main/ble.cpp:

    "BLE1" magic, then
    enabled(1) token(16) name(16)

token and name are NUL-padded. Fields are only ever APPENDED (same
compatibility rule as pwrcfg/fancfg).

The token is the shared secret a phone must present with every command — the
only thing standing between "anyone within radio range" and your power
button, so this encoder refuses a missing or weak one rather than letting a
bare `BLE=on` ship an open switch. The name is what the phone's device picker
shows; keep it recognizable, it is public by definition.
"""
import argparse
import sys

TOKEN_LEN = 16
NAME_LEN = 16

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', required=True, help='output file')
p.add_argument('--disabled', action='store_true',
               help='write the config with the feature switched off')
p.add_argument('--token', default='',
               help='shared secret a phone must send with every command '
                    '(8-16 ASCII chars; e.g. from `openssl rand -hex 6`)')
p.add_argument('--name', default='BC250',
               help='advertised BLE device name (1-16 ASCII chars)')
a = p.parse_args()

errors = []

# validated even when writing "disabled"? No — BLE=off must never be refused
# over a token nobody will check (mirrors pwrcfg's rule for pins).
if not a.disabled:
    if not 8 <= len(a.token) <= TOKEN_LEN:
        errors.append(f'--token: need 8-{TOKEN_LEN} characters (got '
                      f'{len(a.token)}) — BLE=on requires BLE_TOKEN, e.g. '
                      'BLE_TOKEN=$(openssl rand -hex 6)')
    elif not all(33 <= ord(ch) <= 126 for ch in a.token):
        errors.append('--token: printable ASCII only, no spaces — the web '
                      'page must be able to type it back byte-for-byte')

if not 1 <= len(a.name) <= NAME_LEN:
    errors.append(f'--name: need 1-{NAME_LEN} characters (got {len(a.name)})')
elif not all(32 <= ord(ch) <= 126 for ch in a.name):
    errors.append('--name: printable ASCII only')

if errors:
    for e in errors:
        print(f'blecfg: {e}', file=sys.stderr)
    sys.exit(1)

blob = (b'BLE1'
        + bytes([0 if a.disabled else 1])
        + a.token.encode('ascii').ljust(TOKEN_LEN, b'\0')
        + a.name.encode('ascii').ljust(NAME_LEN, b'\0'))

with open(a.out, 'wb') as f:
    f.write(blob)
