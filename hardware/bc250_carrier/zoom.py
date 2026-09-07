"""Crop a kicad-cli board PDF to the board area by rewriting its MediaBox, so a
viewer shows the 70 x 45 mm board filling the page instead of a stamp in the
corner of an A4 sheet. Byte length is preserved so the xref table stays valid."""
import re

import generate as g


def crop(src, dst, mirrored, margin_mm=2.0):
    b = open(src, 'rb').read()
    m = re.search(rb'/MediaBox\s*\[([^\]]*)\]', b)
    old = m.group(0)
    pt = 72 / 25.4
    bx0, by0, bx1, by1 = g.BX0, g.BY0, g.BX1, g.BY1
    page_w, page_h = 297.0, 210.0        # A4 landscape, KiCad's default sheet
    if mirrored:
        x0, x1 = (page_w - bx1 - margin_mm) * pt, (page_w - bx0 + margin_mm) * pt
    else:
        x0, x1 = (bx0 - margin_mm) * pt, (bx1 + margin_mm) * pt
    y0, y1 = (page_h - by1 - margin_mm) * pt, (page_h - by0 + margin_mm) * pt
    new = ('/MediaBox[%d %d %d %d]' % (x0, y0, x1, y1)).encode()
    assert len(new) <= len(old)
    new = new[:-1] + b' ' * (len(old) - len(new)) + b']'
    open(dst, 'wb').write(b.replace(old, new, 1))
