#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "config_store.h"
#include "midi_out.h"
#include "state_machine.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "midictrl";

static state_machine_t *g_sm = NULL;
static web_server_t *g_web = NULL;

// StateMachine callbacks forward to the web_server broadcasts so any
// connected browser sees live MIDI emission and state changes. Also
// logs to the serial console for debugging when no browser is connected.

static void sm_send_midi_cb(const midi_message_t *msg, void *ctx)
{
    (void)ctx;
    if (msg->type == MIDI_CC) {
        ESP_LOGI(TAG, "MIDI -> CC num=%u val=%u ch=%u", msg->num, msg->val, msg->ch);
    } else {
        ESP_LOGI(TAG, "MIDI -> PC num=%u ch=%u", msg->num, msg->ch);
    }
    midi_out_send(msg);
    if (g_web) web_server_broadcast_midi(g_web, msg);
}

static void sm_state_changed_cb(void *ctx)
{
    (void)ctx;
    sm_snapshot_t s;
    state_machine_snapshot(g_sm, &s);
    ESP_LOGI(TAG, "SM bank=%u target=%u slot=%u browse=%d alt=%d tuner=%d",
             s.current_bank, s.target_bank, s.current_slot,
             (int)s.in_browse_mode, (int)s.current_slot_alt_active,
             (int)s.tuner_on);
    if (g_web) web_server_broadcast_state(g_web);
}

static uint32_t sm_now_ms_cb(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void sm_tick_task(void *arg)
{
    (void)arg;
    sm_event_t e = { .type = SM_EVT_TICK };
    while (true) {
        if (g_sm) state_machine_dispatch(g_sm, &e);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// --- WiFi state breadcrumb --------------------------------------------------

static const char *wifi_state_str(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_STA_CONNECTING: return "STA_CONNECTING";
    case WIFI_MGR_STA_CONNECTED:  return "STA_CONNECTED";
    case WIFI_MGR_AP:             return "AP";
    default:                      return "OFF";
    }
}

static void wifi_log_task(void *arg)
{
    wifi_manager_t *wm = (wifi_manager_t *)arg;
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
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- app_main ---------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "MIDI footswitch boot — skeleton");

    esp_err_t err = midi_out_init(BOARD_MIDI_UART_NUM, BOARD_MIDI_TX_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "midi_out_init failed: %s", esp_err_to_name(err));
    }

    config_store_t *cs = config_store_init();
    if (!cs) {
        ESP_LOGE(TAG, "config_store_init failed; aborting");
        return;
    }
    const config_t *cfg = config_store_get(cs);
    ESP_LOGI(TAG, "config: %u bank(s), midi_channel=%u, alt_toggle=%s",
             cfg->bank_count, cfg->global.midi_channel,
             cfg->global.alt_toggle_behavior == ALT_TOGGLE_A ? "ALT_A" : "ALT_B");

    sm_callbacks_t sm_cb = {
        .send_midi     = sm_send_midi_cb,
        .state_changed = sm_state_changed_cb,
        .now_ms        = sm_now_ms_cb,
        .ctx           = NULL,
    };
    g_sm = state_machine_create(config_store_get(cs), &sm_cb);
    if (!g_sm) {
        ESP_LOGE(TAG, "state_machine_create failed");
    } else {
        xTaskCreate(sm_tick_task, "sm_tick", 2048, NULL, 4, NULL);
    }

    wifi_manager_t *wm = wifi_manager_init();
    if (!wm) {
        ESP_LOGE(TAG, "wifi_manager_init failed");
    } else {
        xTaskCreate(wifi_log_task, "wifi_log", 2048, wm, 3, NULL);
    }

    web_server_deps_t deps = { .cs = cs, .sm = g_sm, .wm = wm };
    g_web = web_server_start(&deps);
    if (!g_web) {
        ESP_LOGE(TAG, "web_server_start failed");
    }
}
