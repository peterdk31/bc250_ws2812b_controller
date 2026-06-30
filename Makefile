CXX = g++
# source is grouped by where it runs: common/ compiles into both the daemon
# and the firmware (the daemon<->receiver link layer only), daemon/ is the
# host daemon, firmware/ is the ESP32 sketch, vendor/ is third-party.
# includes stay path-less, so each source dir goes on the include path here
# and the receiver build (which copies the common files flat) needs no -I
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra \
           -Icommon -Idaemon -Idaemon/rules -Idaemon/output \
           -Idaemon/sources -Idaemon/color -Idaemon/effects -Ivendor
PREFIX = /usr/local

# ESP32 receiver flashing via arduino-cli. PORT, BAUD and TIMEOUT_MS default
# to the matching keys from the installed config (or the repo one); override
# on the command line, e.g. `make flash PORT=/dev/ttyACM0`. BAUD and TIMEOUT_MS
# are baked into the sketch so receiver and daemon agree. The receiver renders
# no effects, so it needs no strip/effect config baked in — geometry and the
# power-on/shutdown animations arrive as recordings the daemon streams at
# runtime (see common/protocol.hpp).
#
# the values are read through the daemon's own JSON parser (`led
# --config-get <dotted.path>`) rather than scraped, so they can't drift
# from how the daemon reads the same file. `receiver` depends on `led`
# so it exists by the time these expand; if it somehow doesn't, each
# falls back to the default below (same as before)
CONFIG ?= $(firstword $(wildcard /etc/led-controller/config.json config.json))
CONFIG_GET = ./led --config-get $(CONFIG)
BAUD ?= $(or $(shell $(CONFIG_GET) sinks.serial.baud 2>/dev/null),921600)
TIMEOUT_MS ?= $(or $(shell $(CONFIG_GET) esp32.host_timeout_ms 2>/dev/null),5000)

# PORT: config (or a command-line override) is authoritative; detection only
# fills a gap. Resolution order: command-line override > configured
# sinks.serial.port > first board arduino-cli lists > default. Config wins over
# detection on purpose — a port pinned in config (e.g. a C3 on /dev/ttyACM1)
# must not be overridden just because arduino-cli happens to list another device
# (e.g. /dev/ttyACM0) first. Detection is the fallback for a fresh setup with no
# configured port.
DETECTED_PORT = $(shell arduino-cli board list 2>/dev/null | awk '/^\/dev\/tty/{print $$1; exit}')
PORT ?= $(or $(shell $(CONFIG_GET) sinks.serial.port 2>/dev/null),$(DETECTED_PORT),/dev/ttyUSB0)

# FQBN: fingerprint the board actually on PORT (native-USB boards like the C3/S3
# report one; bridge boards don't), else the configured esp32.fqbn, else the
# plain-ESP32 default. A command-line override always wins, e.g.
# `make flash FQBN=esp32:esp32:esp32s3`. The FQBN matters because an ESP32-C3 is
# RISC-V, not Xtensa — the plain-esp32 FQBN builds a binary for the wrong arch.
# PORT may be a stable /dev/serial/by-id/... symlink (recommended, so it can't
# renumber when other USB devices are plugged in), but `arduino-cli board list`
# reports the raw /dev/ttyACM*. Resolve the symlink so the match below works
# either way. readlink -f returns the path unchanged when it isn't a symlink.
PORT_REAL = $(shell readlink -f $(PORT) 2>/dev/null)
# filter-out esp32_family: native-USB Espressif chips (C3/S3/…) all share one USB
# identity, so `arduino-cli board list` reports the generic "esp32:esp32:esp32_family"
# board for them. That board has NO concrete build.mcu/build.partitions (you pick
# the chip via a menu), so building with it leaves literal "{build.mcu}" /
# "{build.partitions}" and fails. It's useless for us — drop it so FQBN falls back
# to the configured esp32.fqbn (the real chip, e.g. esp32c3).
DETECTED_FQBN = $(filter-out esp32:esp32:esp32_family,$(shell arduino-cli board list 2>/dev/null | \
	awk -v p='$(PORT)' -v r='$(PORT_REAL)' '$$1==p || ($$1==r && r!=""){for(i=1;i<=NF;i++) if($$i ~ /^esp32:/){print $$i; exit}}'))
FQBN ?= $(or $(DETECTED_FQBN),$(shell $(CONFIG_GET) esp32.fqbn 2>/dev/null),esp32:esp32:esp32)

# native-USB chips (C3/S2/S3/C6/H2) expose their own USB as a CDC serial port;
# a board reached over that USB (it enumerates as /dev/ttyACM*) is only readable
# by the sketch if Serial is mapped to the USB CDC. The obvious way is the
# CDCOnBoot=cdc FQBN menu option — but on esp32 3.3.x, ANY menu option in the
# FQBN makes the toolchain stop expanding board properties, so the build/upload
# end up with literal "{build.partitions}.csv" / "--chip {build.mcu}" and fail.
# So we keep the FQBN bare (which resolves cleanly) and instead define the CDC
# macro directly in the compile flags — same effect, no menu option. The
# original ESP32 has no native USB (UART bridge wired to UART0, which Serial
# already is), so it gets no flag. Partitions are handled by the sketch-local
# firmware/partitions.csv, also avoiding a PartitionScheme menu option.
NATIVE_USB_BOARDS = esp32c3 esp32s2 esp32s3 esp32c6 esp32h2
FQBN_BOARD = $(word 3,$(subst :, ,$(FQBN)))
CDC_FLAG = $(if $(filter $(FQBN_BOARD),$(NATIVE_USB_BOARDS)),-DARDUINO_USB_CDC_ON_BOOT=1)
ESP32_URL = https://espressif.github.io/arduino-esp32/package_esp32_index.json

HEADERS = daemon/output/strip.hpp daemon/config_loader.hpp vendor/json.hpp \
          daemon/effects/effect.hpp daemon/rules/condition.hpp daemon/color/color.hpp \
          daemon/sources/hwmon.hpp daemon/sources/steam.hpp daemon/rules/rules.hpp \
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
	rm -rf $(RECEIVER_SRC) firmware/build

# add the user to the serial port's group (uucp on Arch-likes, dialout
# on Debian/Fedora) so running `led` and arduino-cli uploads work
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

# install the ESP32 core and the strip library; runs automatically on
# the first `make receiver`/`make flash`
receiver-setup:
	arduino-cli core update-index --additional-urls $(ESP32_URL)
	arduino-cli core install esp32:esp32 --additional-urls $(ESP32_URL)
	arduino-cli lib install "Freenove WS2812 Lib for ESP32"

receiver-toolchain:
	@command -v arduino-cli >/dev/null 2>&1 || \
		{ echo "arduino-cli not found, install it first:" \
		       "https://arduino.github.io/arduino-cli/"; exit 1; }
	@arduino-cli core list 2>/dev/null | grep -q '^esp32:esp32' && \
	 arduino-cli lib list 2>/dev/null | grep -q 'Freenove WS2812' || \
		$(MAKE) receiver-setup

# the receiver shares only the wire protocol, the streaming frame parser and
# the recording format/replay logic with the daemon — i.e. all of common/, no
# effect code, no color LUT (it replays already-corrected recordings the daemon
# streams). arduino-cli compiles a sketch's src/ subdir, so mirror the common
# headers in there (flat, which is why includes are path-less); it's
# regenerated each build and gitignored.
RECEIVER_SRC = firmware/src
# compile output: arduino-cli writes the binaries here (incl. the all-in-one
# firmware.ino.merged.bin we flash). Gitignored.
RECEIVER_BUILD = firmware/build
MERGED_BIN = $(RECEIVER_BUILD)/firmware.ino.merged.bin
SHARED = $(wildcard common/*.hpp)

# baked into the sketch so receiver and daemon agree on the link (everything
# else — geometry, animations — arrives at runtime as recordings).
# compiler.cpp.extra_flags rather than build.extra_flags: the latter would
# replace the esp32 platform's own extra flags.
BUILD_PROPS = --build-property "compiler.cpp.extra_flags=-DHOST_BAUD=$(BAUD) -DHOST_TIMEOUT_MS=$(TIMEOUT_MS) $(CDC_FLAG)"

# esptool for flashing directly, bypassing `arduino-cli upload` (which on this
# arduino-cli/core leaves the chip arg as a literal "--chip {build.mcu}"). Prefer
# the esptool the esp32 core ships — guaranteed to match the chip — found under
# the *invoking* user's arduino data dir even under sudo (cores live there, not
# root's). Fall back to a PATH esptool. Override with ESPTOOL=/path if needed.
ARDUINO_DATA = $(shell getent passwd $${SUDO_USER:-$$(id -un)} 2>/dev/null | cut -d: -f6)/.arduino15
ESPTOOL ?= $(or $(firstword $(wildcard $(ARDUINO_DATA)/packages/esp32/tools/esptool_py/*/esptool)),$(shell command -v esptool.py 2>/dev/null),$(shell command -v esptool 2>/dev/null))

receiver-shared:
	rm -rf $(RECEIVER_SRC)
	mkdir -p $(RECEIVER_SRC)
	cp $(SHARED) $(RECEIVER_SRC)/

# compile-only check (flash compiles+uploads in one step). Recordings live on
# LittleFS in the default partition scheme's filesystem region, formatted on
# first boot. `receiver` depends on `led` so the CONFIG_GET reads above resolve
# against the freshly built daemon.
receiver: led receiver-toolchain receiver-shared
	arduino-cli compile --fqbn $(FQBN) $(BUILD_PROPS) --output-dir $(RECEIVER_BUILD) firmware

# compile (via the `receiver` prereq) produces the binaries; we then flash the
# all-in-one merged image with esptool DIRECTLY rather than `arduino-cli upload`
# — on this arduino-cli/core the upload recipe leaves the chip arg as a literal
# "--chip {build.mcu}" and esptool rejects it. esptool --chip auto detects the
# board over USB, so we never depend on that property. Flashing the merged.bin
# at 0x0 writes bootloader+partitions+app at their right offsets in one shot.
#
# The daemon holds the serial port open, so free it around the flash and restart
# only if it was running. The port isn't opened exclusively
# (daemon/output/serial_sink.hpp), so a daemon still writing LED frames collides
# with esptool ("Invalid head of packet"). Stopping the unit covers the normal
# case; fuser -k clears a hand-started daemon. fuser is best-effort and only ever
# targets PORT. Compile runs first (daemon still up — it doesn't need the port).
flash: receiver
	-@$(MAKE) --no-print-directory serial-perms
	@tool="$(ESPTOOL)"; \
	[ -n "$$tool" ] || { echo "esptool not found; set ESPTOOL=/path/to/esptool or 'pip install esptool'"; exit 1; }; \
	[ -f "$(MERGED_BIN)" ] || { echo "merged image missing: $(MERGED_BIN) (did the compile run?)"; exit 1; }; \
	active=0; systemctl is-active --quiet led-controller && active=1; \
	[ $$active = 1 ] && systemctl stop led-controller; \
	if command -v fuser >/dev/null 2>&1 && fuser $(PORT) >/dev/null 2>&1; then \
		echo "$(PORT) still held after stopping the service; freeing it:"; \
		fuser -v $(PORT) || true; \
		fuser -k $(PORT) || true; \
		sleep 1; \
	fi; \
	echo "flashing $(MERGED_BIN) via $$tool"; \
	"$$tool" --chip auto --port $(PORT) --baud $(BAUD) write_flash 0x0 "$(MERGED_BIN)"; \
	rc=$$?; \
	[ $$active = 1 ] && systemctl start led-controller; \
	exit $$rc

.PHONY: all install install-config uninstall clean udev-rule serial-perms receiver-setup receiver-toolchain receiver-shared receiver flash
