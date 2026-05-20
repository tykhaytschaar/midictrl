#pragma once

// First-boot / no-credentials path brings up an open AP named
// "midifoot-XXXX" (XXXX from the MAC) and registers an mDNS hostname.
// If saved credentials exist in NVS, STA is attempted first; an STA
// failure within WIFI_MGR_STA_TIMEOUT_MS falls back to AP. Non-blocking:
// init returns immediately, transitions happen on the internal task.

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WIFI_MGR_OFF = 0,
    WIFI_MGR_STA_CONNECTING,
    WIFI_MGR_STA_CONNECTED,
    WIFI_MGR_AP,
} wifi_mgr_state_t;

typedef struct wifi_manager wifi_manager_t;

wifi_manager_t *wifi_manager_init(void);
void wifi_manager_destroy(wifi_manager_t *mgr);

wifi_mgr_state_t wifi_manager_state(const wifi_manager_t *mgr);

// Stable references — only valid while wifi_manager_t lives, mutated
// internally on state transitions. Empty string for "n/a" rather than NULL.
const char *wifi_manager_ssid(const wifi_manager_t *mgr);
const char *wifi_manager_ip(const wifi_manager_t *mgr);

// Synchronous SSID scan. On entry *count is the array capacity; on
// return it's the number of records actually written. Scanning requires
// the STA radio, so the manager will transition to APSTA mode if it was
// AP-only.
typedef struct {
    char    ssid[33];        // 32 chars + NUL
    int8_t  rssi;
    bool    needs_password;  // false for WIFI_AUTH_OPEN
} wifi_scan_result_t;

esp_err_t wifi_manager_scan(wifi_manager_t *mgr, wifi_scan_result_t *out, size_t *count);

// Persist new STA credentials to NVS. The currently running WiFi state
// is not affected — call esp_restart() (e.g. from the web layer after
// the HTTP response has been flushed) to apply.
esp_err_t wifi_manager_set_sta_credentials(wifi_manager_t *mgr,
                                            const char *ssid, const char *pass);
