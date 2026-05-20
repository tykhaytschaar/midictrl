#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config_store.h"

static const char *TAG = "midictrl";

static void heartbeat_task(void *arg)
{
    int beat = 0;
    while (true) {
        ESP_LOGI(TAG, "heartbeat %d", beat++);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MIDI footswitch boot — skeleton");

    config_store_t *cs = config_store_init();
    if (cs) {
        const config_t *cfg = config_store_get(cs);
        ESP_LOGI(TAG, "config: %u bank(s), midi_channel=%u, alt_toggle=%s",
                 cfg->bank_count, cfg->global.midi_channel,
                 cfg->global.alt_toggle_behavior == ALT_TOGGLE_A ? "ALT_A" : "ALT_B");
    } else {
        ESP_LOGE(TAG, "config_store_init failed — running without persistence");
    }

    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 5, NULL);
}
