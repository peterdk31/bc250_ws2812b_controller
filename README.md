# Serial LED controller

A Linux daemon that renders WS2812 (NeoPixel) effects on the host and streams
the frames over USB serial to an ESP32, which just clocks them out to the strip.
All the logic — sensors, rules, effects — lives on the host, so changing
behavior never means reflashing the microcontroller.

Built for an AMD BC-250 running its case lighting from CPU temperature and load,
but nothing is board-specific. Co-authored with
[Claude](https://claude.com/claude-code) (Anthropic).

```
host (led daemon)                      ESP32              strip
sensors → rules → effect → frames ──serial──→ RMT ──→ WS2812
```

## Hardware

- An ESP32 or ESP32-C3 dev board over USB (`/dev/ttyUSB0` or `/dev/ttyACM0`).
- A WS2812/WS2812B strip: data to the GPIO set as `strip.pin` (default 13), plus
  5 V and GND from a supply that can handle it (not the board's regulator; share
  grounds).

## Getting started

Two things are needed on the host:

- **`g++` and `make`** to build the daemon. On CachyOS (or any Arch) they come
  in the `base-devel` package group.
- **`esptool`** to flash the receiver (plus `curl`, which is preinstalled).
  You do **not** need an ESP32 toolchain — `make flash` downloads a prebuilt
  firmware image.

```sh
sudo pacman -S --needed base-devel    # compiler + make
pipx install esptool                  # ESP32 flasher (or: pip install --user esptool)
```

On Bazzite (immutable base image) `esptool` installs the same way, but
`g++`/`make` aren't in the base image — build the daemon inside a distrobox,
or copy a prebuilt `led` binary over.

Then build, flash, and start:

```sh
sudo make install                            # daemon + systemd unit + default config
sudo make flash                              # download prebuilt firmware + flash the ESP32
sudo systemctl enable --now led-controller
```

`make flash` reads the chip (`esp32.target`, default `esp32c3`) and port
(`sinks.serial.port`) from the config; override on the command line, e.g.
`sudo make flash PORT=/dev/ttyACM0 TARGET=esp32`. It stops the service during the
write and restarts it after.

Finally, edit `/etc/led-controller/config.json` for your hardware — at least
`strip.leds`, `strip.pin`, and `sinks.serial.port` — then `sudo systemctl
restart led-controller`.

To try an effect directly (stop the service first, it holds the port):

```sh
led /etc/led-controller/config.json rainbow
```

This runs unprivileged once the serial-group membership added by `make install`
is active — that takes effect on your next login; before then, prefix `sudo`.

## How it works

The daemon renders every frame and sends it over serial; the receiver only
displays what arrives. Pin and LED count travel in each frame, so the firmware
is generic and never needs reflashing for a config or effect change.

While the daemon isn't driving the line — at cold boot before it starts, and
after it exits on shutdown — the receiver plays a **boot** and **shutdown**
animation from flash. The daemon renders those too (through the same color
pipeline), records them, and uploads them once at startup; the receiver stores
them and replays them. So editing the boot/shutdown effects also needs no
reflash — just a daemon restart. A fresh, never-recorded board stays blank
until the daemon has run once and uploaded the recordings.

The serial baud auto-negotiates: if bytes arrive but no frame decodes, the
receiver cycles through the supported rates until frames validate, then
remembers the working rate in flash. Changing `sinks.serial.baud` just needs a
service restart.

## Flashing the receiver

The firmware is a native [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/)
project under `firmware/` (no arduino-cli, no `.ino`). CI builds it for each
target and publishes merged images to GitHub Releases; `make flash` downloads
the one for `TARGET` and writes it — no toolchain needed.

```sh
sudo make flash                     # download prebuilt image + flash (default)
sudo make flash FW_RELEASE=v1.0.0   # pin a release instead of latest
```

The image is written at `0x0` and doesn't touch the NVS or LittleFS partitions,
so a reflash keeps a board's saved state.

**Building from source** is only needed to modify the firmware, or to bake a
non-default `sinks.serial.baud` / `esp32.host_timeout_ms` into it. It builds inside
Espressif's ESP-IDF container (needs `docker` or `podman` — nothing is installed
on the host, and `make receiver-clean` drops the cached image), or against a
native ESP-IDF if you set `IDF_PATH=...`:

```sh
sudo make flash-source              # build (container/native) + flash
make receiver                       # build only, no flash
```

## Configuration

`/etc/led-controller/config.json`:

```jsonc
{
    "sinks": {
        "serial": { "port": "/dev/ttyUSB0", "baud": 921600 }
    },                          // port "none" or "" (or LED_PORT=none) runs headless;
                                // omitting the key defaults to /dev/ttyUSB0
    "strip": {
        "leds": 58,
        "pin": 13,              // ESP32 GPIO driving the strip
        "reverse": false,       // flip so LED 0 is at the far end
        "brightness": 0.2,      // 0..1, linear
        "gamma": "2.2",         // one value, or three ("2.0 2.2 2.4") per R/G/B
        "white_balance": "b4ffff" // RRGGBB neutral-white gain; ffffff = off
    },
    "sensors": [ ... ],         // hwmon candidates, see Sensors
    "esp32": {
        "target": "esp32c3",    // chip to build/flash for (esp32, esp32c3, ...)
        "host_timeout_ms": 5000, // receiver blanks after this long with no frame —
                                 // baked into the firmware by `make flash-source`
                                 // only; the prebuilt image always uses 5000
        "power_on": { ... },    // boot/shutdown animations the receiver replays
        "shutdown": { ... }     // (standalone effects only)
    },
    "rules": [ ... ]
}
```

### Tuning the colors

`brightness`, `white_balance`, and `gamma` fold into one per-channel lookup
table applied on the host, so they cost nothing per frame. Dial them in this
order:

1. **`brightness`** — set for the room.
2. **`white_balance`** (RRGGBB, `ffffff` = off) — command full white and lower
   whichever channel leaks through your diffuser too strongly (a black film
   passes red most → around `b4ffff`). It's a linear gain, so it holds at any
   brightness.
3. **`gamma`** (default `2.2`) — perceptual midtone correction. If dim white
   picks up a tint that full white doesn't, use per-channel values to fix the
   channel that fades too fast (bluish dim = red dropping → `"2.0 2.2 2.2"`).

Low brightness costs no smoothness: the corrected values ride the wire with 8
extra fractional bits and the receiver rounds them away with a temporal dither
at its own strip-refresh rate (several hundred Hz — far above flicker fusion),
so dim gradients glide instead of stepping or shimmering. There is nothing to
configure; the old `dither` key is gone (this needs receiver firmware from the
same version — reflash once after updating). Restart the daemon after any
change so the boot/shutdown recordings re-capture with the new colors.

### Sensors

`sensors` is an ordered list of `chip:label` hwmon candidates; the first present
wins (a bare chip name means its `temp1_input`). Used by `temp` conditions and
the `cpu_temp` effect. The daemon keeps rescanning until one appears. The default
covers the BC-250 (`k10temp:Tctl`, then the NCT6686D under either nct driver).

### Rules

Evaluated top to bottom every 0.5 s; the first rule whose `if` holds wins. A rule
without `if` always matches (put a catch-all last). No match → strip dark.

```jsonc
{
    "if": "proc:gamescope & temp>75",
    "effect": "ember",
    "hold": 5,                       // min seconds since last switch (default 0)
    "settings": { "speed": 1.4 }     // effect-specific
}
```

Switches crossfade over the top-level `crossfade_ms` (default 600; 0 =
instant). `hold` debounces a flapping rule.

### Conditions

| syntax | true when |
|---|---|
| `always` | always |
| `temp>N` / `temp<N` | sensor temperature above/below N °C |
| `cpu_load>N` / `cpu_load<N` | CPU busy above/below N % (bare `cpu_load` = `>20`) |
| `gpu_load>N` / `gpu_load<N` | amdgpu busy above/below N % (bare `gpu_load` = `>20`) |
| `proc:NAME` | a process with that name is running (15-char kernel limit) |
| `steam_dl` | a Steam download is actively moving bytes |
| `file:/PATH` | the file exists |
| `!COND` | negation |
| `A & B` | both hold |
| `A \| B` | either holds (`&` binds tighter; no parentheses) |

`file:` is the external control surface — `touch /tmp/led-night` to switch modes,
`rm` to switch back.

The `steam_download` effect turns the strip into a live download progress bar
(percent driven by network throughput; green pulse at 100%). Reading other
users' Steam libraries needs the daemon running as root (the systemd unit does).

## Effects

| name | description | key settings (defaults) |
|---|---|---|
| `alarm` | urgent heartbeat throbbing from the center | `color` (ff0000), `pulses_per_second` (2) |
| `aurora` | slow drifting color curtains walking a palette | `palette` (10ff80,10a0ff,8040ff), `speed` (1.0) |
| `boot` | calm bloom from the center into a full wash, then exhales into a dim hold | `intro_seconds` (7), `color` (ffffff) |
| `breathe` | palette wash on a slow breath with a drifting bright band | `palette` (0d4a44,30e090), `period_seconds` (3.5) |
| `button_off` | experimental: the lit strip collapses into the power button | `duration_seconds` (1.4), `color` (ffffff), `reverse` (true) |
| `button_on` | experimental: light spills out of the power button, then pulses | `spread_seconds` (2.5), `period_seconds` (4), `color` (ffffff) |
| `comet` | Larson scanner with a fading tail, optional mirror | `palette` (8040ff,30c0ff), `sweeps_per_second` (0.7), `tail_pixels` (8) |
| `cpu_temp` | temperature bar along a hue ramp | `temp_min` (40), `temp_max` (85), `hot_color` (ff0000) |
| `cycle` | rotates through a list of other effects | `period_seconds` (30), `effects` (list) |
| `drift` | soft palette glows wandering over a dark base | `palette` (2858ff,30d0b0), `blobs` (3), `speed` (1.4) |
| `ember` | slow warm aurora-style flow through an ember palette | `palette` (2a0a00,ff7d1e), `speed` (1.4) |
| `lava` | lava lamp: soft blobs drift, meet and merge into hotter spots | `palette` (ff0050,ff5a00,ffd000,00e5ff,7a00ff,ff00d0), `blobs` (4), `speed` (5.0) |
| `load` | CPU/GPU bars from center over a palette wash, each with a heartbeat that quickens under load | `palette` (00e0c0,2060ff,a040ff), `smoothing_seconds` (0.7), `pulse` (0.5) |
| `plasma` | drifting sines + noise walking a palette | `palette` (0040ff,00d0a0,c040ff,0040ff), `speed` (1.4) |
| `pulse` | soft palette rings expanding from the center | `palette` (5028ff,30d0ff), `period_seconds` (4) |
| `rainbow` | scrolling hue cycle with shimmer | `cycles_per_second` (0.9) |
| `shutdown` | CRT-style collapse to a point, then a slow phosphor fade | `duration_seconds` (5.0), `color` (0028ff) |
| `steam_download` | live Steam download bar, green pulse at 100% | `palette` (d4009c,ff5a1e,ffd23c), `done_color` (00ff66) |
| `tide` | a palette gradient sliding and breathing | `palette` (102060,1890d0,30d0a0,8040ff,102060), `speed` (1.4) |

Colors are `"RRGGBB"` or `"#RRGGBB"`; a `palette` is several separated by
non-hex (`"2a0a00, ff7d1e"`), spaced evenly and blended in RGB. Every effect
also takes `frame_ms` (delay between frames; default 16 ≈ 60 fps).
Settings resolve per-rule first, then top-level config, then the defaults above.

### Boot & shutdown animations

Set `esp32.power_on` / `esp32.shutdown` to any standalone effect, or a
`sequence` played into one recording — typically a one-shot intro into a looping
idle:

```jsonc
"esp32": {
    "power_on": {
        "sequence": [
            { "effect": "boot",    "record_seconds": 4,
              "settings": { "intro_seconds": 3, "color": "ff7818" } },
            { "effect": "breathe", "record_seconds": 5, "loop": true,
              "settings": { "palette": "2a0a00,ff7818", "period_seconds": 5 } }
        ]
    },
    "shutdown": { "effect": "shutdown", "settings": { "color": "0028ff" } }
}
```

The last segment's `loop` decides the tail (loop from its first frame, else hold
the last frame); earlier segments play once. For a seamless loop make the
looping segment's `record_seconds` a whole multiple of its period. The whole
recording replays at one rate — the first segment's `frame_ms` — so a later
segment's own `frame_ms` has no effect on replay. Finite
effects (`shutdown`) record to completion; open-ended ones (`boot`) record for
`record_seconds`. Unchanged recordings aren't rewritten to flash (a hash is
compared), so wear is near zero. `cycle` can't be a slot (it needs the JSON
config).

Preview a slot exactly as the receiver will replay it, without hardware:

```sh
make virtual-strip && ./virtual-strip                    # one terminal
LED_PORT=none ./led config.json --preview power_on       # another
```

## Previewing without hardware

Frames go to a list of *sinks* — the serial transport and/or an on-screen
viewer — so the daemon runs with no strip attached. Set `LED_PORT=none` (or
`sinks.serial.port` empty) to skip serial:

```sh
make virtual-strip
./virtual-strip                          # on-screen strip; waits for frames
LED_PORT=none led config.json aurora     # renders into the viewer, no device
```

The viewer shows the exact bytes the strip would (already gamma/white-balance/
brightness corrected). Drop `LED_PORT=none` to drive a real strip and preview at
once.

## The daemon

```sh
make                     # build ./led
sudo make install        # /usr/local/bin/led + systemd unit + default config
sudo make uninstall      # remove everything, including the config
```

`make install` never overwrites an existing config, and adds your user to the
serial port's group (effective next login) so `led` can run unprivileged for
testing. The unit restarts on serial failure every 2 s, so unplugging the
adapter heals itself.

```
led <config>            run the rules
led <config> <effect>   run a single effect forever (testing)
led --list              list available effects
```

### Adding an effect

Drop a file in `daemon/effects/` — it's compiled in and self-registers. Effects
render against the host `Strip`; the receiver just replays what's streamed. Pure
animations (no host data) can also be a boot/shutdown slot; data-driven ones
(`cpu_temp`, `load`) can't.

```cpp
#include "effect.hpp"

class MyEffect : public Effect {
public:
    void init(const EffectConfig& cfg, int leds) override { ... }
    void render(Strip& strip, float t) override { ... }
};

REGISTER_EFFECT("my_effect", MyEffect)
```

## Wire protocol

Pixel frame, host → receiver:

| bytes | content |
|---|---|
| 2 | sync `0xAA 0x55` |
| 1 | GPIO pin |
| 2 | LED count (LE) |
| 3·n | RGB, one triple per LED |
| 1 | checksum: XOR of pin, count, and RGB bytes |

Command frame (distinct second sync byte, so the pixel parser skips it):

| bytes | content |
|---|---|
| 2 | sync `0xAA 0x56` |
| 1 | command (`0x01` shutdown, `0x02`–`0x04` recording upload) |
| 2 | payload length (LE) |
| n | payload |
| 1 | checksum: XOR of command, length, and payload bytes |

Byte values and payload formats live in `common/protocol.hpp`, shared by host
and firmware. The receiver drops bad-checksum frames and rescans for sync, so a
desync recovers within a frame; if nothing valid arrives for
`esp32.host_timeout_ms`, it blanks the strip. **Host and receiver must be updated
together when the protocol changes.**
