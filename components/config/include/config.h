#pragma once

// In-memory representation of the configuration described in spec section 6.
// Pure C structs, no IDF dependencies — the same header is consumed by the
// firmware (idf build) and by the host-side StateMachine unit tests.

#include <stdbool.h>
#include <stdint.h>

#define PROGRAMS_PER_BANK       5
#define MAX_BANKS               32
#define MAX_MIDI_MSGS_PER_LIST  8
#define MAX_NAME_LEN            32

typedef enum {
    MIDI_PC = 0,  // Program Change — uses `num`, ignores `val`
    MIDI_CC = 1,  // Control Change — uses `num` and `val`
} midi_msg_type_t;

typedef struct {
    midi_msg_type_t type;
    uint8_t num;   // PC: program number (0–127); CC: controller number (0–127)
    uint8_t val;   // CC: value (0–127); unused for PC
    uint8_t ch;    // MIDI channel (1–16)
} midi_message_t;

typedef struct {
    midi_message_t msgs[MAX_MIDI_MSGS_PER_LIST];
    uint8_t count;
} midi_message_list_t;

typedef struct {
    bool present;   // false = inherit / no override
    uint8_t cc;     // controller number (0–127)
    uint8_t ch;     // MIDI channel (1–16)
} expression_target_t;

// Shared shape for a program's primary payload and its alternative.
typedef struct {
    char name[MAX_NAME_LEN];
    midi_message_list_t messages;     // .count==0 means "empty slot" (no MIDI on activation)
    expression_target_t expression;   // .present==false means "inherit bank default"
} program_payload_t;

typedef struct {
    program_payload_t primary;        // the slot itself
    bool has_alternative;
    program_payload_t alternative;    // only valid when has_alternative
} program_slot_t;

typedef struct {
    char name[MAX_NAME_LEN];
    expression_target_t expression_default;  // .present==false means "no bank default"
    program_slot_t programs[PROGRAMS_PER_BANK];
} bank_t;

typedef enum {
    ALT_TOGGLE_A = 0,
    ALT_TOGGLE_B = 1,
} alt_toggle_behavior_t;

typedef enum {
    EXPR_CURVE_LINEAR = 0,
    EXPR_CURVE_LOG    = 1,
    EXPR_CURVE_EXP    = 2,
} expression_curve_t;

typedef struct {
    uint16_t adc_min;
    uint16_t adc_max;
    expression_curve_t curve;
    float smoothing;
} expression_config_t;

typedef struct {
    uint8_t midi_channel;
    midi_message_t tuner_message;
    midi_message_t tap_message;
    midi_message_list_t user_function_a;   // .count==0 means "no-op"
    midi_message_list_t user_function_b;
    uint32_t long_press_short_ms;
    uint32_t long_press_long_ms;
    uint32_t browse_timeout_ms;            // 0 disables the timeout
    alt_toggle_behavior_t alt_toggle_behavior;
    bool boot_resume;
    uint8_t display_brightness;            // 0–255
    uint8_t led_brightness;                // 0–255
    expression_config_t expression;
} global_config_t;

typedef struct {
    global_config_t global;
    bank_t banks[MAX_BANKS];
    uint8_t bank_count;
} config_t;
