#pragma once

// The single source of truth for current/target bank, slot selection, alt
// toggle state, browse mode, and tuner state. Pure logic — no IDF or
// peripheral dependencies. Side effects flow out through callbacks so the
// same module compiles inside the firmware and inside macOS-side unit tests.

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

typedef enum {
    SM_FS_BANK_DOWN = 0,
    SM_FS_BANK_UP   = 1,
    SM_FS_PROG_1    = 2,
    SM_FS_PROG_2    = 3,
    SM_FS_PROG_3    = 4,
    SM_FS_PROG_4    = 5,
    SM_FS_PROG_5    = 6,
    SM_FS_TAP       = 7,
    SM_FS_COUNT     = 8,
} sm_footswitch_t;

typedef enum {
    SM_PRESS_SHORT = 0,
    SM_PRESS_LONG  = 1,
} sm_press_kind_t;

typedef enum {
    SM_EVT_FOOTSWITCH,
    SM_EVT_TICK,         // periodic check used for browse-mode timeout
    SM_EVT_EXPRESSION,   // value already mapped to MIDI (0–127) by ExpressionPedal
} sm_event_type_t;

typedef struct {
    sm_event_type_t type;
    union {
        struct {
            sm_footswitch_t fs;
            sm_press_kind_t press;
        } footswitch;
        struct {
            uint8_t value;  // 0–127
        } expression;
    };
} sm_event_t;

typedef struct {
    // Emit a MIDI message. Called synchronously from state_machine_dispatch.
    void (*send_midi)(const midi_message_t *msg, void *ctx);
    // Visible state has changed — UI / LED / display should re-read the snapshot.
    void (*state_changed)(void *ctx);
    // Monotonic clock in milliseconds — used for browse-mode timeout.
    uint32_t (*now_ms)(void *ctx);
    void *ctx;
} sm_callbacks_t;

typedef struct state_machine state_machine_t;

// Allocates and initialises a state machine instance. `cfg` and `cb` must
// outlive the returned machine. Returns NULL on allocation failure.
state_machine_t *state_machine_create(const config_t *cfg, const sm_callbacks_t *cb);
void state_machine_destroy(state_machine_t *sm);

// Dispatch a single event. All callbacks fire synchronously before this returns.
void state_machine_dispatch(state_machine_t *sm, const sm_event_t *evt);

// Pre-computed LED state per program slot (spec sections 2.3 + 5).
typedef enum {
    SM_SLOT_LED_INACTIVE,                // dim red, steady
    SM_SLOT_LED_ACTIVE_PRIMARY,          // green, steady
    SM_SLOT_LED_ACTIVE_ALT,              // blue, steady
    SM_SLOT_LED_BROWSE_INACTIVE,         // red, blinking
    SM_SLOT_LED_BROWSE_ACTIVE_PRIMARY,   // green, blinking
    SM_SLOT_LED_BROWSE_ACTIVE_ALT,       // blue, blinking
} sm_slot_led_t;

typedef struct {
    uint8_t current_bank;
    uint8_t target_bank;
    uint8_t current_slot;
    bool in_browse_mode;
    bool current_slot_alt_active;
    bool current_slot_empty;
    bool tuner_on;
    sm_slot_led_t slot_leds[PROGRAMS_PER_BANK];
} sm_snapshot_t;

void state_machine_snapshot(const state_machine_t *sm, sm_snapshot_t *out);
