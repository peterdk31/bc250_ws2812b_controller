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

## Getting started

### Prerequisites

**Daemon** (the host side): `g++` and `make`.

**Firmware** (the ESP32 receiver): by default you **don't build it** — `make
flash` downloads a prebuilt image from the project's GitHub Releases (built by
CI) and writes it. The receiver is generic and auto-hunts the baud rate, so one
prebuilt image fits every config; you flash it once and never again for config
or effect changes. That means the only firmware prerequisite is a host
`esptool` (tiny, pure-Python — *not* the toolchain) plus `curl`/`wget`:

```sh
# CachyOS / Arch (mutable)
sudo pacman -S base-devel          # daemon's g++/make (curl is already present)
pipx install esptool               # or: pip install --user esptool
```

On Bazzite (immutable) `esptool` and `curl` install the same way; the daemon's
`g++`/`make` aren't in the base image, so build the daemon in a distrobox (or
copy the prebuilt `led` binary over).

**Building the firmware from source** is only needed if you modify it (or want a
non-default baud baked in). Then you need a cross-compiler — pick one:

- **Container (recommended)** — install `docker` or `podman` (`sudo pacman -S
  podman`; podman is preinstalled on Bazzite). `make receiver` / `make
  flash-source` build inside Espressif's official ESP-IDF image in a throwaway
  container; the ~2 GB toolchain is never installed on the host and the cached
  image is disposable (`make receiver-clean`).
- **Native ESP-IDF** — install ESP-IDF (v5.1+) once and set `IDF_PATH=...`; no
  container, but the toolchain stays installed:
  ```sh
  git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
  ~/esp/esp-idf/install.sh esp32c3
  ```

### Build, flash, install

```sh
sudo make install    # build daemon + systemd unit + default config
sudo make flash      # download the prebuilt receiver image + flash it
sudo systemctl enable --now led-controller
```

To build and flash from source instead of downloading, use `make flash-source`
(container by default, or native ESP-IDF if `IDF_PATH` is set). Prebuilt images
are published when a `v*` tag is pushed; until the first release exists, use
`make flash-source`.

Then adjust `/etc/led-controller/config.json` for your hardware — at
least `strip.leds`, `strip.pin` and `sinks.serial.port` — and
`sudo systemctl restart led-controller`.

To test an effect directly, stop the service (it holds the serial
port) and run e.g. `led /etc/led-controller/config.json rainbow` —
unprivileged once the port group set up by `make install` is active
(next login), with `sudo` before that. If the strip stays dark,
`journalctl -u led-controller` shows what the daemon is unhappy about.

The sections below cover each step in detail.

## Flashing the receiver

The receiver firmware is a native [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/)
project under `firmware/` — no arduino-cli, no `.ino`. You normally don't build
it: `make flash` downloads a prebuilt merged image (built by CI, see
`.github/workflows/firmware.yml`) and writes it with esptool.

```sh
sudo make flash                      # download prebuilt image + flash
sudo make flash PORT=/dev/ttyACM0    # explicit port; TARGET=esp32 for other chips
sudo make flash FW_RELEASE=v1.2.0    # pin a specific release (default: latest)

sudo make flash-source               # build from source (container/native) + flash
make receiver                        # build from source only, no flash
make receiver-clean                  # reclaim space: build dirs, downloads, ESP-IDF image
```

`make flash` needs only `esptool` + `curl`/`wget` — no toolchain. The merged
image is flashed at `0x0` (it bakes in the per-chip bootloader offset, e.g.
`0x1000` on the ESP32 vs `0x0` on the C3), and doesn't touch the NVS or LittleFS
partitions, so a reflash keeps a board's saved state.

`make flash-source` / `make receiver` build in a throwaway container by default,
or against a native ESP-IDF when `IDF_PATH` is set (see
[Getting started](#getting-started)). The first container build pulls the image
(~2 GB, one time) plus the `led_strip` + `littlefs` components; each per-target
build lives in its own `firmware/build-<target>/`, and `make receiver-clean`
drops the lot.

The port, baud rate and target chip default to the matching keys from
`/etc/led-controller/config.json` (falling back to the repo's `config.json`), so
flashing targets the same adapter the daemon uses. `esp32.target` picks the chip
(`esp32c3` by default — an ESP32-C3 is RISC-V, so it can't run a plain-ESP32
Xtensa binary); esptool auto-detects the connected chip when it writes. The
prebuilt image uses the firmware's default baud (the receiver auto-hunts to
whatever the daemon sends); a source build bakes in your config's baud/timeout.
Nothing else is baked in — strip geometry and the power-on/shutdown animations
arrive at runtime as frames the daemon streams, so a flash is only needed for a
firmware change, never for a config or effect change.

`flash` stops the led-controller service around the write (the daemon
holds the port open) and restarts it afterwards if it was running.
`sudo` is needed for that and, typically, for port access. On a dev machine
without the service, everything works unprivileged once your user is in the
port's group (usually `dialout` or `uucp`). `make install` and `make flash`
add you to it automatically (also available standalone as `make serial-perms`);
membership takes effect on your next login.

The receiver is generic: pin and LED count come from the host in every
frame, and it renders no effects of its own — so it never needs reflashing
for config or effect changes.

From power-on until the first valid host frame, the receiver replays a
**power-on recording** so the strip isn't dark while the OS boots and the
daemon starts; on a clean shutdown it replays a **shutdown recording** after
the host has gone. These aren't rendered on the device — the daemon renders
the configured effects through its own Strip (so the bytes are identical to
what the hardware shows), captures the frames, and streams them over serial;
the receiver stores each in flash (LittleFS) and clocks them back out
frame-by-frame. This is why iterating on the boot/shutdown effects no longer
needs a reflash.

Both slots are configured from the `esp32` block in `config.json` — set each
to any **standalone** effect (the data-driven effects need live sensor data,
so they're not meaningful here) with its own settings. A
slot can also be a **`sequence`** of effects played back to back into one
recording — typically a one-shot intro leading into a looping idle:

```jsonc
"esp32": {
    "power_on": {
        "sequence": [
            // CRT power-on, plays once
            { "effect": "boot", "record_seconds": 4,
              "settings": { "intro_seconds": 3, "color": "ff7818" } },
            // then a breathing glow that loops until the daemon takes over
            { "effect": "breathe", "record_seconds": 5, "loop": true,
              "settings": { "color": "ff7818", "period_seconds": 5 } }
        ]
    },
    "shutdown": { "effect": "shutdown", "settings": { "color": "0028ff" } }
}
```

The daemon records both slots once at startup and streams them over, so an
edited effect is captured on the next daemon (re)start and shown one power
cycle later (the power-on recording runs *before* the daemon is up, so it's
always the previous session's capture). An unchanged recording is dropped on
the device instead of rewritten to flash (the upload carries a hash the
receiver compares against the stored file), so flash wear is near zero.

Per segment (or for a single-effect slot): a finite effect (one that reports
finished, like `shutdown`) is captured to completion; an open-ended effect
(like `boot`) is captured for `record_seconds`. The **last** segment decides
the tail — `"loop": true` loops the recording from that segment's first frame
(so the intro plays once, then the tail loops forever), while the default
holds the last frame. Earlier segments always play once. The whole recording
replays at one rate (the first segment's `frame_ms`). For a *seamless* loop,
make the looping segment's `record_seconds` a whole multiple of its period
(above, breathe's 5 s capture matches its 5 s `period_seconds`). A fresh,
never-recorded board stays blank until the first recording arrives.

Preview exactly what the receiver will replay — sequence, loop and all —
without hardware: run the viewer, then record-and-replay a slot to it.

```sh
make virtual-strip && ./virtual-strip          # one terminal
LED_PORT=none ./led config.json --preview power_on   # another
```

(Running a bare effect, `./led config.json boot`, shows the *live* effect
instead — it won't reflect the `record_seconds` cap, the sequence, or the
loop/hold the receiver applies.)

Strip geometry comes from each recording's own header, and otherwise from
the last valid host frame (remembered in NVS). The receiver — independently
powered — outlives the host, which is what lets it finish the shutdown
recording. A host that goes quiet *without* sending the shutdown command (a
crash or pulled cable) still blanks on the host timeout. A later valid frame
cancels any replay, so `systemctl stop` → `start` resumes cleanly.

**Host and receiver must be updated together** when the wire protocol
changes (it carries a checksum and command frames — an old receiver ignores
unknown commands and falls back to the timeout blank; mismatched checksums
are rejected outright).

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

### Previewing without hardware

The frame the daemon renders goes to a list of *sinks* — the serial
transport for the real strip, and/or an on-screen viewer — so it can run
with no LED hardware attached. Set `LED_PORT=none` (or `sinks.serial.port` to
`none`/empty in the config) to skip the serial transport; a *real* port that
fails to open still exits so systemd reopens it.

Run the terminal viewer in one shell and a headless daemon in another:

```
make virtual-strip
./virtual-strip                          # on-screen strip; waits for frames

LED_PORT=none led config.json aurora     # renders into the viewer, no device
```

The viewer shows exactly what the strip would (same bytes, already
brightness/gamma/white-balance corrected). Drop `LED_PORT=none` to drive a
real strip and watch the preview at the same time.

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
frame. If no valid frame arrives for `esp32.host_timeout_ms` (default
5000 — host crashed, cable pulled), the receiver blanks the strip.

The baud rate adapts at runtime: when bytes keep arriving but no frame
decodes for 0.5 s, the receiver steps to the next rate from the host's
supported set and retries until frames validate ("baud hunting"), then
remembers the working rate in flash (NVS). So changing `sinks.serial.baud`
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
    "sinks": {                      // where rendered frames go. Each entry is
        "serial": {                 // a Sink; omit serial (or LED_PORT=none)
            "port": "/dev/ttyUSB0", // to run headless. The on-screen viewer
            "baud": 921600          // is a separate Sink with no config (see
                                    // "Previewing without hardware"); it
        }                           // attaches automatically.
    },                              // baud: receiver locks on by itself;
                                    // the prebuilt image boots at the default
                                    // and auto-hunts; `make flash-source` can
                                    // bake this value as the boot default
    "strip": {
        "leds": 58,
        "pin": 13,              // ESP32 GPIO driving the strip
        "reverse": false,       // flip direction so LED 0 is at the far
                                // end of the strip
        "brightness": 0.2,      // 0..1, scales linear light output
        "gamma": "2.2",         // perceptual correction; 1.0 disables.
                                // three space-separated values ("2.0 2.2
                                // 2.4") set it per R/G/B channel
        "white_balance": "b4ffff", // RRGGBB "white" that reads neutral
                                  // through the diffuser/filter; ffffff
                                  // disables
        "dither": "spatial"     // "spatial" (default) or "temporal";
                                // how sub-code levels are dithered
    },
    "sensors": [ ... ],         // temp sensor candidates, see below
    "esp32": {                  // the receiver. See "Flashing the receiver"
        "host_timeout_ms": 5000, // blanks the strip after this long without a
                                // frame; the prebuilt image uses the 5000
                                // default, `make flash-source` bakes this value.
                                // Not read by the daemon
        "power_on": { ... },    // effects the receiver runs on its own,
        "shutdown": { ... }     // pushed over serial. Standalone effects only
    },
    "rules": [ ... ]            // see below
}
```

### Tuning the colors

Three `strip` settings shape what reaches your eye. They stack in this
order — gamma curves each channel, white balance rescales it, then
brightness scales the whole thing — and all three are folded into one
per-channel lookup table on the host, so changing them is free per
frame. No reflash is needed for the power-on/shutdown animations either:
they're recorded by the daemon through this same correction, so a restart
re-captures them with the new values.

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

**`dither`** (`"spatial"` default, or `"temporal"`) — how the host
fakes the brightness levels between two adjacent 8-bit codes. Gamma plus
a low brightness collapse the 256 input codes onto only a few dozen
outputs, so a slowly drifting effect would otherwise hold flat for many
frames then jump a whole code — visible stepping. Both modes round
neighbouring values opposite ways so the average lands in between; they
differ in *where* that averaging happens:

- **`spatial`** dithers across adjacent LEDs and lets the diffuser blend
  them optically. It carries no frame-to-frame change, so it never
  flickers, and through a diffuser it resolves the finest gradients. This
  is the right choice for a diffused strip.
- **`temporal`** dithers across frames, letting the eye average over
  time. It works on a *bare* (undiffused) strip where there are no
  neighbours to blend, but at low brightness the per-LED toggle is a
  large relative step and, at the 30–60 fps frame rate, falls below
  flicker fusion — so it reads as a low-level scintillation. Use it only
  without a diffuser.

The power-on/shutdown animations pick up the mode on the next daemon
restart (they're re-recorded through this correction) — no reflash needed.

Suggested order when dialing it in:

1. Set `brightness` for the room.
2. At a *bright* white, adjust `white_balance` until white looks
   neutral.
3. Drop to a *dim* white. If it now has a tint (e.g. goes bluish),
   lower the gamma of the channel that fades too fast — bluish means
   red is dropping out, so try `"2.0 2.2 2.2"`. Raising a channel's
   gamma darkens its midtones; lowering it lifts them.
4. Re-check the bright end (step 2 is unaffected by gamma at full
   white) and restart the daemon so it re-records and re-uploads the
   power-on animation with the new balance (no reflash needed).

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
    "effect": "ember",
    "hold": 5,                       // optional, min seconds since last
    "settings": { "speed": 1.4 }     // switch; optional, effect-specific
}
```

Switching to a different effect (or the same effect with different
settings) waits until the rule's `"hold"` seconds have passed since the
last switch, so a rule that flaps doesn't strobe the strip. `"hold"`
defaults to 0 — switch immediately — so only a rule that wants
debouncing sets its own.

A switch isn't abrupt: the daemon crossfades from the last displayed
frame into the new effect over the top-level `transition_seconds`
(default 0.6; set 0 for an instant cut). The dissolve is eased (it starts
and settles gently rather than at a constant rate) and runs on
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
| `alarm` | urgent heartbeat (lub-dub) throbbing from the center outward | `color` (ff0000), `pulses_per_second` (2), `min_brightness` (0.16), `radiate` (0.12) |
| `aurora` | slow drifting color curtains (flow + value noise) walking a palette | `palette` (10ff80,10a0ff,8040ff), `speed` (1.0), `noise` (0.3) |
| `boot` | power-on: a CRT-style ignition — a white-hot point flares at the center, whips outward into a scan line, resolves to the body color, then holds a living glow indefinitely (never finishes, never fades); the host's first frame ends it | `intro_seconds` (3), `color` (0028ff), `flash_color` (ffffff) |
| `breathe` | single color on a slow breath, with a soft brighter band drifting along the strip | `color` (ff7818), `period_seconds` (5), `min_brightness` (0.05), `wave_depth` (0.30) |
| `comet` | Larson scanner with fading tail, eased turnaround, optional mirrored second eye | `color` (ff0000), `sweeps_per_second` (0.5), `tail_pixels` (8), `turn_ease` (1.0), `mirror` (0) |
| `cpu_temp` | temperature bar graph along a hue ramp, soft tip + slow shimmer, faint cold glow on the unlit track | `temp_min` (40), `temp_max` (85), `cold_color` (0000ff), `hot_color` (ff0000), `speed` (1.0), `floor_brightness` (0.04), `sensors` |
| `cycle` | rotates through a list of other effects, switching to a random next one each period | `period_seconds` (30), `effects` (list of `{effect, settings, seconds}`) |
| `drift` | soft glows wandering slowly over a dark base | `color` (6078ff), `base_color` (000014), `blobs` (3), `width` (0.10), `speed` (1.0) |
| `ember` | slow warm aurora-style flow (flow + noise) through an ember palette | `palette` (2a0a00,ff7d1e), `speed` (1.0), `min_brightness` (0.18), `noise` (0.4) |
| `load` | CPU/GPU bars from center, palette wash (flow + noise) with soft tips + shimmer, faint floor wash | `smoothing_seconds` (0.7), `palette` (00e0c0,2060ff,a040ff), `speed` (1.0), `noise` (0.3), `floor_brightness` (0.04) |
| `plasma` | summed drifting sines + value noise walking a palette; organic, never repeats | `palette` (0040ff,00d0a0,c040ff,0040ff), `speed` (1.0), `noise` (0.5) |
| `pulse` | soft rings born at the center, expanding and fading outward | `color` (30c0ff), `base_color` (000010), `period_seconds` (4), `width` (0.18), `travel` (1.4) |
| `rainbow` | scrolling hue cycle, lit from within (shimmer + saturation breathing) | `cycles_per_second` (0.625), `shimmer_depth` (0.18), `sat_depth` (0.15) |
| `shutdown` | power-down sequence: a CRT-style collapse — the picture snaps inward to a white-hot point, then fades out slowly with phosphor persistence so it lingers through the OS power-down; reports finished | `duration_seconds` (5.0), `color` (0028ff), `flash_color` (ffffff) |
| `steam_download` | Steam download bar, slow breath + flow sweep, green pulse at 100 | `color` (00a0ff), `done_color` (00ff40), `smoothing_seconds` (0.4), `flow_period` (2.5), `flow_depth` (0.35), `shimmer_depth` (0.15), `shimmer_period` (6.0) |
| `tide` | a palette gradient sliding and breathing along the strip | `palette` (102060,ff5028,ffb020,4060c0,102060), `speed` (1.0), `span` (1.0) |

Colors are `"RRGGBB"` or `"#RRGGBB"`. A `palette` is a list of those
separated by anything non-hex, e.g. `"2a0a00, ff7d1e, ffd060"`; the
stops are spaced evenly and blended in RGB, so for a vivid multi-hue
sweep add the in-between colors as stops rather than relying on one long
blend, and repeat the first color as the last for a seamless loop.
Effect settings come from the matching rule's `"settings"`, falling back
to top-level config keys, then the defaults above.

Every effect also takes `frame_ms` — the delay between frames in
milliseconds (lower is smoother). Like any setting it resolves per-rule
first, so a top-level `"frame_ms": 16` is a global default that an
individual rule's `"settings"` can override. Effects default to 33ms
(~30fps); a few one-shot/transition effects (`alarm`, `boot`, `comet`,
`rainbow`, `shutdown`) default to 16ms (~60fps). Effects that integrate
state per frame (`load`, `comet`, `steam_download`) use this
as their timestep too, so lowering it makes the motion smoother without
changing its speed. The floor is 1ms, but the serial link and WS2812
refresh set the practical ceiling on frame rate.

`cycle` composes the others: each entry in its `"effects"` list carries
its own `"settings"` (resolved exactly like a rule's — entry settings
first, then top-level keys, then that effect's defaults) and an optional
`"seconds"` to override `period_seconds` for that one. It picks a random
next entry each time, runs each sub-effect on its own clock, and hands
off early if a sub-effect reports itself finished. It's host-only (the
nested list needs the JSON config), so it can't be a receiver
power-on/shutdown effect.

### Adding an effect

Drop a new file in `daemon/effects/` — it's compiled into the daemon and
registers itself automatically. All effects are daemon-only: the framework
renders against the host `Strip` and the receiver just replays what the
daemon streams.

Two flavours live side by side in that folder:

- **pure animation** (no host data) — e.g. `comet`, `aurora`, `breathe`.
  These can also be a receiver power-on/shutdown slot: the daemon renders
  them through its own `Strip`, captures the frames and streams the
  resulting **recording** to the receiver (it doesn't run the effect code
  on the device).
- **data-driven** (sensors, Steam, …) — e.g. `cpu_temp`, `load`. These need
  live host data, so they aren't meaningful as a power-on/shutdown slot.

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
