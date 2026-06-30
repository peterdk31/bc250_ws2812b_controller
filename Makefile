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

# auto-detect the connected board so `make flash` targets whatever's plugged in
# without setting anything. BOARD_ROW is the first serial line arduino-cli
# reports; DETECTED_PORT/FQBN are its port and (if the board fingerprinted) its
# FQBN. A native-USB board (ESP32-C3/S3) reports both — so a C3 on /dev/ttyACM*
# is found even though the old default port was /dev/ttyUSB0. A bridge board
# (plain ESP32 on CH340/CP2102) reports its port but no identity, so its FQBN
# falls back to config/default. filter esp32:% grabs both the FQBN and Core
# columns; firstword keeps the FQBN (it comes first in the row).
BOARD_ROW = $(shell arduino-cli board list 2>/dev/null | awk '/^\/dev\/tty/{print; exit}')
DETECTED_PORT = $(firstword $(BOARD_ROW))
DETECTED_FQBN = $(firstword $(filter esp32:%,$(BOARD_ROW)))

# PORT/FQBN resolution, most specific first: a detected board, then config, then
# the plain-ESP32 default. A command-line override always wins, e.g.
# `make flash PORT=/dev/ttyACM0 FQBN=esp32:esp32:esp32s3`. The FQBN matters
# because an ESP32-C3 is RISC-V, not Xtensa — flashing it with the plain-esp32
# FQBN builds a binary for the wrong architecture.
PORT ?= $(or $(DETECTED_PORT),$(shell $(CONFIG_GET) sinks.serial.port 2>/dev/null),/dev/ttyUSB0)
FQBN ?= $(or $(DETECTED_FQBN),$(shell $(CONFIG_GET) esp32.fqbn 2>/dev/null),esp32:esp32:esp32)

# native-USB chips (C3/S2/S3/C6/H2) expose their own USB as a CDC serial port;
# a board reached over that USB (it enumerates as /dev/ttyACM*) is only readable
# by the sketch if Serial is mapped to the USB CDC, so do that automatically for
# those chips. The original ESP32 has no native USB and no such menu option —
# its USB is an external UART bridge wired to UART0, which Serial already is, so
# it's left untouched. Same firmware, correct transport per chip. Skipped if a
# CDCOnBoot option is already present in the FQBN. (Done with make functions,
# not a shell case: the ')' in case patterns confuses $(shell)'s paren matching.)
NATIVE_USB_BOARDS = esp32c3 esp32s2 esp32s3 esp32c6 esp32h2
FQBN_BOARD = $(word 3,$(subst :, ,$(FQBN)))
CDC_SUFFIX = $(if $(findstring CDCOnBoot=,$(FQBN)),,$(if $(filter $(FQBN_BOARD),$(NATIVE_USB_BOARDS)),:CDCOnBoot=cdc))
FQBN_FULL = $(FQBN)$(CDC_SUFFIX)
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
	-@$(MAKE) --no-print-directory serial-perms

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
	rm -rf /etc/led-controller
	systemctl daemon-reload

clean:
	rm -f led virtual-strip
	rm -rf $(RECEIVER_SRC)

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
SHARED = $(wildcard common/*.hpp)

receiver-shared:
	rm -rf $(RECEIVER_SRC)
	mkdir -p $(RECEIVER_SRC)
	cp $(SHARED) $(RECEIVER_SRC)/

# compiler.cpp.extra_flags rather than build.extra_flags: the latter would
# replace the esp32 platform's own extra flags. Only BAUD/TIMEOUT are baked in
# now; everything else (geometry, the power-on/shutdown animations) arrives at
# runtime as recordings. The recordings live on LittleFS in the default
# partition scheme's filesystem region, formatted on first boot. `receiver`
# depends on `led` so the CONFIG_GET reads above resolve against the freshly
# built daemon.
receiver: led receiver-toolchain receiver-shared
	arduino-cli compile --fqbn $(FQBN_FULL) \
		--build-property "compiler.cpp.extra_flags=-DHOST_BAUD=$(BAUD) -DHOST_TIMEOUT_MS=$(TIMEOUT_MS)" \
		firmware

# the daemon holds the serial port open, so free it around the upload and only
# restart the service if it was running. The port isn't opened exclusively
# (daemon/output/serial_sink.hpp), so a daemon still writing LED frames during
# the upload collides with esptool and shows up as "Invalid head of packet:
# possible serial noise or corruption". Stopping the systemd unit covers the
# normal case; fuser -k then clears a daemon started by hand (./led ...) or any
# other writer the unit-stop wouldn't catch. fuser is best-effort (skipped if
# absent), and only ever targets PORT, so it can't kill anything unrelated.
flash: receiver
	-@$(MAKE) --no-print-directory serial-perms
	@active=0; systemctl is-active --quiet led-controller && active=1; \
	[ $$active = 1 ] && systemctl stop led-controller; \
	if command -v fuser >/dev/null 2>&1 && fuser $(PORT) >/dev/null 2>&1; then \
		echo "$(PORT) still held after stopping the service; freeing it:"; \
		fuser -v $(PORT) || true; \
		fuser -k $(PORT) || true; \
		sleep 1; \
	fi; \
	arduino-cli upload -p $(PORT) --fqbn $(FQBN_FULL) firmware; \
	rc=$$?; \
	[ $$active = 1 ] && systemctl start led-controller; \
	exit $$rc

.PHONY: all install uninstall clean serial-perms receiver-setup receiver-toolchain receiver-shared receiver flash
