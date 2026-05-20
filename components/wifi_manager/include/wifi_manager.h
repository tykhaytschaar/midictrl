#pragma once

// First-boot / no-credentials path brings up an open AP named
// "midifoot-XXXX" (XXXX from the MAC) and registers an mDNS hostname.
// If saved credentials exist in NVS, STA is attempted first; an STA
// failure within WIFI_MGR_STA_TIMEOUT_MS falls back to AP. Non-blocking:
// init returns immediately, transitions happen on the internal task.

#include "esp_err.h"

#include <stdbool.h>

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
