#include "midi_out.h"

#include "driver/uart.h"
#include "esp_log.h"

#include <stdbool.h>

static const char *TAG = "midi_out";

#define MIDI_BAUD       31250
#define MIDI_BUF_BYTES  256

static uart_port_t s_port;
static bool        s_ready = false;

esp_err_t midi_out_init(int uart_port, int tx_gpio)
{
    s_port = (uart_port_t)uart_port;
    uart_config_t cfg = {
        .baud_rate  = MIDI_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(s_port, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(s_port, tx_gpio, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(s_port, MIDI_BUF_BYTES, MIDI_BUF_BYTES,
                              0, NULL, 0);
    if (err != ESP_OK) return err;
    s_ready = true;
    ESP_LOGI(TAG, "MIDI out up — UART%d TX on GPIO %d @ %d baud",
             (int)s_port, tx_gpio, MIDI_BAUD);
    return ESP_OK;
}

void midi_out_send(const midi_message_t *msg)
{
    if (!s_ready || !msg) return;

    // MIDI channel encoding: status byte's low nibble is (channel - 1).
    uint8_t ch = (msg->ch >= 1 && msg->ch <= 16) ? (uint8_t)(msg->ch - 1) : 0;
    uint8_t bytes[3];
    size_t  len = 0;
    switch (msg->type) {
    case MIDI_PC:
        bytes[0] = (uint8_t)(0xC0 | ch);
        bytes[1] = (uint8_t)(msg->num & 0x7F);
        len = 2;
        break;
    case MIDI_CC:
        bytes[0] = (uint8_t)(0xB0 | ch);
        bytes[1] = (uint8_t)(msg->num & 0x7F);
        bytes[2] = (uint8_t)(msg->val & 0x7F);
        len = 3;
        break;
    default:
        return;
    }
    uart_write_bytes(s_port, bytes, len);
}
