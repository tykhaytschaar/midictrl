#include "config.h"
#include "test_helpers.h"

#include <string.h>

int g_tests_total = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
bool g_current_test_failed = false;

static void test_defaults_two_banks_with_demo_alt_slot(void)
{
    config_t cfg;
    config_init_defaults(&cfg);
    TEST_ASSERT_EQ_INT(cfg.bank_count, 2);

    // Bank 0 slot 0 is the demo alt slot — primary PC 10 ch 1, alt PC 11 ch 1.
    const program_slot_t *demo = &cfg.banks[0].programs[0];
    TEST_ASSERT_EQ_INT(demo->primary.messages.count, 1);
    TEST_ASSERT_EQ_INT(demo->primary.messages.msgs[0].type, MIDI_PC);
    TEST_ASSERT_EQ_INT(demo->primary.messages.msgs[0].num, 10);
    TEST_ASSERT_EQ_INT(demo->primary.messages.msgs[0].ch, 1);
    TEST_ASSERT_TRUE(demo->has_alternative);
    TEST_ASSERT_EQ_INT(demo->alternative.messages.count, 1);
    TEST_ASSERT_EQ_INT(demo->alternative.messages.msgs[0].type, MIDI_PC);
    TEST_ASSERT_EQ_INT(demo->alternative.messages.msgs[0].num, 11);
    TEST_ASSERT_EQ_INT(demo->alternative.messages.msgs[0].ch, 1);

    // Every other slot in both banks is empty with no alt.
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < PROGRAMS_PER_BANK; i++) {
            if (b == 0 && i == 0) continue;  // the demo slot above
            TEST_ASSERT_EQ_INT(cfg.banks[b].programs[i].primary.messages.count, 0);
            TEST_ASSERT_FALSE(cfg.banks[b].programs[i].has_alternative);
            TEST_ASSERT_FALSE(cfg.banks[b].programs[i].primary.expression.present);
        }
        TEST_ASSERT_FALSE(cfg.banks[b].expression_default.present);
    }
}

static void test_defaults_global_values(void)
{
    config_t cfg;
    config_init_defaults(&cfg);
    TEST_ASSERT_EQ_INT(cfg.global.midi_channel, 1);
    TEST_ASSERT_EQ_INT(cfg.global.alt_toggle_behavior, ALT_TOGGLE_B);
    TEST_ASSERT_TRUE(cfg.global.boot_resume);
    TEST_ASSERT_EQ_INT(cfg.global.long_press_short_ms, 500);
    TEST_ASSERT_EQ_INT(cfg.global.long_press_long_ms, 1500);
    TEST_ASSERT_EQ_INT(cfg.global.browse_timeout_ms, 10000);
    TEST_ASSERT_EQ_INT(cfg.global.display_brightness, 200);
    TEST_ASSERT_EQ_INT(cfg.global.led_brightness, 128);
}

static void test_defaults_tuner_and_tap_messages(void)
{
    config_t cfg;
    config_init_defaults(&cfg);
    TEST_ASSERT_EQ_INT(cfg.global.tuner_message.type, MIDI_CC);
    TEST_ASSERT_EQ_INT(cfg.global.tuner_message.num, 68);
    TEST_ASSERT_EQ_INT(cfg.global.tuner_message.val, 127);
    TEST_ASSERT_EQ_INT(cfg.global.tuner_message.ch, 1);
    TEST_ASSERT_EQ_INT(cfg.global.tap_message.type, MIDI_CC);
    TEST_ASSERT_EQ_INT(cfg.global.tap_message.num, 64);
    TEST_ASSERT_EQ_INT(cfg.global.tap_message.val, 127);
    TEST_ASSERT_EQ_INT(cfg.global.tap_message.ch, 1);
}

static void test_defaults_user_functions_empty(void)
{
    config_t cfg;
    config_init_defaults(&cfg);
    TEST_ASSERT_EQ_INT(cfg.global.user_function_a.count, 0);
    TEST_ASSERT_EQ_INT(cfg.global.user_function_b.count, 0);
}

static void test_defaults_expression_calibration(void)
{
    config_t cfg;
    config_init_defaults(&cfg);
    TEST_ASSERT_EQ_INT(cfg.global.expression.adc_min, 200);
    TEST_ASSERT_EQ_INT(cfg.global.expression.adc_max, 3900);
    TEST_ASSERT_EQ_INT(cfg.global.expression.curve, EXPR_CURVE_LINEAR);
}

int main(void)
{
    RUN_TEST(test_defaults_two_banks_with_demo_alt_slot);
    RUN_TEST(test_defaults_global_values);
    RUN_TEST(test_defaults_tuner_and_tap_messages);
    RUN_TEST(test_defaults_user_functions_empty);
    RUN_TEST(test_defaults_expression_calibration);
    TEST_SUMMARY_AND_EXIT();
}
