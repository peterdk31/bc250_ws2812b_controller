#!/usr/bin/env python3
"""Clearance, edge, keepout and connectivity check for the generated board.

KiCad 7's kicad-cli cannot run DRC (that arrived in KiCad 8), so this is the
automated stand-in: it rebuilds the same geometry generate.py writes and checks
copper-to-copper spacing on each layer, copper-to-board-edge distance, that no
track enters the antenna keepout, and that every net is one connected piece.
Pads are their bounding rectangles (exact for the rectangular and oval header
pads, conservative for round ones); vias are round pads on both layers.
Exit status 1 on any finding.
"""
import math
import sys

import generate as g

CLEAR = 0.2        # min copper clearance
EDGE = 0.3         # min copper to board edge
HOLE_CLEAR = 0.25  # copper to a mounting hole wall / an unplated peg hole


def seg_point_dist(a, b, p):
    (x1, y1), (x2, y2), (px, py) = a, b, p
    dx, dy = x2 - x1, y2 - y1
    if dx == dy == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def segs_intersect(a, b, c, d):
    def cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1, d2 = cross(c, d, a), cross(c, d, b)
    d3, d4 = cross(a, b, c), cross(a, b, d)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)) and d1 != 0 and d2 != 0 and d3 != 0 and d4 != 0


def seg_seg_dist(a, b, c, d):
    if segs_intersect(a, b, c, d):
        return 0.0
    return min(seg_point_dist(a, b, c), seg_point_dist(a, b, d), seg_point_dist(c, d, a), seg_point_dist(c, d, b))


def box_edges(c, hw, hh):
    (x, y) = c
    p = [(x - hw, y - hh), (x + hw, y - hh), (x + hw, y + hh), (x - hw, y + hh)]
    return [(p[i], p[(i + 1) % 4]) for i in range(4)]


def seg_box_dist(a, b, c, hw, hh):
    """Distance from segment a-b (centreline) to the axis-aligned box."""
    for p in (a, b):
        if abs(p[0] - c[0]) <= hw and abs(p[1] - c[1]) <= hh:
            return 0.0
    edges = box_edges(c, hw, hh)
    if any(segs_intersect(a, b, e0, e1) for e0, e1 in edges):
        return 0.0
    return min(seg_seg_dist(a, b, e0, e1) for e0, e1 in edges)


def box_box_dist(c1, hw1, hh1, c2, hw2, hh2):
    dx = max(0.0, abs(c1[0] - c2[0]) - hw1 - hw2)
    dy = max(0.0, abs(c1[1] - c2[1]) - hh1 - hh2)
    return math.hypot(dx, dy)


def main():
    d = g.design()
    pads = d['pads']
    nets = d['nets']
    bx0, by0, bx1, by1 = d['outline']
    findings = []

    # copper items: ('pad', key, net, layer, centre, (hw, hh)) / ('seg', idx, net, layer, a, b, half_width)
    items = []
    for key, (x, y, hw, hh) in pads.items():
        n = nets.get(key)
        for layer in ('F.Cu', 'B.Cu'):
            items.append(('pad', key, n, layer, (x, y), (hw, hh)))
    for i, (n, layer, w, pts) in enumerate(d['tracks']):
        for a, b in zip(pts, pts[1:]):
            items.append(('seg', f'{n}#{i}', n, layer, a, b, w / 2))

    def is_hole(it):
        return it[0] == 'pad' and (it[1][0].startswith('H') or it[1][1].startswith('NP'))

    def dist(p, q):
        if p[0] == 'pad' and q[0] == 'pad':
            return box_box_dist(p[4], *p[5], q[4], *q[5])
        if p[0] == 'seg' and q[0] == 'seg':
            return seg_seg_dist(p[4], p[5], q[4], q[5]) - p[6] - q[6]
        if p[0] == 'pad':
            p, q = q, p
        return seg_box_dist(p[4], p[5], q[4], *q[5]) - p[6]

    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            p, q = items[i], items[j]
            if p[3] != q[3]:
                continue
            if p[2] is not None and p[2] == q[2]:
                continue
            if p[0] == 'pad' and q[0] == 'pad' and p[1][0] == q[1][0]:
                continue  # pads of one stock footprint: its own geometry
            need = HOLE_CLEAR if is_hole(p) or is_hole(q) else CLEAR
            dd = dist(p, q)
            if dd < need - 1e-6:
                findings.append(f'clearance {dd:.3f} < {need} on {p[3]}: {p[0]} {p[1]} [{p[2]}] vs {q[0]} {q[1]} [{q[2]}]')

    # board edge
    for it in items:
        if it[0] == 'pad':
            (x, y), (hw, hh) = it[4], it[5]
            m = min(x - hw - bx0, bx1 - x - hw, y - hh - by0, by1 - y - hh)
            if is_hole(it):
                continue
        else:
            m = min(min(x - bx0, bx1 - x, y - by0, by1 - y) for (x, y) in (it[4], it[5])) - it[6]
        if m < EDGE - 1e-6:
            findings.append(f'edge clearance {m:.3f} < {EDGE}: {it[0]} {it[1]}')

    # keepout: no track segment may enter the antenna zone
    kx0, ky0, kx1, ky1 = d['keepout']
    corners = [(kx0, ky0), (kx1, ky0), (kx1, ky1), (kx0, ky1)]
    for it in items:
        if it[0] != 'seg':
            continue
        a, b = it[4], it[5]
        # the copper shape, not the centreline: grow the zone by the half-width (as KiCad's DRC sees it)
        hw = it[6]
        kc = [(kx0 - hw, ky0 - hw), (kx1 + hw, ky0 - hw), (kx1 + hw, ky1 + hw), (kx0 - hw, ky1 + hw)]
        inside = any(kc[0][0] < x < kc[2][0] and kc[0][1] < y < kc[2][1] for (x, y) in (a, b))
        crosses = any(segs_intersect(a, b, kc[k], kc[(k + 1) % 4]) for k in range(4))
        if inside or crosses:
            findings.append(f'track in antenna keepout: {it[1]} {a}-{b}')

    # connectivity per net (both layers joined through the THT pads and vias)
    for n in d['net_names']:
        nodes = [it for it in items if it[2] == n and it[3] == 'F.Cu' and it[0] == 'pad'] + \
                [it for it in items if it[2] == n and it[0] == 'seg']
        parent = list(range(len(nodes)))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i

        def union(i, j):
            parent[find(i)] = find(j)

        for i in range(len(nodes)):
            for j in range(i + 1, len(nodes)):
                p, q = nodes[i], nodes[j]
                touch = False
                if p[0] == 'pad' and q[0] == 'pad':
                    touch = box_box_dist(p[4], *p[5], q[4], *q[5]) <= 1e-6
                elif p[0] == 'seg' and q[0] == 'seg':
                    if p[3] == q[3]:
                        touch = seg_seg_dist(p[4], p[5], q[4], q[5]) <= 1e-6
                else:
                    if p[0] == 'pad':
                        p, q = q, p
                    touch = seg_box_dist(p[4], p[5], q[4], *q[5]) <= 1e-6
                if touch:
                    union(i, j)
        roots = {find(i) for i in range(len(nodes))}
        if len(roots) > 1:
            groups = {}
            for i in range(len(nodes)):
                groups.setdefault(find(i), []).append(nodes[i][1])
            findings.append(f'net {n} is in {len(roots)} pieces: ' + ' | '.join(str(v) for v in groups.values()))
        pad_count = sum(1 for k, v in nets.items() if v == n and k[1] != 'via')
        if pad_count < 2:
            findings.append(f'net {n} has {pad_count} pad(s)')

    for f in findings:
        print('FAIL', f)
    print(f'{len(items)} copper items, {len(d["net_names"])} nets, {len(findings)} findings')
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
