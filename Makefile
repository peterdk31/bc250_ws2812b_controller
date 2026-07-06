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
BAUD ?= $(or $(shell $(CONFIG_GET) sinks.serial.baud 2>/dev/null),921600)
TIMEOUT_MS ?= $(or $(shell $(CONFIG_GET) esp32.host_timeout_ms 2>/dev/null),5000)

# PORT: config (or a command-line override) is authoritative; detection only
# fills a gap. Resolution order: command-line override > configured
# sinks.serial.port > first likely device node > default. Config wins over
# detection on purpose — a port pinned in config (e.g. a C3 on /dev/ttyACM1)
# must not be overridden just because another device enumerated first. The udev
# rule pins /dev/led-controller, so it's preferred by the glob when present.
DETECTED_PORT = $(firstword $(wildcard /dev/led-controller /dev/ttyACM* /dev/ttyUSB*))
PORT ?= $(or $(shell $(CONFIG_GET) sinks.serial.port 2>/dev/null),$(DETECTED_PORT),/dev/ttyUSB0)

# TARGET: which ESP chip to build for. An ESP32-C3 is RISC-V, not Xtensa, so the
# firmware must be built for the right arch. From esp32.target in config, else
# the C3 (the board this project ships with). A command-line override always
# wins, e.g. `make flash TARGET=esp32`. esptool auto-detects the connected chip
# when flashing, but the *build* needs the target named explicitly.
VALID_TARGETS = esp32 esp32c3 esp32s2 esp32s3 esp32c6 esp32h2
TARGET ?= $(or $(filter $(VALID_TARGETS),$(shell $(CONFIG_GET) esp32.target 2>/dev/null)),esp32c3)

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

HEADERS = daemon/output/strip.hpp daemon/config_loader.hpp vendor/json.hpp \
          daemon/effects/effect.hpp daemon/rules/condition.hpp daemon/color/color.hpp \
          daemon/sources/hwmon.hpp daemon/sources/steam.hpp \
          daemon/sources/audio.hpp daemon/rules/rules.hpp \
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
                -DHOST_BAUD=$(BAUD) -DHOST_TIMEOUT_MS=$(TIMEOUT_MS) build

receiver: led receiver-toolchain
	@if [ -n "$(IDF_NATIVE)" ]; then \
		echo "building firmware ($(TARGET)) with native ESP-IDF at $(IDF_PATH)"; \
		bash -c 'ROOT="$(CURDIR)"; . "$(IDF_PATH)/export.sh" >/dev/null && $(IDF_BUILD_CMD)'; \
	else \
		echo "building firmware ($(TARGET)) in a throwaway $(CONTAINER) container ($(IDF_IMAGE))"; \
		$(CONTAINER) run --rm $(DOCKER_USER) -v "$(CURDIR)":/project -w /project $(IDF_IMAGE) \
			bash -c 'ROOT=/project; git config --global --add safe.directory "*" >/dev/null 2>&1; $(IDF_BUILD_CMD)'; \
	fi

# default flash: download the prebuilt merged image for TARGET and write it at
# 0x0. Needs no ESP-IDF/container — just esptool and curl/wget. The image is
# generic (built with default HOST_BAUD; the receiver auto-hunts the baud), so
# it fits any config. Build-from-source instead with `make flash-source`.
#
# The daemon holds the serial port open, so free it around the flash and restart
# only if it was running (the port isn't opened exclusively — a daemon still
# streaming frames collides with esptool, "Invalid head of packet"). Stopping
# the unit covers the normal case; fuser -k clears a hand-started daemon.
flash:
	-@$(MAKE) --no-print-directory serial-perms
	@$(ESPTOOL_RESOLVE); \
	mkdir -p firmware/dist; img="firmware/dist/$(FW_BIN)"; \
	echo "fetching $(FW_URL)"; \
	if command -v curl >/dev/null 2>&1; then fetch="curl -fSL -o $$img $(FW_URL)"; \
	elif command -v wget >/dev/null 2>&1; then fetch="wget -qO $$img $(FW_URL)"; \
	else echo "need curl or wget to download the prebuilt image"; exit 1; fi; \
	if ! $$fetch; then \
		if [ -f "$$img" ]; then echo "download failed; using cached $$img"; \
		else echo "no prebuilt image for $(TARGET) — has a 'v*' release been published? otherwise build locally: make flash-source"; rm -f "$$img"; exit 1; fi; \
	fi; \
	active=0; systemctl is-active --quiet led-controller && active=1; \
	[ $$active = 1 ] && systemctl stop led-controller; \
	if command -v fuser >/dev/null 2>&1 && fuser $(PORT) >/dev/null 2>&1; then \
		echo "$(PORT) still held after stopping the service; freeing it:"; \
		fuser -v $(PORT) || true; fuser -k $(PORT) || true; sleep 1; \
	fi; \
	echo "flashing $(TARGET) via $$tool on $(PORT)"; \
	$$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) write_flash 0x0 "$$img"; \
	rc=$$?; \
	[ $$active = 1 ] && systemctl start led-controller; \
	exit $$rc

# build the firmware from source (container or native, see `receiver`) and flash
# it, instead of downloading a prebuilt image. Use this when you've changed the
# firmware or want a non-default HOST_BAUD/HOST_TIMEOUT_MS baked in. Flashes with
# the build's flash_args manifest (bootloader/partition-table/app at their right
# offsets) — exactly what idf.py runs under the hood, wrapped to free the port.
flash-source: receiver
	-@$(MAKE) --no-print-directory serial-perms
	@[ -f "$(RECEIVER_BUILD)/flash_args" ] || { echo "flash_args missing in $(RECEIVER_BUILD) (did the build run?)"; exit 1; }; \
	$(ESPTOOL_RESOLVE); \
	active=0; systemctl is-active --quiet led-controller && active=1; \
	[ $$active = 1 ] && systemctl stop led-controller; \
	if command -v fuser >/dev/null 2>&1 && fuser $(PORT) >/dev/null 2>&1; then \
		echo "$(PORT) still held after stopping the service; freeing it:"; \
		fuser -v $(PORT) || true; fuser -k $(PORT) || true; sleep 1; \
	fi; \
	echo "flashing $(TARGET) via $$tool on $(PORT)"; \
	( cd "$(RECEIVER_BUILD)" && $$tool --chip $(TARGET) --port "$(PORT)" --baud $(BAUD) write_flash @flash_args ); \
	rc=$$?; \
	[ $$active = 1 ] && systemctl start led-controller; \
	exit $$rc

# reclaim the space: build dirs, downloaded prebuilt images, and the cached
# ESP-IDF container image (the ~2 GB download). Run this once you're done.
receiver-clean:
	rm -rf firmware/build firmware/build-* firmware/dist $(IDF_GENERATED)
	-@[ -n "$(CONTAINER)" ] && $(CONTAINER) rmi $(IDF_IMAGE) 2>/dev/null || true

.PHONY: all install install-config uninstall clean udev-rule serial-perms receiver-toolchain receiver receiver-clean flash flash-source
