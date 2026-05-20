#pragma once

// Hardware MIDI 1.0 transmitter. Wraps an ESP-IDF UART driver in a tiny
// API the StateMachine's send_midi callback can call from any task.
// Configuration (UART unit + TX GPIO) is passed in at init time so this
// component stays board-agnostic — main.c picks the values from
// board_pins.h.

#include "config.h"      // for midi_message_t
#include "esp_err.h"

#include <stdint.h>

// 31250 baud, 8N1, hardware MIDI standard. UART RX, RTS, CTS are unused
// — pass a 1-stop, no-parity TX-only line out of the chip.
esp_err_t midi_out_init(int uart_port, int tx_gpio);

// Encode and transmit a single MIDI message. Safe to call from any task —
// uart_write_bytes is internally serialised by the driver. Drops the call
// silently if midi_out_init has not run.
void midi_out_send(const midi_message_t *msg);
