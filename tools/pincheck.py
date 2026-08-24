"""Per-chip GPIO facts shared by the config encoders (pwrcfg.py, fancfg.py).

The firmware has no console and quietly drops pins it can't use, so the
encoders are the only place a wiring mistake can be caught — and they must
agree on what the chips look like. This module is that single source of
truth; the encoders keep their own role-specific rules (what an ADC sense
pin needs, what a PWM output needs) and error wording.

Not an executable — imported. (pwrcfg.py itself can't be imported: it runs
argparse at module top level, which is why the tables live here instead.)
"""

# Per-chip pin facts for the targets CI publishes images for; the rest of the
# Makefile's VALID_TARGETS get only a range check (MAX_PIN).
#   flash:      pads bonded to the SPI flash — driving one bricks the boot
#   input_only: no output driver (can't drive anything) and no internal
#               pull-up either (can't be a button, which relies on one)
#   adc:        pins an analog sense wire can use. C3 note: GPIO5 is its ADC2,
#               unreliable per errata — deliberately excluded
#   hold:       pins where gpio_hold_en can latch a level through a reset; on
#               the plain ESP32 only the RTC-capable ones. None = every pin
#   strap:      pins whose level at reset selects the BOOT MODE. A hazard for
#               anything that can sit low at reset (a sense wire on a dead
#               rail, an external device's pull) — warning, not error. The
#               C3's GPIO2 is deliberately NOT listed: it is a strapping pin,
#               but per Espressif boot mode is GPIO9/8 and GPIO2 "does not
#               determine" it — its only caveat is a pull-up recommendation
#               for glitch immunity, it is this project's sense pin, and it
#               boots fine on the real hardware. Warning on it just teaches
#               people to ignore warnings
#   reserved:   pins this project already talks to the host on
CHIPS = {
    'esp32c3': dict(
        max_pin=21,
        flash={11, 12, 13, 14, 15, 16, 17},
        input_only=set(),
        adc={0, 1, 2, 3, 4},
        hold=None,
        strap={8, 9},
        reserved={18: 'USB D- (the host link)', 19: 'USB D+ (the host link)'},
    ),
    'esp32': dict(
        max_pin=39,
        flash={6, 7, 8, 9, 10, 11},
        input_only={34, 35, 36, 37, 38, 39},
        adc={32, 33, 34, 35, 36, 37, 38, 39,        # ADC1
             0, 2, 4, 12, 13, 14, 15, 25, 26, 27},  # ADC2 (fine: no WiFi here)
        hold={0, 2, 4, 12, 13, 14, 15, 25, 26, 27, 32, 33},
        strap={0, 2, 5, 12, 15},
        reserved={1: 'UART0 TX (the host link)', 3: 'UART0 RX (the host link)'},
    ),
}
MAX_PIN = {'esp32s2': 46, 'esp32s3': 48, 'esp32c6': 30, 'esp32h2': 27}


def top_pin(target):
    """Highest GPIO number the target has, or None for an unknown target."""
    chip = CHIPS.get(target)
    return chip['max_pin'] if chip else MAX_PIN.get(target)


def pin_byte(v):
    """GPIO number to its wire byte: 0xFF = not wired (matches the firmware's
    decode, where erased flash reads 0xFF)."""
    return 0xFF if v < 0 else v


def parse_avoid(entries):
    """Parse repeated --avoid values, each "GPIO:label" (e.g. "3:--ps-on
    (power switch)"), into {gpio: label}. Raises ValueError on a malformed
    entry so the encoder can report the flag by name."""
    taken = {}
    for e in entries or []:
        num, sep, label = e.partition(':')
        if not sep or not num.strip().isdigit() or not label.strip():
            raise ValueError(e)
        taken[int(num.strip())] = label.strip()
    return taken
