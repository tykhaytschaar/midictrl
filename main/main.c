#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config_store.h"
#include "wifi_manager.h"

static const char *TAG = "midictrl";

static const char *wifi_state_str(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_STA_CONNECTING: return "STA_CONNECTING";
    case WIFI_MGR_STA_CONNECTED:  return "STA_CONNECTED";
    case WIFI_MGR_AP:             return "AP";
    default:                      return "OFF";
    }
}

static void status_task(void *arg)
{
    wifi_manager_t *wm = (wifi_manager_t *)arg;
    int beat = 0;
    wifi_mgr_state_t last = WIFI_MGR_OFF;
    while (true) {
        wifi_mgr_state_t s = wifi_manager_state(wm);
        if (s != last) {
            ESP_LOGI(TAG, "wifi -> %s (ssid=%s ip=%s)",
                     wifi_state_str(s),
                     wifi_manager_ssid(wm),
                     wifi_manager_ip(wm));
            last = s;
        }
        if ((beat++ % 10) == 0) {
            ESP_LOGI(TAG, "heartbeat (wifi=%s)", wifi_state_str(s));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
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

    wifi_manager_t *wm = wifi_manager_init();
    if (!wm) {
        ESP_LOGE(TAG, "wifi_manager_init failed");
    }

    xTaskCreate(status_task, "status", 3072, wm, 4, NULL);
}
