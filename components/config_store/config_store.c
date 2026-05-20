#include "config_store.h"

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "config_store";

#define LFS_BASE_PATH       "/storage"
#define LFS_PARTITION_LABEL "storage"
#define CONFIG_PATH         LFS_BASE_PATH "/config.json"
#define CONFIG_TMP_PATH     LFS_BASE_PATH "/config.json.tmp"
#define MAX_CONFIG_BYTES    (64 * 1024)  // soft ceiling, the partition holds far more

struct config_store {
    config_t cfg;
    bool lfs_mounted;
};

// ===== JSON helpers ========================================================

static cJSON *encode_midi_message(const midi_message_t *m)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "type", m->type == MIDI_PC ? "PC" : "CC");
    cJSON_AddNumberToObject(obj, "num", m->num);
    if (m->type == MIDI_CC) {
        cJSON_AddNumberToObject(obj, "val", m->val);
    }
    cJSON_AddNumberToObject(obj, "ch", m->ch);
    return obj;
}

static bool decode_midi_message(const cJSON *obj, midi_message_t *out)
{
    if (!cJSON_IsObject(obj)) return false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "type");
    const cJSON *num  = cJSON_GetObjectItemCaseSensitive(obj, "num");
    const cJSON *ch   = cJSON_GetObjectItemCaseSensitive(obj, "ch");
    if (!cJSON_IsString(type) || !cJSON_IsNumber(num) || !cJSON_IsNumber(ch)) return false;
    if (strcmp(type->valuestring, "PC") == 0) {
        out->type = MIDI_PC;
        out->val = 0;
    } else if (strcmp(type->valuestring, "CC") == 0) {
        out->type = MIDI_CC;
        const cJSON *val = cJSON_GetObjectItemCaseSensitive(obj, "val");
        if (!cJSON_IsNumber(val)) return false;
        out->val = (uint8_t)val->valueint;
    } else {
        return false;
    }
    out->num = (uint8_t)num->valueint;
    out->ch  = (uint8_t)ch->valueint;
    return true;
}

static cJSON *encode_midi_list(const midi_message_list_t *list)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (uint8_t i = 0; i < list->count; i++) {
        cJSON *msg = encode_midi_message(&list->msgs[i]);
        if (!msg) { cJSON_Delete(arr); return NULL; }
        cJSON_AddItemToArray(arr, msg);
    }
    return arr;
}

static bool decode_midi_list(const cJSON *arr, midi_message_list_t *out)
{
    out->count = 0;
    if (cJSON_IsNull(arr) || !arr) return true;  // null/missing → empty list
    if (!cJSON_IsArray(arr)) return false;
    int n = cJSON_GetArraySize(arr);
    if (n > MAX_MIDI_MSGS_PER_LIST) n = MAX_MIDI_MSGS_PER_LIST;
    for (int i = 0; i < n; i++) {
        if (!decode_midi_message(cJSON_GetArrayItem(arr, i), &out->msgs[out->count])) return false;
        out->count++;
    }
    return true;
}

static cJSON *encode_expression_target(const expression_target_t *t)
{
    if (!t->present) return cJSON_CreateNull();
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddNumberToObject(obj, "cc", t->cc);
    cJSON_AddNumberToObject(obj, "ch", t->ch);
    return obj;
}

static bool decode_expression_target(const cJSON *obj, expression_target_t *out)
{
    if (!obj || cJSON_IsNull(obj)) { out->present = false; return true; }
    if (!cJSON_IsObject(obj)) return false;
    const cJSON *cc = cJSON_GetObjectItemCaseSensitive(obj, "cc");
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(obj, "ch");
    if (!cJSON_IsNumber(cc) || !cJSON_IsNumber(ch)) return false;
    out->present = true;
    out->cc = (uint8_t)cc->valueint;
    out->ch = (uint8_t)ch->valueint;
    return true;
}

static cJSON *encode_payload(const program_payload_t *p)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "name", p->name);
    cJSON_AddItemToObject(obj, "messages", encode_midi_list(&p->messages));
    cJSON_AddItemToObject(obj, "expression", encode_expression_target(&p->expression));
    return obj;
}

static bool decode_payload(const cJSON *obj, program_payload_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(obj)) return false;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "name");
    if (cJSON_IsString(name)) {
        strncpy(out->name, name->valuestring, MAX_NAME_LEN - 1);
        out->name[MAX_NAME_LEN - 1] = '\0';
    }
    if (!decode_midi_list(cJSON_GetObjectItemCaseSensitive(obj, "messages"), &out->messages)) return false;
    if (!decode_expression_target(cJSON_GetObjectItemCaseSensitive(obj, "expression"), &out->expression)) return false;
    return true;
}

static cJSON *encode_slot(const program_slot_t *s)
{
    cJSON *obj = encode_payload(&s->primary);
    if (!obj) return NULL;
    if (s->has_alternative) {
        cJSON_AddItemToObject(obj, "alternative", encode_payload(&s->alternative));
    } else {
        cJSON_AddItemToObject(obj, "alternative", cJSON_CreateNull());
    }
    return obj;
}

static bool decode_slot(const cJSON *obj, program_slot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!decode_payload(obj, &out->primary)) return false;
    const cJSON *alt = cJSON_GetObjectItemCaseSensitive(obj, "alternative");
    if (alt && !cJSON_IsNull(alt)) {
        if (!decode_payload(alt, &out->alternative)) return false;
        out->has_alternative = true;
    }
    return true;
}

static cJSON *encode_bank(const bank_t *b)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "name", b->name);
    cJSON_AddItemToObject(obj, "expression_default", encode_expression_target(&b->expression_default));
    cJSON *progs = cJSON_CreateArray();
    cJSON_AddItemToObject(obj, "programs", progs);
    for (int i = 0; i < PROGRAMS_PER_BANK; i++) {
        cJSON_AddItemToArray(progs, encode_slot(&b->programs[i]));
    }
    return obj;
}

static bool decode_bank(const cJSON *obj, bank_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(obj)) return false;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "name");
    if (cJSON_IsString(name)) {
        strncpy(out->name, name->valuestring, MAX_NAME_LEN - 1);
        out->name[MAX_NAME_LEN - 1] = '\0';
    }
    if (!decode_expression_target(cJSON_GetObjectItemCaseSensitive(obj, "expression_default"),
                                   &out->expression_default)) return false;
    const cJSON *progs = cJSON_GetObjectItemCaseSensitive(obj, "programs");
    if (!cJSON_IsArray(progs)) return false;
    int n = cJSON_GetArraySize(progs);
    if (n > PROGRAMS_PER_BANK) n = PROGRAMS_PER_BANK;
    for (int i = 0; i < n; i++) {
        if (!decode_slot(cJSON_GetArrayItem(progs, i), &out->programs[i])) return false;
    }
    return true;
}

static const char *alt_toggle_to_str(alt_toggle_behavior_t b)
{
    return (b == ALT_TOGGLE_A) ? "ALT_A" : "ALT_B";
}

static alt_toggle_behavior_t alt_toggle_from_str(const char *s, alt_toggle_behavior_t fallback)
{
    if (s && strcmp(s, "ALT_A") == 0) return ALT_TOGGLE_A;
    if (s && strcmp(s, "ALT_B") == 0) return ALT_TOGGLE_B;
    return fallback;
}

static const char *curve_to_str(expression_curve_t c)
{
    switch (c) {
        case EXPR_CURVE_LOG: return "log";
        case EXPR_CURVE_EXP: return "exp";
        default:             return "linear";
    }
}

static expression_curve_t curve_from_str(const char *s, expression_curve_t fallback)
{
    if (s && strcmp(s, "linear") == 0) return EXPR_CURVE_LINEAR;
    if (s && strcmp(s, "log") == 0)    return EXPR_CURVE_LOG;
    if (s && strcmp(s, "exp") == 0)    return EXPR_CURVE_EXP;
    return fallback;
}

static cJSON *encode_global(const global_config_t *g)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddNumberToObject(obj, "midi_channel", g->midi_channel);
    cJSON_AddItemToObject(obj, "tuner_message", encode_midi_message(&g->tuner_message));
    cJSON_AddItemToObject(obj, "tap_message",   encode_midi_message(&g->tap_message));
    cJSON_AddItemToObject(obj, "user_function_a",
        g->user_function_a.count ? encode_midi_list(&g->user_function_a) : cJSON_CreateNull());
    cJSON_AddItemToObject(obj, "user_function_b",
        g->user_function_b.count ? encode_midi_list(&g->user_function_b) : cJSON_CreateNull());
    cJSON_AddNumberToObject(obj, "long_press_short_ms", g->long_press_short_ms);
    cJSON_AddNumberToObject(obj, "long_press_long_ms",  g->long_press_long_ms);
    cJSON_AddNumberToObject(obj, "browse_timeout_ms",   g->browse_timeout_ms);
    cJSON_AddStringToObject(obj, "alt_toggle_behavior", alt_toggle_to_str(g->alt_toggle_behavior));
    cJSON_AddBoolToObject(obj,   "boot_resume", g->boot_resume);
    cJSON_AddNumberToObject(obj, "display_brightness", g->display_brightness);
    cJSON_AddNumberToObject(obj, "led_brightness", g->led_brightness);

    cJSON *expr = cJSON_CreateObject();
    cJSON_AddNumberToObject(expr, "adc_min", g->expression.adc_min);
    cJSON_AddNumberToObject(expr, "adc_max", g->expression.adc_max);
    cJSON_AddStringToObject(expr, "curve", curve_to_str(g->expression.curve));
    cJSON_AddNumberToObject(expr, "smoothing", g->expression.smoothing);
    cJSON_AddItemToObject(obj, "expression", expr);

    return obj;
}

static void decode_global(const cJSON *obj, global_config_t *out)
{
    // Decode field-by-field, leaving anything missing at its default
    // (the caller seeded `out` with config_init_defaults before calling us).
    if (!cJSON_IsObject(obj)) return;

    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, "midi_channel");
    if (cJSON_IsNumber(n)) out->midi_channel = (uint8_t)n->valueint;

    midi_message_t tmp;
    if (decode_midi_message(cJSON_GetObjectItemCaseSensitive(obj, "tuner_message"), &tmp)) {
        out->tuner_message = tmp;
    }
    if (decode_midi_message(cJSON_GetObjectItemCaseSensitive(obj, "tap_message"), &tmp)) {
        out->tap_message = tmp;
    }
    decode_midi_list(cJSON_GetObjectItemCaseSensitive(obj, "user_function_a"),
                     &out->user_function_a);
    decode_midi_list(cJSON_GetObjectItemCaseSensitive(obj, "user_function_b"),
                     &out->user_function_b);

    n = cJSON_GetObjectItemCaseSensitive(obj, "long_press_short_ms");
    if (cJSON_IsNumber(n)) out->long_press_short_ms = (uint32_t)n->valuedouble;
    n = cJSON_GetObjectItemCaseSensitive(obj, "long_press_long_ms");
    if (cJSON_IsNumber(n)) out->long_press_long_ms = (uint32_t)n->valuedouble;
    n = cJSON_GetObjectItemCaseSensitive(obj, "browse_timeout_ms");
    if (cJSON_IsNumber(n)) out->browse_timeout_ms = (uint32_t)n->valuedouble;

    const cJSON *atb = cJSON_GetObjectItemCaseSensitive(obj, "alt_toggle_behavior");
    if (cJSON_IsString(atb)) {
        out->alt_toggle_behavior = alt_toggle_from_str(atb->valuestring, out->alt_toggle_behavior);
    }

    const cJSON *br = cJSON_GetObjectItemCaseSensitive(obj, "boot_resume");
    if (cJSON_IsBool(br)) out->boot_resume = cJSON_IsTrue(br);

    n = cJSON_GetObjectItemCaseSensitive(obj, "display_brightness");
    if (cJSON_IsNumber(n)) out->display_brightness = (uint8_t)n->valueint;
    n = cJSON_GetObjectItemCaseSensitive(obj, "led_brightness");
    if (cJSON_IsNumber(n)) out->led_brightness = (uint8_t)n->valueint;

    const cJSON *expr = cJSON_GetObjectItemCaseSensitive(obj, "expression");
    if (cJSON_IsObject(expr)) {
        n = cJSON_GetObjectItemCaseSensitive(expr, "adc_min");
        if (cJSON_IsNumber(n)) out->expression.adc_min = (uint16_t)n->valueint;
        n = cJSON_GetObjectItemCaseSensitive(expr, "adc_max");
        if (cJSON_IsNumber(n)) out->expression.adc_max = (uint16_t)n->valueint;
        const cJSON *curve = cJSON_GetObjectItemCaseSensitive(expr, "curve");
        if (cJSON_IsString(curve)) {
            out->expression.curve = curve_from_str(curve->valuestring, out->expression.curve);
        }
        n = cJSON_GetObjectItemCaseSensitive(expr, "smoothing");
        if (cJSON_IsNumber(n)) out->expression.smoothing = (float)n->valuedouble;
    }
}

// ===== Top-level encode / decode ===========================================

static cJSON *encode_config(const config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddItemToObject(root, "global", encode_global(&cfg->global));
    cJSON *banks = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "banks", banks);
    for (uint8_t i = 0; i < cfg->bank_count; i++) {
        cJSON_AddItemToArray(banks, encode_bank(&cfg->banks[i]));
    }
    return root;
}

static bool decode_config(const cJSON *root, config_t *out)
{
    if (!cJSON_IsObject(root)) return false;
    config_init_defaults(out);
    decode_global(cJSON_GetObjectItemCaseSensitive(root, "global"), &out->global);
    const cJSON *banks = cJSON_GetObjectItemCaseSensitive(root, "banks");
    if (!cJSON_IsArray(banks)) return false;
    int n = cJSON_GetArraySize(banks);
    if (n > MAX_BANKS) n = MAX_BANKS;
    out->bank_count = 0;
    for (int i = 0; i < n; i++) {
        if (!decode_bank(cJSON_GetArrayItem(banks, i), &out->banks[out->bank_count])) {
            ESP_LOGW(TAG, "skipping malformed bank at index %d", i);
            continue;
        }
        out->bank_count++;
    }
    if (out->bank_count == 0) {
        ESP_LOGW(TAG, "config had no valid banks — restoring defaults");
        config_init_defaults(out);
    }
    return true;
}

// ===== File I/O ============================================================

static esp_err_t read_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || (size_t)len > MAX_CONFIG_BYTES) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read != (size_t)len) {
        free(buf);
        return ESP_FAIL;
    }
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = (size_t)len;
    return ESP_OK;
}

static esp_err_t write_file_atomic(const char *path, const char *tmp_path,
                                   const char *data, size_t len)
{
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: %s", tmp_path, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    int close_err = fclose(f);
    if (written != len || close_err != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename(%s -> %s) failed: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static char *config_to_json_alloc(const config_t *cfg)
{
    cJSON *root = encode_config(cfg);
    if (!root) return NULL;
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

static esp_err_t save_config(const config_t *cfg)
{
    char *str = config_to_json_alloc(cfg);
    if (!str) return ESP_ERR_NO_MEM;
    esp_err_t err = write_file_atomic(CONFIG_PATH, CONFIG_TMP_PATH, str, strlen(str));
    cJSON_free(str);
    return err;
}

// ===== Lifecycle ===========================================================

static esp_err_t mount_storage(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LFS_BASE_PATH,
        .partition_label = LFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    return esp_vfs_littlefs_register(&conf);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing (%s) — wiping and reinitialising",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

config_store_t *config_store_init(void)
{
    config_store_t *cs = calloc(1, sizeof(*cs));
    if (!cs) return NULL;

    init_nvs();

    esp_err_t err = mount_storage();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        free(cs);
        return NULL;
    }
    cs->lfs_mounted = true;

    size_t total = 0, used = 0;
    if (esp_littlefs_info(LFS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u / %u bytes used",
                 LFS_BASE_PATH, (unsigned)used, (unsigned)total);
    }

    char *buf = NULL;
    size_t len = 0;
    err = read_file(CONFIG_PATH, &buf, &len);
    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) {
            ESP_LOGW(TAG, "config.json failed to parse — restoring defaults");
            config_init_defaults(&cs->cfg);
            (void)save_config(&cs->cfg);
        } else if (!decode_config(root, &cs->cfg)) {
            ESP_LOGW(TAG, "config.json shape invalid — restoring defaults");
            cJSON_Delete(root);
            config_init_defaults(&cs->cfg);
            (void)save_config(&cs->cfg);
        } else {
            cJSON_Delete(root);
            ESP_LOGI(TAG, "loaded config.json (%u bytes, %u banks)",
                     (unsigned)len, cs->cfg.bank_count);
        }
    } else {
        ESP_LOGI(TAG, "no config.json present — writing factory defaults");
        config_init_defaults(&cs->cfg);
        esp_err_t save_err = save_config(&cs->cfg);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "default config save failed: %s", esp_err_to_name(save_err));
        }
    }
    return cs;
}

void config_store_destroy(config_store_t *cs)
{
    if (!cs) return;
    if (cs->lfs_mounted) {
        esp_vfs_littlefs_unregister(LFS_PARTITION_LABEL);
    }
    free(cs);
}

config_t *config_store_get(config_store_t *cs)
{
    return &cs->cfg;
}

esp_err_t config_store_save(const config_store_t *cs)
{
    return save_config(&cs->cfg);
}

char *config_store_to_json_alloc(const config_store_t *cs)
{
    return config_to_json_alloc(&cs->cfg);
}

esp_err_t config_store_from_json(config_store_t *cs, const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "config_store_from_json: parse failed");
        return ESP_ERR_INVALID_ARG;
    }
    // config_t is ~24 KB once you account for MAX_BANKS * PROGRAMS_PER_BANK
    // plus the primary+alt payloads, which blows the httpd worker's 8 KB
    // task stack instantly. Heap-allocate the scratch copy.
    config_t *parsed = calloc(1, sizeof(*parsed));
    if (!parsed) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    bool ok = decode_config(root, parsed);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "config_store_from_json: shape invalid");
        free(parsed);
        return ESP_ERR_INVALID_ARG;
    }
    cs->cfg = *parsed;
    free(parsed);
    return ESP_OK;
}
