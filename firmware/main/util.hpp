#pragma once

#include <cstdint>

#include "esp_timer.h"

// millis() shim: 32-bit millisecond counter (wraps like Arduino's; every use
// in this firmware is unsigned `now - then` difference math, which is
// wrap-safe).
static inline uint32_t millis()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
