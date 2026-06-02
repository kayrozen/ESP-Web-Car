#include "telemetry.h"
#include "device_identity.h"
#include "config.h"
#include "storage.h"
#include "wifi.h"
#include "ota.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "mbedtls/sha256.h"
#include "esp_random.h"

static const char *TAG = "telemetry";

/* ── In-memory ring buffer ───────────────────────────────────────────── */

typedef struct {
    uint64_t device_ts_ms;
    char     event_type[32];
    char     payload[320];
    bool     sent;
} tm_event_t;

static tm_event_t        *s_ring       = NULL;
static uint32_t           s_ring_head  = 0;   /* next write slot */
static uint32_t           s_ring_count = 0;   /* valid entries */
static SemaphoreHandle_t  s_ring_mutex = NULL;

/* ── Flash log (NVS "log" partition) ─────────────────────────────────── */

#define LOG_NAMESPACE   "tm_log"
#define LOG_KEY_HEAD    "head"
#define LOG_KEY_COUNT   "cnt"

static nvs_handle_t s_log_nvs   = 0;
static uint32_t     s_log_head  = 0;
static uint32_t     s_log_count = 0;

/* ── Session ─────────────────────────────────────────────────────────── */

static char s_session_id[DEVICE_ID_LEN] = {0};

/* ── Enable flag ─────────────────────────────────────────────────────── */

static volatile bool s_enabled    = true;
static volatile bool s_initialized = false;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static void make_auth_header(char *buf, size_t len)
{
    char id[DEVICE_ID_LEN], key[API_KEY_LEN];
    device_identity_get_id(id, sizeof(id));
    device_identity_get_api_key(key, sizeof(key));
    snprintf(buf, len, "Bearer %s:%s", id, key);
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power_on";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP:return "deepsleep";
        case ESP_RST_TASK_WDT: return "task_wdt";
        default:               return "unknown";
    }
}

/* ── NVS flash log ───────────────────────────────────────────────────── */

static void log_nvs_init(void)
{
    esp_err_t err = nvs_flash_init_partition("log");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition("log");
        err = nvs_flash_init_partition("log");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "log partition NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_open_from_partition("log", LOG_NAMESPACE, NVS_READWRITE, &s_log_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "log nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_get_u32(s_log_nvs, LOG_KEY_HEAD,  &s_log_head);
    nvs_get_u32(s_log_nvs, LOG_KEY_COUNT, &s_log_count);
    ESP_LOGI(TAG, "Log partition ready (head=%"PRIu32" count=%"PRIu32")",
             s_log_head, s_log_count);
}

static void log_nvs_write(uint64_t ts, const char *event_type, const char *payload)
{
    if (!s_log_nvs) return;

    char key[8];
    snprintf(key, sizeof(key), "%"PRIu32, s_log_head % TELEMETRY_LOG_SLOTS);

    /* Compact JSON for NVS storage */
    char entry[TELEMETRY_EVENT_MAX];
    snprintf(entry, sizeof(entry),
             "{\"ts\":%"PRIu64",\"et\":\"%s\",\"p\":%s}",
             ts, event_type, payload[0] ? payload : "{}");

    nvs_set_str(s_log_nvs, key, entry);

    s_log_head = (s_log_head + 1) % TELEMETRY_LOG_SLOTS;
    if (s_log_count < TELEMETRY_LOG_SLOTS) s_log_count++;

    nvs_set_u32(s_log_nvs, LOG_KEY_HEAD,  s_log_head);
    nvs_set_u32(s_log_nvs, LOG_KEY_COUNT, s_log_count);
    nvs_commit(s_log_nvs);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void telemetry_log(const char *event_type, const char *payload_json)
{
    if (!s_initialized || !s_ring || !s_ring_mutex) return;
    if (!event_type || event_type[0] == '\0') return;

    uint64_t ts = now_ms();
    const char *payload = (payload_json && payload_json[0]) ? payload_json : "{}";

    /* Write to flash log */
    log_nvs_write(ts, event_type, payload);

    /* Write to in-memory ring */
    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    tm_event_t *e = &s_ring[s_ring_head % TELEMETRY_RAM_RING_SIZE];
    e->device_ts_ms = ts;
    strncpy(e->event_type, event_type, sizeof(e->event_type) - 1);
    e->event_type[sizeof(e->event_type) - 1] = '\0';
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    e->payload[sizeof(e->payload) - 1] = '\0';
    e->sent = false;

    s_ring_head = (s_ring_head + 1) % TELEMETRY_RAM_RING_SIZE;
    if (s_ring_count < TELEMETRY_RAM_RING_SIZE) s_ring_count++;

    xSemaphoreGive(s_ring_mutex);
}

void telemetry_set_enabled(bool enabled)
{
    s_enabled = enabled;
    storage_set_tm_enabled(enabled);
    ESP_LOGI(TAG, "Telemetry %s", enabled ? "enabled" : "disabled");
}

bool telemetry_is_enabled(void) { return s_enabled; }

/* ── Hash helper ─────────────────────────────────────────────────────── */

void telemetry_hash_id(const char *raw, char *out, size_t outlen)
{
    if (!raw || !out || outlen < 33) return;

    char salt[TM_SALT_LEN];
    device_identity_get_salt(salt, sizeof(salt));

    char salted[TM_SALT_LEN + 256];
    snprintf(salted, sizeof(salted), "%s%s", salt, raw);

    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)salted, strlen(salted), hash, 0);

    for (int i = 0; i < 16 && (size_t)(i * 2 + 3) <= outlen; i++) {
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    }
    out[32 < (int)outlen ? 32 : (int)outlen - 1] = '\0';
}

/* ── Log read (for upload_full_log) ──────────────────────────────────── */

int telemetry_log_read_all(char *buf, size_t buf_size)
{
    if (!s_log_nvs || !buf || buf_size < 4) return 0;

    /* Start from oldest entry: (head - count + SLOTS) % SLOTS */
    uint32_t start = (s_log_head + TELEMETRY_LOG_SLOTS - s_log_count)
                     % TELEMETRY_LOG_SLOTS;
    size_t pos = 0;
    int count = 0;

    buf[pos++] = '[';

    for (uint32_t i = 0; i < s_log_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "%"PRIu32, (start + i) % TELEMETRY_LOG_SLOTS);

        char entry[TELEMETRY_EVENT_MAX];
        size_t entry_len = sizeof(entry);
        if (nvs_get_str(s_log_nvs, key, entry, &entry_len) != ESP_OK) continue;

        size_t needed = strlen(entry) + 2; /* comma + entry */
        if (pos + needed + 2 >= buf_size) break; /* +2 for "]\0" */

        if (count > 0) buf[pos++] = ',';
        size_t elen = strlen(entry);
        memcpy(buf + pos, entry, elen);
        pos += elen;
        count++;
    }

    buf[pos++] = ']';
    buf[pos]   = '\0';
    return count;
}

/* ── Session start ───────────────────────────────────────────────────── */

static void generate_uuid4(char *out, size_t len)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
             b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

static void session_start(void)
{
    generate_uuid4(s_session_id, sizeof(s_session_id));

    char base_url[STORAGE_URL_MAX] = {0};
    if (storage_get_api_base_url(base_url, sizeof(base_url)) != ESP_OK ||
        !base_url[0]) {
        strncpy(base_url, TELEMETRY_API_BASE_DEFAULT, sizeof(base_url) - 1);
    }

    char url[STORAGE_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/api/v1/sessions", base_url);

    char body[256];
    snprintf(body, sizeof(body),
             "{\"session_id\":\"%s\",\"firmware_version\":\"%s\","
             "\"reset_reason\":\"%s\",\"telemetry_enabled\":%s}",
             s_session_id, ota_get_app_version(),
             reset_reason_str(), s_enabled ? "true" : "false");

    char auth[DEVICE_ID_LEN + API_KEY_LEN + 10];
    make_auth_header(auth, sizeof(auth));

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

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Session started: %s (HTTP %d)",
                 s_session_id, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "Session POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/* ── Flush task ──────────────────────────────────────────────────────── */

static void flush_events_to_server(void)
{
    if (!s_ring || !s_ring_mutex) return;

    /* Collect up to TELEMETRY_FLUSH_BATCH unsent events */
    char *body = heap_caps_malloc(TELEMETRY_FLUSH_BATCH * 420 + 32,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(TELEMETRY_FLUSH_BATCH * 420 + 32);
        if (!body) return;
    }

    uint32_t indices[TELEMETRY_FLUSH_BATCH];
    int      n_events = 0;
    size_t   pos = 0;

    body[pos++] = '[';

    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        /* Oldest entry is at (head - count + RING_SIZE) % RING_SIZE */
        uint32_t start = (s_ring_head + TELEMETRY_RAM_RING_SIZE - s_ring_count)
                         % TELEMETRY_RAM_RING_SIZE;

        for (uint32_t i = 0; i < s_ring_count && n_events < TELEMETRY_FLUSH_BATCH; i++) {
            uint32_t idx = (start + i) % TELEMETRY_RAM_RING_SIZE;
            tm_event_t *e = &s_ring[idx];
            if (e->sent) continue;

            char entry[512];
            int elen = snprintf(entry, sizeof(entry),
                                "%s{\"session_id\":\"%s\",\"device_ts_ms\":%"PRIu64","
                                "\"event_type\":\"%s\",\"payload\":%s}",
                                n_events > 0 ? "," : "",
                                s_session_id, e->device_ts_ms,
                                e->event_type, e->payload);

            if (elen > 0 && pos + (size_t)elen + 2 < (size_t)(TELEMETRY_FLUSH_BATCH * 420 + 32)) {
                memcpy(body + pos, entry, elen);
                pos += elen;
                indices[n_events++] = idx;
            }
        }
        xSemaphoreGive(s_ring_mutex);
    }

    if (n_events == 0) {
        free(body);
        return;
    }

    body[pos++] = ']';
    body[pos]   = '\0';

    /* POST to server */
    char base_url[STORAGE_URL_MAX] = {0};
    if (storage_get_api_base_url(base_url, sizeof(base_url)) != ESP_OK ||
        !base_url[0]) {
        strncpy(base_url, TELEMETRY_API_BASE_DEFAULT, sizeof(base_url) - 1);
    }

    char url[STORAGE_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/api/v1/events", base_url);

    char auth[DEVICE_ID_LEN + API_KEY_LEN + 10];
    make_auth_header(auth, sizeof(auth));

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = CARRADIO_HTTP_TIMEOUT_MS,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); return; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)pos);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    bool mark_sent = (err == ESP_OK && status >= 200 && status < 300);
    bool drop      = (err == ESP_OK && status >= 400 && status < 500);

    if (mark_sent || drop) {
        if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            for (int i = 0; i < n_events; i++) {
                s_ring[indices[i]].sent = true;
            }
            xSemaphoreGive(s_ring_mutex);
        }
        ESP_LOGD(TAG, "Flushed %d events (HTTP %d)", n_events, status);
    } else {
        ESP_LOGW(TAG, "Flush failed (err=%s status=%d) — will retry",
                 esp_err_to_name(err), status);
    }
}

static void telemetry_flush_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_FLUSH_INTERVAL_MS));
        if (s_enabled && wifi_is_connected()) {
            flush_events_to_server();
        }
    }
}

/* ── Init ────────────────────────────────────────────────────────────── */

esp_err_t telemetry_init(void)
{
    if (s_initialized) return ESP_OK;

    /* Allocate ring buffer in PSRAM */
    s_ring = heap_caps_calloc(TELEMETRY_RAM_RING_SIZE, sizeof(tm_event_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        s_ring = calloc(TELEMETRY_RAM_RING_SIZE, sizeof(tm_event_t));
        if (!s_ring) return ESP_ERR_NO_MEM;
    }

    s_ring_mutex = xSemaphoreCreateMutex();
    if (!s_ring_mutex) return ESP_ERR_NO_MEM;

    /* Restore telemetry_enabled from NVS */
    uint8_t tm_en = 1;
    storage_get_tm_enabled(&tm_en);
    s_enabled = (tm_en != 0);

    log_nvs_init();
    s_initialized = true;

    /* Register + start session (best-effort; device works without server) */
    device_identity_register();
    session_start();

    /* Boot event */
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"reset_reason\":\"%s\",\"free_heap\":%"PRIu32"}",
             reset_reason_str(), (uint32_t)esp_get_free_heap_size());
    telemetry_log("boot", payload);

    /* Start background flush task (low priority, Core 1) */
    xTaskCreatePinnedToCore(telemetry_flush_task, "tm_flush",
                            TASK_STACK_TELEMETRY, NULL,
                            TASK_PRIO_TELEMETRY, NULL, 1);

    ESP_LOGI(TAG, "Telemetry initialized (session %s)", s_session_id);
    return ESP_OK;
}
