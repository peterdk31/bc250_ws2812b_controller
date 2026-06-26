CXX = g++
# source is grouped by where it runs: shared/ compiles into both the
# daemon and the firmware, host/ is daemon-only, vendor/ is third-party.
# includes stay path-less, so each dir goes on the include path here and
# the receiver build (which copies the shared files flat) needs no -I
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -Ishared -Ihost -Ivendor
PREFIX = /usr/local

# ESP32 receiver flashing via arduino-cli. PORT, BAUD and TIMEOUT_MS default
# to the matching keys from the installed config (or the repo one); override
# on the command line, e.g. `make flash PORT=/dev/ttyACM0`. BAUD and TIMEOUT_MS
# are baked into the sketch so receiver and daemon agree. The receiver renders
# no effects, so it needs no strip/effect config baked in — geometry and the
# power-on/shutdown animations arrive as recordings the daemon streams at
# runtime (see shared/protocol.hpp).
#
# the values are read through the daemon's own JSON parser (`led
# --config-get <dotted.path>`) rather than scraped, so they can't drift
# from how the daemon reads the same file. `receiver` depends on `led`
# so it exists by the time these expand; if it somehow doesn't, each
# falls back to the default below (same as before)
CONFIG ?= $(firstword $(wildcard /etc/led-controller/config.json config.json))
CONFIG_GET = ./led --config-get $(CONFIG)
PORT ?= $(or $(shell $(CONFIG_GET) sinks.serial.port 2>/dev/null),/dev/ttyUSB0)
BAUD ?= $(or $(shell $(CONFIG_GET) sinks.serial.baud 2>/dev/null),921600)
TIMEOUT_MS ?= $(or $(shell $(CONFIG_GET) esp32.host_timeout_ms 2>/dev/null),5000)
FQBN ?= esp32:esp32:esp32
ESP32_URL = https://espressif.github.io/arduino-esp32/package_esp32_index.json

HEADERS = host/strip.hpp host/config_loader.hpp vendor/json.hpp \
          shared/effect.hpp host/condition.hpp shared/color.hpp \
          host/hwmon.hpp host/steam.hpp host/rules.hpp \
          host/sink.hpp host/serial_sink.hpp host/virtual_sink.hpp \
          host/virtual_strip_socket.hpp host/recorder.hpp \
          shared/color_lut.hpp shared/protocol.hpp shared/motion.hpp \
          shared/recording.hpp shared/fade.hpp

# every effect compiles in and registers itself. shared/effects/*
# are host-independent (also linked into the firmware); host/effects/*
# need live host data (sensors, Steam) and only run on the daemon
EFFECT_SRCS = $(wildcard shared/effects/*.cpp host/effects/*.cpp)
SRCS = host/main.cpp shared/effect_registry.cpp host/conditions.cpp \
       host/rules.cpp $(EFFECT_SRCS)

all: led

led: $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ -lm

# on-screen virtual LED strip: a standalone terminal viewer that renders the
# wire frames the daemon mirrors to it (see host/virtual_sink.hpp). No
# hardware and no extra deps — plain g++. Built on demand, not part of `all`,
# so the deploy path is unchanged. Run it, then start `led` (or run a single
# effect) to preview an animation before pushing it to the BC-250.
virtual-strip: host/virtual_strip.cpp host/virtual_strip_socket.hpp shared/receiver.hpp shared/protocol.hpp shared/fade.hpp shared/motion.hpp
	$(CXX) $(CXXFLAGS) host/virtual_strip.cpp -o $@

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
# the recording format/replay logic with the daemon — no effect code, no color
# LUT (it replays already-corrected recordings the daemon streams). arduino-cli
# compiles a sketch's src/ subdir, so mirror the shared headers in there (flat,
# which is why includes are path-less); it's regenerated each build and gitignored.
RECEIVER_SRC = esp32_receiver/src
SHARED = shared/protocol.hpp shared/receiver.hpp shared/recording.hpp \
         shared/fade.hpp shared/motion.hpp

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
	arduino-cli compile --fqbn $(FQBN) \
		--build-property "compiler.cpp.extra_flags=-DHOST_BAUD=$(BAUD) -DHOST_TIMEOUT_MS=$(TIMEOUT_MS)" \
		esp32_receiver

# the daemon holds the serial port open, so stop it around the upload
# and only restart it if it was running
flash: receiver
	-@$(MAKE) --no-print-directory serial-perms
	@active=0; systemctl is-active --quiet led-controller && active=1; \
	[ $$active = 1 ] && systemctl stop led-controller; \
	arduino-cli upload -p $(PORT) --fqbn $(FQBN) esp32_receiver; \
	rc=$$?; \
	[ $$active = 1 ] && systemctl start led-controller; \
	exit $$rc

.PHONY: all install uninstall clean serial-perms receiver-setup receiver-toolchain receiver-shared receiver flash
