# ESP-IDF project for the LED receiver. Replaces the old arduino-cli / .ino
# build: `idf.py build` compiles, esptool flashes (see the repo Makefile's
# `receiver`/`flash` targets, which wrap this via $(IDF_PATH)/export.sh).
#
# The receiver renders no effects. It receives already-corrected pixel frames
# and power-on/shutdown recordings from the host daemon over the serial link,
# stores the recordings on LittleFS, and clocks frames to the WS2812 strip.
# The whole wire protocol / framing / recording / crossfade logic is shared
# with the daemon and lives in ../common (added to the include path in
# main/CMakeLists.txt — nothing is copied).
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(led_receiver)
