#pragma once

// GPIO-driven footswitch reader. Polls each configured pin every ~10 ms,
// debounces, and produces SM_PRESS_SHORT / SM_PRESS_LONG events through
// the supplied callback — the caller forwards them to the StateMachine.
//
// Active-low buttons: GPIO → switch → GND, with the internal pull-up
// enabled when the pin supports one. For input-only ESP32 pins (34–39)
// the board has to supply an external pull-up; this module just polls.

#include "esp_err.h"
#include "state_machine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int             gpio;
    sm_footswitch_t fs;
    bool            has_internal_pullup;
    uint32_t        long_press_threshold_ms;
} input_manager_button_t;

typedef void (*input_manager_cb_t)(const sm_event_t *evt, void *ctx);

esp_err_t input_manager_init(const input_manager_button_t *buttons,
                              size_t count,
                              input_manager_cb_t cb,
                              void *ctx);
