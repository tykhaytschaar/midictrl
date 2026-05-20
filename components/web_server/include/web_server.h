#pragma once

// HTTP / REST front-end. Wraps esp_http_server, serves the embedded
// browser UI, and exposes /api/status and /api/config endpoints backed by
// the other components.

#include "config_store.h"
#include "esp_err.h"
#include "state_machine.h"
#include "wifi_manager.h"

typedef struct web_server web_server_t;

typedef struct {
    config_store_t *cs;
    state_machine_t *sm;
    wifi_manager_t *wm;
} web_server_deps_t;

web_server_t *web_server_start(const web_server_deps_t *deps);
void web_server_stop(web_server_t *ws);

// Push helpers — call from SM callbacks (or anywhere). Both serialise
// the payload to JSON and fan it out to every connected websocket
// client. Safe to call from any task; dead sockets are reaped lazily.
void web_server_broadcast_state(web_server_t *ws);
void web_server_broadcast_midi(web_server_t *ws, const midi_message_t *msg);
