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

    // The RX ring must ride out flash stalls: a recording upload is appended
    // to LittleFS frame by frame as it streams in (rec_store::saveFrame), and
    // a page program or block erase freezes this task for tens of ms with the
    // host still sending — the driver drops bytes once the ring is full, and
    // every drop costs upload frames (the stream then fails its integrity
    // hash at END and nothing is stored). At the host's paced ~100 KB/s
    // (SerialSink spaces CMD_REC_FRAMEs), 32 KB buys ~300 ms of stall.
    // We never transmit, so no TX buffer.
    uart_driver_install(PORT, 32768, 0, 0, nullptr, 0);
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

void write(const uint8_t* buf, size_t len)
{
    // no TX ring was installed (see begin); this blocks only until the bytes
    // are in the FIFO, which for a short log frame is trivial
    uart_write_bytes(PORT, buf, len);
}

#else // USB Serial/JTAG (C3/C6/H2/S3 native USB)

void begin(uint32_t /*baud*/)
{
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 256; // we never really transmit, but 0 is rejected

    // The RX ring must ride out flash stalls: a recording upload is appended
    // to LittleFS frame by frame as it streams in (rec_store::saveFrame), and
    // a page program or block erase freezes this task for tens of ms with the
    // host still sending. USB gives no relief here — the driver's ISR (IDF
    // v5.3.1) unconditionally drains the 64-byte hardware FIFO and silently
    // discards what a full ring buffer won't take, so there's no NAK
    // backpressure and every overflow costs upload frames (the stream then
    // fails its integrity hash at END and nothing is stored). At the host's
    // paced ~100 KB/s (SerialSink spaces CMD_REC_FRAMEs), 32 KB buys ~300 ms
    // of stall. The RAM is what the MAX_LEDS 2048->100 drop reclaimed.
    cfg.rx_buffer_size = 32768;
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

void write(const uint8_t* buf, size_t len)
{
    // bounded: if no host is draining and the TX buffer fills, drop the rest
    // rather than stall the caller (the log line is debug, not load-bearing)
    usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(20));
}
#endif

bool rateSelectable()
{
#if LINK_UART
    return true;
#else
    return false;
#endif
}

bool hostPresent()
{
#if LINK_UART
    return true;
#else
    return usb_serial_jtag_is_connected();
#endif
}
} // namespace link
