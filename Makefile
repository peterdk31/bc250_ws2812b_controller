CXX = g++
# source is grouped by where it runs: common/ compiles into both the daemon
# and the firmware (the daemon<->receiver link layer only), daemon/ is the
# host daemon, firmware/ is the ESP32 receiver (ESP-IDF), vendor/ is third-party.
# includes stay path-less, so each source dir goes on the include path here
# and the receiver build (which copies the common files flat) needs no -I
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra \
           -Icommon -Idaemon -Idaemon/rules -Idaemon/output \
           -Idaemon/sources -Idaemon/color -Idaemon/effects -Ivendor
PREFIX = /usr/local

# ESP32 receiver firmware — a native ESP-IDF project (firmware/), built with
# idf.py and flashed with esptool. No arduino-cli, no .ino. PORT, BAUD, TIMEOUT_MS
# and TARGET default to the matching keys from the installed config (or the repo
# one); override on the command line, e.g. `make flash PORT=/dev/ttyACM0`. BAUD
# and TIMEOUT_MS are baked into the firmware so receiver and daemon agree. The
# receiver renders no effects, so it needs no strip/effect config baked in —
# geometry and the power-on/shutdown animations arrive as recordings the daemon
# streams at runtime (see common/protocol.hpp).
#
# the values are read through the daemon's own JSON parser (`led
# --config-get <dotted.path>`) rather than scraped, so they can't drift
# from how the daemon reads the same file. `receiver` depends on `led`
# so it exists by the time these expand; if it somehow doesn't, each
# falls back to the default below.
CONFIG ?= $(firstword $(wildcard /etc/led-controller/config.json config.json))
CONFIG_GET = ./led --config-get $(CONFIG)
BAUD ?= $(or $(shell $(CONFIG_GET) serial.baud 2>/dev/null),921600)
TIMEOUT_MS ?= $(or $(shell $(CONFIG_GET) host_timeout_ms 2>/dev/null),5000)
STRIP_PIN ?= $(or $(shell $(CONFIG_GET) strip.pin 2>/dev/null),4)

# Receiver features configured at flash time — the ATX power switch, the PWM
# fans, the BLE power remote — each live in a small flash partition of their
# own (pwrcfg / fancfg / blecfg), not in the firmware image, so the prebuilt
# image fits every board and a setting changes in seconds without a toolchain.
# Their settings are blocks of the daemon config (`power_switch`, `fans`,
# `ble_remote` — README, each feature's section), the one place the box is
# described; `flash` / `flash-source` encode all three from it, and
# `flash-pwr` / `flash-fan` / `flash-ble` rewrite one partition. No PWR_* /
# FAN_* / BLE_* variables any more: edit the config, flash.
#
# One asymmetry, on purpose. A config with no `fans` or `ble_remote` block
# writes that feature OFF (harmless: a floating PWM fan runs full, a phone
# can't find the board). A config with no `power_switch` block leaves the
# chip's power-switch settings ALONE: writing that feature off releases PS_ON#
# once the receiver reboots — it cuts the machine's power — so only an
# explicit "enabled": false may do it (pwrcfg.py exits 3 for "no block").

# The fans' standalone part (which headers are enabled, each one's fallback
# duty and boost, the boost length) is what fancfg holds; curves are the
# daemon's. The header → GPIO map is the carrier board's (tools/pincheck.py
# FAN_PINS; "pins" in the block overrides it for a hand-wired build). Each
# encoder's --list-pins feeds the other's collision check below.
FAN_PINS_USED = $(if $(CONFIG),$(shell python3 tools/fancfg.py --config "$(CONFIG)" --target $(TARGET) --list-pins 2>/dev/null))


PWR_PINS_USED = $(if $(CONFIG),$(shell python3 tools/pwrcfg.py --config "$(CONFIG)" --target $(TARGET) --list-pins 2>/dev/null))

# cross-feature pin collision feeds: each encoder is told which pins the
# *other* feature claims, as read from the same config. In `flash` and
# `flash-source` both partitions are written from that config in one run, so
# the pins are known-true and a collision is a hard error; from `flash-pwr` /
# `flash-fan` alone what is actually on the chip's other partition is
# unknowable from here, so it is a warning.
comma := ,
FAN_AVOID = $(foreach p,$(subst $(comma), ,$(PWR_PINS_USED)),--avoid "$(p):claimed by the power switch (the config's power_switch block)") \
	$(if $(KNOWN),--avoid-hard)
PWR_AVOID = $(foreach p,$(subst $(comma), ,$(FAN_PINS_USED)),--avoid "$(p):a fan header (the config's fans block)") \
	$(if $(KNOWN),--avoid-hard)
flash flash-source: KNOWN := 1

# PORT: config (or a command-line override) is authoritative; detection only
# fills a gap. Resolution order: command-line override > configured
# serial.port > first likely device node > default. Config wins over
# detection on purpose — a port pinned in config (e.g. a C3 on /dev/ttyACM1)
# must not be overridden just because another device enumerated first. The udev
# rule pins /dev/led-controller, so it's preferred by the glob when present.
DETECTED_PORT = $(firstword $(wildcard /dev/led-controller /dev/ttyACM* /dev/ttyUSB*))
PORT ?= $(or $(shell $(CONFIG_GET) serial.port 2>/dev/null),$(DETECTED_PORT),/dev/ttyUSB0)

# TARGET: which ESP chip to build for. An ESP32-C3 is RISC-V, not Xtensa, so the
# firmware must be built for the right arch. From `target` in config, else
# the C3 (the board this project ships with). A command-line override always
# wins, e.g. `make flash TARGET=esp32`. esptool auto-detects the connected chip
# when flashing, but the *build* needs the target named explicitly.
VALID_TARGETS = esp32 esp32c3 esp32s2 esp32s3 esp32c6 esp32h2
TARGET ?= $(or $(filter $(VALID_TARGETS),$(shell $(CONFIG_GET) target 2>/dev/null)),esp32c3)

# ESP-IDF build strategy. The firmware needs the ~2 GB ESP-IDF toolchain, which
# we DON'T want permanently installed on the host. So by default the build runs
# inside Espressif's official container image in a throwaway (--rm) container:
# nothing is installed on the host, and the only cached artifact is the image
# itself, which `make receiver-clean` deletes when you're done flashing. The
# heavy part is disposable; flashing uses a tiny host esptool (a few MB).
#
# Power users who already have ESP-IDF installed can skip the container entirely:
# set IDF_PATH=/path/to/esp-idf and the build sources its export.sh directly.
IDF_IMAGE ?= espressif/idf:v5.3.1
# container runtime: podman (preinstalled on Bazzite) or docker, whichever is
# found. Override with CONTAINER=docker.
CONTAINER ?= $(firstword $(foreach c,podman docker,$(if $(shell command -v $(c) 2>/dev/null),$(c))))
# a native ESP-IDF install (IDF_PATH set and holding export.sh) is used when
# present; otherwise the build goes through the container.
IDF_PATH ?=
IDF_NATIVE = $(and $(IDF_PATH),$(wildcard $(IDF_PATH)/export.sh))
# docker runs the container as root by default, which would leave root-owned
# build files in the repo; pass --user (the real invoking user, even under sudo)
# so they come out user-owned, with HOME pointed somewhere writable for that uid.
# Rootless podman already maps the container root to the host user, so it must
# NOT get --user (the host uid is out of its namespace range).
DOCKER_USER = $(if $(filter docker,$(CONTAINER)),--user $${SUDO_UID:-$$(id -u)}:$${SUDO_GID:-$$(id -g)} -e HOME=/tmp)
# idf.py leaves managed_components/ and dependencies.lock in the project dir
# (sdkconfig is relocated into the per-target build dir, below).
IDF_GENERATED = firmware/managed_components firmware/dependencies.lock

HEADERS = daemon/output/strip.hpp daemon/config_loader.hpp daemon/fans.hpp vendor/json.hpp \
          daemon/effects/effect.hpp daemon/rules/condition.hpp daemon/color/color.hpp \
          daemon/sources/hwmon.hpp daemon/sources/steam.hpp \
          daemon/sources/audio.hpp daemon/sources/audio_detect.hpp \
          daemon/rules/rules.hpp \
          daemon/output/sink.hpp daemon/output/serial_sink.hpp daemon/output/virtual_sink.hpp \
          daemon/output/virtual_strip_socket.hpp daemon/output/recorder.hpp \
          daemon/color/color_lut.hpp common/protocol.hpp common/motion.hpp \
          common/recording.hpp common/fade.hpp

# every effect compiles in and registers itself. all effects are daemon-only:
# the framework (daemon/effects/effect.hpp) depends on host strip/config, and
# the receiver replays recordings rather than rendering. registry.cpp is the
# self-registration table and is picked up by the same wildcard.
EFFECT_SRCS = $(wildcard daemon/effects/*.cpp)
SRCS = daemon/main.cpp daemon/rules/conditions.cpp daemon/rules/rules.cpp \
       $(EFFECT_SRCS)

all: led

led: $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ -lm

# on-screen virtual LED strip: a standalone terminal viewer that renders the
# wire frames the daemon mirrors to it (see daemon/output/virtual_sink.hpp). No
# hardware and no extra deps — plain g++. Built on demand, not part of `all`,
# so the deploy path is unchanged. Run it, then start `led` (or run a single
# effect) to preview an animation before pushing it to the BC-250.
virtual-strip: tools/virtual_strip.cpp daemon/output/virtual_strip_socket.hpp common/receiver.hpp common/protocol.hpp common/fade.hpp common/motion.hpp
	$(CXX) $(CXXFLAGS) tools/virtual_strip.cpp -o $@

install: led
	install -Dm755 led $(PREFIX)/bin/led
	install -Dm644 led-controller.service /etc/systemd/system/led-controller.service
	test -f /etc/led-controller/config.json || install -Dm644 config.json /etc/led-controller/config.json
	systemctl daemon-reload
	-systemctl enable led-controller
	-systemctl restart led-controller
	-@$(MAKE) --no-print-directory udev-rule
	-@$(MAKE) --no-print-directory serial-perms

# install the udev rule that pins /dev/led-controller to the receiver's serial
# port (see led-controller.rules), so the daemon's configured port and `make
# flash` never depend on the volatile /dev/ttyACM* numbering. Runs from
# `install`; reloading + triggering applies it to an already-connected board
# without a replug. Best-effort: a box without udev (or without the device yet)
# just won't get the symlink until the rule fires on the next plug/boot.
udev-rule:
	install -Dm644 led-controller.rules /etc/udev/rules.d/99-led-controller.rules
	-udevadm control --reload-rules
	-udevadm trigger --subsystem-match=tty --action=add

# force-overwrite the deployed config with the repo's and apply it.
# `install` leaves an existing /etc config untouched (so a fresh deploy
# can't clobber local tweaks); use this after editing config.json in the
# repo. try-restart only restarts the daemon if it's already running.
install-config:
	install -Dm644 config.json /etc/led-controller/config.json
	-systemctl try-restart led-controller

uninstall:
	-systemctl disable --now led-controller
	rm -f /etc/systemd/system/led-controller.service
	rm -f $(PREFIX)/bin/led
	rm -f /etc/udev/rules.d/99-led-controller.rules
	rm -rf /etc/led-controller
	systemctl daemon-reload
	-udevadm control --reload-rules

clean:
	rm -f led virtual-strip
	rm -rf firmware/build firmware/build-* firmware/dist firmware/managed_components \
	       firmware/sdkconfig firmware/sdkconfig.old firmware/dependencies.lock \
	       firmware/src

# add the user to the serial port's group (uucp on Arch-likes, dialout
# on Debian/Fedora) so running `led` and esptool flashing work
# unprivileged; detects the group from the device itself when present.
# Works with or without sudo; membership applies on next login. Runs
# automatically (non-fatally) from `install` and `flash`
serial-perms:
	@user=$${SUDO_USER:-$$USER}; \
	if [ -e $(PORT) ]; then \
		group=$$(stat -c %G $(PORT)); \
	else \
		group=$$(getent group dialout uucp | head -n1 | cut -d: -f1); \
		echo "$(PORT) not present, assuming group '$$group'"; \
	fi; \
	[ -n "$$group" ] && [ "$$group" != root ] || \
		{ echo "could not determine serial port group"; exit 1; }; \
	if id -nG "$$user" | tr ' ' '\n' | grep -qx "$$group"; then \
		echo "$$user is already in $$group"; \
	else \
		sudo usermod -aG "$$group" "$$user" && \
		echo "added $$user to $$group; log out and back in" \
		     "(or run 'newgrp $$group') for it to take effect"; \
	fi

# fail fast if there's no way to build: either a native ESP-IDF (IDF_PATH) or a
# container runtime for the throwaway image. Better a clear message here than a
# confusing error mid-build.
receiver-toolchain:
	@if [ -n "$(IDF_NATIVE)" ] || [ -n "$(CONTAINER)" ]; then :; else \
		echo "Can't build the firmware: no container runtime and no ESP-IDF."; \
		echo "Easiest: install podman (preinstalled on Bazzite) or docker — the"; \
		echo "firmware then builds in a throwaway $(IDF_IMAGE) container, with"; \
		echo "nothing left installed on the host afterwards (drop the cached image"; \
		echo "with 'make receiver-clean')."; \
		echo "Or, for a native build, set IDF_PATH=/path/to/esp-idf (v5.1+)."; \
		exit 1; \
	fi

# per-target build dir so switching TARGET never trips CMake's "target already
# set" guard — each chip gets its own configured tree. idf.py writes the app,
# bootloader, partition table and a flash_args manifest here. Gitignored.
RECEIVER_BUILD = firmware/build-$(TARGET)
# host esptool for flashing (tiny, pure-Python; NOT the 2 GB toolchain). Prefer
# esptool.py / esptool on PATH; the recipe falls back to `python3 -m esptool`.
ESPTOOL ?= $(firstword $(shell command -v esptool.py 2>/dev/null) $(shell command -v esptool 2>/dev/null))
# shell snippet (used by both flash targets) that resolves a host esptool into
# $$tool, or errors with an install hint.
ESPTOOL_RESOLVE = tool="$(ESPTOOL)"; \
	[ -n "$$tool" ] || { python3 -c 'import esptool' >/dev/null 2>&1 && tool="python3 -m esptool"; }; \
	[ -n "$$tool" ] || { echo "esptool not found on host (it's tiny — 'pipx install esptool' or 'pip install --user esptool')"; exit 1; }

# The daemon holds the serial port open, so free it around anything esptool
# does and put it back afterwards. The port isn't opened exclusively — a daemon
# still streaming frames collides with esptool ("Invalid head of packet").
# Stopping the unit covers the normal case; fuser -k clears a hand-started
# daemon. PORT_FREE remembers in $$active whether the service was running;
# PORT_RESTORE (paired with it, after the esptool run) starts it again if so.
PORT_FREE = active=0; systemctl is-active --quiet led-controller && active=1; \
	[ $$active = 1 ] && systemctl stop led-controller; \
	if command -v fuser >/dev/null 2>&1 && fuser $(PORT) >/dev/null 2>&1; then \
		echo "$(PORT) still held after stopping the service; freeing it:"; \
		fuser -v $(PORT) || true; fuser -k $(PORT) || true; sleep 1; \
	fi
PORT_RESTORE = [ $$active = 1 ] && systemctl start led-controller

# partition geometry, read from the table itself so no offset can drift out of
# sync with it (columns: Name,Type,SubType,Offset,Size,Flags)
part_off = $(shell awk -F, '$$1=="$(1)" {gsub(/ /,""); print $$4}' firmware/partitions.csv)
part_size = $(shell awk -F, '$$1=="$(1)" {gsub(/ /,""); print $$5}' firmware/partitions.csv)

NVS_OFF = $(call part_off,nvs)
NVS_SIZE = $(call part_size,nvs)
# The CI merged image spans bootloader → app in one file, with the gaps
# 0xFF-padded — and the gap between the partition table and the app IS the nvs
# partition. Written whole at 0x0 it therefore paved over NVS on every
# reflash: the remembered baud and strip geometry (an annoyance), and the
# power switch's "the PSU is meant to be on" intent (a disaster — the freshly
# booted firmware read "off", released PS_ON#, and cut the very machine the
# flash ran on, hard enough to corrupt its filesystem). This snippet slices
# the downloaded image around the nvs hole into $$img.pre/$$img.post and sets
# $$imgargs to the offset+file pairs for esptool write_flash, so NVS is never
# touched by `make flash`.
IMG_SPLIT_NVS = \
	if [ -n "$(NVS_OFF)" ] && [ -n "$(NVS_SIZE)" ]; then \
		python3 -c 'import sys; \
p, o, e = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[2], 0) + int(sys.argv[3], 0); \
d = open(p, "rb").read(); \
assert len(d) > e, "image ends inside the nvs region — layout changed?"; \
open(p + ".pre", "wb").write(d[:o]); \
open(p + ".post", "wb").write(d[e:])' "$$img" $(NVS_OFF) $(NVS_SIZE) || exit 1; \
		imgargs="0x0 $$img.pre $$(( $(NVS_OFF) + $(NVS_SIZE) )) $$img.post"; \
	else \
		echo "warning: no nvs row in firmware/partitions.csv — writing the image whole (wipes NVS)"; \
		imgargs="0x0 $$img"; \
	fi

PWRCFG_OFF = $(call part_off,pwrcfg)
PWRCFG_BIN = firmware/dist/pwrcfg.bin
# shell snippet (companion to ESPTOOL_RESOLVE): encode the config's
# "power_switch" block into $(PWRCFG_BIN) and set $$pwr to the extra
# offset+file pair for esptool write_flash. No block (pwrcfg.py exit 3) or no
# config at all = $$pwr stays empty and the chip's settings are left alone (see
# the note above on why this feature, alone, is never written off implicitly).
# pwrcfg.py refuses wiring the firmware would silently drop (bad pin for
# TARGET, non-ADC sense, collision with the strip.pin data pin, ...) — the
# firmware has no console, so this is the only place a mistake can be caught.
# Absolute path — flash-source runs esptool from the build dir.
PWRCFG_RESOLVE = pwr=""; \
	if [ -n "$(CONFIG)" ]; then \
		[ -n "$(PWRCFG_OFF)" ] || { echo "no pwrcfg offset found in firmware/partitions.csv"; exit 1; }; \
		mkdir -p firmware/dist; \
		python3 tools/pwrcfg.py --out "$(PWRCFG_BIN)" --config "$(CONFIG)" \
			--target $(TARGET) --strip-pin $(STRIP_PIN) \
			$(PWR_AVOID); rc=$$?; \
		if [ $$rc -eq 0 ]; then pwr="$(PWRCFG_OFF) $(CURDIR)/$(PWRCFG_BIN)"; \
		elif [ $$rc -ne 3 ]; then exit 1; fi; \
	fi

FANCFG_OFF = $(call part_off,fancfg)
FANCFG_BIN = firmware/dist/fancfg.bin
# companion to PWRCFG_RESOLVE for the fan controller: encode the config's
# "fans" block into $(FANCFG_BIN) and set $$fan to the extra offset+file pair
# for esptool write_flash. Always, when there is a config to read (no block or
# no enabled header = written off); with no config at all the chip's fancfg is
# left alone. As with pwrcfg.py, fancfg.py is the only place a mistake can be
# caught — the firmware silently treats a bad pin as "not wired" and that fan
# just never spins — so it validates the block like the daemon does, and the
# pins against the chip and the other features.
FANCFG_RESOLVE = fan=""; \
	if [ -n "$(CONFIG)" ]; then \
		[ -n "$(FANCFG_OFF)" ] || { echo "no fancfg offset found in firmware/partitions.csv"; exit 1; }; \
		mkdir -p firmware/dist; \
		python3 tools/fancfg.py --out "$(FANCFG_BIN)" --config "$(CONFIG)" \
			--target $(TARGET) --strip-pin $(STRIP_PIN) \
			$(FAN_AVOID) || exit 1; \
		fan="$(FANCFG_OFF) $(CURDIR)/$(FANCFG_BIN)"; \
	else \
		echo "no config found (CONFIG) — leaving the fan settings on the chip alone"; \
	fi

BLECFG_OFF = $(call part_off,blecfg)
BLECFG_BIN = firmware/dist/blecfg.bin
# companion to PWRCFG_RESOLVE for the BLE power remote: encode the config's
# "ble_remote" block into $(BLECFG_BIN) and set $$ble to the extra offset+file
# pair for esptool write_flash. Always, when there is a config to read (no
# block = written off); blecfg.py is where a missing/weak token is caught —
# the firmware would happily serve whatever secret it is given.
BLECFG_RESOLVE = ble=""; \
	if [ -n "$(CONFIG)" ]; then \
		[ -n "$(BLECFG_OFF)" ] || { echo "no blecfg offset found in firmware/partitions.csv"; exit 1; }; \
		mkdir -p firmware/dist; \
		python3 tools/blecfg.py --out "$(BLECFG_BIN)" --config "$(CONFIG)" || exit 1; \
		ble="$(BLECFG_OFF) $(CURDIR)/$(BLECFG_BIN)"; \
	fi

# prebuilt firmware images published to GitHub Releases by .github/workflows/
# firmware.yml. `make flash` downloads the merged image for TARGET and writes it,
# so the common case needs no ESP-IDF and no container — just esptool. Pin a
# specific release with FW_RELEASE=v1.2.3 (default: latest).
FW_REPO ?= peterdk31/bc250_ws2812b_controller
FW_RELEASE ?= latest
FW_BIN = firmware-$(TARGET).bin
FW_URL = https://github.com/$(FW_REPO)/releases/$(if $(filter latest,$(FW_RELEASE)),latest/download,download/$(FW_RELEASE))/$(FW_BIN)

# the idf.py build command, run either natively or in the container. The wire
# protocol / frame parser / recording / crossfade headers compile straight from
# common/ (on the include path — see firmware/main/CMakeLists.txt), so there's
# nothing to copy. HOST_BAUD/HOST_TIMEOUT_MS are baked in as CMake cache defines;
# -DIDF_TARGET pins the arch. -DSDKCONFIG puts the generated sdkconfig INSIDE the
# per-target build dir (ESP-IDF otherwise writes one shared sdkconfig in the
# project dir, which would clash when switching between esp32 and esp32c3). $$ROOT
# is the absolute repo root in whichever context runs (host or container mount).
# `receiver` depends on `led` so the CONFIG_GET reads above resolve against the
# freshly built daemon.
IDF_BUILD_CMD = idf.py -C firmware -B $(RECEIVER_BUILD) -DIDF_TARGET=$(TARGET) \
                -DSDKCONFIG=$$ROOT/$(RECEIVER_BUILD)/sdkconfig \
                -DHOST_BAUD=$(BAUD) -DHOST_TIMEOUT_MS=$(TIMEOUT_MS) \
                $(if $(STRIP_USE_RMT),-DSTRIP_USE_RMT=$(STRIP_USE_RMT)) build

receiver: led receiver-toolchain
	@# idf.py only merges sdkconfig.defaults into a build dir's sdkconfig when
	@# that file doesn't exist yet — an existing build dir silently ignores a
	@# changed defaults (symptom: "Missing header ... found in component bt"
	@# right after a component was enabled there). Regenerate when stale.
	@if [ -f "$(RECEIVER_BUILD)/sdkconfig" ] && [ firmware/sdkconfig.defaults -nt "$(RECEIVER_BUILD)/sdkconfig" ]; then \
		echo "sdkconfig.defaults is newer than $(RECEIVER_BUILD)/sdkconfig — regenerating"; \
		rm -f "$(RECEIVER_BUILD)/sdkconfig" "$(RECEIVER_BUILD)/sdkconfig.old"; \
	fi
	@if [ -n "$(IDF_NATIVE)" ]; then \
		echo "building firmware ($(TARGET)) with native ESP-IDF at $(IDF_PATH)"; \
		bash -c 'ROOT="$(CURDIR)"; . "$(IDF_PATH)/export.sh" >/dev/null && $(IDF_BUILD_CMD)'; \
	else \
		echo "building firmware ($(TARGET)) in a throwaway $(CONTAINER) container ($(IDF_IMAGE))"; \
		$(CONTAINER) run --rm $(DOCKER_USER) -v "$(CURDIR)":/project -w /project $(IDF_IMAGE) \
			bash -c 'ROOT=/project; git config --global --add safe.directory "*" >/dev/null 2>&1; $(IDF_BUILD_CMD)'; \
	fi

# default flash: download the prebuilt merged image for TARGET and write it,
# skipping the nvs region (see IMG_SPLIT_NVS). Needs no ESP-IDF/container —
# just esptool and curl/wget. The image is generic (built with default
# HOST_BAUD; the receiver auto-hunts the baud), so it fits any config.
# Build-from-source instead with `make flash-source`.
flash:
	-@$(MAKE) --no-print-directory serial-perms
	@$(ESPTOOL_RESOLVE); $(PWRCFG_RESOLVE); $(FANCFG_RESOLVE); $(BLECFG_RESOLVE); \
	mkdir -p firmware/dist; img="firmware/dist/$(FW_BIN)"; \
	echo "fetching $(FW_URL)"; \
	if command -v curl >/dev/null 2>&1; then fetch="curl -fSL -o $$img $(FW_URL)"; \
	elif command -v wget >/dev/null 2>&1; then fetch="wget -qO $$img $(FW_URL)"; \
	else echo "need curl or wget to download the prebuilt image"; exit 1; fi; \
	if ! $$fetch; then \
		if [ -f "$$img" ]; then echo "download failed; using cached $$img"; \
		else echo "no prebuilt image for $(TARGET) — has a 'v*' release been published? otherwise build locally: make flash-source"; rm -f "$$img"; exit 1; fi; \
	fi; \
	$(IMG_SPLIT_NVS); \
	$(PORT_FREE); \
	echo "flashing $(TARGET) via $$tool on $(PORT)"; \
	$$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) write_flash $$imgargs $$pwr $$fan $$ble; \
	rc=$$?; \
	$(PORT_RESTORE); \
	exit $$rc

# build the firmware from source (container or native, see `receiver`) and flash
# it, instead of downloading a prebuilt image. Use this when you've changed the
# firmware or want a non-default HOST_BAUD/HOST_TIMEOUT_MS baked in. Flashes with
# the build's flash_args manifest (bootloader/partition-table/app at their right
# offsets) — exactly what idf.py runs under the hood, wrapped to free the port.
flash-source: receiver
	-@$(MAKE) --no-print-directory serial-perms
	@[ -f "$(RECEIVER_BUILD)/flash_args" ] || { echo "flash_args missing in $(RECEIVER_BUILD) (did the build run?)"; exit 1; }; \
	$(ESPTOOL_RESOLVE); $(PWRCFG_RESOLVE); $(FANCFG_RESOLVE); $(BLECFG_RESOLVE); \
	$(PORT_FREE); \
	echo "flashing $(TARGET) via $$tool on $(PORT)"; \
	( cd "$(RECEIVER_BUILD)" && $$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) write_flash @flash_args $$pwr $$fan $$ble ); \
	rc=$$?; \
	$(PORT_RESTORE); \
	exit $$rc

# write only the 4 KB pwrcfg partition — the config's "power_switch" block,
# after an edit — without reflashing the firmware (a couple of seconds). Note
# esptool still resets the chip — the reflashing caveat in the README (Power
# switch) applies here too.
flash-pwr:
	-@$(MAKE) --no-print-directory serial-perms
	@[ -n "$(CONFIG)" ] || { echo 'no config found — set CONFIG=path/to/config.json (its "power_switch" block is what gets written)'; exit 1; }; \
	$(ESPTOOL_RESOLVE); $(PWRCFG_RESOLVE); \
	[ -n "$$pwr" ] || { echo 'nothing to write: the config has no "power_switch" block'; exit 1; }; \
	$(PORT_FREE); \
	echo "writing power-switch config from $(CONFIG) via $$tool on $(PORT)"; \
	$$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) write_flash $$pwr; \
	rc=$$?; \
	$(PORT_RESTORE); \
	exit $$rc

# write only the 4 KB fancfg partition — the config's "fans" block, after an
# edit — without reflashing the firmware (seconds). Only useful once the chip
# runs a firmware that reads the FAN2 layout (this change on) and whose
# partition table has the fancfg entry — against an older layout the write
# lands in the (unused) factory tail and the firmware never sees it; the debug
# log says so at boot. An older firmware that knows only FAN1 reads a FAN2 blob
# as "no config" and turns the fans off — reflash the firmware first.
flash-fan:
	-@$(MAKE) --no-print-directory serial-perms
	@[ -n "$(CONFIG)" ] || { echo 'no config found — set CONFIG=path/to/config.json (its "fans" block is what gets written)'; exit 1; }; \
	$(ESPTOOL_RESOLVE); $(FANCFG_RESOLVE); \
	$(PORT_FREE); \
	echo "writing fan config from $(CONFIG) via $$tool on $(PORT)"; \
	$$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) write_flash $$fan; \
	rc=$$?; \
	$(PORT_RESTORE); \
	exit $$rc

# write only the 4 KB blecfg partition — the config's "ble_remote" block —
# without reflashing the firmware. Like flash-fan, the chip must already run a
# firmware whose partition table has the blecfg entry — against an older
# layout the write lands in the (unused) factory tail and the firmware never
# sees it; the debug log says so at boot.
flash-ble:
	-@$(MAKE) --no-print-directory serial-perms
	@[ -n "$(CONFIG)" ] || { echo 'no config found — set CONFIG=path/to/config.json (its "ble_remote" block is what gets written)'; exit 1; }; \
	$(ESPTOOL_RESOLVE); $(BLECFG_RESOLVE); \
	$(PORT_FREE); \
	echo "writing BLE remote config from $(CONFIG) via $$tool on $(PORT)"; \
	$$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) write_flash $$ble; \
	rc=$$?; \
	$(PORT_RESTORE); \
	exit $$rc

# Wipe the receiver's saved state without reflashing the app — the reset switch
# for "it behaves as if it remembers something wrong":
#
#   make clear-nvs          the NVS key store: remembered baud + strip geometry
#                           (prefs.*) and the power switch's own namespace. The
#                           firmware re-learns all of it from the next frames.
#   make clear-recordings   the LittleFS partition holding the boot/shutdown
#                           animations. The daemon re-uploads them (and the
#                           receiver re-formats the partition) on its next start,
#                           so this also clears a stale or half-written slot that
#                           the skip-unchanged hash check would otherwise keep.
#
# Both are plain erases of one partition region, offsets read from
# partitions.csv — the app, and pwrcfg (the power-switch wiring), are untouched.
# Note esptool resets the chip: the reflashing caveat in the README's "Power
# switch" section applies here too. Erasing nvs also erases the power switch's
# saved "PSU is on" intent, but on a C3 the firmware re-derives it from the
# still-latched PS_ON# pad hold and rewrites it (see psOnHeld in
# power_switch.cpp), so this doesn't cut a machine whose power hangs on the
# receiver.
clear-nvs:
	@$(call erase_part,nvs,saved baud + strip geometry)
	@echo "(the boot/shutdown recordings are not in NVS — they live on the" \
	      "'storage' partition: make clear-recordings)"

clear-recordings:
	@$(call erase_part,storage,boot + shutdown recordings)

# erase one named partition region: $(1) = partition name, $(2) = what it holds
erase_part = \
	off="$(call part_off,$(1))"; size="$(call part_size,$(1))"; \
	[ -n "$$off" ] && [ -n "$$size" ] || { echo "no '$(1)' partition in firmware/partitions.csv"; exit 1; }; \
	$(ESPTOOL_RESOLVE); \
	$(PORT_FREE); \
	echo "erasing $(1) ($(2)) at $$off, $$size bytes, via $$tool on $(PORT)"; \
	$$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) erase_region $$off $$size; \
	rc=$$?; \
	$(PORT_RESTORE); \
	exit $$rc

# reclaim the space: build dirs, downloaded prebuilt images, and the cached
# ESP-IDF container image (the ~2 GB download). Run this once you're done.
receiver-clean:
	rm -rf firmware/build firmware/build-* firmware/dist $(IDF_GENERATED)
	-@[ -n "$(CONTAINER)" ] && $(CONTAINER) rmi $(IDF_IMAGE) 2>/dev/null || true

.PHONY: all install install-config uninstall clean udev-rule serial-perms receiver-toolchain receiver receiver-clean flash flash-source flash-pwr flash-fan flash-ble clear-nvs clear-recordings
