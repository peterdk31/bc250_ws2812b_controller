#include "link.hpp"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32
#define LINK_UART 1
#include "driver/uart.h"
#else
#define LINK_USB_SERIAL_JTAG 1
#include "driver/usb_serial_jtag.h"
#endif

namespace link
{
#if LINK_UART
static const uart_port_t PORT = UART_NUM_0;

void begin(uint32_t baud)
{
    uart_config_t cfg = {};
    cfg.baud_rate = (int)baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // a generous RX ring so a recording upload (many frames back-to-back)
    // can't outrun us while we copy each into RAM (mirrors the old
    // Serial.setRxBufferSize(1024)). We never transmit, so no TX buffer.
    uart_driver_install(PORT, 2048, 0, 0, nullptr, 0);
    uart_param_config(PORT, &cfg);
    // UART0's default pins are already wired to the USB-serial bridge; keep them.
}

int read(uint8_t* buf, size_t maxlen, TickType_t wait)
{
    int n = uart_read_bytes(PORT, buf, maxlen, wait);
    return n < 0 ? 0 : n;
}

void setBaud(uint32_t baud) { uart_set_baudrate(PORT, baud); }
void flushInput() { uart_flush_input(PORT); }

#else // USB Serial/JTAG (C3/C6/H2/S3 native USB)

void begin(uint32_t /*baud*/)
{
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 256;  // we never really transmit, but 0 is rejected
    cfg.rx_buffer_size = 1024; // matches the old Serial.setRxBufferSize(1024)
    usb_serial_jtag_driver_install(&cfg);
}

int read(uint8_t* buf, size_t maxlen, TickType_t wait)
{
    int n = usb_serial_jtag_read_bytes(buf, maxlen, wait);
    return n < 0 ? 0 : n;
}

void setBaud(uint32_t /*baud*/) {} // USB CDC: line rate is set by the host
void flushInput()
{
    uint8_t sink[64];
    while (usb_serial_jtag_read_bytes(sink, sizeof sink, 0) > 0)
    {
    }
}
#endif

bool hostPresent()
{
#if LINK_UART
    return true;
#else
    return usb_serial_jtag_is_connected();
#endif
}
} // namespace link
