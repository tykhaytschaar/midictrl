#pragma once

// ESP32 WROOM-32 (MVP) pinout. See spec section 1.2.1 for the design rationale,
// strap/flash pin exclusions, and the external pull-ups required on the
// input-only footswitch pins.

// Footswitches — active low, internal pull-up enabled in code.
#define BOARD_FS_BANK_DOWN_GPIO   4
#define BOARD_FS_BANK_UP_GPIO     13
#define BOARD_FS_PROG1_GPIO       14
#define BOARD_FS_PROG2_GPIO       16
#define BOARD_FS_PROG3_GPIO       33

// Footswitches — active low. Input-only pins; require an external
// 10 kohm pull-up to 3V3 on each line.
#define BOARD_FS_PROG4_GPIO       34
#define BOARD_FS_PROG5_GPIO       35
#define BOARD_FS_TAP_GPIO         36

// Rotary encoder — active low, internal pull-up.
#define BOARD_ENC_A_GPIO          26
#define BOARD_ENC_B_GPIO          27
#define BOARD_ENC_BTN_GPIO        32

// WS2812 RGB chain — RMT TX channel.
#define BOARD_WS2812_GPIO         25

// MIDI out — UART1 TX, remapped from the default GPIO 10 (which is on flash).
#define BOARD_MIDI_TX_GPIO        17
#define BOARD_MIDI_UART_NUM       1

// TFT (ST7796) on SPI3_HOST (VSPI). MISO is not wired — ST7796 is write-only.
#define BOARD_TFT_SPI_HOST_ID     3
#define BOARD_TFT_SCK_GPIO        18
#define BOARD_TFT_MOSI_GPIO       23
#define BOARD_TFT_CS_GPIO         5
#define BOARD_TFT_DC_GPIO         21
#define BOARD_TFT_RST_GPIO        22
#define BOARD_TFT_BL_GPIO         19

// Expression pedal — ADC1, 12-bit. GPIO 39 = ADC1_CH3 on the classic ESP32.
#define BOARD_EXPR_ADC_UNIT_ID    1
#define BOARD_EXPR_ADC_CHANNEL    3
