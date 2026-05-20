#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi_manager";

#define WIFI_NVS_NS              "wifi"
#define WIFI_NVS_KEY_SSID        "ssid"
#define WIFI_NVS_KEY_PASS        "pass"
#define WIFI_MGR_STA_TIMEOUT_MS  20000
#define WIFI_MGR_MAX_STA_TRIES   5     // give up + fall back to AP after this many disconnects
#define MDNS_HOSTNAME            "midifoot"
#define MDNS_INSTANCE            "MIDI Footswitch"
#define AP_SSID_PREFIX           "midifoot-"
#define AP_MAX_CONNECTIONS       4

#define STA_GOT_IP_BIT       BIT0
#define STA_DISCONNECTED_BIT BIT1

struct wifi_manager {
    wifi_mgr_state_t state;
    EventGroupHandle_t events;
    esp_netif_t *netif_sta;
    esp_netif_t *netif_ap;
    char ssid[33];     // 32 + NUL
    char ip[16];       // dotted-quad
    bool mdns_started;
    int sta_attempts;
};

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_manager_t *mgr = (wifi_manager_t *)arg;
    if (base != WIFI_EVENT) return;

    switch (id) {
    case WIFI_EVENT_STA_START:
        mgr->sta_attempts = 1;
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (mgr->sta_attempts < WIFI_MGR_MAX_STA_TRIES) {
            mgr->sta_attempts++;
            ESP_LOGW(TAG, "STA disconnected — retry %d/%d",
                     mgr->sta_attempts, WIFI_MGR_MAX_STA_TRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA gave up after %d attempts — falling back to AP",
                     WIFI_MGR_MAX_STA_TRIES);
            xEventGroupSetBits(mgr->events, STA_DISCONNECTED_BIT);
        }
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "client connected: " MACSTR, MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "client disconnected: " MACSTR, MAC2STR(e->mac));
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_manager_t *mgr = (wifi_manager_t *)arg;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(mgr->ip, sizeof(mgr->ip), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "STA got IP %s", mgr->ip);
    // Reset the retry budget so a runtime disconnect later gets a full
    // round of reconnect attempts before any AP fallback signal.
    mgr->sta_attempts = 0;
    xEventGroupSetBits(mgr->events, STA_GOT_IP_BIT);
}

static bool load_credentials(char *ssid_out, size_t ssid_len,
                              char *pass_out, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t s_len = ssid_len, p_len = pass_len;
    esp_err_t err_s = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid_out, &s_len);
    esp_err_t err_p = nvs_get_str(h, WIFI_NVS_KEY_PASS, pass_out, &p_len);
    nvs_close(h);
    return (err_s == ESP_OK && err_p == ESP_OK && ssid_out[0] != '\0');
}

static void start_mdns_if_needed(wifi_manager_t *mgr)
{
    if (mgr->mdns_started) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed");
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(MDNS_INSTANCE);
    mgr->mdns_started = true;
    ESP_LOGI(TAG, "mDNS up — http://" MDNS_HOSTNAME ".local");
}

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  // accept any, will negotiate
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

static void apply_ap_config(wifi_manager_t *mgr)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(mgr->ssid, sizeof(mgr->ssid), AP_SSID_PREFIX "%02X%02X",
             mac[4], mac[5]);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, mgr->ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen(mgr->ssid);
    cfg.ap.max_connection = AP_MAX_CONNECTIONS;
    cfg.ap.authmode = WIFI_AUTH_OPEN;  // captive-portal style, open join
    cfg.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(mgr->netif_ap, &ip_info) == ESP_OK) {
        snprintf(mgr->ip, sizeof(mgr->ip), IPSTR, IP2STR(&ip_info.ip));
    }
    ESP_LOGI(TAG, "AP mode: SSID=%s, ip=%s (open)", mgr->ssid, mgr->ip);
}

static void controller_task(void *arg)
{
    wifi_manager_t *mgr = (wifi_manager_t *)arg;

    char ssid[33] = {0}, pass[65] = {0};
    bool have_creds = load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (have_creds) {
        ESP_LOGI(TAG, "STA: connecting to %s", ssid);
        strncpy(mgr->ssid, ssid, sizeof(mgr->ssid) - 1);
        mgr->state = WIFI_MGR_STA_CONNECTING;
        apply_sta_config(ssid, pass);
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(
            mgr->events,
            STA_GOT_IP_BIT | STA_DISCONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_MGR_STA_TIMEOUT_MS));

        if (bits & STA_GOT_IP_BIT) {
            mgr->state = WIFI_MGR_STA_CONNECTED;
            start_mdns_if_needed(mgr);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "STA didn't connect within %d ms — falling back to AP",
                 WIFI_MGR_STA_TIMEOUT_MS);
        esp_wifi_stop();
        mgr->state = WIFI_MGR_OFF;
    } else {
        ESP_LOGI(TAG, "no STA credentials in NVS — AP mode");
    }

    apply_ap_config(mgr);
    ESP_ERROR_CHECK(esp_wifi_start());
    mgr->state = WIFI_MGR_AP;
    start_mdns_if_needed(mgr);
    vTaskDelete(NULL);
}

wifi_manager_t *wifi_manager_init(void)
{
    wifi_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->events = xEventGroupCreate();
    if (!mgr->events) {
        free(mgr);
        return NULL;
    }
    strcpy(mgr->ip, "");

    ESP_ERROR_CHECK(esp_netif_init());
    // ConfigStore initialised NVS already; an event loop may also be needed
    // by other components later, but at minimum WiFi requires one.
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    mgr->netif_sta = esp_netif_create_default_wifi_sta();
    mgr->netif_ap  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, mgr, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, mgr, NULL));

    if (xTaskCreate(controller_task, "wifi_ctrl", 4096, mgr, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create controller task");
        free(mgr);
        return NULL;
    }
    return mgr;
}

void wifi_manager_destroy(wifi_manager_t *mgr)
{
    if (!mgr) return;
    if (mgr->mdns_started) mdns_free();
    esp_wifi_stop();
    esp_wifi_deinit();
    vEventGroupDelete(mgr->events);
    free(mgr);
}

wifi_mgr_state_t wifi_manager_state(const wifi_manager_t *mgr) { return mgr->state; }
const char *wifi_manager_ssid(const wifi_manager_t *mgr) { return mgr->ssid; }
const char *wifi_manager_ip(const wifi_manager_t *mgr) { return mgr->ip; }

esp_err_t wifi_manager_scan(wifi_manager_t *mgr, wifi_scan_result_t *out, size_t *count)
{
    if (!mgr || !out || !count) return ESP_ERR_INVALID_ARG;

    // Scanning needs the STA radio. We're typically in AP-only mode here;
    // flip to APSTA so we don't drop already-connected clients.
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
    }

    wifi_scan_config_t cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);  // blocking
    if (err != ESP_OK) return err;

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    uint16_t to_copy = (found > *count) ? (uint16_t)*count : found;

    if (to_copy == 0) {
        *count = 0;
        return ESP_OK;
    }
    wifi_ap_record_t *records = calloc(to_copy, sizeof(*records));
    if (!records) return ESP_ERR_NO_MEM;
    uint16_t actual = to_copy;
    err = esp_wifi_scan_get_ap_records(&actual, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }
    for (uint16_t i = 0; i < actual; i++) {
        strncpy(out[i].ssid, (const char *)records[i].ssid, sizeof(out[i].ssid) - 1);
        out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
        out[i].rssi = records[i].rssi;
        out[i].needs_password = (records[i].authmode != WIFI_AUTH_OPEN);
    }
    free(records);
    *count = actual;
    return ESP_OK;
}

esp_err_t wifi_manager_set_sta_credentials(wifi_manager_t *mgr,
                                            const char *ssid, const char *pass)
{
    (void)mgr;
    if (!ssid) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, WIFI_NVS_KEY_PASS, pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
