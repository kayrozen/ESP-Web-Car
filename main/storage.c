#include "storage.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "storage";
static nvs_handle_t s_nvs_handle = 0;

/* ── Internal helpers ────────────────────────────────────────────────── */

static esp_err_t nvs_open_rw(void)
{
    if (s_nvs_handle != 0) return ESP_OK;
    return nvs_open(CARRADIO_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
}

static void migrate_config(void)
{
    uint8_t ver = 0;
    esp_err_t err = nvs_get_u8(s_nvs_handle, NVS_KEY_CONFIG_VERSION, &ver);
    if (err == ESP_ERR_NVS_NOT_FOUND || ver < CARRADIO_CONFIG_VERSION) {
        ESP_LOGI(TAG, "Config version %u → %u, migrating", ver, CARRADIO_CONFIG_VERSION);
        /* Future migrations would go here.  For now just stamp the version. */
        nvs_set_u8(s_nvs_handle, NVS_KEY_CONFIG_VERSION, CARRADIO_CONFIG_VERSION);
        nvs_commit(s_nvs_handle);
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    migrate_config();
    return ESP_OK;
}

esp_err_t storage_erase_all(void)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_erase_all(s_nvs_handle);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

/* ── Phase ───────────────────────────────────────────────────────────── */

uint8_t storage_get_phase(void)
{
    if (nvs_open_rw() != ESP_OK) return CARRADIO_PHASE_WIFI;
    uint8_t phase = CARRADIO_PHASE_WIFI;
    nvs_get_u8(s_nvs_handle, NVS_KEY_PHASE, &phase);
    return phase;
}

esp_err_t storage_set_phase(uint8_t phase)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_u8(s_nvs_handle, NVS_KEY_PHASE, phase);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_reset_phase(void)
{
    return storage_set_phase(CARRADIO_PHASE_WIFI);
}

/* ── WiFi ────────────────────────────────────────────────────────────── */

esp_err_t storage_get_wifi_ssid(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_WIFI_SSID, buf, &req);
}

esp_err_t storage_set_wifi_ssid(const char *ssid)
{
    if (!storage_validate_ssid(ssid)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_WIFI_SSID, ssid);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_get_wifi_pass(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_WIFI_PASS, buf, &req);
}

esp_err_t storage_set_wifi_pass(const char *pass)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_WIFI_PASS, pass);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

/* ── Stream URL ──────────────────────────────────────────────────────── */

esp_err_t storage_get_stream_url(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_STREAM_URL, buf, &req);
}

esp_err_t storage_set_stream_url(const char *url)
{
    if (!storage_validate_url(url)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_STREAM_URL, url);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

/* ── Bluetooth ───────────────────────────────────────────────────────── */

esp_err_t storage_get_bt_name(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_BT_NAME, buf, &req);
}

esp_err_t storage_set_bt_name(const char *name)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_BT_NAME, name);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_get_bt_mac(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_BT_MAC, buf, &req);
}

esp_err_t storage_set_bt_mac(const char *mac_str)
{
    if (!storage_validate_mac(mac_str)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_BT_MAC, mac_str);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_get_bt_mac_bytes(uint8_t mac[6])
{
    char mac_str[STORAGE_BT_MAC_MAX] = {0};
    esp_err_t err = storage_get_bt_mac(mac_str, sizeof(mac_str));
    if (err != ESP_OK) return err;
    if (!storage_validate_mac(mac_str)) return ESP_ERR_INVALID_ARG;

    unsigned int v[6];
    if (sscanf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return ESP_OK;
}

/* ── Boot-fail counter ───────────────────────────────────────────────── */

uint8_t storage_get_boot_fail_count(void)
{
    if (nvs_open_rw() != ESP_OK) return 0;
    uint8_t count = 0;
    nvs_get_u8(s_nvs_handle, NVS_KEY_BOOT_FAIL_COUNT, &count);
    return count;
}

esp_err_t storage_set_boot_fail_count(uint8_t count)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_u8(s_nvs_handle, NVS_KEY_BOOT_FAIL_COUNT, count);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

/* ── Device identity & telemetry ─────────────────────────────────────── */

esp_err_t storage_get_device_id(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_DEVICE_ID, buf, &req);
}

esp_err_t storage_set_device_id(const char *id)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_DEVICE_ID, id);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_get_api_key(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_API_KEY, buf, &req);
}

esp_err_t storage_set_api_key(const char *key)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_API_KEY, key);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_get_tm_salt(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_TM_SALT, buf, &req);
}

esp_err_t storage_set_tm_salt(const char *salt)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_TM_SALT, salt);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_get_api_base_url(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_API_BASE_URL, buf, &req);
}

esp_err_t storage_set_api_base_url(const char *url)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_API_BASE_URL, url);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

esp_err_t storage_get_tm_enabled(uint8_t *out)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    *out = 1; /* default enabled */
    nvs_get_u8(s_nvs_handle, NVS_KEY_TM_ENABLED, out);
    return ESP_OK;
}

esp_err_t storage_set_tm_enabled(bool enabled)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_u8(s_nvs_handle, NVS_KEY_TM_ENABLED, enabled ? 1 : 0);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

/* ── Device name ─────────────────────────────────────────────────────── */

esp_err_t storage_get_device_name(char *buf, size_t len)
{
    if (nvs_open_rw() != ESP_OK) return ESP_FAIL;
    size_t req = len;
    return nvs_get_str(s_nvs_handle, NVS_KEY_DEVICE_NAME, buf, &req);
}

esp_err_t storage_set_device_name(const char *name)
{
    esp_err_t err = nvs_open_rw();
    if (err != ESP_OK) return err;
    err = nvs_set_str(s_nvs_handle, NVS_KEY_DEVICE_NAME, name);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs_handle);
}

bool storage_is_phase_ready(void)
{
    if (nvs_open_rw() != ESP_OK) return false;

    /* Check playlist_json exists */
    size_t plen = 0;
    esp_err_t err = nvs_get_str(s_nvs_handle, NVS_KEY_PLAYLIST_JSON, NULL, &plen);
    if (err != ESP_OK || plen == 0) return false;

    /* Check WiFi SSID exists */
    size_t slen = 0;
    err = nvs_get_str(s_nvs_handle, NVS_KEY_WIFI_SSID, NULL, &slen);
    if (err != ESP_OK || slen == 0) return false;

    return true;
}

/* ── Validation ──────────────────────────────────────────────────────── */

bool storage_validate_ssid(const char *ssid)
{
    if (!ssid) return false;
    size_t len = strlen(ssid);
    return (len > 0 && len <= 32);
}

bool storage_validate_url(const char *url)
{
    if (!url) return false;
    return (strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "https://", 8) == 0);
}

bool storage_validate_mac(const char *mac_str)
{
    if (!mac_str) return false;
    /* Must match "HH:HH:HH:HH:HH:HH" exactly (17 chars + null) */
    if (strlen(mac_str) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac_str[i] != ':') return false;
        } else {
            if (!isxdigit((unsigned char)mac_str[i])) return false;
        }
    }
    return true;
}
