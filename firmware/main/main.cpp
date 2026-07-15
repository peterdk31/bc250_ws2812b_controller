// ESP-IDF receiver firmware (native — no Arduino, no arduino-cli). Replays
// pixel frames streamed from the host daemon over the serial link.
//
// The receiver renders no effects: the daemon records the power-on/shutdown
// animations to already-corrected pixel frames and streams them here
// (CMD_REC_*); this firmware stores them on LittleFS and replays them. So an
// effect tweak no longer means a reflash — it's picked up on the next daemon
// start and shown one power cycle later.
//
// app_main only brings up the substrate shared by every feature on this board
// — NVS flash and the LittleFS partition — and starts the LED service in its
// own task. The LED code lives in:
//   link.*         the serial link (UART0 on ESP32, USB Serial/JTAG on C3/...)
//   prefs.*        the NVS key store (the old Arduino Preferences)
//   render.*       strip output: led_strip device, crossfade blend, dither
//   rec_store.*    the boot/shutdown recordings on LittleFS
//   led_service.*  the protocol endpoint, baud hunt, host liveness, replay
//
// power_switch.* is the first such sibling feature: the ATX PS_ON# power
// button, its own task with its own NVS namespace, wiring read from the
// `pwrcfg` flash partition — the two services never touch.
// The wire protocol, framing, recording format/replay and crossfading are the
// shared common/ code the daemon also compiles (on the include path — see
// CMakeLists.txt).
//
// A new feature slots in beside the LED service: do its one-time bring-up
// here, run it as its own task, and let it own its state — if it needs to
// influence the LEDs, hand the LED task a message, don't reach into it. The
// LED task's timing is its quality: it latches the strip roughly every
// millisecond and its dither degrades with the cadence, which is why it runs
// at a raised priority and shares its loop with nothing.

#include "esp_littlefs.h"
#include "nvs_flash.h"

#include "led_service.hpp"
#include "power_switch.hpp"
#include "rec_store.hpp" // LFS_BASE

extern "C" void app_main(void)
{
    // NVS (backs prefs.* and the power switch). Re-init after erasing if the
    // partition is from an old layout or full.
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // the power switch first, before the (slower) filesystem mount: if this is
    // a reset that interrupted an asserted PS_ON#, the board's power is
    // floating on the PSU's pull-up until start() re-asserts it
    pwr::start();

    // LittleFS on the "storage" partition; format on first boot if unformatted
    esp_vfs_littlefs_conf_t lc = {};
    lc.base_path = LFS_BASE;
    lc.partition_label = "storage";
    lc.format_if_mount_failed = true;
    lc.dont_mount = false;
    esp_vfs_littlefs_register(&lc);

    led::start();

    // app_main returns; FreeRTOS keeps running the service tasks
}
