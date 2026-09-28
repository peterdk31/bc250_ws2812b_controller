#!/usr/bin/env python3
"""Fill the board's ground zones and stitch the two layers together.

generate.py writes one GND zone per copper layer; this fills them with
KiCad's own zone filler (the pcbnew Python module), then drops a ground via
wherever both layers' fill has room for one (on a grid, never closer than
STITCH_PITCH to another stitch), and fills again so the fill clears the new
vias.  The vias are tented like the design's own.  Islands that reach no
ground pad or track are removed by the filler.

Usage:  python3 fill.py bc250_carrier.kicad_pcb
"""
import math
import sys

import pcbnew

import generate as g

STITCH_PITCH = 5.0     # mm between stitching vias
GRID = 0.5             # candidate grid, mm
HOLE_GAP = 0.5         # mm from a stitch's copper to any other drilled hole or via
MARGIN = 0.05          # a via must sit this far inside both fills (they already keep GND_CLEAR from everything else)


def mm(v):
    return pcbnew.FromMM(v)


def fill(board):
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def main():
    path = sys.argv[1]
    board = pcbnew.LoadBoard(path)
    fill(board)
    zones = {z.GetLayer(): z for z in board.Zones() if z.GetNetname() == 'GND' and not z.GetIsRuleArea()}
    front, back = zones[pcbnew.F_Cu], zones[pcbnew.B_Cu]
    r = g.VIA_D / 2 + MARGIN
    ring = [(r * math.cos(a * math.pi / 8), r * math.sin(a * math.pi / 8)) for a in range(16)]

    def room(x, y):
        for zone, layer in ((front, pcbnew.F_Cu), (back, pcbnew.B_Cu)):
            for dx, dy in [(0, 0)] + ring:
                if not zone.HitTestFilledArea(layer, pcbnew.VECTOR2I(mm(x + dx), mm(y + dy))):
                    return False
        return True

    # drilled holes already on the board (pads, the design's vias, mounting holes): a stitch keeps HOLE_GAP from them
    holes = [(pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y), pcbnew.ToMM(p.GetDrillSizeX()) / 2)
             for p in board.GetPads() if p.GetDrillSizeX() > 0]
    holes += [(pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y), g.VIA_D / 2)
              for t in board.GetTracks() if t.GetClass() == 'PCB_VIA']

    def clear_of_holes(x, y):
        return all(math.hypot(x - hx, y - hy) >= hr + g.VIA_D / 2 + HOLE_GAP for hx, hy, hr in holes)

    placed = []
    nx = int((g.BX1 - g.BX0) / GRID)
    ny = int((g.BY1 - g.BY0) / GRID)
    for j in range(ny + 1):
        for i in range(nx + 1):
            x, y = g.BX0 + i * GRID, g.BY0 + j * GRID
            if any(math.hypot(x - px, y - py) < STITCH_PITCH for px, py in placed):
                continue
            if clear_of_holes(x, y) and room(x, y):
                placed.append((x, y))
    gnd = board.FindNet('GND')
    for x, y in placed:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        v.SetWidth(mm(g.VIA_D))
        v.SetDrill(mm(g.VIA_DRILL))
        v.SetNet(gnd)
        board.Add(v)
    fill(board)
    board.Save(path)
    print(f'filled {len(zones)} ground zones, {len(placed)} stitching vias')


if __name__ == '__main__':
    main()
