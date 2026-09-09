# BC-250 carrier board (rev C)

An 80 × 34.5 mm two-layer carrier for the ESP32-C3 Super Mini that runs the
receiver firmware in the BC-250. It replaces the loose wiring for the
[power switch](../../README.md#power-switch), the WS2812B strip and the
[fans](../../README.md#fans) with one board: the Super Mini solders flat onto
it, the PSU's own 10-pin Mini-Fit Jr plug goes straight onto a right-angle
header and lies flat over the board, the strip leaves through a 10 A JST-VH,
the illuminated power button plugs into a 4-pin JST-XH, and the sense wire
solders to a single pin. An optional 2 × 8 pin header along the left edge
breaks out the three rails (two pins each) and the module's five spare
GPIOs, each beside a ground pin.

The only discrete parts are the power switch's 2N7000 and its 100 kΩ gate
pull-down, exactly the circuit the README describes. No regulator (the Super
Mini has one), no level shifter (the strip reads 3.3 V data fine), no
capacitors or series resistors — the wired-up prototype works without them.

`make` regenerates the KiCad files, checks them (the geometry checker, then
KiCad's own DRC through its Python module), renders `out/preview.png` and
exports the board to `out/zoom-top.pdf` / `out/zoom-bottom.pdf`, the
schematic to `out/schematic.pdf`, the BOM to `out/bom.xml`; gerbers and
drill files land in `out/gerbers/` with a JLCPCB-ready zip beside them.

## Layout

Three columns, left to right:

- **The Super Mini**, USB-C at the top edge, with the button connector and
  the sense pin under its antenna end and the optional breakout header along
  the board's left edge.
- **The PSU header** on the bottom edge, a right-angle Mini-Fit Jr whose
  mating face points *up* the board. The PSU plug slides on from above and
  lies flat over the middle of the board; its wires leave over the top edge.
  So the board takes no extra room in the cable's direction: the plug and
  the first stretch of cable sit inside the board's own footprint. The whole
  column above the header is the **plug zone** — tracks only, nothing taller
  than the solder mask, because the plug body rides about 1.3 mm above the
  board and the wires drape over the rest. Keep it that way if you edit.
- **The strip VH** at the top right, the four fan headers as a 2 × 2 block
  under it, the MOSFET and its resistor along the bottom edge. The fan
  headers are KF2510 parts with a 12.7 × 5.8 mm body, the same size as a PC
  fan plug, so the block is spaced for the plugs: columns 13.2 mm apart, rows
  9.6 mm apart (3.8 mm between the bodies for the plugs' latches, which face
  down the board on all four).

Rev B was 80 × 55 mm with a vertical PSU header on the right edge; rev C is
37 % smaller in area and the PSU cable no longer sticks out of the side.

## The PSU header

J1 is a Molex Mini-Fit Jr 5569-10A2 (39-30-0100; the DLL-5569-10AW clone on
the parts list), the 2 × 5 right-angle header that the FSP500-30AS's own
10-pin plug mates with. It sits with its two pin rows along the board's
bottom edge: the **rear row** (pins 6–10) right at the edge, the **front
row** (pins 1–5) 5.5 mm further in, pin 1/6 at the left. The footprint is
KiCad's Molex one, checked against the DLL datasheet (Sep 2026): same 4.2 mm
pitch, 5.5 mm row spacing and 1.8 mm holes, and the 3 mm peg holes are where
DLL's pegged variant (DLL-5569-10AWS) puts them. The part LCSC sells is the
AW without pegs, so those two holes stay empty — it solders fine without
them. One difference: the DLL body ends 3.5 mm in front of the front pin
row where Molex's ends 1.1 mm, so the mating face sits 2.4 mm further up the
board (at about y = 110 on the drawings) and the plug overhangs the top edge
by that much more.

The pin map comes from the PSU's pinout drawing (`fsp500-30as.webp`, "looking
into the front face of the connector", latch up):

| plug row (face view, latch up) | left → right |
|---|---|
| latch side | 3.3 V, GND, PS_ON, GND, GND |
| other side | 3.3 V, GND, 5V STBY, +12 V, +12 V |

In a right-angle Mini-Fit Jr the rear pin row feeds the upper contact row and
the latch ramp is on top, away from the board, so pins 6–10 are the latch
row (the same rows the vertical 5566 footprint has — KiCad draws its ramp on
the 6–10 row). Looking into the header's face from the top edge of the board,
+x is on your left, so the header face reads 5 4 3 2 1 over 10 9 8 7 6 and
the plug face is its mirror image: 1 2 3 4 5 on the lower row, 6 7 8 9 10 on
the latch row, left to right. Lined up with the drawing that gives `MF_NET`:

| header row | pins, left to right on the board |
|---|---|
| front row (pins 1–5, pin 1 is the square pad) | 3.3V, GND, 5VSB, +12 V, +12 V |
| rear / edge row (pins 6–10, latch side) | 3.3V, GND, PS_ON#, GND, GND |

The silkscreen prints the **edge row's** names in the 1.8 mm gap between the
rows on the front of the board and the **front row's** names in the same gap
on the back (there is no room anywhere else once the housing is on). Before
soldering the header, hold the PSU plug against it the way it will mate
(latch up) and confirm the 3.3 V pair lands on the left — the drawing is the
only source for this map, and the 5569's row numbering was derived, not
measured. If it turns out mirrored, the fix is one line in `MF_NET`.

## What is on it

| ref | part | footprint |
|---|---|---|
| U1 | ESP32-C3 Super Mini | 2 × 8 through-hole, 2.54 mm pitch, 15.24 mm row spacing |
| Q1 | 2N7000 N-MOSFET | TO-92 |
| R1 | 100 kΩ | axial, 7.62 mm pitch |
| J1 | PSU in: 3.3V, 5VSB, PS_ON#, +12 V, GND | Molex Mini-Fit Jr 5569-10A2, 2 × 5 right-angle header with pegs (takes the PSU's 10-pin plug) |
| J3 | strip out: 3.3V, DIN, GND | JST-VH B3P-VH, 3.96 mm, 10 A contacts |
| J9 | power button: 12V, GND (ring LED), NO, C (switch) | JST-XH 4-pin (B4B-XH-A) |
| J10 | sense: TPMS1 pin 9 | one 2.54 mm pin (or solder the wire in) |
| J11 | optional: 3.3V, 5VSB, 12V (two pins each), then GPIO8, GPIO9, GPIO20, GPIO21, GPIO0 each with a GND beside it | 2 × 8 2.54 mm pin header, normally left off |
| J5–J8 | four 4-pin PWM fan headers | KF2510 4-pin straight header with friction-lock ramp (Ckmtw W-2510S04P-0000; pads are KiCad's Molex KK-254 footprint, so a Molex 47053-1000 fits too) |
| H1–H4 | M3 mounting holes | 3.2 mm, 73.6 × 28.1 mm spacing |

Matching housings: VHR-3N with SVH-21T-P1.1 crimps for the strip (20 AWG
wire for 3 A); XHP-4 with SXH-001T-P0.6 for the button.

## Wiring

| connector | pin | goes to |
|---|---|---|
| PSU J1 | see the table above | the FSP500-30AS 10-pin plug, straight on |
| STRIP J3 | 1 `3.3V`, 2 `DIN`, 3 `GND` | the WS2812B strip. 3.3V is the PSU's 3.3 V rail straight through; DIN is GPIO4 |
| BUTTON J9 | 1 `12V`, 2 `GND`, 3 `NO`, 4 `C` | the illuminated button: 1–2 feed its ring LED (a 12 V LED, i.e. one with its own resistor — the pins are the raw 12 V rail), 3–4 are the switch: `NO` → GPIO1, `C` is a **real ground**. Use the normally-open terminal: the firmware sees a press as the contact closing, and the NC terminal makes the PSU click on and off (Jul 2026) |
| SENSE J10 | 1 `SNS` | BC-250 TPMS1 pin 9 (the main 3.3 V rail) → GPIO2; single wire, the ground is shared through the PSU |
| J11 | rows top to bottom, outer pin (odd, nearest the board edge) / inner pin (even): 1+2 `3.3V`, 3+4 `5VSB`, 5+6 `12V`, 7 `GND` / 8 `GPIO8`, 9 `GND` / 10 `GPIO9`, 11 `GND` / 12 `GPIO20`, 13 `GND` / 14 `GPIO21`, 15 `GND` / 16 `GPIO0` | optional breakout: the three rails straight from the PSU header on two pins each (see the current budget below), then every GPIO the board leaves unused with a ground beside it. GPIO8 is also the module's blue LED. GPIO8 and GPIO9 are ESP32-C3 strapping pins: leave them high or floating at reset (GPIO9 low at reset enters download mode). GPIO0 is a plain GPIO on the C3 |
| FAN1 J5 … FAN4 J8 | 1 `G`, 2 `12V`, 3 `T`, 4 `PWM` | standard 4-pin fan pinout, tach unconnected |

The fan headers are mounted rotated, so on the board their pins read
`PWM T 12V G` left to right (pin 1, GND, is the *right* pin of each header)
and every header's lock ramp faces down the board, away from the strip
connector.
The header names are the config's: FAN1 (top left) is GPIO5 and `header1` in
the config's `fans` block, FAN2 top right, FAN3 bottom left, FAN4 (bottom
right) is GPIO10 and `header4` (the header → GPIO map is `FAN_PINS` in
`tools/pincheck.py`). Each header's name is printed just below it.

The strip path is sized for 3 A: a 3 mm 3.3V trace from the header's front
3.3 V pin (the rear one is tied behind it) up the header's left side and
across the plug zone into the VH, and a 2.5 mm ground return from the VH
back to the header's ground pins, both on 1 oz copper (about 15 °C rise at
3 A). All grounds are one net; 5VSB, 3.3V and 12 V are three separate rails.
The fan block's 12 V is split: a 1.5 mm top-layer track feeds the left column
(FAN1, FAN3) and a 1.2 mm bottom-layer track the right column (FAN2, FAN4),
each good for two fans or a fan and an AIO pump with margin (about 2.7 A per
column at a 10 °C rise; a pump is 0.3–1 A, a fan 0.1–0.5 A).

J11's rails are sized for about 2 A each, not for the PSU's full output.
The button LED's and J11's 12 V (1.2 mm) leaves the header through the gap
between its pin rows, runs along the bottom edge under the connectors and
climbs the strip between J11 and the module on the bottom layer; J11's 3.3V
(0.9 mm — the most that fits between the mounting holes and the board edge)
takes the same gap and the bottom edge a little lower, then climbs the left
edge; its 5VSB (1.0 mm) comes from the module's 5V pad over the top of the
module and down the same strip on the top layer. The 5VSB trunk from the
header to the module is 1.5 mm, since everything J11 draws flows through it.

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
| C120103 | CCO CF1/4W-100KΩ±5%, ¼ W carbon film, axial | R1 | 1 | 100 |
| C22365658 | DLL-5569-10AW, Mini-Fit Jr 2×5 4.2 mm **right-angle** header (Molex 5569-10A2 clone, no pegs) | J1 | 1 | 5 |
| C160316 | JST B3P-VH(LF)(SN), 3.96 mm 3-pin, 10 A | J3 | 1 | 5 |
| C594232 | JST B4B-XH-A-G, 2.5 mm 4-pin (gold flash) | J9 | 1 | 5 |
| C2337 | BOOMELE 2.54-1*40P, 1×40 pin header strip (break off 1 pin for J10) | J10 | 1 | 5 |
| C124382 | Ckmtw B-2100S32P-B110, 2×16 pin header (cut to 2×8), optional | J11 | 1 | 5 |
| C140769 | Ckmtw W-2510S04P-0000, KF2510 4-pin straight header with lock ramp | J5–J8 | 4 | 10 |
| C157899 | JST VHR-3N housing | strip plug | 1 | 10 |
| C160349 | JST SVH-21T-P1.1 crimp contact, 18–22 AWG | strip plug (3 + spares) | 5 | 100 |
| C144403 | JST XHP-4 housing | button plug | 1 | 20 |
| C140573 | JST SXH-001T-P0.6 crimp contact, 22–28 AWG | button plug (4 + spares) | 6 | 100 |

Every number, name, stock and minimum order above was checked against
LCSC's product data on 8 Sep 2026, and the J1 and fan-header datasheets
were read against the footprints. Do **not** substitute BOOMELE 2.54-4AS
(C41927) for the fan headers: despite its listing it is a fully shrouded
2.54 mm wafer (CJT A2541 style) that a PC fan plug cannot enter.

The whole list is a few dollars; the minimum orders are what set the
quantities (the two crimp contacts come in bags of 100). The PSU header mates with the PSU's own plug and the fan headers
with the fans' plugs, so those need nothing. The JST contacts are crimp
type: a cheap ratchet crimper for open-barrel terminals (an SN-01BM or
similar) does both series, or solder the wire into the contact's barrel and
squeeze the insulation tabs with pliers. Use 20 AWG for the strip wires.

Two things to check when the parts arrive, before soldering:

- **Fan headers.** A PC fan plug is a KF2510-family housing, so it mates
  with the W-2510S04P; the header's lock ramp sets which way round the plug
  goes. Before powering a fan, put a meter on the header and confirm the
  fan's black wire lands on the pad marked `G` and yellow/red on `12V`. If
  the plug only fits the other way round, change the fan headers' rotation
  from 180 to 0 in `generate.py` (`PARTS`, the `J5..J8` loop) and re-order,
  or re-pin the fan plug.
- **The PSU header** orientation, as described above.

## Flashing for this board

```sh
# the button's second pin is a real GND, not GPIO21
sudo make flash-pwr PWR=on PWR_BUTTON_GND=-1
# fans: header1..header4 in the config's fans block are FAN1..FAN4 (GPIO 5, 6,
# 7, 10) — enable the ones you use, then write the block
sudo make flash-fan
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
  USB-C away from you, the 5V pin is the top pin of the **right** column.
  If your module differs, swap `SM_LEFT` and `SM_RIGHT` in `generate.py` —
  the nets follow the names, but the tracks are laid out for this
  arrangement and will need re-routing.
- **Check the row spacing.** Drawn at 15.24 mm (0.6″). Print
  `out/zoom-top.pdf` at 1:1 or measure the module before ordering.
- **The USB-C legs.** Many Super Minis have the USB-C shell soldered through
  the board, leaving solder bumps underneath. If yours does, mount the module
  on ~1 mm pin stubs (or cut pin-header pins short) rather than dead flat; the
  carrier has no copper on the top layer under the module, only its own pads
  (the fan PWM lines run under its USB end on the *bottom* layer). The
  module's top edge is 3.9 mm inside the board edge, so a USB-C plug's
  overmould hangs over the carrier — it clears, there is nothing under it.
- **Antenna.** The module's ceramic antenna sits at the end away from the
  USB-C. The carrier keeps a copper-free zone under it and has no ground pour
  at all, so BLE range is whatever the Super Mini manages on its own.
- **The PSU header** solders with its housing over the front pin row and its
  face pointing up the board; the two pegs snap into the 3 mm holes. Nothing
  may be placed in the plug zone in front of it.
- **J11** is optional: leave it off unless you need a rail or a spare GPIO.
  It sits in the strip between the board's left edge and the module's left
  pin column, so that column has no pin names printed (the right column's
  names orient the module); the header's own pin names are printed on the
  **back** of the board, under the module, readable with the board flipped
  (`2x 3.3V` means both pins of that row).
  Its rows sit half a pitch below the module's so its plastic clears the
  corner screw head; a 5.5 mm head fits, a washer does not. The same goes
  for the button connector J9 and the screw at H3: 0.75 mm from a 5.6 mm
  pan head, no room for a washer. (KiCad's DRC flags both as courtyard
  overlaps because the stock M3 footprint draws a 6.9 mm courtyard; `drc.py`
  reports those and does not fail on them.)
- Q1's flat face follows the silkscreen outline (pins D, G, S left to right
  as printed); R1 has no polarity.

## Editing

The whole design is `generate.py`: parts, nets, placement and every track.
Edit it and run `make`, which regenerates the KiCad files, runs `check.py`
(copper clearance ≥ 0.2 mm, edge ≥ 0.3 mm, no track copper in the antenna
zone, every net one connected piece — pads are checked as their true
rectangles, which is what lets tracks pass between the header's pads), runs
KiCad's real DRC through `drc.py` (the `pcbnew` Python module; fails on any
error except the two mounting-hole courtyard overlaps above), renders
`out/preview.png` (a quick Pillow drawing of the layout, no KiCad needed)
and exports `out/`. The KiCad files are ordinary KiCad 7 files too — open
`bc250_carrier.kicad_pro` and edit in the GUI if you prefer; then the
generator no longer describes the board, so pick one.

The board has two vias (both on GATE, which has to hop over a vertical on
each layer); they are listed in `VIAS` and treated as two-layer pads by the
check. Everything else changes layer through the through-hole pads. The
button's ground reaches the module's GND pad up the module's right side, just
outside the antenna zone, so the gap between the header's pin rows is free
for the LED's 12 V.

KiCad 7's `kicad-cli` cannot run DRC or ERC (those came with KiCad 8); the
DRC comes from `drc.py` instead, ERC has no stand-in. The remaining DRC
warnings are silkscreen text touching footprint outlines by hundredths of a
millimetre.

Fab settings: 2 layers, 1.6 mm, 1 oz copper, min track 0.5 mm, min
clearance 0.2 mm, min drill 0.5 mm (the vias) — any board house's cheapest
tier. Stock symbols and footprints are copied from the local KiCad install
(`KICAD_SHARE=/usr/share/kicad`), so the project opens without extra
libraries.
