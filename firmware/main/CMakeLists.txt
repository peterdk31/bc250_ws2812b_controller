# One source file per module (see main.cpp for the map). The shared wire
# protocol / frame parser / recording / crossfade headers live in the repo's
# common/ dir (the same code the daemon compiles), so we put it on the include
# path directly rather than copying files into the build — includes stay
# path-less (#include "protocol.hpp"). led_strip and littlefs come in as
# managed components (idf_component.yml); the component manager adds them as
# requirements automatically, so they don't need listing here.
idf_component_register(
    SRCS "main.cpp" "link.cpp" "prefs.cpp" "render.cpp" "rec_store.cpp"
         "led_service.cpp" "power_switch.cpp" "dbglog.cpp" "hostreq.cpp"
         "fan.cpp" "ble.cpp"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
    REQUIRES driver nvs_flash esp_timer esp_adc esp_partition bt
)

# HOST_BAUD and HOST_TIMEOUT_MS are baked in so the receiver and daemon agree on
# the link (everything else — geometry, animations — arrives at runtime as
# recordings). The Makefile passes them as CMake cache vars:
#   idf.py -DHOST_BAUD=921600 -DHOST_TIMEOUT_MS=5000 build
# Each has a compiled-in fallback in led_service.cpp when unset.
if(DEFINED HOST_BAUD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HOST_BAUD=${HOST_BAUD})
endif()
if(DEFINED HOST_TIMEOUT_MS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HOST_TIMEOUT_MS=${HOST_TIMEOUT_MS})
endif()
