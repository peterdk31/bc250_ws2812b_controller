# Serial LED controller

A Linux daemon that renders WS2812 (NeoPixel) effects on the host and
streams the frames over USB serial to an ESP32, which only clocks them
out to the strip. All logic — sensor reading, rule evaluation, effect
rendering — lives on the host, so changing behavior never requires
reflashing the microcontroller.

Built for an AMD BC-250 running its case lighting from CPU temperature
and load, but nothing is board-specific. Co-authored with
[Claude](https://claude.com/claude-code) (Anthropic).

```
host (led daemon)                      ESP32                 strip
sensors → rules → effect → frames ──serial 921600──→ RMT ──→ WS2812
```

## Hardware

- Any ESP32 dev board connected over USB (shows up as `/dev/ttyUSB0`
  or `/dev/ttyACM0`).
- WS2812/WS2812B strip: data to the GPIO configured as `strip.pin`
  (default 13), plus 5 V and GND. Power the strip from a supply that
  can handle it, not the dev board's regulator; share grounds.

## Quick start

Prerequisites: `g++` and `make` for the daemon,
[arduino-cli](https://arduino.github.io/arduino-cli/) for flashing the
receiver. On an immutable distro like Bazzite the base image has no
compiler — layer it (`rpm-ostree install gcc-c++ make`) or build in a
distrobox.

```sh
sudo make install    # build daemon + systemd unit + default config
sudo make flash      # compile + upload the ESP32 receiver
sudo systemctl enable --now led-controller
```

Then adjust `/etc/led-controller/config.json` for your hardware — at
least `strip.leds`, `strip.pin` and `serial.port` — and
`sudo systemctl restart led-controller`.

To test an effect directly, stop the service (it holds the serial
port) and run e.g. `led /etc/led-controller/config.json rainbow` —
unprivileged once the port group set up by `make install` is active
(next login), with `sudo` before that. If the strip stays dark,
`journalctl -u led-controller` shows what the daemon is unhappy about.

The sections below cover each step in detail.

## Flashing the receiver

With [arduino-cli](https://arduino.github.io/arduino-cli/) installed:

```sh
sudo make flash                     # compile + upload
sudo make flash PORT=/dev/ttyACM0   # explicit port; FQBN=... for other boards
```

The port, baud rate, host timeout, LED count, pin and brightness
default to the matching keys from `/etc/led-controller/config.json`
(falling back to the repo's `config.json`), so flashing targets the
same adapter the daemon uses and bakes the matching settings into the
sketch. On first run, `flash` installs the ESP32 core and
the strip library automatically (also available separately as
`make receiver-setup`); `make receiver` compiles without uploading.

`flash` stops the led-controller service around the upload (the daemon
holds the port open) and restarts it afterwards if it was running.
`sudo` is needed for that and, typically, for port access. arduino-cli
keeps installed cores per user, so stick to one user for flashing — on
a dev machine without the service, everything works unprivileged once
your user is in the port's group (usually `dialout` or `uucp`).
`make install` and `make flash` add you to it automatically (also
available standalone as `make serial-perms`); membership takes effect
on your next login.

Alternatively, open `esp32_receiver/esp32_receiver.ino` in the Arduino
IDE with the ESP32 core and the `Freenove_WS2812_Lib_for_ESP32`
library installed, and flash from there.

The receiver is generic: pin and LED count come from the host in every
frame, so it never needs reflashing for config changes.

From power-on until the first valid host frame, the receiver runs a
standalone breathing animation (the host's idle warm amber, through
the same gamma/brightness curve), so the strip isn't dark while the OS
boots and the daemon starts. The geometry comes from the last valid
frame (remembered in flash), falling back to the `strip.leds`/
`strip.pin` baked in by `make flash`; a never-flashed,
never-driven board stays dark. The animation never resumes after the
host goes quiet — that means shutdown or crash, where the usual
timeout blank is the right answer.

**Host and receiver must be updated together** when the wire protocol
changes (it gained a checksum byte — an old receiver will reject every
frame from a new host and vice versa).

## Building and installing the host daemon

```sh
make
sudo make install        # /usr/local/bin/led + systemd unit + default config
sudo systemctl enable --now led-controller
```

`make install` never overwrites an existing `/etc/led-controller/config.json`.
It also adds your user to the serial port's group so `led` can be run
directly for testing (takes effect on next login). `make uninstall`
removes everything, including the config.

The daemon exits on serial open/write failure and the systemd unit
restarts it every 2 s indefinitely, so unplugging the adapter (or the
adapter enumerating late at boot) heals itself.

Usage:

```
led <config>            run startup effect, then rules
led <config> <effect>   run a single effect forever (testing)
led --list              list available effects
```

## Wire protocol

Each frame, host → receiver:

| bytes | content |
|---|---|
| 2 | sync `0xAA 0x55` |
| 1 | GPIO pin |
| 2 | LED count, little-endian |
| 3·n | RGB, one triple per LED |
| 1 | checksum: XOR of pin, count and RGB bytes |

The receiver drops frames with a bad checksum and rescans for sync, so
a desync (sync bytes occurring inside pixel data) recovers within a
frame. If no valid frame arrives for `serial.host_timeout_ms` (default
5000 — host crashed, cable pulled), the receiver blanks the strip.

The baud rate adapts at runtime: when bytes keep arriving but no frame
decodes for 0.5 s, the receiver steps to the next rate from the host's
supported set and retries until frames validate ("baud hunting"), then
remembers the working rate in flash (NVS). So changing `serial.baud`
only needs a service restart: the strip recovers within a few seconds,
once — after that the receiver boots straight into the remembered
rate, no reflash needed. Reflashing with a new baud also gives an
instant lock (a changed firmware default outranks the remembered
rate). A silent line never triggers hunting, so a host that merely
stopped finds the receiver where it left it.

Gamma correction (`strip.gamma`), white balance (`strip.white_balance`)
and brightness (`strip.brightness`) are applied on the host before
sending; the bytes on the wire are final PWM values. The three fold
into one per-channel lookup table, so they cost nothing per frame. See
[Tuning the colors](#tuning-the-colors) for how to set them.

## Configuration

`/etc/led-controller/config.json`. Top-level keys:

```jsonc
{
    "serial": {
        "port": "/dev/ttyUSB0",
        "baud": 921600,             // receiver locks onto this by itself;
                                    // `make flash` bakes it in as the
                                    // boot default for an instant lock
        "host_timeout_ms": 5000     // receiver blanks the strip after this
                                    // long without a frame; only read by
                                    // `make flash`, not the daemon
    },
    "strip": {
        "leds": 58,
        "pin": 13,              // ESP32 GPIO driving the strip
        "brightness": 0.2,      // 0..1, scales linear light output
        "gamma": "2.2",         // perceptual correction; 1.0 disables.
                                // three space-separated values ("2.0 2.2
                                // 2.4") set it per R/G/B channel
        "white_balance": "b4ffff" // RRGGBB "white" that reads neutral
                                  // through the diffuser/filter; ffffff
                                  // disables
    },
    "sensors": [ ... ],         // temp sensor candidates, see below
    "hold_seconds": 10,         // min seconds between effect switches
    "startup": { ... },         // one-shot effect on boot, see below
    "rules": [ ... ]            // see below
}
```

### Tuning the colors

Three `strip` settings shape what reaches your eye. They stack in this
order — gamma curves each channel, white balance rescales it, then
brightness scales the whole thing — and all three are folded into one
per-channel lookup table on the host, so changing them is free per
frame. A reflash (`make flash`) is only needed to match the receiver's
standalone power-on breathe; the daemon picks up changes on restart.

**`brightness`** (0..1) — overall output, linear in light. Set this
first, to whatever is comfortable in the room. The two corrections
below hold across the whole range, so you don't need to re-tune them
when you change brightness.

**`white_balance`** (RRGGBB, `ffffff` = off) — corrects an overall
color cast, typically from a diffuser or filter in front of the strip.
It's a per-channel *linear gain*: each hex byte scales that channel
(`b4` on red = red runs at 0xB4/0xFF ≈ 71%). To set it: command a full
white and find the RRGGBB that looks neutral through your cover —
whichever channel leaks through too strongly gets pulled below `ff`. A
black film tends to pass red most, so the fix lands around `b4ffff`
(dimmer red, full green/blue). Because the gain is linear, the balance
that looks neutral at full brightness stays neutral when dimmed.

**`gamma`** (default `"2.2"`, `1.0` = off) — perceptual correction so
mid-level values look mid-bright. One value applies to all channels;
three space-separated values (`"2.0 2.2 2.4"`) set red/green/blue
independently. Per-channel gamma is the tool for a cast that *only*
shows at one end of the brightness range — most often dim whites
drifting in color because the WS2812's three dies don't track each
other near their PWM floor. White balance can't fix that (it's the
same gain at every level); a per-channel gamma can, because it bends
the midtones without moving the endpoints.

Suggested order when dialing it in:

1. Set `brightness` for the room.
2. At a *bright* white, adjust `white_balance` until white looks
   neutral.
3. Drop to a *dim* white. If it now has a tint (e.g. goes bluish),
   lower the gamma of the channel that fades too fast — bluish means
   red is dropping out, so try `"2.0 2.2 2.2"`. Raising a channel's
   gamma darkens its midtones; lowering it lifts them.
4. Re-check the bright end (step 2 is unaffected by gamma at full
   white) and `make flash` so the power-on breathe matches.

### Sensors

`sensors` is an ordered list of `chip:label` hwmon candidates; the
first one present wins. A bare chip name means that chip's
`temp1_input`. Used by `temp` conditions and the `cpu_temp` effect.
If no candidate is present at startup (e.g. the sensor module loads
late), the daemon keeps rescanning on every rule tick until one
appears.

The default covers the BC-250: `k10temp:Tctl`, then the NCT6686D under
the nct6687d driver (`nct6686:CPU`) and the in-kernel nct6683 driver
(`nct6686:AMD TSI Addr 98h`), with fallbacks.

### Startup

```jsonc
"startup": {
    "effect": "boot",
    "settings": { "duration_seconds": 5 },
    "max_seconds": 30   // safety cap if the effect never reports finished
}
```

Runs once until the effect reports finished (or `max_seconds` elapses,
default 30), then the rule engine takes over. Omit for no startup
effect.

### Rules

Evaluated top to bottom every 0.5 s; the first rule whose condition
holds wins. A rule without `"if"` always matches (put a catch-all
last). With no rules at all, `cpu_temp` runs.

```jsonc
{
    "if": "proc:gamescope & temp>75",
    "effect": "fire",
    "hold": 0,                       // optional, overrides hold_seconds
    "settings": { "speed": 1.6 }     // optional, effect-specific
}
```

Switching to a different effect (or the same effect with different
settings) waits until `hold_seconds` have passed since the last switch,
so rules that flap don't strobe the strip. A per-rule `"hold"`
overrides that — `"hold": 0` on the overheat alarm makes it take over
immediately.

### Conditions

| syntax | true when |
|---|---|
| `always` | always |
| `temp>N` / `temp<N` | sensor temperature above/below N °C |
| `cpu_load>N` / `cpu_load<N` | CPU busy above/below N % (`/proc/stat`); bare `cpu_load` means `>20` |
| `gpu_load>N` / `gpu_load<N` | GPU busy above/below N % (amdgpu `gpu_metrics` gfx activity, falling back to `gpu_busy_percent`); bare `gpu_load` means `>20` |
| `proc:NAME` | a process with that name is running (comm, 15-char kernel limit) |
| `steam_dl` | a Steam download is actively moving bytes |
| `file:/PATH` | the file exists |
| `!COND` | negation |
| `A & B` | both hold |
| `A \| B` | either holds; `&` binds tighter, no parentheses |

`file:` conditions are the external control surface — e.g.
`touch /tmp/led-night` from a script or cron to switch modes, `rm` it
to switch back. (`/tmp` clears on reboot.)

### Steam download progress

The `steam_dl` condition is true while Steam is actively downloading,
and the `steam_download` effect turns the strip into the download's progress
bar (the default config ships this rule). The daemon polls every Steam
library's `appmanifest_*.acf` files — Steam rewrites them as chunks
land — and shows percent of outstanding bytes across all active
downloads. The head pixel is antialiased, so the bar resolves single
percents even on short strips.

It finds Steam roots under `/home/*` and ostree's `/var/home/*`,
follows extra libraries from `libraryfolders.vdf` (SD cards, second
disks), ignores games whose update is pending but not started
("update on next launch"), and hides the bar if a download stalls for
2 minutes (Steam doesn't always set the paused flag). The strip pulses
green while a finished download installs, then returns to normal.
Reading other users' Steam directories relies on the daemon running as
root (the systemd unit does).

## Effects

| name | description | settings (defaults) |
|---|---|---|
| `alarm` | whole-strip pulse | `color` (ff0000), `pulses_per_second` (2) |
| `aurora` | slow drifting color curtains | `speed` (1.0), `hue_min` (0.30), `hue_max` (0.85) |
| `boot` | power-on sequence: spark sweep, rippling charge-up, flash-out; reports finished | `duration_seconds` (5), `color` (0028ff), `flash_color` (ffffff) |
| `breathe` | single color on a slow sine | `color` (ff7818), `period_seconds` (5), `min_brightness` (0.05) |
| `comet` | Larson scanner with fading tail | `color` (ff0000), `sweeps_per_second` (0.5), `tail_pixels` (8) |
| `cpu_temp` | temperature bar graph along a hue ramp | `temp_min` (40), `temp_max` (85), `cold_color` (0000ff), `hot_color` (ff0000), `sensors` |
| `fire` | per-LED candle flicker | `speed` (1.0), `min_heat` (0.25) |
| `load` | CPU/GPU bars growing from center | `smoothing_seconds` (0.5), `center_color` (00ff00), `edge_color` (ff0000) |
| `rainbow` | scrolling hue cycle | `cycles_per_second` (0.625) |
| `steam_download` | Steam download bar, green pulse at 100 | `color` (00a0ff), `done_color` (00ff40), `smoothing_seconds` (0.4) |
| `twinkle` | sparks fading over a base color | `color` (ffffff), `base_color` (000020), `sparks_per_second` (6), `fade_seconds` (1.0) |

Colors are `"RRGGBB"` or `"#RRGGBB"`. Effect settings come from the
matching rule's `"settings"`, falling back to top-level config keys,
then the defaults above.

### Adding an effect

Drop a new file in `effects/` — every `effects/*.cpp` is compiled in
automatically:

```cpp
#include "effect.hpp"

class MyEffect : public Effect
{
public:
    void init(const EffectConfig& cfg, int leds) override { ... }
    void render(WS2812Serial& strip, float t) override { ... }
    int frameDelayMs() const override { return 16; }   // optional
    bool finished() const override { return false; }   // optional
};

REGISTER_EFFECT("my_effect", MyEffect)
```
