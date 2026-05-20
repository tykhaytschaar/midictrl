#pragma once

// Owns the on-flash configuration: mounts the LittleFS storage partition,
// loads /storage/config.json (or writes defaults if missing / corrupt), and
// exposes the in-RAM `config_t` for the rest of the firmware to read and —
// from the web UI editor — mutate, then save.
//
// Single-threaded for now: callers must not mutate while another task is
// reading. A mutex / RCU-style snapshot will be added when the web server
// task lands.

#include "config.h"
#include "esp_err.h"

typedef struct config_store config_store_t;

// Mount LittleFS and load the configuration. On a missing or corrupt file,
// writes the factory defaults back to flash and returns a store holding
// those defaults. Returns NULL only on hard infrastructure failure (NVS or
// LittleFS itself refusing to come up).
config_store_t *config_store_init(void);

void config_store_destroy(config_store_t *cs);

// In-place access to the canonical configuration. Returned pointer stays
// valid for the lifetime of the store.
config_t *config_store_get(config_store_t *cs);

// Serialise the in-RAM config to JSON and write to LittleFS, atomically
// where possible (write to a tmp path, then rename).
esp_err_t config_store_save(const config_store_t *cs);

// Render the in-memory config as a JSON string. Caller frees with free().
// Returns NULL on allocation failure.
char *config_store_to_json_alloc(const config_store_t *cs);

// Replace the in-memory config from a JSON string. Does not touch flash;
// call config_store_save() to persist. Falls back to defaults on parse
// failure (so an invalid POST never leaves the device with no banks).
esp_err_t config_store_from_json(config_store_t *cs, const char *json_str);
