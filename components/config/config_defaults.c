#include "config.h"

#include <string.h>

void config_init_defaults(config_t *out)
{
    memset(out, 0, sizeof(*out));

    // Globals — values from the example in spec section 6.
    out->global.midi_channel = 1;
    out->global.tuner_message = (midi_message_t){MIDI_CC, 68, 127, 1};
    out->global.tap_message   = (midi_message_t){MIDI_CC, 64, 127, 1};
    // user_function_a / _b stay .count=0 → no-op long press
    out->global.long_press_short_ms = 500;
    out->global.long_press_long_ms  = 1500;
    out->global.browse_timeout_ms   = 10000;
    out->global.alt_toggle_behavior = ALT_TOGGLE_B;
    out->global.boot_resume         = true;
    out->global.display_brightness  = 200;
    out->global.led_brightness      = 128;
    out->global.expression.adc_min   = 200;
    out->global.expression.adc_max   = 3900;
    out->global.expression.curve     = EXPR_CURVE_LINEAR;
    out->global.expression.smoothing = 0.2f;

    // One bank, five empty slots (spec section 7.3 step 1).
    out->bank_count = 1;
    // The bank name stays empty — the UI substitutes a placeholder like "Bank 1"
    // when name[0] == '\0', per spec section 6 constraints.
}
