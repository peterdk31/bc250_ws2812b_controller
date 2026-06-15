CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -I.
PREFIX = /usr/local

# ESP32 receiver flashing via arduino-cli; PORT, BAUD, TIMEOUT_MS,
# LEDS, LED_PIN, BRIGHTNESS and WHITE_BALANCE default to the matching
# keys from the installed config (or the repo one), override on the
# command line, e.g. `make flash PORT=/dev/ttyACM0`. All but PORT are
# baked into the sketch so receiver and daemon always agree;
# LEDS/LED_PIN/BRIGHTNESS/WHITE_BALANCE drive the receiver's standalone
# power-on breathe before the daemon is up
CONFIG = $(firstword $(wildcard /etc/led-controller/config.json config.json))
PORT ?= $(or $(shell sed -n 's/.*"port"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' $(CONFIG) /dev/null | head -n1),/dev/ttyUSB0)
BAUD ?= $(or $(shell sed -n 's/.*"baud"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' $(CONFIG) /dev/null | head -n1),921600)
TIMEOUT_MS ?= $(or $(shell sed -n 's/.*"host_timeout_ms"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' $(CONFIG) /dev/null | head -n1),5000)
LEDS ?= $(or $(shell sed -n 's/.*"leds"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' $(CONFIG) /dev/null | head -n1),0)
LED_PIN ?= $(or $(shell sed -n 's/.*"pin"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' $(CONFIG) /dev/null | head -n1),13)
BRIGHTNESS ?= $(or $(shell sed -n 's/.*"brightness"[[:space:]]*:[[:space:]]*\([0-9.][0-9.]*\).*/\1/p' $(CONFIG) /dev/null | head -n1),0.2)
# strip leading '#' if present; default to ffffff (no correction)
WHITE_BALANCE ?= $(or $(shell sed -n 's/.*"white_balance"[[:space:]]*:[[:space:]]*"#\?\([0-9A-Fa-f]*\)".*/\1/p' $(CONFIG) /dev/null | head -n1),ffffff)
FQBN ?= esp32:esp32:esp32
ESP32_URL = https://espressif.github.io/arduino-esp32/package_esp32_index.json

HEADERS = ws2812_serial.hpp config_loader.hpp json.hpp effect.hpp condition.hpp color.hpp hwmon.hpp steam.hpp rules.hpp

# every effects/*.cpp is compiled in and registers itself
EFFECT_SRCS = $(wildcard effects/*.cpp)
SRCS = main.cpp effect_registry.cpp conditions.cpp rules.cpp $(EFFECT_SRCS)

all: led

led: $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ -lm

install: led
	install -Dm755 led $(PREFIX)/bin/led
	install -Dm644 led-controller.service /etc/systemd/system/led-controller.service
	test -f /etc/led-controller/config.json || install -Dm644 config.json /etc/led-controller/config.json
	systemctl daemon-reload
	-@$(MAKE) --no-print-directory serial-perms

uninstall:
	-systemctl disable --now led-controller
	rm -f /etc/systemd/system/led-controller.service
	rm -f $(PREFIX)/bin/led
	rm -rf /etc/led-controller
	systemctl daemon-reload

clean:
	rm -f led

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

# compiler.cpp.extra_flags rather than build.extra_flags: the latter
# would replace the esp32 platform's own extra flags
receiver: receiver-toolchain
	arduino-cli compile --fqbn $(FQBN) \
		--build-property "compiler.cpp.extra_flags=-DHOST_BAUD=$(BAUD) -DHOST_TIMEOUT_MS=$(TIMEOUT_MS) -DDEFAULT_LED_COUNT=$(LEDS) -DDEFAULT_LED_PIN=$(LED_PIN) -DSTRIP_BRIGHTNESS=$(BRIGHTNESS) -DSTRIP_WHITE_BALANCE=0x$(WHITE_BALANCE)" \
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

.PHONY: all install uninstall clean serial-perms receiver-setup receiver-toolchain receiver flash
