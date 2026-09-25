#include "cfgstore.hpp"

#include <cstring>

#include "esp_partition.h"

namespace cfgstore
{
bool partition(const char* label, uint8_t* out, size_t len, bool* found)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (found)
        *found = part != nullptr;
    if (!part)
        return false;
    return esp_partition_read(part, 0, out, len) == ESP_OK;
}

bool load(nvs_handle_t nvs, const char* key, const char* baseKey, const uint8_t* base,
          size_t len, uint8_t* out)
{
    // the base is compared first, into a stack copy, so a mismatch (or a
    // blob of another length — an older firmware's layout) leaves out
    // untouched. Sized for the largest override (the fans' 150-byte
    // standalone blob); only ever called from a feature's start().
    uint8_t saved[256];
    if (len > sizeof saved)
        return false;

    size_t n = len;
    if (nvs_get_blob(nvs, baseKey, saved, &n) != ESP_OK || n != len)
        return false;
    if (memcmp(saved, base, len) != 0)
        return false; // the partition was re-flashed with other values since

    n = len;
    return nvs_get_blob(nvs, key, out, &n) == ESP_OK && n == len;
}

void save(nvs_handle_t nvs, const char* key, const char* baseKey, const uint8_t* value,
          const uint8_t* base, size_t len)
{
    nvs_set_blob(nvs, key, value, len);
    nvs_set_blob(nvs, baseKey, base, len);
    nvs_commit(nvs);
}
} // namespace cfgstore
