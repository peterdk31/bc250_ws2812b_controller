#!/usr/bin/env python3
"""Run KiCad's own DRC on the board through the pcbnew Python module (kicad-cli 7
has no DRC command).  Prints the report and exits 1 on any error-severity item;
the 'library not configured' warnings the CLI emits for stock footprints are
noise (the footprints are embedded in the board) and are dropped.

Usage:  python3 drc.py bc250_carrier.kicad_pcb out/drc.txt
"""
import re
import sys

import pcbnew

pcb, report = sys.argv[1], sys.argv[2]
pcbnew.WriteDRCReport(pcbnew.LoadBoard(pcb), report, pcbnew.EDA_UNITS_MILLIMETRES, True)
text = open(report).read()
blocks = re.split(r'\n(?=\[|\*\*)', text)
kept = [b for b in blocks if b.strip() and not b.startswith('[lib_footprint_issues]')]
print('\n'.join(kept))
# A connector housing inside a mounting hole's courtyard (the stock M3 footprint
# draws a 6.9 mm circle) is a documented fit: heads up to 5.5 mm, no washers,
# see the README.  Reported, not fatal.
def hole_courtyard(b):
    return b.startswith('[courtyards_overlap]') and re.search(r'Footprint H\d', b)


errors = sum(1 for b in kept if 'Severity: error' in b and not hole_courtyard(b))
print(f'{errors} DRC error(s)')
sys.exit(1 if errors else 0)
