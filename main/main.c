#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "config_store.h"
#include "input_manager.h"
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
    // Logging moved to input_to_sm so each button press shows up paired
    // with the state delta it caused (or "no change"). Browse-mode
    // timeouts still go through here silently — the web broadcast tells
    // the browser; no need to spam the UART log.
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

static const char *fs_name(sm_footswitch_t fs)
{
    switch (fs) {
    case SM_FS_BANK_DOWN: return "BANK_DOWN";
    case SM_FS_BANK_UP:   return "BANK_UP";
    case SM_FS_PROG_1:    return "PROG_1";
    case SM_FS_PROG_2:    return "PROG_2";
    case SM_FS_PROG_3:    return "PROG_3";
    case SM_FS_PROG_4:    return "PROG_4";
    case SM_FS_PROG_5:    return "PROG_5";
    case SM_FS_TAP:       return "TAP";
    default:              return "?";
    }
}

static bool snapshot_equal(const sm_snapshot_t *a, const sm_snapshot_t *b)
{
    return a->current_bank == b->current_bank
        && a->target_bank == b->target_bank
        && a->current_slot == b->current_slot
        && a->in_browse_mode == b->in_browse_mode
        && a->current_slot_alt_active == b->current_slot_alt_active
        && a->tuner_on == b->tuner_on;
}

// Bridge for input_manager_cb_t (sm_event_t *, void *) to the SM's
// dispatch entry point. Logs every footswitch press alongside its
// effect on the SM snapshot: the visible-state line if it changed,
// "no change" if not, "not in use" for program slots above
// PROGRAMS_PER_BANK that the SM silently rejects anyway.
static void input_to_sm(const sm_event_t *evt, void *ctx)
{
    state_machine_t *sm = (state_machine_t *)ctx;

    if (evt->type != SM_EVT_FOOTSWITCH) {
        state_machine_dispatch(sm, evt);
        return;
    }

    const char *name = fs_name(evt->footswitch.fs);
    const char *kind = (evt->footswitch.press == SM_PRESS_LONG) ? "LP" : "SP";

    if (evt->footswitch.fs >= SM_FS_PROG_1 && evt->footswitch.fs <= SM_FS_PROG_5) {
        uint8_t slot_idx = (uint8_t)(evt->footswitch.fs - SM_FS_PROG_1);
        if (slot_idx >= PROGRAMS_PER_BANK) {
            ESP_LOGI(TAG, "%s %s: not in use (PROGRAMS_PER_BANK=%d)",
                     name, kind, PROGRAMS_PER_BANK);
            return;
        }
    }

    sm_snapshot_t before, after;
    state_machine_snapshot(sm, &before);
    state_machine_dispatch(sm, evt);
    state_machine_snapshot(sm, &after);

    if (snapshot_equal(&before, &after)) {
        ESP_LOGI(TAG, "%s %s: no change", name, kind);
    } else {
        ESP_LOGI(TAG, "%s %s: bank=%u target=%u slot=%u browse=%d alt=%d tuner=%d",
                 name, kind,
                 after.current_bank, after.target_bank, after.current_slot,
                 (int)after.in_browse_mode, (int)after.current_slot_alt_active,
                 (int)after.tuner_on);
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
    ESP_LOGI(TAG, "config: %u bank(s), midi_channel=%u",
             cfg->bank_count, cfg->global.midi_channel);

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

    // Long-press thresholds. spec 2.6 has the tap LP at the SHORT timing
    // (tuner trigger), while spec 2.2 has bank LPs at the LONG timing
    // (user functions). Program LPs use LONG too — the SM ignores them
    // for now (spec 2.2 "future hook").
    const uint32_t lps = cfg->global.long_press_short_ms;
    const uint32_t lpl = cfg->global.long_press_long_ms;
    const input_manager_button_t buttons[] = {
        {BOARD_FS_BANK_DOWN_GPIO, SM_FS_BANK_DOWN, true,  lpl},
        {BOARD_FS_BANK_UP_GPIO,   SM_FS_BANK_UP,   true,  lpl},
        {BOARD_FS_PROG1_GPIO,     SM_FS_PROG_1,    true,  lpl},
        {BOARD_FS_PROG2_GPIO,     SM_FS_PROG_2,    true,  lpl},
        {BOARD_FS_PROG3_GPIO,     SM_FS_PROG_3,    true,  lpl},
        {BOARD_FS_PROG4_GPIO,     SM_FS_PROG_4,    false, lpl},  // input-only, needs external 10k pull-up
        {BOARD_FS_PROG5_GPIO,     SM_FS_PROG_5,    false, lpl},  // input-only, needs external 10k pull-up
        {BOARD_FS_TAP_GPIO,       SM_FS_TAP,       false, lps},  // input-only, needs external 10k pull-up; tuner timing
    };
    input_manager_init(buttons,
                       sizeof(buttons) / sizeof(buttons[0]),
                       input_to_sm,
                       g_sm);

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
