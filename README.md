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
**power-on effect** on its own, so the strip isn't dark while the OS boots
and the daemon starts; on a clean shutdown it runs a **shutdown effect**
after the host has gone. These standalone animations are the *actual* host
effects compiled into the firmware (`make receiver` mirrors `effect.hpp`,
the color LUT and every `shared/effects/*.cpp` into the sketch's
`src/`), so they render through the same correction as the daemon's frames
and can't drift from it.

Both slots are configurable from the `esp32` block in `config.json` — set
each to any **standalone** effect (the host-data effects in
`host/effects/` can't run on the receiver) with its own settings:

```jsonc
"esp32": {
    "power_on": { "effect": "breathe",  "settings": { "color": "ff7818" } },
    "shutdown": { "effect": "shutdown", "settings": { "color": "0028ff" } }
}
```

The receiver has no JSON parser, so the daemon sends this config over
serial at startup and the receiver remembers it in flash (NVS). It takes
effect on the next daemon (re)start: the **shutdown** slot applies that
same session, while the **power-on** slot — which runs before the daemon
is up — applies on the *next* power cycle. A fresh, never-driven board
uses the firmware's built-in defaults (`boot` / `shutdown`) until the
daemon first pushes config. A power-on effect that loops (e.g. `rainbow`)
runs until the host takes over; a finite one (e.g. `boot`) plays once and
then holds its last frame (`boot` ends dark) until the host takes over —
for a boot-then-idle look, set the power-on slot to a looping effect or
compose one effect that does both. A shutdown effect plays to completion —
or, if you pick a looping one, until the strip loses power.

Strip geometry comes from the last valid frame (remembered in NVS),
falling back to the `strip.leds`/`strip.pin` baked in by `make flash`; a
never-flashed, never-driven board stays dark. The receiver — independently
powered — outlives the host, which is what lets it finish the shutdown
effect. A host that goes quiet *without* sending the shutdown command (a
crash or pulled cable) still blanks on the host timeout. A later valid
frame cancels any standalone state, so `systemctl stop` → `start` resumes
cleanly.

**Host and receiver must be updated together** when the wire protocol
changes (e.g. it gained a checksum byte, then a command frame — an old
receiver ignores the shutdown command and falls back to the timeout
blank; mismatched checksums are rejected outright).

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
led <config>            run the rules
led <config> <effect>   run a single effect forever (testing)
led --list              list available effects
```

## Wire protocol

Pixel frame, host → receiver:

| bytes | content |
|---|---|
| 2 | sync `0xAA 0x55` |
| 1 | GPIO pin |
| 2 | LED count, little-endian |
| 3·n | RGB, one triple per LED |
| 1 | checksum: XOR of pin, count and RGB bytes |

There is also a **command frame**, distinguished by a different second
sync byte so the pixel-frame parser skips over it:

| bytes | content |
|---|---|
| 2 | sync `0xAA 0x56` |
| 1 | command (`0x01` = shutdown, `0x02` = config) |
| 2 | payload length, little-endian |
| n | payload |
| 1 | checksum: XOR of command, length and payload bytes |

`shutdown` carries no payload. `config` carries the power-on and shutdown
slots as two NUL-terminated strings (effect name, then `key=value` lines).
The byte values and payload format live in `protocol.hpp`, shared by host
and firmware.

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
    "esp32": {                  // effects the receiver runs on its own,
        "power_on": { ... },    // pushed to it over serial; see "Flashing
        "shutdown": { ... }     // the receiver". Standalone effects only
    },
    "rules": [ ... ]            // see below
}
```

### Tuning the colors

Three `strip` settings shape what reaches your eye. They stack in this
order — gamma curves each channel, white balance rescales it, then
brightness scales the whole thing — and all three are folded into one
per-channel lookup table on the host, so changing them is free per
frame. A reflash (`make flash`) is only needed to match the receiver's
standalone power-on animation; the daemon picks up changes on restart.

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
   white) and `make flash` so the power-on animation matches.

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

There's no host-side startup effect: the boot animation is the receiver's
power-on effect (the `esp32.power_on` block above), played at true
power-on while the OS boots. The daemon goes straight to the rules when it
connects.

### Rules

Evaluated top to bottom every 0.5 s; the first rule whose condition
holds wins. A rule without `"if"` always matches (put a catch-all
last). When no rule matches — including an empty or missing `rules`
list — the daemon sends no frames and leaves the strip dark.

```jsonc
{
    "if": "proc:gamescope & temp>75",
    "effect": "fire",
    "hold": 5,                       // optional, min seconds since last
    "settings": { "speed": 1.6 }     // switch; optional, effect-specific
}
```

Switching to a different effect (or the same effect with different
settings) waits until the rule's `"hold"` seconds have passed since the
last switch, so a rule that flaps doesn't strobe the strip. `"hold"`
defaults to 0 — switch immediately — so only a rule that wants
debouncing sets its own.

A switch isn't abrupt: the daemon crossfades from the last displayed
frame into the new effect over the top-level `transition_seconds`
(default 0.6; set 0 for an instant cut). The dissolve runs on
wall-clock, so it's the same length whatever the effects' frame rates,
and the very first effect after startup appears immediately (nothing to
fade from).

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
library's `appmanifest_*.acf` files: a download is "in progress" while a
manifest is marked actively updating (which holds across every part of a
multi-part install), and "transferring right now" additionally requires
real bytes arriving over the network — so a pause or the verify phase of
a finished download (manifest still says "running", but nothing comes off
the wire) drops the bar. `BytesDownloaded` in the manifest barely moves
mid-download (Steam flushes it on pause/stop), so the live percent is
driven by integrating network throughput onto the manifest's total; the
bar restarts at 0 for each part as its `BytesToDownload` changes. The
head pixel is antialiased, so the bar resolves single percents even on
short strips.

It finds Steam roots under `/home/*` and ostree's `/var/home/*`,
follows extra libraries from `libraryfolders.vdf` (SD cards, second
disks), ignores games whose update is pending but not started
("update on next launch"), and hides the bar a few seconds after the
network goes quiet (a pause or a finished download being verified). The
percent is kept across brief inactivity, so the effect resuming after
another effect borrows the strip picks up where it left off instead of
snapping to 0. The strip pulses green while a finished download installs,
then returns to normal.
Reading other users' Steam directories relies on the daemon running as
root (the systemd unit does).

## Effects

| name | description | settings (defaults) |
|---|---|---|
| `alarm` | whole-strip pulse | `color` (ff0000), `pulses_per_second` (2) |
| `aurora` | slow drifting color curtains | `speed` (1.0), `hue_min` (0.30), `hue_max` (0.85) |
| `boot` | power-on: a CRT-style ignition — a white-hot point flares at the center, whips outward into a scan line, resolves to the body color, holds, then fades to black; reports finished | `duration_seconds` (6), `color` (0028ff), `flash_color` (ffffff) |
| `breathe` | single color on a slow sine | `color` (ff7818), `period_seconds` (5), `min_brightness` (0.05) |
| `comet` | Larson scanner with fading tail | `color` (ff0000), `sweeps_per_second` (0.5), `tail_pixels` (8) |
| `cpu_temp` | temperature bar graph along a hue ramp, soft tip + slow brightness shimmer | `temp_min` (40), `temp_max` (85), `cold_color` (0000ff), `hot_color` (ff0000), `speed` (1.0), `sensors` |
| `cycle` | rotates through a list of other effects, switching to a random next one each period | `period_seconds` (30), `effects` (list of `{effect, settings, seconds}`) |
| `fire` | per-LED candle flicker with a slow flowing drift | `speed` (1.0), `min_heat` (0.25) |
| `load` | CPU/GPU bars from center, aurora-washed with soft tips + shimmer | `smoothing_seconds` (0.7), `hue_min` (0.45), `hue_max` (0.83), `speed` (1.0) |
| `rainbow` | scrolling hue cycle | `cycles_per_second` (0.625) |
| `shutdown` | power-down sequence: a CRT-style collapse — the picture snaps inward to a white-hot point, then fades out with phosphor persistence; reports finished | `duration_seconds` (2.0), `color` (0028ff), `flash_color` (ffffff) |
| `steam_download` | Steam download bar, slow breath + flow sweep, green pulse at 100 | `color` (00a0ff), `done_color` (00ff40), `smoothing_seconds` (0.4), `flow_period` (2.5), `flow_depth` (0.35), `shimmer_depth` (0.15), `shimmer_period` (6.0) |
| `twinkle` | sparks fading over a base color | `color` (ffffff), `base_color` (000020), `sparks_per_second` (6), `fade_seconds` (1.0) |

Colors are `"RRGGBB"` or `"#RRGGBB"`. Effect settings come from the
matching rule's `"settings"`, falling back to top-level config keys,
then the defaults above.

`cycle` composes the others: each entry in its `"effects"` list carries
its own `"settings"` (resolved exactly like a rule's — entry settings
first, then top-level keys, then that effect's defaults) and an optional
`"seconds"` to override `period_seconds` for that one. It picks a random
next entry each time, runs each sub-effect on its own clock, and hands
off early if a sub-effect reports itself finished. It's host-only (the
nested list needs the JSON config), so it can't be a receiver
power-on/shutdown effect.

### Adding an effect

Drop a new file in one of two folders — both are compiled into the daemon
automatically:

- **`shared/effects/`** — pure animation, no host data. Also linked
  into the firmware, so it can be a receiver power-on/shutdown effect.
  Render against `Strip&` (it's `WS2812Serial` on the host, the receiver's
  driver on the ESP32).
- **`host/effects/`** — needs live host data (sensors, Steam, …). Daemon
  only; can't run on the receiver.

```cpp
#include "effect.hpp"

class MyEffect : public Effect
{
public:
    void init(const EffectConfig& cfg, int leds) override { ... }
    void render(Strip& strip, float t) override { ... }
    int frameDelayMs() const override { return 16; }   // optional
    bool finished() const override { return false; }   // optional
};

REGISTER_EFFECT("my_effect", MyEffect)
```

Use `Strip&` (not `WS2812Serial&`) for a standalone effect so it compiles
for the firmware too; host-only effects may use either.
