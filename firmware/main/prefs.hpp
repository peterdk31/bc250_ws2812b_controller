#pragma once

#include <cstdint>

// Thin wrapper around one NVS namespace (the old Arduino Preferences store).
// init() opens the namespace once (nvs_flash_init must already have run — see
// app_main); gets return the default when the key is missing, sets commit
// immediately. One handle only — a future feature wanting its own keys should
// open its own namespace rather than widen this one.
namespace prefs
{
void init(const char* ns);

uint32_t getU32(const char* key, uint32_t def);
uint16_t getU16(const char* key, uint16_t def);
uint8_t getU8(const char* key, uint8_t def);

void setU32(const char* key, uint32_t v);
void setU16(const char* key, uint16_t v);
void setU8(const char* key, uint8_t v);
} // namespace prefs
