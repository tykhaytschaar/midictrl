#include "input_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "input_manager";

#define INPUT_MGR_MAX_BUTTONS   16
// FreeRTOS default tick is 10 ms — anything shorter rounds to 0 in
// pdMS_TO_TICKS and the vTaskDelay turns into a bare yield, which
// starves IDLE0 and trips the task watchdog. Poll one tick at a time
// and use a 2-sample debounce so the response window stays ~20 ms.
#define POLL_INTERVAL_MS        10
#define DEBOUNCE_SAMPLES        2

typedef struct {
    input_manager_button_t cfg;

    // Debounce: count consecutive raw samples in the candidate state.
    bool     candidate_pressed;
    uint8_t  raw_count;
    // Confirmed (post-debounce) state.
    bool     debounced_pressed;
    uint32_t press_start_ms;
    bool     long_press_fired;
} button_state_t;

static button_state_t   s_buttons[INPUT_MGR_MAX_BUTTONS];
static size_t           s_button_count = 0;
static input_manager_cb_t s_cb = NULL;
static void              *s_cb_ctx = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void poll_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t t = now_ms();
        for (size_t i = 0; i < s_button_count; i++) {
            button_state_t *b = &s_buttons[i];

            // Active low: GPIO low = pressed.
            bool raw_pressed = (gpio_get_level((gpio_num_t)b->cfg.gpio) == 0);

            if (raw_pressed == b->candidate_pressed) {
                if (b->raw_count < DEBOUNCE_SAMPLES) b->raw_count++;
            } else {
                b->candidate_pressed = raw_pressed;
                b->raw_count = 1;
            }

            if (b->raw_count >= DEBOUNCE_SAMPLES &&
                b->candidate_pressed != b->debounced_pressed) {
                b->debounced_pressed = b->candidate_pressed;
                if (b->debounced_pressed) {
                    // Newly pressed.
                    b->press_start_ms   = t;
                    b->long_press_fired = false;
                } else {
                    // Released. If we never fired a long-press, this was a short press.
                    if (!b->long_press_fired && s_cb) {
                        sm_event_t e = { .type = SM_EVT_FOOTSWITCH };
                        e.footswitch.fs    = b->cfg.fs;
                        e.footswitch.press = SM_PRESS_SHORT;
                        s_cb(&e, s_cb_ctx);
                    }
                }
            }

            // Fire long-press while still held, exactly once per press.
            if (b->debounced_pressed && !b->long_press_fired &&
                (t - b->press_start_ms) >= b->cfg.long_press_threshold_ms) {
                if (s_cb) {
                    sm_event_t e = { .type = SM_EVT_FOOTSWITCH };
                    e.footswitch.fs    = b->cfg.fs;
                    e.footswitch.press = SM_PRESS_LONG;
                    s_cb(&e, s_cb_ctx);
                }
                b->long_press_fired = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t input_manager_init(const input_manager_button_t *buttons,
                              size_t count,
                              input_manager_cb_t cb,
                              void *ctx)
{
    if (!buttons || !cb || count == 0) return ESP_ERR_INVALID_ARG;
    if (count > INPUT_MGR_MAX_BUTTONS) return ESP_ERR_INVALID_SIZE;

    memset(s_buttons, 0, sizeof(s_buttons));
    s_button_count = count;
    s_cb     = cb;
    s_cb_ctx = ctx;

    uint64_t pull_up_mask  = 0;
    uint64_t no_pull_mask  = 0;
    for (size_t i = 0; i < count; i++) {
        s_buttons[i].cfg = buttons[i];
        s_buttons[i].candidate_pressed = false;
        s_buttons[i].raw_count = 0;
        s_buttons[i].debounced_pressed = false;
        s_buttons[i].long_press_fired = false;
        uint64_t bit = 1ULL << buttons[i].gpio;
        if (buttons[i].has_internal_pullup) {
            pull_up_mask |= bit;
        } else {
            no_pull_mask |= bit;
        }
    }

    if (pull_up_mask) {
        gpio_config_t c = {
            .pin_bit_mask = pull_up_mask,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&c));
    }
    if (no_pull_mask) {
        gpio_config_t c = {
            .pin_bit_mask = no_pull_mask,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&c));
    }

    BaseType_t ok = xTaskCreate(poll_task, "input_poll", 3072, NULL, 6, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create poll task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "input_manager up — polling %u button(s) every %d ms",
             (unsigned)count, POLL_INTERVAL_MS);
    return ESP_OK;
}
