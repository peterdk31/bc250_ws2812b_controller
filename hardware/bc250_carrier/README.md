# BC-250 carrier board (rev B)

An 80 × 55 mm two-layer carrier for the ESP32-C3 Super Mini that runs the
receiver firmware in the BC-250. It replaces the loose wiring for the
[power switch](../../README.md#power-switch), the WS2812B strip and the
[fans](../../README.md#fans) with one board: the Super Mini solders flat onto
it, the PSU's own 10-pin Mini-Fit Jr plug goes straight onto a header, the
strip leaves through a 10 A JST-VH, and the button and sense wires solder into
JST-XH footprints (or take crimped XH housings later — same holes).

The only discrete parts are the power switch's 2N7000 and its 100 kΩ gate
pull-down, exactly the circuit the README describes. No regulator (the Super
Mini has one), no level shifter (the strip reads 3.3 V data fine), no
capacitors or series resistors — the wired-up prototype works without them.

`make export` renders the board to `out/zoom-top.pdf` and `out/zoom-bottom.pdf`
and the schematic to `out/schematic.pdf`; gerbers and drill files land in
`out/gerbers/`.

## The PSU header

J1 is a Molex Mini-Fit Jr 5566-10A, the 2 × 5 vertical header that the
FSP500-30AS's own 10-pin plug mates with. The pin map comes from the PSU's
pinout drawing (`fsp500-30as.webp`, "looking into the front face of the
connector", latch up):

| plug row (face view, latch up) | left → right |
|---|---|
| latch side | 3.3 V, GND, PS_ON, GND, GND |
| other side | 3.3 V, GND, 5V STBY, +12 V, +12 V |

KiCad's 5566 footprint has the latch ramp on its pin 6–10 row, and a header
seen from above is the mirror image of a plug seen face-on, which gives the
header map in `MF_NET`:

| header column | pins, top to bottom on the board |
|---|---|
| edge column (pins 1–5, pin 1 is the square pad) | VIN (3.3 V), GND, 5VSB, +12 V, +12 V |
| inner column (pins 6–10, latch ramp on this side) | VIN (3.3 V), GND, PS_ON#, GND, GND |

The silkscreen prints the inner-column names left of the header and the
edge-column names between the columns. Before soldering the header, hold
the PSU plug against it the way it will mate (its latch on the inner,
ramp side) and confirm the 3.3 V pair lands on the top row — the drawing
is the only source for this map.

## What is on it

| ref | part | footprint |
|---|---|---|
| U1 | ESP32-C3 Super Mini | 2 × 8 through-hole, 2.54 mm pitch, 15.24 mm row spacing |
| Q1 | 2N7000 N-MOSFET | TO-92 |
| R1 | 100 kΩ | axial, 10.16 mm pitch |
| J1 | PSU in: VIN (3.3 V), 5VSB, PS_ON#, +12 V, GND | Molex Mini-Fit Jr 5566-10A, 2 × 5 vertical header (takes the PSU's 10-pin plug) |
| J3 | strip out: VIN, DIN, GND | JST-VH B3P-VH, 3.96 mm, 10 A contacts |
| J9 | power button: BTN, GND | JST-XH 2-pin (B2B-XH-A) |
| J10 | sense: GND, TPMS1 pin 9 | JST-XH 2-pin |
| J5–J8 | four 4-pin PWM fan headers | Molex KK-254 (the standard PC fan header; a 47053-1000 fits) |
| H1–H4 | M3 mounting holes | 3.2 mm, 68.6 × 46 mm spacing |

Matching housings: VHR-3N with SVH-21T-P1.1 crimps for the strip (20 AWG
wire for 3 A); XHP-2 with SXH-001T-P0.6 for the button and sense.

## Wiring

| connector | pin | goes to |
|---|---|---|
| PSU J1 | see the table above | the FSP500-30AS 10-pin plug, straight on |
| STRIP J3 | 1 `VIN`, 2 `DIN`, 3 `GND` | the WS2812B strip. VIN is the PSU's 3.3 V straight through (5 V works too); DIN is GPIO4 |
| BUTTON J9 | 1 `BTN`, 2 `GND` | momentary switch → GPIO1, the other terminal to a **real ground** |
| SENSE J10 | 1 `GND`, 2 `TPMS1.9` | BC-250 TPMS1 pin 9 (the main 3.3 V rail) → GPIO2; the GND is optional |
| FAN1 J5 … FAN4 J8 | 1 `G`, 2 `12V`, 3 `T`, 4 `PWM` | standard 4-pin fan pinout, tach unconnected |

Fan channel order follows the firmware's `FAN_PINS=5,6,7,10` default:
FAN1 (leftmost) is GPIO5 and `fans.duty` index 0, FAN4 is GPIO10 and index 3.

The strip path is sized for 3 A: a 3 mm VIN trace from the header's inner
3.3 V pin to the VH (the edge 3.3 V pin is tied across), and a 2.5 mm ground
return from the VH back to the header's ground pins, both on 1 oz copper
(about 15 °C rise at 3 A). All grounds are one net; 5VSB, VIN and 12 V are
three separate rails.

## Ordering

Two orders, one shipment: JLCPCB makes the board, LCSC (its sister shop)
supplies every part except the Super Mini, and LCSC's checkout offers to
combine the parcel with a JLCPCB order.

1. **Board.** `make` leaves `out/bc250_carrier-gerbers.zip`; upload it at
   jlcpcb.com. Defaults are fine: 2 layers, 1.6 mm, 1 oz, any colour, no
   assembly. Note the JLCPCB order number.
2. **Parts.** `out/lcsc_parts.csv` is the shopping list, one line per LCSC
   stock number with the quantity already rounded up to LCSC's minimum
   order. It is sized for ten boards; `BOARDS=3 make` resizes it. Upload it to the BOM
   tool at lcsc.com (map the "LCSC Part Number" and "Quantity" columns) or
   add the numbers to the cart by hand. At checkout pick the option to ship
   together with the JLCPCB order and give its number. Same account and
   currency on both sites, and it cannot be undone once combined.

| LCSC # | part | for | per board | min. order |
|---|---|---|---|---|
| C9114 | JSCJ 2N7000, TO-92 | Q1 | 1 | 10 |
| C120103 | CCO 100 kΩ ¼ W carbon film, axial | R1 | 1 | 100 |
| C22365703 | DLL-5566-10A, Mini-Fit Jr 2×5 vertical, 9 A | J1 | 1 | 5 |
| C160316 | JST B3P-VH(LF)(SN), 3.96 mm 3-pin, 10 A | J3 | 1 | 5 |
| C158012 | JST B2B-XH-A(LF)(SN), 2.5 mm 2-pin | J9, J10 | 2 | 20 |
| C41927 | BOOMELE 2.54-4AS, KF2510-style 4-pin with lock | J5–J8 | 4 | 5 |
| C157899 | JST VHR-3N housing | strip plug | 1 | 5 |
| C160349 | JST SVH-21T-P1.1 crimp contact, 18–22 AWG | strip plug (3 + spares) | 5 | 10 |
| C144401 | JST XHP-2 housing | button, sense plugs | 2 | 10 |
| C140573 | JST SXH-001T-P0.6 crimp contact, 22–28 AWG | button, sense plugs (4 + spares) | 6 | 10 |

The whole list is a few dollars; the minimum orders are what set the
quantities. The PSU header mates with the PSU's own plug and the fan headers
with the fans' plugs, so those need nothing. The JST contacts are crimp
type: a cheap ratchet crimper for open-barrel terminals (an SN-01BM or
similar) does both series, or solder the wire into the contact's barrel and
squeeze the insulation tabs with pliers. Use 20 AWG for the strip wires.

Two things to check when the parts arrive, before soldering:

- **Fan headers.** A PC fan plug is a KF2510-family housing, so it mates
  with the 2.54-4AS; the header's lock ramp sets which way round the plug
  goes. Before powering a fan, put a meter on the header and confirm the
  fan's black wire lands on the pad marked `G` and yellow/red on `12V`. If
  the plug only fits the other way round, rotate the four headers 180° in
  `generate.py` (the `FAN_Y` row) and re-order, or re-pin the fan plug.
- **The PSU header** orientation, as described above.

## Flashing for this board

```sh
# the button's second pin is a real GND, not GPIO21
sudo make flash-pwr PWR=on PWR_BUTTON_GND=-1
# fans: the default FAN_PINS=5,6,7,10 already matches FAN1..FAN4
sudo make flash-fan FAN=on
```

`PWR_LED` can stay at 8: that is the Super Mini's own blue LED, so the
button-feedback blink still works.

**USB.** The Super Mini ties its USB-C VBUS straight to the 5V pin, and this
board feeds that pin from 5VSB. Plugging in a normal cable parallels the two
supplies and back-feeds the host's dead USB port while the machine is off —
the README's [5VSB and USB warning](../../README.md#power-switch) applies
unchanged: use a cable with the red wire cut. Data, flashing and the USB
host-presence detection all still work.

## Building it

- **The Super Mini goes on top, component side up, USB-C at the board's top
  edge.** Its pin names are printed on its underside, so they read mirrored
  when you flip it over to look; the carrier prints each name next to its
  hole for the module as mounted. With the module component-side up and the
  USB-C away from you, the 5V pin is the top pin of the **right** column
  (rev B; rev A had it on the left). If your module differs, swap `SM_LEFT`
  and `SM_RIGHT` in `generate.py` — the nets follow the names, but the tracks
  are laid out for this arrangement and will need re-routing.
- **Check the row spacing.** Drawn at 15.24 mm (0.6″). Print
  `out/zoom-top.pdf` at 1:1 or measure the module before ordering.
- **The USB-C legs.** Many Super Minis have the USB-C shell soldered through
  the board, leaving solder bumps underneath. If yours does, mount the module
  on ~1 mm pin stubs (or cut pin-header pins short) rather than dead flat; the
  carrier has no copper on the top layer under the module, only its own pads.
- **Antenna.** The module's ceramic antenna sits at the end away from the
  USB-C. The carrier keeps a copper-free zone under it and has no ground pour
  at all, so BLE range is whatever the Super Mini manages on its own.
- Q1's flat face follows the silkscreen outline (pins D, G, S left to right
  as printed); R1 has no polarity.

## Editing

The whole design is `generate.py`: parts, nets, placement and every track.
Edit it and run `make`, which regenerates the KiCad files, runs `check.py`
(copper clearance ≥ 0.2 mm, edge ≥ 0.3 mm, nothing in the antenna zone,
every net one connected piece) and exports `out/`. The KiCad files are
ordinary KiCad 7 files too — open `bc250_carrier.kicad_pro` and edit in the
GUI if you prefer; then the generator no longer describes the board, so pick
one.

KiCad 7's `kicad-cli` cannot run DRC or ERC (those came with KiCad 8), so
`check.py` stands in. Run the GUI DRC once before ordering anyway.

Fab settings: 2 layers, 1.6 mm, 1 oz copper, min track 0.5 mm, min
clearance 0.2 mm, min drill 0.75 mm — any board house's cheapest tier.
Stock symbols and footprints are copied from the local KiCad install
(`KICAD_SHARE=/usr/share/kicad`), so the project opens without extra
libraries.
