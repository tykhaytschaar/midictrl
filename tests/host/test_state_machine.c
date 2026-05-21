#include "config.h"
#include "state_machine.h"
#include "test_helpers.h"

#include <string.h>

int g_tests_total = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
bool g_current_test_failed = false;

// --- Harness state ---------------------------------------------------------

typedef struct {
    midi_message_t midi_log[256];
    int midi_count;
    int state_changed_count;
    uint32_t now_ms;
} harness_t;

static config_t s_cfg;
static harness_t s_h;

static void mock_send_midi(const midi_message_t *msg, void *ctx)
{
    harness_t *h = (harness_t *)ctx;
    if (h->midi_count < (int)(sizeof(h->midi_log) / sizeof(h->midi_log[0]))) {
        h->midi_log[h->midi_count++] = *msg;
    }
}

static void mock_state_changed(void *ctx)
{
    harness_t *h = (harness_t *)ctx;
    h->state_changed_count++;
}

static uint32_t mock_now_ms(void *ctx)
{
    harness_t *h = (harness_t *)ctx;
    return h->now_ms;
}

static void set_program(uint8_t bank, uint8_t slot, midi_msg_type_t type,
                       uint8_t num, uint8_t val, uint8_t ch)
{
    s_cfg.banks[bank].programs[slot].primary.messages.count = 1;
    s_cfg.banks[bank].programs[slot].primary.messages.msgs[0] =
        (midi_message_t){type, num, val, ch};
}

// Initialise a fresh fixture: 3 banks, with varied slot shapes in bank 0
// (slot 0 has an alt, slot 1 has no alt, slot 2 is empty, slot 3 has an
// alt, slot 4 has multiple messages). Banks 1 and 2 each have five simple
// single-message slots, so bank-switching tests can spot which bank ran.
static void init_config(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    s_cfg.global.midi_channel = 1;
    s_cfg.global.tuner_message = (midi_message_t){MIDI_CC, 68, 127, 1};
    s_cfg.global.tap_message   = (midi_message_t){MIDI_CC, 64, 127, 1};
    s_cfg.global.user_function_a.count = 1;
    s_cfg.global.user_function_a.msgs[0] = (midi_message_t){MIDI_CC, 90, 127, 1};
    s_cfg.global.user_function_b.count = 1;
    s_cfg.global.user_function_b.msgs[0] = (midi_message_t){MIDI_CC, 91, 127, 1};
    s_cfg.global.long_press_short_ms   = 500;
    s_cfg.global.long_press_long_ms    = 1500;
    s_cfg.global.browse_timeout_ms     = 10000;
    s_cfg.global.boot_resume           = true;
    s_cfg.global.display_brightness    = 200;
    s_cfg.global.led_brightness        = 128;
    s_cfg.global.expression.adc_min    = 200;
    s_cfg.global.expression.adc_max    = 3900;
    s_cfg.global.expression.curve      = EXPR_CURVE_LINEAR;
    s_cfg.global.expression.smoothing  = 0.2f;

    s_cfg.bank_count = 3;

    // Bank 0 — varied shapes for slot-selection / alt tests.
    set_program(0, 0, MIDI_PC, 10, 0, 1);   // slot 0 primary
    s_cfg.banks[0].programs[0].has_alternative = true;
    s_cfg.banks[0].programs[0].alternative.messages.count = 1;
    s_cfg.banks[0].programs[0].alternative.messages.msgs[0] =
        (midi_message_t){MIDI_PC, 11, 0, 1};

    set_program(0, 1, MIDI_PC, 20, 0, 1);   // slot 1 — no alt

    // slot 2 — empty (messages.count stays 0 from memset)

    set_program(0, 3, MIDI_PC, 30, 0, 1);   // slot 3 — has alt
    s_cfg.banks[0].programs[3].has_alternative = true;
    s_cfg.banks[0].programs[3].alternative.messages.count = 1;
    s_cfg.banks[0].programs[3].alternative.messages.msgs[0] =
        (midi_message_t){MIDI_PC, 31, 0, 1};

    // slot 4 — multiple messages
    s_cfg.banks[0].programs[4].primary.messages.count = 2;
    s_cfg.banks[0].programs[4].primary.messages.msgs[0] =
        (midi_message_t){MIDI_PC, 40, 0, 1};
    s_cfg.banks[0].programs[4].primary.messages.msgs[1] =
        (midi_message_t){MIDI_CC, 100, 64, 1};

    // Bank 1 — slots PC 50..54 (channel 2).
    for (uint8_t i = 0; i < 5; i++) {
        set_program(1, i, MIDI_PC, (uint8_t)(50 + i), 0, 2);
    }
    // Bank 2 — slots PC 60..64 (channel 3).
    for (uint8_t i = 0; i < 5; i++) {
        set_program(2, i, MIDI_PC, (uint8_t)(60 + i), 0, 3);
    }
}

static state_machine_t *make_sm(void)
{
    init_config();
    memset(&s_h, 0, sizeof(s_h));
    sm_callbacks_t cb = {
        .send_midi     = mock_send_midi,
        .state_changed = mock_state_changed,
        .now_ms        = mock_now_ms,
        .ctx           = &s_h,
    };
    return state_machine_create(&s_cfg, &cb);
}

static void press_short(state_machine_t *sm, sm_footswitch_t fs)
{
    sm_event_t e = { .type = SM_EVT_FOOTSWITCH };
    e.footswitch.fs    = fs;
    e.footswitch.press = SM_PRESS_SHORT;
    state_machine_dispatch(sm, &e);
}

static void press_long(state_machine_t *sm, sm_footswitch_t fs)
{
    sm_event_t e = { .type = SM_EVT_FOOTSWITCH };
    e.footswitch.fs    = fs;
    e.footswitch.press = SM_PRESS_LONG;
    state_machine_dispatch(sm, &e);
}

static void tick(state_machine_t *sm)
{
    sm_event_t e = { .type = SM_EVT_TICK };
    state_machine_dispatch(sm, &e);
}

// --- Tests -----------------------------------------------------------------

static void test_initial_state(void)
{
    state_machine_t *sm = make_sm();
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_bank, 0);
    TEST_ASSERT_EQ_INT(s.target_bank, 0);
    TEST_ASSERT_EQ_INT(s.current_slot, 0);
    TEST_ASSERT_FALSE(s.in_browse_mode);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);
    TEST_ASSERT_FALSE(s.tuner_on);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 0);
    state_machine_destroy(sm);
}

static void test_bank_up_enters_browse(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_BANK_UP);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_bank, 0);
    TEST_ASSERT_EQ_INT(s.target_bank, 1);
    TEST_ASSERT_TRUE(s.in_browse_mode);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 0);
    state_machine_destroy(sm);
}

static void test_bank_up_wraparound_cancels(void)
{
    state_machine_t *sm = make_sm();
    // Three Bank Up presses with bank_count = 3 should wrap target back to 0.
    press_short(sm, SM_FS_BANK_UP);
    press_short(sm, SM_FS_BANK_UP);
    press_short(sm, SM_FS_BANK_UP);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.target_bank, 0);
    TEST_ASSERT_FALSE(s.in_browse_mode);  // implicit cancel
    state_machine_destroy(sm);
}

static void test_bank_down_wraparound(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_BANK_DOWN);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.target_bank, 2);
    TEST_ASSERT_TRUE(s.in_browse_mode);
    state_machine_destroy(sm);
}

static void test_implicit_cancel(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_BANK_UP);
    press_short(sm, SM_FS_BANK_DOWN);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.target_bank, 0);
    TEST_ASSERT_FALSE(s.in_browse_mode);
    state_machine_destroy(sm);
}

static void test_browse_commit_emits_primary(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_BANK_UP);
    press_short(sm, SM_FS_PROG_1);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_bank, 1);
    TEST_ASSERT_EQ_INT(s.target_bank, 1);
    TEST_ASSERT_EQ_INT(s.current_slot, 0);
    TEST_ASSERT_FALSE(s.in_browse_mode);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 1);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 50);
    state_machine_destroy(sm);
}

static void test_browse_commit_selects_pressed_slot(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_BANK_UP);
    press_short(sm, SM_FS_PROG_3);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_bank, 1);
    TEST_ASSERT_EQ_INT(s.current_slot, 2);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 52);
    state_machine_destroy(sm);
}

static void test_browse_timeout_reverts(void)
{
    state_machine_t *sm = make_sm();
    s_h.now_ms = 1000;
    press_short(sm, SM_FS_BANK_UP);
    s_h.now_ms = 1000 + 9999;
    tick(sm);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_TRUE(s.in_browse_mode);
    s_h.now_ms = 1000 + 10000;
    tick(sm);
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_FALSE(s.in_browse_mode);
    TEST_ASSERT_EQ_INT(s.target_bank, s.current_bank);
    state_machine_destroy(sm);
}

static void test_browse_timeout_disabled(void)
{
    state_machine_t *sm = make_sm();
    s_cfg.global.browse_timeout_ms = 0;
    s_h.now_ms = 1000;
    press_short(sm, SM_FS_BANK_UP);
    s_h.now_ms = 1000 + 100000;
    tick(sm);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_TRUE(s.in_browse_mode);
    state_machine_destroy(sm);
}

static void test_same_slot_alt_toggle(void)
{
    state_machine_t *sm = make_sm();
    // From boot, current_slot=0 with primary; first Prog_1 press is a
    // same-slot toggle, swinging to alt.
    press_short(sm, SM_FS_PROG_1);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_TRUE(s.current_slot_alt_active);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 1);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 11);

    press_short(sm, SM_FS_PROG_1);
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 2);
    TEST_ASSERT_EQ_INT(s_h.midi_log[1].num, 10);
    state_machine_destroy(sm);
}

static void test_different_slot_select(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_PROG_2);  // slot 1, primary, no alt
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 1);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 1);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 20);
    state_machine_destroy(sm);
}

static void test_same_slot_no_alt_noop(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_PROG_2);  // select slot 1 (no alt)
    int before = s_h.midi_count;
    press_short(sm, SM_FS_PROG_2);  // same slot, no alt → no-op
    TEST_ASSERT_EQ_INT(s_h.midi_count, before);
    state_machine_destroy(sm);
}

static void test_one_time_alt_clears_on_slot_leave(void)
{
    state_machine_t *sm = make_sm();
    s_cfg.banks[0].programs[0].alt_persistence = ALT_PERSIST_ONE_TIME;
    press_short(sm, SM_FS_PROG_1);  // toggle slot 0 to alt
    press_short(sm, SM_FS_PROG_4);  // leave slot 0 → one_time clears its alt state
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 3);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);  // slot 3 starts primary
    press_short(sm, SM_FS_PROG_1);  // back to slot 0 — original (primary), since alt was cleared
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 0);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);
    state_machine_destroy(sm);
}

static void test_permanent_alt_preserves_per_slot(void)
{
    state_machine_t *sm = make_sm();
    // Permanent is the fixture default (zero-init = ALT_PERSIST_PERMANENT).
    press_short(sm, SM_FS_PROG_1);  // toggle slot 0 to alt
    press_short(sm, SM_FS_PROG_4);  // leave slot 0 → permanent keeps its alt state
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 3);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);
    press_short(sm, SM_FS_PROG_1);  // back to slot 0 — alt should be restored
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 0);
    TEST_ASSERT_TRUE(s.current_slot_alt_active);
    state_machine_destroy(sm);
}

// Two slots in the same bank with different persistence settings; each respects its own.
static void test_mixed_persistence_per_slot(void)
{
    state_machine_t *sm = make_sm();
    s_cfg.banks[0].programs[0].alt_persistence = ALT_PERSIST_ONE_TIME;
    s_cfg.banks[0].programs[3].alt_persistence = ALT_PERSIST_PERMANENT;

    press_short(sm, SM_FS_PROG_1);  // slot 0 → alt
    press_short(sm, SM_FS_PROG_4);  // leave slot 0 (one_time → cleared); slot 3 = primary
    press_short(sm, SM_FS_PROG_4);  // slot 3 → alt
    press_short(sm, SM_FS_PROG_1);  // leave slot 3 (permanent → kept); slot 0 = primary (was cleared)

    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 0);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);  // slot 0 stayed primary

    press_short(sm, SM_FS_PROG_4);  // return to slot 3 — alt should be restored
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 3);
    TEST_ASSERT_TRUE(s.current_slot_alt_active);
    state_machine_destroy(sm);
}

static void test_bank_commit_clears_alt(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_PROG_1);  // slot 0 → alt
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_TRUE(s.current_slot_alt_active);
    press_short(sm, SM_FS_BANK_UP);
    press_short(sm, SM_FS_PROG_1);  // commit to bank 1, slot 0
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_bank, 1);
    TEST_ASSERT_FALSE(s.current_slot_alt_active);
    state_machine_destroy(sm);
}

static void test_tap_short_emits_tap_message(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_TAP);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 1);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].type, MIDI_CC);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 64);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].val, 127);
    state_machine_destroy(sm);
}

static void test_tap_long_toggles_tuner(void)
{
    state_machine_t *sm = make_sm();
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_FALSE(s.tuner_on);
    press_long(sm, SM_FS_TAP);
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_TRUE(s.tuner_on);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 1);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 68);
    press_long(sm, SM_FS_TAP);
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_FALSE(s.tuner_on);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 2);
    state_machine_destroy(sm);
}

static void test_bank_long_emits_user_function(void)
{
    state_machine_t *sm = make_sm();
    press_long(sm, SM_FS_BANK_DOWN);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 1);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 90);
    press_long(sm, SM_FS_BANK_UP);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 2);
    TEST_ASSERT_EQ_INT(s_h.midi_log[1].num, 91);
    state_machine_destroy(sm);
}

static void test_empty_slot_no_midi(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_PROG_3);  // select slot 2 (empty)
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.current_slot, 2);
    TEST_ASSERT_TRUE(s.current_slot_empty);
    TEST_ASSERT_EQ_INT(s_h.midi_count, 0);
    state_machine_destroy(sm);
}

static void test_browse_mode_leds(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_BANK_UP);
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_TRUE(s.in_browse_mode);
    TEST_ASSERT_EQ_INT(s.slot_leds[0], SM_SLOT_LED_BROWSE_ACTIVE_PRIMARY);
    for (int i = 1; i < PROGRAMS_PER_BANK; i++) {
        TEST_ASSERT_EQ_INT(s.slot_leds[i], SM_SLOT_LED_BROWSE_INACTIVE);
    }
    state_machine_destroy(sm);
}

static void test_normal_mode_leds(void)
{
    state_machine_t *sm = make_sm();
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.slot_leds[0], SM_SLOT_LED_ACTIVE_PRIMARY);
    for (int i = 1; i < PROGRAMS_PER_BANK; i++) {
        TEST_ASSERT_EQ_INT(s.slot_leds[i], SM_SLOT_LED_INACTIVE);
    }
    state_machine_destroy(sm);
}

static void test_alt_active_led_color(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_PROG_1);  // toggle slot 0 to alt
    sm_snapshot_t s;
    state_machine_snapshot(sm, &s);
    TEST_ASSERT_EQ_INT(s.slot_leds[0], SM_SLOT_LED_ACTIVE_ALT);
    state_machine_destroy(sm);
}

static void test_slot_messages_emitted_in_order(void)
{
    state_machine_t *sm = make_sm();
    press_short(sm, SM_FS_PROG_5);  // slot 4 — two messages
    TEST_ASSERT_EQ_INT(s_h.midi_count, 2);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].type, MIDI_PC);
    TEST_ASSERT_EQ_INT(s_h.midi_log[0].num, 40);
    TEST_ASSERT_EQ_INT(s_h.midi_log[1].type, MIDI_CC);
    TEST_ASSERT_EQ_INT(s_h.midi_log[1].num, 100);
    TEST_ASSERT_EQ_INT(s_h.midi_log[1].val, 64);
    state_machine_destroy(sm);
}

// --- main ------------------------------------------------------------------

int main(void)
{
    RUN_TEST(test_initial_state);
    RUN_TEST(test_bank_up_enters_browse);
    RUN_TEST(test_bank_up_wraparound_cancels);
    RUN_TEST(test_bank_down_wraparound);
    RUN_TEST(test_implicit_cancel);
    RUN_TEST(test_browse_commit_emits_primary);
    RUN_TEST(test_browse_commit_selects_pressed_slot);
    RUN_TEST(test_browse_timeout_reverts);
    RUN_TEST(test_browse_timeout_disabled);
    RUN_TEST(test_same_slot_alt_toggle);
    RUN_TEST(test_different_slot_select);
    RUN_TEST(test_same_slot_no_alt_noop);
    RUN_TEST(test_one_time_alt_clears_on_slot_leave);
    RUN_TEST(test_permanent_alt_preserves_per_slot);
    RUN_TEST(test_mixed_persistence_per_slot);
    RUN_TEST(test_bank_commit_clears_alt);
    RUN_TEST(test_tap_short_emits_tap_message);
    RUN_TEST(test_tap_long_toggles_tuner);
    RUN_TEST(test_bank_long_emits_user_function);
    RUN_TEST(test_empty_slot_no_midi);
    RUN_TEST(test_browse_mode_leds);
    RUN_TEST(test_normal_mode_leds);
    RUN_TEST(test_alt_active_led_color);
    RUN_TEST(test_slot_messages_emitted_in_order);
    TEST_SUMMARY_AND_EXIT();
}
