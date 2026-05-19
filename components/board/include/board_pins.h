#pragma once

// Pulls in the right per-target pinout. Consumers include only this header.
#if CONFIG_IDF_TARGET_ESP32
#include "board_pins_esp32.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "board_pins_esp32s3.h"
#else
#error "Unsupported IDF target — only esp32 and esp32s3 are wired up here."
#endif
