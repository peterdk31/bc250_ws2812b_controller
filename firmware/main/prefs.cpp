#include "prefs.hpp"

#include "nvs.h"

namespace prefs
{
static nvs_handle_t g_nvs = 0;

void init(const char* ns) { nvs_open(ns, NVS_READWRITE, &g_nvs); }

uint32_t getU32(const char* key, uint32_t def)
{
    uint32_t v;
    return nvs_get_u32(g_nvs, key, &v) == ESP_OK ? v : def;
}
uint16_t getU16(const char* key, uint16_t def)
{
    uint16_t v;
    return nvs_get_u16(g_nvs, key, &v) == ESP_OK ? v : def;
}
uint8_t getU8(const char* key, uint8_t def)
{
    uint8_t v;
    return nvs_get_u8(g_nvs, key, &v) == ESP_OK ? v : def;
}
void setU32(const char* key, uint32_t v)
{
    nvs_set_u32(g_nvs, key, v);
    nvs_commit(g_nvs);
}
void setU16(const char* key, uint16_t v)
{
    nvs_set_u16(g_nvs, key, v);
    nvs_commit(g_nvs);
}
void setU8(const char* key, uint8_t v)
{
    nvs_set_u8(g_nvs, key, v);
    nvs_commit(g_nvs);
}
} // namespace prefs
