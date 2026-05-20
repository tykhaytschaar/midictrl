#include "web_server.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define MAX_WS_CLIENTS 4

typedef struct {
    int sockfd;
    bool is_writer;
    bool active;
} ws_client_t;

struct web_server {
    httpd_handle_t handle;
    web_server_deps_t deps;
    ws_client_t clients[MAX_WS_CLIENTS];
    SemaphoreHandle_t clients_mutex;
};

// One server instance per app. Stored here so SM callbacks can broadcast
// without needing the pointer threaded through every layer.
static web_server_t *s_ws = NULL;

// ===== WS client tracking ===================================================

static int find_slot(web_server_t *ws, int sockfd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws->clients[i].active && ws->clients[i].sockfd == sockfd) return i;
    }
    return -1;
}

static int find_free_slot(web_server_t *ws)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!ws->clients[i].active) return i;
    }
    return -1;
}

static bool any_writer(web_server_t *ws)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws->clients[i].active && ws->clients[i].is_writer) return true;
    }
    return false;
}

static esp_err_t send_text_to(web_server_t *ws, int sockfd, const char *text)
{
    httpd_ws_frame_t f = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len     = strlen(text),
    };
    return httpd_ws_send_frame_async(ws->handle, sockfd, &f);
}

static void send_role(web_server_t *ws, int sockfd, bool writer)
{
    const char *msg = writer
        ? "{\"type\":\"role\",\"role\":\"writer\"}"
        : "{\"type\":\"role\",\"role\":\"visitor\"}";
    send_text_to(ws, sockfd, msg);
}

static void broadcast_text(web_server_t *ws, const char *text)
{
    int dead[MAX_WS_CLIENTS];
    int n_dead = 0;
    xSemaphoreTake(ws->clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!ws->clients[i].active) continue;
        if (send_text_to(ws, ws->clients[i].sockfd, text) != ESP_OK) {
            dead[n_dead++] = i;
        }
    }
    for (int i = 0; i < n_dead; i++) {
        ws->clients[dead[i]].active = false;
    }
    // If a writer dropped, promote the lowest-index active client.
    bool need_promote = !any_writer(ws);
    int promoted_fd = -1;
    if (need_promote) {
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (ws->clients[i].active) {
                ws->clients[i].is_writer = true;
                promoted_fd = ws->clients[i].sockfd;
                break;
            }
        }
    }
    xSemaphoreGive(ws->clients_mutex);
    if (promoted_fd >= 0) {
        ESP_LOGI(TAG, "WS client %d promoted to writer", promoted_fd);
        send_role(ws, promoted_fd, true);
    }
}

static void register_client(web_server_t *ws, int sockfd)
{
    bool became_writer = false;
    bool registered = false;
    xSemaphoreTake(ws->clients_mutex, portMAX_DELAY);
    if (find_slot(ws, sockfd) < 0) {
        int slot = find_free_slot(ws);
        if (slot >= 0) {
            bool make_writer = !any_writer(ws);
            ws->clients[slot] = (ws_client_t){.sockfd = sockfd, .is_writer = make_writer, .active = true};
            became_writer = make_writer;
            registered = true;
        }
    }
    xSemaphoreGive(ws->clients_mutex);
    if (!registered) {
        ESP_LOGW(TAG, "WS client %d rejected — %d slots full", sockfd, MAX_WS_CLIENTS);
        return;
    }
    ESP_LOGI(TAG, "WS client %d registered as %s", sockfd, became_writer ? "writer" : "visitor");
    send_role(ws, sockfd, became_writer);
    // Push an initial state snapshot so the new client renders immediately.
    web_server_broadcast_state(ws);
}

static bool is_writer(web_server_t *ws, int sockfd)
{
    bool w = false;
    xSemaphoreTake(ws->clients_mutex, portMAX_DELAY);
    int slot = find_slot(ws, sockfd);
    if (slot >= 0) w = ws->clients[slot].is_writer;
    xSemaphoreGive(ws->clients_mutex);
    return w;
}

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

#define WIFI_SCAN_MAX 24

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    web_server_deps_t *deps = (web_server_deps_t *)req->user_ctx;
    wifi_scan_result_t *results = calloc(WIFI_SCAN_MAX, sizeof(*results));
    if (!results) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t count = WIFI_SCAN_MAX;
    esp_err_t err = wifi_manager_scan(deps->wm, results, &count);
    if (err != ESP_OK) {
        free(results);
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < count; i++) {
        if (results[i].ssid[0] == '\0') continue;  // skip hidden APs
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", results[i].rssi);
        cJSON_AddBoolToObject(e, "needs_password", results[i].needs_password);
        cJSON_AddItemToArray(arr, e);
    }
    free(results);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t sret = httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(str);
    return sret;
}

static void deferred_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "restarting to apply new WiFi credentials");
    esp_restart();
}

static esp_err_t wifi_sta_post_handler(httpd_req_t *req)
{
    web_server_deps_t *deps = (web_server_deps_t *)req->user_ctx;
    if (req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "wifi payload too large");
        return ESP_FAIL;
    }
    char buf[260] = {0};
    int read = 0;
    while (read < (int)req->content_len) {
        int chunk = httpd_req_recv(req, buf + read, (int)req->content_len - read);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            return ESP_FAIL;
        }
        read += chunk;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    const char *pass_str = (cJSON_IsString(pass)) ? pass->valuestring : "";
    esp_err_t err = wifi_manager_set_sta_credentials(deps->wm, ssid->valuestring, pass_str);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi creds save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"saved\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(deferred_restart_task, "restart", 2048, NULL, 1, NULL);
    return ESP_OK;
}

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
    // Keep the StateMachine coherent with the new config (current_bank /
    // current_slot may now be out of range) and let every connected WS
    // client know the live config has changed so editors can re-fetch.
    state_machine_resync(deps->sm);
    if (s_ws) {
        broadcast_text(s_ws, "{\"type\":\"config_changed\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"saved\":true}", HTTPD_RESP_USE_STRLEN);
}

// ===== WebSocket handler ===================================================

static struct {
    const char *id;
    sm_footswitch_t fs;
} FS_TABLE[] = {
    {"bank_down", SM_FS_BANK_DOWN},
    {"bank_up",   SM_FS_BANK_UP},
    {"prog1",     SM_FS_PROG_1},
    {"prog2",     SM_FS_PROG_2},
    {"prog3",     SM_FS_PROG_3},
    {"prog4",     SM_FS_PROG_4},
    {"prog5",     SM_FS_PROG_5},
    {"tap",       SM_FS_TAP},
};

static bool footswitch_id_to_enum(const char *id, sm_footswitch_t *out)
{
    for (size_t i = 0; i < sizeof(FS_TABLE) / sizeof(FS_TABLE[0]); i++) {
        if (strcmp(id, FS_TABLE[i].id) == 0) {
            *out = FS_TABLE[i].fs;
            return true;
        }
    }
    return false;
}

static void handle_input_event(web_server_t *ws, const cJSON *evt)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(evt, "type");
    if (!cJSON_IsString(type)) return;

    if (strcmp(type->valuestring, "fs") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(evt, "id");
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(evt, "kind");
        if (!cJSON_IsString(id) || !cJSON_IsString(kind)) return;
        sm_footswitch_t fs;
        if (!footswitch_id_to_enum(id->valuestring, &fs)) return;
        sm_event_t e = { .type = SM_EVT_FOOTSWITCH };
        e.footswitch.fs = fs;
        e.footswitch.press = (strcmp(kind->valuestring, "long") == 0) ? SM_PRESS_LONG : SM_PRESS_SHORT;
        state_machine_dispatch(ws->deps.sm, &e);
    } else if (strcmp(type->valuestring, "exp") == 0) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(evt, "value");
        if (!cJSON_IsNumber(value)) return;
        sm_event_t e = { .type = SM_EVT_EXPRESSION };
        e.expression.value = (uint8_t)value->valueint;
        state_machine_dispatch(ws->deps.sm, &e);
    }
    // Encoder events arrive when the encoder is implemented.
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    web_server_t *ws = (web_server_t *)req->user_ctx;

    if (req->method == HTTP_GET) {
        // Initial upgrade handshake.
        register_client(ws, httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;

    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0 || frame.len > 4096) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (!is_writer(ws, sockfd)) {
        // Visitor — input ignored silently.
        free(buf);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse((const char *)buf);
    free(buf);
    if (root) {
        handle_input_event(ws, root);
        cJSON_Delete(root);
    }
    return ESP_OK;
}

// ===== Lifecycle ===========================================================

web_server_t *web_server_start(const web_server_deps_t *deps)
{
    if (!deps || !deps->cs || !deps->sm || !deps->wm) return NULL;

    web_server_t *ws = calloc(1, sizeof(*ws));
    if (!ws) return NULL;
    ws->deps = *deps;
    ws->clients_mutex = xSemaphoreCreateMutex();
    if (!ws->clients_mutex) {
        free(ws);
        return NULL;
    }

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.max_uri_handlers = 8;
    conf.stack_size = 8192;
    conf.lru_purge_enable = true;

    if (httpd_start(&ws->handle, &conf) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        vSemaphoreDelete(ws->clients_mutex);
        free(ws);
        return NULL;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_handler,       .user_ctx = &ws->deps },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_handler,      .user_ctx = &ws->deps },
        { .uri = "/api/config",    .method = HTTP_GET,  .handler = config_get_handler,  .user_ctx = &ws->deps },
        { .uri = "/api/config",    .method = HTTP_POST, .handler = config_post_handler, .user_ctx = &ws->deps },
        { .uri = "/api/wifi/scan", .method = HTTP_GET,  .handler = wifi_scan_handler,   .user_ctx = &ws->deps },
        { .uri = "/api/wifi/sta",  .method = HTTP_POST, .handler = wifi_sta_post_handler, .user_ctx = &ws->deps },
        { .uri = "/ws",            .method = HTTP_GET,  .handler = ws_handler,          .user_ctx = ws,          .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(ws->handle, &routes[i]);
    }
    ESP_LOGI(TAG, "HTTP server up — GET /, /api/{status,config,wifi/scan}, POST /api/{config,wifi/sta}, WS /ws");
    s_ws = ws;
    return ws;
}

void web_server_stop(web_server_t *ws)
{
    if (!ws) return;
    if (ws->handle) httpd_stop(ws->handle);
    if (ws->clients_mutex) vSemaphoreDelete(ws->clients_mutex);
    if (s_ws == ws) s_ws = NULL;
    free(ws);
}

// ===== Public broadcasts ====================================================

static const char *led_to_str(sm_slot_led_t led)
{
    switch (led) {
    case SM_SLOT_LED_INACTIVE:                return "inactive";
    case SM_SLOT_LED_ACTIVE_PRIMARY:          return "active_primary";
    case SM_SLOT_LED_ACTIVE_ALT:              return "active_alt";
    case SM_SLOT_LED_BROWSE_INACTIVE:         return "browse_inactive";
    case SM_SLOT_LED_BROWSE_ACTIVE_PRIMARY:   return "browse_active_primary";
    case SM_SLOT_LED_BROWSE_ACTIVE_ALT:       return "browse_active_alt";
    default:                                  return "?";
    }
}

void web_server_broadcast_state(web_server_t *ws)
{
    if (!ws || !ws->deps.sm) return;
    sm_snapshot_t s;
    state_machine_snapshot(ws->deps.sm, &s);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");
    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON_AddNumberToObject(data, "current_bank", s.current_bank);
    cJSON_AddNumberToObject(data, "target_bank", s.target_bank);
    cJSON_AddNumberToObject(data, "current_slot", s.current_slot);
    cJSON_AddBoolToObject(data,   "in_browse_mode", s.in_browse_mode);
    cJSON_AddBoolToObject(data,   "current_slot_alt_active", s.current_slot_alt_active);
    cJSON_AddBoolToObject(data,   "current_slot_empty", s.current_slot_empty);
    cJSON_AddBoolToObject(data,   "tuner_on", s.tuner_on);
    cJSON *leds = cJSON_AddArrayToObject(data, "slot_leds");
    for (int i = 0; i < PROGRAMS_PER_BANK; i++) {
        cJSON_AddItemToArray(leds, cJSON_CreateString(led_to_str(s.slot_leds[i])));
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return;
    broadcast_text(ws, str);
    cJSON_free(str);
}

void web_server_broadcast_midi(web_server_t *ws, const midi_message_t *msg)
{
    if (!ws || !msg) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "midi");
    cJSON_AddNumberToObject(root, "ts", (double)(esp_timer_get_time() / 1000));
    cJSON *m = cJSON_AddObjectToObject(root, "msg");
    cJSON_AddStringToObject(m, "type", msg->type == MIDI_PC ? "PC" : "CC");
    cJSON_AddNumberToObject(m, "num", msg->num);
    if (msg->type == MIDI_CC) {
        cJSON_AddNumberToObject(m, "val", msg->val);
    }
    cJSON_AddNumberToObject(m, "ch", msg->ch);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return;
    broadcast_text(ws, str);
    cJSON_free(str);
}
