#!/usr/bin/env python3
"""Encode the receiver's BLE power-remote settings into the blob the firmware
reads from the `blecfg` flash partition at boot. Invoked by the Makefile
(`make flash`, `make flash-ble`); not normally run by hand.

The settings come from the daemon config's "ble_remote" block (--config):

    "ble_remote": { "enabled": false, "name": "BC250", "token": "" }

No block, or "enabled": false, writes the feature off (harmless: the phone
just can't reach the board — unlike the power switch, whose absent block is
left alone).

The layout must match Config::decode in firmware/main/ble.cpp:

    "BLE1" magic, then
    enabled(1) token(16) name(16)

token and name are NUL-padded. Fields are only ever APPENDED (same
compatibility rule as pwrcfg/fancfg).

The token is the shared secret a phone must present with every command — the
only thing standing between "anyone within radio range" and your power
button, so this encoder refuses a missing or weak one rather than letting an
enabled block ship an open switch. It sits in the config file in the clear;
anyone with a shell on the box can read it, and can also just run
`systemctl poweroff`, which is all it guards. The name is what the phone's
device picker shows; keep it recognizable, it is public by definition.
"""
import argparse
import json
import sys

TOKEN_LEN = 16
NAME_LEN = 16
KEYS = ('enabled', 'name', 'token')

p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--out', required=True, help='output file')
p.add_argument('--config', required=True,
               help='the daemon config (its "ble_remote" block is what gets encoded)')
a = p.parse_args()

errors = []

try:
    with open(a.config) as f:
        root = json.load(f)
except OSError as e:
    print(f'blecfg: {a.config}: {e.strerror}', file=sys.stderr)
    sys.exit(1)
except ValueError as e:
    print(f'blecfg: {a.config}: {e}', file=sys.stderr)
    sys.exit(1)

block = root.get('ble_remote') if isinstance(root, dict) else None
if block is None:
    block = {'enabled': False, 'name': 'BC250', 'token': ''}
if not isinstance(block, dict):
    print('blecfg: ble_remote: expected an object', file=sys.stderr)
    sys.exit(1)

for k in block:
    if k not in KEYS:
        errors.append(f'ble_remote.{k}: unknown key')
for k in KEYS:
    if k not in block:
        errors.append(f'ble_remote: missing "{k}" (the block has ' + ', '.join(KEYS) + ')')
if errors:
    for e in errors:
        print(f'blecfg: {e}', file=sys.stderr)
    sys.exit(1)

enabled, name, token = block['enabled'], block['name'], block['token']
if not isinstance(enabled, bool):
    errors.append('ble_remote.enabled: expected true or false')
    enabled = False
if not isinstance(name, str):
    errors.append('ble_remote.name: expected a string')
    name = ''
if not isinstance(token, str):
    errors.append('ble_remote.token: expected a string')
    token = ''

# validated even when writing "disabled"? No — "enabled": false must never be
# refused over a token nobody will check (mirrors pwrcfg's rule for pins).
if enabled:
    if not 8 <= len(token) <= TOKEN_LEN:
        errors.append(f'ble_remote.token: need 8-{TOKEN_LEN} characters (got '
                      f'{len(token)}) — an enabled remote requires one, e.g. '
                      'from `openssl rand -hex 6`')
    elif not all(33 <= ord(ch) <= 126 for ch in token):
        errors.append('ble_remote.token: printable ASCII only, no spaces — the web '
                      'page must be able to type it back byte-for-byte')
    if not 1 <= len(name) <= NAME_LEN:
        errors.append(f'ble_remote.name: need 1-{NAME_LEN} characters (got {len(name)})')
    elif not all(32 <= ord(ch) <= 126 for ch in name):
        errors.append('ble_remote.name: printable ASCII only')

if errors:
    for e in errors:
        print(f'blecfg: {e}', file=sys.stderr)
    sys.exit(1)

blob = (b'BLE1'
        + bytes([1 if enabled else 0])
        + token.encode('ascii')[:TOKEN_LEN].ljust(TOKEN_LEN, b'\0')
        + (name or 'BC250').encode('ascii')[:NAME_LEN].ljust(NAME_LEN, b'\0'))

with open(a.out, 'wb') as f:
    f.write(blob)
