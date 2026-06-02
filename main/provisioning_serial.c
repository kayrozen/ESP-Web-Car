#include "provisioning_serial.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "prov_serial";

#define PROV_TIMEOUT_US     (30LL * 1000 * 1000)   /* 30 seconds */
#define PROV_LINE_MAX       2048
#define PROV_PREFIX         "PROVISION:"
#define PROV_PREFIX_LEN     10
#define UART_RX_TIMEOUT_MS  20

/* ── Validation ──────────────────────────────────────────────────────── */

static bool validate_device_name(const char *name)
{
    if (!name) return false;
    int len = (int)strlen(name);
    if (len < 2 || len > 24) return false;

    /* First and last must be alnum */
    if (!islower((unsigned char)name[0]) && !isdigit((unsigned char)name[0])) return false;
    if (!islower((unsigned char)name[len-1]) && !isdigit((unsigned char)name[len-1])) return false;

    /* Middle chars: alnum or hyphen */
    for (int i = 1; i < len - 1; i++) {
        char c = name[i];
        if (!islower((unsigned char)c) && !isdigit((unsigned char)c) && c != '-')
            return false;
    }
    return true;
}

/* ── NVS write ───────────────────────────────────────────────────────── */

static bool write_provisioning_to_nvs(const char *device_name, const char *playlist_json)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CARRADIO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(h, NVS_KEY_DEVICE_NAME, device_name);
    if (err != ESP_OK) { nvs_close(h); return false; }

    err = nvs_set_str(h, NVS_KEY_PLAYLIST_JSON, playlist_json);
    if (err != ESP_OK) { nvs_close(h); return false; }

    nvs_set_u8(h, NVS_KEY_PLAYLIST_IDX, 0);

    err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK);
}

/* ── UART helpers ────────────────────────────────────────────────────── */

static void uart_send(const char *s)
{
    uart_write_bytes(UART_NUM_0, s, strlen(s));
}

/* ── Provision line parser ───────────────────────────────────────────── */

static bool process_provision_line(const char *line)
{
    /* line starts with PROVISION: */
    const char *json_start = line + PROV_PREFIX_LEN;

    cJSON *root = cJSON_Parse(json_start);
    if (!root) {
        uart_send("ERR:json_parse_failed\n");
        return false;
    }

    /* provision_version */
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "provision_version");
    if (!cJSON_IsNumber(ver) || (int)ver->valuedouble != 1) {
        cJSON_Delete(root);
        uart_send("ERR:bad_provision_version\n");
        return false;
    }

    /* device_name */
    cJSON *dname = cJSON_GetObjectItemCaseSensitive(root, "device_name");
    if (!cJSON_IsString(dname) || !validate_device_name(dname->valuestring)) {
        cJSON_Delete(root);
        uart_send("ERR:invalid_device_name\n");
        return false;
    }

    /* playlist */
    cJSON *playlist = cJSON_GetObjectItemCaseSensitive(root, "playlist");
    if (!cJSON_IsArray(playlist)) {
        cJSON_Delete(root);
        uart_send("ERR:playlist_not_array\n");
        return false;
    }

    int count = cJSON_GetArraySize(playlist);
    if (count < 1 || count > 5) {
        cJSON_Delete(root);
        uart_send("ERR:playlist_count_out_of_range\n");
        return false;
    }

    /* Validate each entry has name + url */
    cJSON *entry = NULL;
    int idx = 0;
    cJSON_ArrayForEach(entry, playlist) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(entry, "name");
        cJSON *u = cJSON_GetObjectItemCaseSensitive(entry, "url");
        if (!cJSON_IsString(n) || !n->valuestring[0]) {
            cJSON_Delete(root);
            char err_buf[48];
            snprintf(err_buf, sizeof(err_buf), "ERR:entry_%d_missing_name\n", idx);
            uart_send(err_buf);
            return false;
        }
        if (!cJSON_IsString(u) || !u->valuestring[0]) {
            cJSON_Delete(root);
            char err_buf[48];
            snprintf(err_buf, sizeof(err_buf), "ERR:entry_%d_missing_url\n", idx);
            uart_send(err_buf);
            return false;
        }
        idx++;
    }

    /* Serialize playlist array as compact JSON */
    char *playlist_json = cJSON_PrintUnformatted(playlist);
    if (!playlist_json) {
        cJSON_Delete(root);
        uart_send("ERR:oom\n");
        return false;
    }

    bool ok = write_provisioning_to_nvs(dname->valuestring, playlist_json);
    free(playlist_json);
    cJSON_Delete(root);

    if (!ok) {
        uart_send("ERR:nvs_write_failed\n");
        return false;
    }

    uart_send("OK\n");
    ESP_LOGI(TAG, "Provisioning written: device_name=%s, %d stations",
             dname->valuestring, count);
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool provisioning_serial_wait(void)
{
    /* UART0 is already configured by the IDF boot loader.
       We install the driver briefly to do buffered reads. */
    const uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_driver_install(UART_NUM_0, PROV_LINE_MAX * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_cfg);

    ESP_LOGI(TAG, "Waiting up to 30s for provisioning on UART0...");

    char line_buf[PROV_LINE_MAX];
    int  line_pos  = 0;
    bool result    = false;
    int64_t deadline = esp_timer_get_time() + PROV_TIMEOUT_US;

    while (esp_timer_get_time() < deadline) {
        uint8_t ch;
        int got = uart_read_bytes(UART_NUM_0, &ch, 1,
                                  pdMS_TO_TICKS(UART_RX_TIMEOUT_MS));
        if (got <= 0) continue;

        if (ch == '\n' || ch == '\r') {
            if (line_pos == 0) continue;  /* blank line */

            line_buf[line_pos] = '\0';
            ESP_LOGD(TAG, "Line received (%d bytes)", line_pos);

            if (strncmp(line_buf, PROV_PREFIX, PROV_PREFIX_LEN) == 0) {
                result = process_provision_line(line_buf);
                break;  /* done regardless of success/fail */
            }
            /* Not a PROVISION: line — reset and keep waiting */
            line_pos = 0;
            continue;
        }

        if (line_pos < PROV_LINE_MAX - 1) {
            line_buf[line_pos++] = (char)ch;
        } else {
            /* Line too long — discard and restart */
            ESP_LOGW(TAG, "Line too long, discarding");
            line_pos = 0;
        }
    }

    /* Remove driver so normal UART logging can resume */
    uart_driver_delete(UART_NUM_0);

    if (!result) {
        ESP_LOGI(TAG, "Provisioning window closed (no valid provisioning received)");
    }
    return result;
}
