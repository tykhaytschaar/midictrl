#include "config.h"

#include <string.h>

static void copy_name(char *dst, const char *src)
{
    strncpy(dst, src, MAX_NAME_LEN - 1);
    dst[MAX_NAME_LEN - 1] = '\0';
}

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

    // Two banks. The first slot of bank 0 is pre-populated with a demo
    // primary + alt so alt toggling can be exercised from the browser
    // before the in-browser bank editor lands. Every other slot stays
    // empty (messages.count == 0). Spec 7.3 step 1 nominally asks for one
    // empty bank — once the editor lets users add banks themselves the
    // defaults can shrink back.
    out->bank_count = 2;

    program_slot_t *demo = &out->banks[0].programs[0];
    copy_name(demo->primary.name, "Demo");
    demo->primary.messages.count = 1;
    demo->primary.messages.msgs[0] = (midi_message_t){MIDI_PC, 10, 0, 1};
    demo->has_alternative = true;
    copy_name(demo->alternative.name, "Demo ALT");
    demo->alternative.messages.count = 1;
    demo->alternative.messages.msgs[0] = (midi_message_t){MIDI_PC, 11, 0, 1};

    // The bank names stay empty — the UI substitutes a placeholder like
    // "Bank 1" / "Bank 2" when name[0] == '\0', per spec section 6 constraints.
}
