#!/usr/bin/env python3
"""Assembly-order files for the carrier, one folder per fab under out/fab/:

    out/fab/<provider>/bc250_carrier-gerbers.zip   gerbers + drill, what the fab's PCB upload takes
    out/fab/<provider>/bom.csv                      the parts to fit, in that fab's BOM column layout
    out/fab/<provider>/cpl.csv                      where they go (pick-and-place), in that fab's layout

Runs after kicad-cli has filled out/gerbers/ and out/positions.csv (the
Makefile's `export` does both, then this).  The parts are generate.py's ORDER
tables.  A line is a board part when every designator on it is a footprint
in the position file, so the mating plugs and crimp contacts ("J3 plug") fall
out by themselves.  The Super Mini (U1) is schematic-only: it plugs into the
J12/J13 sockets, which are ordinary parts, so every fab delivers a complete
board and you push the module in.  Any footprint without a part is an error.
Adding a fab = one more PROVIDERS entry: which fields go in which column,
under which heading.
"""
import csv
import os
import shutil
import zipfile

import generate as g

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')

# One entry per fab.  `bom` and `cpl` are (column heading, row field) in the
# order the fab's own template has them; `xy` formats a coordinate the way the
# template writes it; `extras` are files from out/ to add to the gerber zip
# under another name (PCBWay reads an assembly drawing, JLCPCB's parser
# wants gerbers only).
PROVIDERS = {
    'jlcpcb': dict(
        bom=[('Comment', 'mpn'), ('Designator', 'refs'), ('Footprint', 'package'), ('LCSC Part #', 'lcsc')],
        cpl=[('Designator', 'ref'), ('Mid X', 'x'), ('Mid Y', 'y'), ('Layer', 'layer'), ('Rotation', 'rot')],
        xy=lambda v: f'{v:.3f}mm',
        extras={},
    ),
    'pcbway': dict(
        bom=[('Item #', 'item'), ('Designator', 'refs'), ('Qty', 'qty'), ('Manufacturer', 'mfr'),
             ('Mfg Part #', 'mpn'), ('Description / Value', 'desc'), ('Package/Footprint', 'package'),
             ('Type', 'type'), ('Your Instructions / Notes', 'notes')],
        cpl=[('Designator', 'ref'), ('Footprint', 'package'), ('Mid X', 'x'), ('Mid Y', 'y'),
             ('Layer', 'layer'), ('Rotation', 'rot'), ('Comment', 'value')],
        xy=lambda v: f'{v:.3f}',
        extras={'assembly-top.pdf': 'board-top.pdf', 'assembly-bottom.pdf': 'board-bottom.pdf'},
    ),
}


def positions():
    """ref -> row of kicad-cli's position CSV (Ref, Val, Package, PosX, PosY, Rot, Side)."""
    with open(os.path.join(OUT, 'positions.csv'), newline='') as f:
        return {r['Ref']: r for r in csv.DictReader(f)}


def board_parts(pos):
    """The ORDER lines whose designators are all footprints on the board, as
    field dicts, and the set of fitted refs; every footprint must be fitted."""
    parts = []
    fitted = set()
    for refs, lcsc, mfr_mpn, desc, per, moq in g.ORDER + g.ORDER_OPTIONAL:
        if not all(r in pos for r in refs):
            continue                     # plugs, contacts: not board parts
        assert per == len(refs), f'{refs}: {per} per board but {len(refs)} designators'
        mfr, mpn = mfr_mpn.split(' ', 1)
        first = pos[refs[0]]
        parts.append(dict(item=len(parts) + 1, refs=','.join(refs), qty=len(refs), mfr=mfr, mpn=mpn, desc=desc,
                          package=first['Package'], value=first['Val'], lcsc=lcsc, type='THT',
                          notes=f'LCSC {lcsc}  https://www.lcsc.com/product-detail/{lcsc}.html'))
        fitted.update(refs)
    missing = set(pos) - fitted
    assert not missing, f'footprints on the board with no part: {sorted(missing)}'
    return parts, fitted


def placements(pos, xy, fitted):
    """One CPL row per fitted footprint, coordinates formatted for the fab."""
    rows = []
    for ref, r in pos.items():
        if ref not in fitted:
            continue
        rows.append(dict(ref=ref, x=xy(float(r['PosX'])), y=xy(float(r['PosY'])), layer=r['Side'].capitalize(),
                         rot=f"{float(r['Rot']):g}", package=r['Package'], value=r['Val']))
    return rows


def write_csv(path, columns, rows):
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow([h for h, _ in columns])
        for r in rows:
            w.writerow([r[field] for _, field in columns])


def write_zip(path, extras):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(os.path.join(OUT, 'gerbers'))):
            z.write(os.path.join(OUT, 'gerbers', name), name)
        for name, src in extras.items():
            z.write(os.path.join(OUT, src), name)


def main():
    pos = positions()
    parts, fitted = board_parts(pos)
    shutil.rmtree(os.path.join(OUT, 'fab'), ignore_errors=True)
    for name, p in PROVIDERS.items():
        d = os.path.join(OUT, 'fab', name)
        os.makedirs(d)
        write_zip(os.path.join(d, g.PROJECT + '-gerbers.zip'), p['extras'])
        write_csv(os.path.join(d, 'bom.csv'), p['bom'], parts)
        write_csv(os.path.join(d, 'cpl.csv'), p['cpl'], placements(pos, p['xy'], fitted))
        print(f'out/fab/{name}: {len(parts)} BOM lines, {len(fitted)} placements')


if __name__ == '__main__':
    main()
