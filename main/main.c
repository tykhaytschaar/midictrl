#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

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
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 5, NULL);
}
