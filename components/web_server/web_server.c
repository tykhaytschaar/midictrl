#include "web_server.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

struct web_server {
    httpd_handle_t handle;
    web_server_deps_t deps;
};

// Single global for handler -> deps wiring. esp_http_server lets us stash
// a `user_ctx` per URI handler, which we use; the global is only here for
// the destroy path.
static web_server_t *s_ws = NULL;

// ===== Handlers ============================================================

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

static const char *wifi_state_name(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_STA_CONNECTING: return "STA_CONNECTING";
    case WIFI_MGR_STA_CONNECTED:  return "STA_CONNECTED";
    case WIFI_MGR_AP:             return "AP";
    default:                      return "OFF";
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    web_server_deps_t *deps = (web_server_deps_t *)req->user_ctx;

    sm_snapshot_t snap;
    state_machine_snapshot(deps->sm, &snap);
    const config_t *cfg = config_store_get(deps->cs);

    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "state", wifi_state_name(wifi_manager_state(deps->wm)));
    cJSON_AddStringToObject(wifi, "ssid",  wifi_manager_ssid(deps->wm));
    cJSON_AddStringToObject(wifi, "ip",    wifi_manager_ip(deps->wm));

    cJSON *sm = cJSON_AddObjectToObject(root, "sm");
    cJSON_AddNumberToObject(sm, "current_bank", snap.current_bank);
    cJSON_AddNumberToObject(sm, "target_bank",  snap.target_bank);
    cJSON_AddNumberToObject(sm, "current_slot", snap.current_slot);
    cJSON_AddBoolToObject(sm,   "in_browse_mode", snap.in_browse_mode);
    cJSON_AddBoolToObject(sm,   "current_slot_alt_active", snap.current_slot_alt_active);
    cJSON_AddBoolToObject(sm,   "current_slot_empty", snap.current_slot_empty);
    cJSON_AddBoolToObject(sm,   "tuner_on", snap.tuner_on);

    cJSON_AddNumberToObject(root, "bank_count", cfg->bank_count);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(str);
    return err;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    web_server_deps_t *deps = (web_server_deps_t *)req->user_ctx;
    char *str = config_store_to_json_alloc(deps->cs);
    if (!str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return err;
}

#define MAX_POST_BYTES (64 * 1024)

static esp_err_t config_post_handler(httpd_req_t *req)
{
    web_server_deps_t *deps = (web_server_deps_t *)req->user_ctx;
    if (req->content_len > MAX_POST_BYTES) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "config too large");
        return ESP_FAIL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int read = 0;
    while (read < (int)req->content_len) {
        int chunk = httpd_req_recv(req, buf + read, req->content_len - read);
        if (chunk <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            return ESP_FAIL;
        }
        read += chunk;
    }
    buf[req->content_len] = '\0';

    esp_err_t err = config_store_from_json(deps->cs, buf);
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config json");
        return ESP_FAIL;
    }
    err = config_store_save(deps->cs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"saved\":true}", HTTPD_RESP_USE_STRLEN);
}

// ===== Lifecycle ===========================================================

web_server_t *web_server_start(const web_server_deps_t *deps)
{
    if (!deps || !deps->cs || !deps->sm || !deps->wm) return NULL;

    web_server_t *ws = calloc(1, sizeof(*ws));
    if (!ws) return NULL;
    ws->deps = *deps;

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.max_uri_handlers = 8;
    conf.stack_size = 8192;
    conf.lru_purge_enable = true;

    if (httpd_start(&ws->handle, &conf) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        free(ws);
        return NULL;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_handler,       .user_ctx = &ws->deps },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_handler,      .user_ctx = &ws->deps },
        { .uri = "/api/config",  .method = HTTP_GET,  .handler = config_get_handler,  .user_ctx = &ws->deps },
        { .uri = "/api/config",  .method = HTTP_POST, .handler = config_post_handler, .user_ctx = &ws->deps },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(ws->handle, &routes[i]);
    }
    ESP_LOGI(TAG, "HTTP server up — GET /, /api/status, /api/config | POST /api/config");
    s_ws = ws;
    return ws;
}

void web_server_stop(web_server_t *ws)
{
    if (!ws) return;
    if (ws->handle) httpd_stop(ws->handle);
    if (s_ws == ws) s_ws = NULL;
    free(ws);
}
