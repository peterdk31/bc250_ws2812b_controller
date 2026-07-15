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
  **Prefer the C3**: its native USB lets the firmware tell whether the host is
  actually running, so the boot animation also plays on reboots and on machines
  that keep standby power on their USB ports. A plain ESP32 can't see any of
  that over UART and only plays it after a true power cycle (see
  [How it works](#how-it-works)).
- A WS2812/WS2812B strip: data to the GPIO set as `strip.pin` (default 13), plus
  5 V and GND from a supply that can handle it (not the board's regulator; share
  grounds).
- Optionally, a momentary power button and a wire to the PSU's PS_ON# line —
  the receiver can then stand in for the PS_ON→GND jumper an ATX/FSP supply
  needs before it starts (see [Power switch](#power-switch)).

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

*When* the boot animation plays depends on the board. It always runs from chip
reset until the daemon's first frame. On USB-native chips (the C3 and friends)
it also re-arms whenever the USB host goes away and comes back — the bus
keepalives stop for a few seconds and return — which covers a reboot (shutdown
animation, dark while the host is down, boot animation as it comes back up)
and a power-on where standby VBUS kept the receiver running the whole time. A
plain ESP32's UART carries no trace of the host's state, so it shows the boot
animation only when it genuinely loses power; on a reboot it stays dark after
the shutdown animation until the daemon returns. If the power-on animation
matters to you, get a C3.

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
        "white_balance": "ffb0f0" // RRGGBB neutral-white gain; ffffff = off
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
table applied on the host, so they cost nothing per frame.

How to read the values — every knob is per-channel, red green blue:

```
white_balance "b4b0f0"          gamma "1.9 1.8 1.7"
               ││││└┴─ blue            └┬┘ └┬┘ └┬┘
               ││└┴─── green          red green blue
               └┴───── red
```

Two rules cover every adjustment:

- **White balance scales a channel the same at every level.** Lower a pair →
  less of that color, from full white down to the dimmest glow. `ff` = leave
  the channel alone.
- **Gamma changes how a channel *fades*.** A higher number makes that channel
  die away faster as colors dim; full-on and off never move. `2.2` is
  neutral; you only need three different numbers when a tint appears *only*
  at low levels.

So: name the color the white is leaning toward, then move that channel —
down in `white_balance` if the tint is there at full brightness, up in
`gamma` if it only creeps in as things get dim.

Dial the knobs in by symptom, in this order:

1. **Everything too bright or dim for the room** → `brightness` (0..1).
2. **Full white looks tinted** → `white_balance`: lower the channel(s) of
   the tint. White leans green → lower the middle pair (`ffb0f0`). This is
   the strip+diffuser calibration; start from the diffuser table below.
3. **Full white is neutral, but dim white picks up a tint** →
   per-channel `gamma`: raise the tint's channel so it fades faster (dim
   white leans blue → `"2.2 2.2 2.4"`) — or equivalently lower the channel
   that's going missing (dim white leans blue because red drops out →
   `"2.0 2.2 2.2"`).

The `solid` effect exists for exactly this — it puts up a fixed test field
through the normal correction pipeline:

```bash
led /etc/led-controller/config.json solid    # full white: judge white_balance
```

Look, adjust the config, run again. For the gamma step you need *dim* white;
a forced effect has no rule settings, but effects fall back to top-level
config keys, so temporarily add `"level": 0.25` at the top level of the
config (next to `"strip"`) and run `solid` again. Remove it when done.

`white_balance` is the *total* correction — strip and diffuser combined into
one measured value. Starting points per diffuser (always confirm against
`solid` white):

| Diffuser | `white_balance` | Why |
|---|---|---|
| none / clear cover | `ffb0f0` | just the strip's own green excess |
| white (milky) | `ffb0f0` | near-neutral filter; scatters but barely tints — trim blue a touch (`ffb0e0`) if white reads cool |
| black / smoked film | `b4b0f0` | dark film passes red most, so take the bare-strip value and pull red down ~30% on top of it. Films vary a lot: one measured black window film landed at `b4ffff` instead — its green/blue cut cancelled the strip's own green excess. Pair with per-channel gamma (e.g. `"1.9 1.8 1.7"`) if dim white still drifts |

Low brightness costs no smoothness: the corrected values ride the wire with 8
extra fractional bits and the receiver rounds them away with a temporal dither
at its own strip-refresh rate (several hundred Hz — far above flicker fusion),
so dim gradients glide instead of stepping or shimmering. There is nothing to
configure; the old `dither` key is gone (this needs receiver firmware from the
same version — reflash once after updating). Restart the daemon after any
change so the boot/shutdown recordings re-capture with the new colors.

### Sensors

`sensors` is an ordered list of `chip:label` hwmon candidates; the first present
wins (a bare chip name means its `temp1_input`). Used by `temp` conditions.
The daemon keeps rescanning until one appears. The default
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
| `audio_playing` | system audio is actually playing (see below) |
| `file:/PATH` | the file exists |
| `!COND` | negation |
| `A & B` | both hold |
| `A \| B` | either holds (`&` binds tighter; no parentheses) |

`file:` is the external control surface — `touch /tmp/led-night` to switch modes,
`rm` to switch back.

The `steam_download` effect turns the strip into a live download progress bar
(percent driven by network throughput; green pulse at 100%). Reading other
users' Steam libraries needs the daemon running as root (the systemd unit does).

The `audio_*` effects visualise what's playing (combine them with `cycle` for
variety — they share one capture stream and auto-gain, so hopping between them
is seamless). The effects capture samples by spawning `parec` (or
`pw-record`) on the default sink's monitor while active; running as root
(the systemd unit does) lets it find the user session's PipeWire socket under
`/run/user/`.

The `audio_playing` condition costs nothing while the system is silent (one
procfs read per tick). Once anything is audible it asks the sound server for
its playback stream list (`pactl -f json list sink-inputs`, spawned
asynchronously about once a second): an uncorked stream means playing, corked
or none means paused/stopped. That verdict is immune to quiet passages inside
the music and reacts to pause/resume within a second or two. It is also the
only source of truth: if the list can't be fetched while audio is audible
(no `pactl`, no reachable session), the condition stays false and a note is
left on stderr. Placing the rule *below* the `load` rule means load takes the
strip whenever it's active and music has it otherwise — first matching rule
wins.

## Effects

| name | description | key settings (defaults) |
|---|---|---|
| `alarm` | urgent heartbeat throbbing from the center | `color` (ff0000), `pulses_per_second` (2) |
| `audio_music` | audio-reactive bloom from the center: loudness sets its reach, bass pumps it and sends crests to the tips | `palette` (4a00b4,e02090,ff9c28), `pulse` (0.8), `gain_seconds` (6) |
| `audio_ripple` | audio-reactive: each bass hit sends a wave crest with a luminous wake racing from the center to the tips, reflecting back off them, colors walking the palette per beat | `palette` (0030a0,00c0e0,8040ff), `travel_seconds` (1.0), `sensitivity` (1.0), `reflect` (0.6) |
| `audio_spectrum` | audio-reactive liquid analyzer: a spring-loaded bar grows from the strip start with the loudness, a meniscus shine on its tip, bass pumping its base and treble its tip, a peak dot falling back with a comet tail between hits | `palette` (00e6b4,3a56ff,ff2e88), `pump` (0.7), `bounce` (0.6), `peak_color` (ffffff) |
| `aurora` | slow drifting color curtains walking a palette | `palette` (10ff80,10a0ff,8040ff), `speed` (1.0) |
| `boot` | calm bloom from the center into a full wash, then exhales into a dim hold | `intro_seconds` (7), `color` (ffffff) |
| `breathe` | palette wash on a slow breath with a drifting bright band | `palette` (0d4a44,30e090), `period_seconds` (3.5) |
| `button_off` | experimental: the lit strip collapses into the power button | `duration_seconds` (1.4), `color` (ffffff), `reverse` (true) |
| `button_on` | experimental: light spills out of the power button, then pulses | `spread_seconds` (2.5), `period_seconds` (4), `color` (ffffff) |
| `comet` | Larson scanner with a fading tail, optional mirror | `palette` (8040ff,30c0ff), `sweeps_per_second` (0.7), `tail_pixels` (8) |
| `cycle` | rotates through a list of other effects | `period_seconds` (30), `effects` (list) |
| `drift` | soft palette glows wandering over a dark base | `palette` (2858ff,30d0b0), `blobs` (3), `speed` (1.4) |
| `ember` | slow warm aurora-style flow through an ember palette | `palette` (2a0a00,ff7d1e), `speed` (1.4) |
| `lava` | lava lamp: soft blobs drift, meet and merge into hotter spots | `palette` (ff0050,ff5a00,ffd000,00e5ff,7a00ff,ff00d0), `blobs` (4), `speed` (5.0) |
| `load` | CPU/GPU bars from center over a palette wash, each with a heartbeat that quickens under load | `palette` (00e0c0,2060ff,a040ff), `smoothing_seconds` (0.7), `pulse` (0.5) |
| `plasma` | drifting sines + noise walking a palette | `palette` (0040ff,00d0a0,c040ff,0040ff), `speed` (1.4) |
| `pulse` | soft palette rings expanding from the center | `palette` (5028ff,30d0ff), `period_seconds` (4) |
| `rainbow` | scrolling hue cycle with shimmer | `cycles_per_second` (0.9) |
| `shutdown` | CRT-style collapse to a point, then a slow phosphor fade | `duration_seconds` (5.0), `color` (0028ff) |
| `solid` | fixed color everywhere — the color-calibration test field, or a plain static light | `color` (ffffff), `level` (1.0) |
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

## Power switch

An ATX/FSP supply (like the BC-250's FSP500-30AS) doesn't start until its
PS_ON# line (the green wire) is pulled to ground — normally a permanent
jumper. With the receiver powered from the PSU's **5VSB** standby rail, it can
drive that line instead and act as the machine's power button
(after [Thunkar/bc250-esp32-switch](https://github.com/Thunkar/bc250-esp32-switch),
minus its WiFi/BLE):

- **Tap** the button while off → PS_ON# is sunk to ground, the PSU starts, the
  board boots.
- **Hold** for 5 s while on → PS_ON# is released — a hard power-off for a
  wedged machine.
- With the optional **sense** wire (BC-250: TPMS1 pin 9, ~3.3 V while the
  board is up): a graceful OS shutdown is followed down (the sense line drops,
  the receiver releases PS_ON# so the PSU turns off too), and a board that
  never comes up within 10 s releases the PSU instead of leaving it energized.
  Without the wire, both are skipped: after an OS shutdown the PSU stays on
  until the next long-press.

Wiring (ESP32-C3 defaults shown; every pin is a `PWR_*` make variable):

| receiver pin | connects to |
|---|---|
| GPIO3 (`PWR_PS_ON`) | PS_ON# — driven open-drain, so the PSU's internal 5 V pull-up is never fought |
| GPIO2 (`PWR_BUTTON`) | momentary switch terminal A (internal pull-up, pressed = low) |
| GPIO1 (`PWR_BUTTON_GND`, -1 if the switch is wired to a real GND) | switch terminal B — driven low as a local ground, so the button needs no run to a real GND |
| GPIO0 (`PWR_SENSE`, -1 = not wired) | board-power sense, e.g. BC-250 TPMS1 pin 9 — read as an averaged ADC voltage with hysteresis (`PWR_SENSE_LOW`/`PWR_SENSE_HIGH` mV), the line is too soft for a digital read |
| 5VSB + GND | PSU standby rail, so the receiver runs while the machine is off |

Two pin caveats: GPIO2 is a C3 *strapping pin* — the pull-up keeps it happy,
but a button held down through a chip reset (or while entering flash mode)
stops the chip booting until it's released. And the defaults are C3-specific:
on a plain ESP32, GPIO1/3 are its UART0 console and 0/2 are strapping pins —
pick different ones. If the sense wire isn't connected, set `PWR_SENSE=-1`
rather than leaving the input floating: a floating ADC pin reads noise, and
the boot timeout may cut the PSU seconds after every power-on.

The feature is **off until opted into at flash time**: the settings live in a
small dedicated flash partition (`pwrcfg`), not in the firmware image, so the
prebuilt image works and pins change without touching source. `PWR=on` writes
the partition alongside a normal flash, and `make flash-pwr` writes *only*
that partition — re-pinning takes a couple of seconds and keeps the installed
firmware. Once written, the settings survive reflashes (a plain `make flash`
never touches them):

```sh
sudo make flash PWR=on                        # flash firmware + default wiring
sudo make flash-pwr PWR=on PWR_SENSE=-1       # re-pin only (e.g. no sense wire)
sudo make flash-pwr PWR=on PWR_HOLD=8         # timings too (seconds)
sudo make flash-pwr PWR=off                   # disable the feature

# the prebuilt image with every setting spelled out (values shown are the
# defaults — name only the ones you change)
sudo make flash PWR=on \
    PWR_PS_ON=3 PWR_BUTTON=2 PWR_BUTTON_GND=1 PWR_SENSE=0 \
    PWR_HOLD=5 PWR_BOOT_TIMEOUT=10 \
    PWR_SENSE_LOW=800 PWR_SENSE_HIGH=2000
```

The switch runs standalone — no daemon involved; the button matters exactly
when the host is off. Disabling with `PWR=off` releases an asserted PS_ON#
once the receiver reboots into the new setting — put the jumper back first if
the machine's power already hangs on the receiver.

**Reflashing caveat:** once the machine's power hangs on this pin, remember
that flashing the receiver *from that machine* resets the chip mid-write. On
a **C3** the firmware defends itself: the asserted level is latched in the
always-on pad domain (`gpio_hold_en`) so it rides through the chip's internal
resets — including into the flashing ROM — and on any reboot that isn't a true
loss of standby power the intent is also restored from flash first thing at
boot. Still, verify a `make flash` on your wiring once — with a temporary
PS_ON→GND jumper in place — before trusting it with unsaved work. On a
**plain ESP32 neither defense works for flashing**: esptool resets it by
pulling the EN pin, a genuine chip power cycle that drops pad holds and reads
as a power-on reset. Flash one from another machine or with the jumper in
place. (Crashes/watchdog resets are still ridden out there too, provided
the PS_ON pin is one of its RTC-capable pins: 0, 2, 4, 12–15, 25–27, 32, 33.)
After a genuine 5VSB power loss the machine always stays off until the button
is pressed.

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
brightness corrected). Drop `LED_PORT=none` to drive a real strip and preview
at once.

## The daemon

```sh
make                     # build ./led
sudo make install        # /usr/local/bin/led + systemd unit + default config
sudo make uninstall      # remove everything, including the config
```

`make install` never overwrites an existing config, restarts the service if
it's running (so a rebuilt daemon goes live immediately), and adds your user
to the serial port's group (effective next login) so `led` can run
unprivileged for testing. The unit restarts on serial failure every 2 s, so unplugging the
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
(`load`) can't.

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
