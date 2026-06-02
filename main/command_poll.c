#include "command_poll.h"
#include "config.h"
#include "storage.h"
#include "device_identity.h"
#include "telemetry.h"
#include "ota.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

#include "cJSON.h"

static const char *TAG = "cmd_poll";

/* ── Pending OTA URL ─────────────────────────────────────────────────── */

static char     s_ota_url[STORAGE_URL_MAX] = {0};
static volatile bool s_ota_pending = false;

bool command_poll_consume_ota_url(char *buf, size_t len)
{
    if (!s_ota_pending) return false;
    s_ota_pending = false;
    strncpy(buf, s_ota_url, len - 1);
    buf[len - 1] = '\0';
    return true;
}

/* ── HTTP response buffer ────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t size;
    size_t pos;
} resp_ctx_t;

static esp_err_t resp_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        size_t space = ctx->size - ctx->pos - 1;
        size_t copy  = (size_t)evt->data_len < space ? (size_t)evt->data_len : space;
        memcpy(ctx->buf + ctx->pos, evt->data, copy);
        ctx->pos += copy;
        ctx->buf[ctx->pos] = '\0';
    }
    return ESP_OK;
}

/* ── Command ACK ─────────────────────────────────────────────────────── */

static void ack_command(int64_t command_id, bool success, const char *result_json)
{
    char base_url[STORAGE_URL_MAX] = {0};
    if (storage_get_api_base_url(base_url, sizeof(base_url)) != ESP_OK ||
        !base_url[0]) {
        strncpy(base_url, TELEMETRY_API_BASE_DEFAULT, sizeof(base_url) - 1);
    }

    char url[STORAGE_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/api/v1/commands/%"PRId64"/complete",
             base_url, command_id);

    char body[256];
    snprintf(body, sizeof(body),
             "{\"success\":%s,\"result\":%s}",
             success ? "true" : "false",
             result_json ? result_json : "null");

    char id[DEVICE_ID_LEN], key[API_KEY_LEN];
    device_identity_get_id(id, sizeof(id));
    device_identity_get_api_key(key, sizeof(key));

    char auth[DEVICE_ID_LEN + API_KEY_LEN + 10];
    snprintf(auth, sizeof(auth), "Bearer %s:%s", id, key);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = CARRADIO_HTTP_TIMEOUT_MS,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

/* ── Command handlers ────────────────────────────────────────────────── */

static void handle_upload_full_log(int64_t command_id)
{
    /* Allocate a large buffer in PSRAM for the JSON log dump */
    size_t buf_size = TELEMETRY_LOG_SLOTS * (TELEMETRY_EVENT_MAX + 2) + 32;
    char *log_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!log_buf) {
        log_buf = malloc(buf_size);
        if (!log_buf) {
            ack_command(command_id, false, "{\"error\":\"oom\"}");
            return;
        }
    }

    int count = telemetry_log_read_all(log_buf, buf_size);
    ESP_LOGI(TAG, "upload_full_log: %d events, %zu bytes", count, strlen(log_buf));

    char base_url[STORAGE_URL_MAX] = {0};
    if (storage_get_api_base_url(base_url, sizeof(base_url)) != ESP_OK ||
        !base_url[0]) {
        strncpy(base_url, TELEMETRY_API_BASE_DEFAULT, sizeof(base_url) - 1);
    }

    char url[STORAGE_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/api/v1/uploads", base_url);

    char id[DEVICE_ID_LEN], key[API_KEY_LEN];
    device_identity_get_id(id, sizeof(id));
    device_identity_get_api_key(key, sizeof(key));

    char auth[DEVICE_ID_LEN + API_KEY_LEN + 10];
    snprintf(auth, sizeof(auth), "Bearer %s:%s", id, key);

    char cmd_id_str[24];
    snprintf(cmd_id_str, sizeof(cmd_id_str), "%"PRId64, command_id);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool success = false;

    if (client) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "X-Command-ID", cmd_id_str);
        esp_http_client_set_post_field(client, log_buf, (int)strlen(log_buf));

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        success = (err == ESP_OK && status >= 200 && status < 300);
        ESP_LOGI(TAG, "Upload: err=%s HTTP %d", esp_err_to_name(err), status);
    }

    free(log_buf);
    ack_command(command_id, success, NULL);
}

static void handle_force_ota_check(int64_t command_id, cJSON *payload)
{
    const char *url = NULL;
    cJSON *url_item = cJSON_GetObjectItem(payload, "url");
    if (cJSON_IsString(url_item)) url = url_item->valuestring;

    if (url && url[0]) {
        strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
        s_ota_url[sizeof(s_ota_url) - 1] = '\0';
        s_ota_pending = true;
        ESP_LOGI(TAG, "force_ota_check: URL=%s", s_ota_url);
    } else {
        s_ota_pending = true;
        s_ota_url[0] = '\0';
        ESP_LOGI(TAG, "force_ota_check: no URL");
    }
    ack_command(command_id, true, NULL);
}

static void handle_set_config(int64_t command_id, cJSON *payload)
{
    cJSON *item;

    item = cJSON_GetObjectItem(payload, "api_base_url");
    if (cJSON_IsString(item) && item->valuestring[0]) {
        storage_set_api_base_url(item->valuestring);
        ESP_LOGI(TAG, "set_config: api_base_url=%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(payload, "telemetry_enabled");
    if (cJSON_IsBool(item)) {
        bool en = cJSON_IsTrue(item);
        telemetry_set_enabled(en);
        ESP_LOGI(TAG, "set_config: telemetry_enabled=%d", en);
    }

    ack_command(command_id, true, NULL);
}

static void handle_restart(int64_t command_id)
{
    ack_command(command_id, true, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── Poll loop ───────────────────────────────────────────────────────── */

static void do_poll(void)
{
    char base_url[STORAGE_URL_MAX] = {0};
    if (storage_get_api_base_url(base_url, sizeof(base_url)) != ESP_OK ||
        !base_url[0]) {
        strncpy(base_url, TELEMETRY_API_BASE_DEFAULT, sizeof(base_url) - 1);
    }

    char url[STORAGE_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/api/v1/commands", base_url);

    char id[DEVICE_ID_LEN], key[API_KEY_LEN];
    device_identity_get_id(id, sizeof(id));
    device_identity_get_api_key(key, sizeof(key));

    char auth[DEVICE_ID_LEN + API_KEY_LEN + 10];
    snprintf(auth, sizeof(auth), "Bearer %s:%s", id, key);

    /* Response buffer in PSRAM */
    size_t resp_size = 4096;
    char *resp_buf = heap_caps_malloc(resp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_buf) resp_buf = malloc(resp_size);
    if (!resp_buf) return;
    resp_buf[0] = '\0';

    resp_ctx_t ctx = { .buf = resp_buf, .size = resp_size, .pos = 0 };

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_GET,
        .timeout_ms    = CARRADIO_HTTP_TIMEOUT_MS,
        .event_handler = resp_event_handler,
        .user_data     = &ctx,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp_buf); return; }

    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGD(TAG, "Poll: err=%s HTTP %d", esp_err_to_name(err), status);
        free(resp_buf);
        return;
    }

    if (!resp_buf[0] || strcmp(resp_buf, "[]") == 0) {
        free(resp_buf);
        return;
    }

    cJSON *cmds = cJSON_Parse(resp_buf);
    free(resp_buf);

    if (!cJSON_IsArray(cmds)) {
        cJSON_Delete(cmds);
        return;
    }

    int n = cJSON_GetArraySize(cmds);
    ESP_LOGI(TAG, "Received %d command(s)", n);

    for (int i = 0; i < n; i++) {
        cJSON *cmd = cJSON_GetArrayItem(cmds, i);
        if (!cJSON_IsObject(cmd)) continue;

        cJSON *id_item   = cJSON_GetObjectItem(cmd, "command_id");
        cJSON *type_item = cJSON_GetObjectItem(cmd, "command_type");
        cJSON *pl_item   = cJSON_GetObjectItem(cmd, "payload");

        if (!cJSON_IsNumber(id_item) || !cJSON_IsString(type_item)) continue;

        int64_t cmd_id   = (int64_t)id_item->valuedouble;
        const char *type = type_item->valuestring;
        cJSON *payload   = cJSON_IsObject(pl_item) ? pl_item : NULL;

        ESP_LOGI(TAG, "Command %"PRId64": %s", cmd_id, type);

        if (strcmp(type, "upload_full_log") == 0) {
            handle_upload_full_log(cmd_id);
        } else if (strcmp(type, "force_ota_check") == 0) {
            handle_force_ota_check(cmd_id, payload);
        } else if (strcmp(type, "set_config") == 0) {
            handle_set_config(cmd_id, payload);
        } else if (strcmp(type, "restart") == 0) {
            handle_restart(cmd_id);
            break; /* won't return after restart */
        } else {
            ESP_LOGW(TAG, "Unknown command type: %s", type);
            ack_command(cmd_id, false, "{\"error\":\"unknown_type\"}");
        }
    }

    cJSON_Delete(cmds);
}

static void command_poll_task(void *arg)
{
    /* Stagger first poll so it doesn't overlap with telemetry flush */
    vTaskDelay(pdMS_TO_TICKS(5000));

    for (;;) {
        do_poll();
        vTaskDelay(pdMS_TO_TICKS(COMMAND_POLL_INTERVAL_MS));
    }
}

esp_err_t command_poll_init(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        command_poll_task, "cmd_poll",
        TASK_STACK_CMD_POLL, NULL,
        TASK_PRIO_CMD_POLL, NULL,
        1 /* Core 1 */
    );
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
