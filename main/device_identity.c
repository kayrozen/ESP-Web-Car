#include "device_identity.h"
#include "config.h"
#include "storage.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_client.h"

static const char *TAG = "device_id";

static char s_device_id[DEVICE_ID_LEN] = {0};
static char s_api_key[API_KEY_LEN]     = {0};
static char s_salt[TM_SALT_LEN]        = {0};

/* ── Random generation helpers ───────────────────────────────────────── */

static void generate_uuid4(char *out, size_t out_len)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;  /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80;  /* variant bits */
    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
             b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

static void generate_hex(char *out, size_t out_len, size_t nbytes)
{
    uint8_t buf[32];
    if (nbytes > sizeof(buf)) nbytes = sizeof(buf);
    esp_fill_random(buf, nbytes);
    size_t i;
    for (i = 0; i < nbytes && (i * 2 + 3) <= out_len; i++) {
        snprintf(out + i * 2, 3, "%02x", buf[i]);
    }
    out[i * 2] = '\0';
}

/* ── NVS load/create ─────────────────────────────────────────────────── */

static void create_fresh_identity(void)
{
    generate_uuid4(s_device_id, sizeof(s_device_id));
    generate_hex(s_api_key, sizeof(s_api_key), 32);
    generate_hex(s_salt, sizeof(s_salt), 16);

    storage_set_device_id(s_device_id);
    storage_set_api_key(s_api_key);
    storage_set_tm_salt(s_salt);

    ESP_LOGI(TAG, "New device identity: %s", s_device_id);
}

esp_err_t device_identity_init(void)
{
    bool needs_create = false;

    if (storage_get_device_id(s_device_id, sizeof(s_device_id)) != ESP_OK ||
        s_device_id[0] == '\0') {
        needs_create = true;
    }
    if (!needs_create &&
        (storage_get_api_key(s_api_key, sizeof(s_api_key)) != ESP_OK ||
         s_api_key[0] == '\0')) {
        needs_create = true;
    }
    if (!needs_create &&
        (storage_get_tm_salt(s_salt, sizeof(s_salt)) != ESP_OK ||
         s_salt[0] == '\0')) {
        needs_create = true;
    }

    if (needs_create) {
        create_fresh_identity();
    } else {
        ESP_LOGI(TAG, "Identity loaded: %s", s_device_id);
    }

    return ESP_OK;
}

/* ── HTTP registration ───────────────────────────────────────────────── */

esp_err_t device_identity_register(void)
{
    char base_url[STORAGE_URL_MAX] = {0};
    if (storage_get_api_base_url(base_url, sizeof(base_url)) != ESP_OK ||
        base_url[0] == '\0') {
        strncpy(base_url, TELEMETRY_API_BASE_DEFAULT, sizeof(base_url) - 1);
    }

    char url[STORAGE_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/api/v1/register", base_url);

    char body[300];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"api_key\":\"%s\",\"hardware_revision\":\"esp32-wrover\"}",
             s_device_id, s_api_key);

    char auth[DEVICE_ID_LEN + API_KEY_LEN + 10];
    snprintf(auth, sizeof(auth), "Bearer %s:%s", s_device_id, s_api_key);

    esp_http_client_config_t cfg = {
        .url         = url,
        .method      = HTTP_METHOD_POST,
        .timeout_ms  = CARRADIO_HTTP_TIMEOUT_MS,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Register HTTP error: %s", esp_err_to_name(err));
        return err;
    }

    if (status == 404) {
        /* Server deleted our record — generate a new identity and retry */
        ESP_LOGW(TAG, "Server returned 404 — re-registering with new identity");
        create_fresh_identity();
        return device_identity_register();
    }

    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Registered (HTTP %d)", status);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Register returned HTTP %d", status);
    return ESP_FAIL;
}

/* ── Getters ─────────────────────────────────────────────────────────── */

void device_identity_get_id(char *buf, size_t len)
{
    strncpy(buf, s_device_id, len - 1);
    buf[len - 1] = '\0';
}

void device_identity_get_api_key(char *buf, size_t len)
{
    strncpy(buf, s_api_key, len - 1);
    buf[len - 1] = '\0';
}

void device_identity_get_salt(char *buf, size_t len)
{
    strncpy(buf, s_salt, len - 1);
    buf[len - 1] = '\0';
}
