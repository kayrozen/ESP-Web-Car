/**
 * @file bluetooth.c
 * @brief Bluetooth Classic A2DP source with GAP device discovery.
 *
 * Task affinity: BT controller runs on Core 0 by default.
 * The A2DP data callback pulls PCM from audio_pipeline's pcm_ringbuf.
 * On underrun it outputs silence (zeros) to keep the A2DP stream alive.
 */

#include "bluetooth.h"
#include "config.h"
#include "audio_pipeline.h"
#include "avrcp.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_bt_device.h"

static const char *TAG = "bluetooth";

/* ── State ───────────────────────────────────────────────────────────── */

static bool              s_bt_initialized  = false;
static bool              s_a2dp_started    = false;
static volatile bool     s_connected       = false;

/* GAP scan results */
static bt_device_t       s_scan_results[CARRADIO_BT_MAX_DEVICES];
static int               s_scan_count      = 0;
static SemaphoreHandle_t s_scan_done_sem   = NULL;
static bool              s_scanning        = false;

/* ── A2DP data callback ──────────────────────────────────────────────── */

static int32_t bt_a2d_data_cb(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) return 0;

    RingbufHandle_t pcm_buf = audio_pipeline_get_pcm_ringbuf();
    if (!pcm_buf) {
        memset(data, 0, len);
        return len;
    }

    size_t received = 0;
    void *item = xRingbufferReceiveUpTo(pcm_buf, &received, 0, (size_t)len);

    if (!item || received == 0) {
        /* Underrun: output silence */
        memset(data, 0, len);
        return len;
    }

    memcpy(data, item, received);
    vRingbufferReturnItem(pcm_buf, item);

    /* Pad with silence if we got less than requested */
    if ((int32_t)received < len) {
        memset(data + received, 0, len - received);
    }

    return len;
}

/* ── A2DP callbacks ──────────────────────────────────────────────────── */

static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                s_connected = true;
                ESP_LOGI(TAG, "A2DP connected");
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                s_connected = false;
                ESP_LOGW(TAG, "A2DP disconnected");
            }
            break;

        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "A2DP audio started");
            } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "A2DP audio stopped");
            }
            break;

        case ESP_A2D_AUDIO_CFG_EVT:
            ESP_LOGI(TAG, "A2DP audio config");
            break;

        default:
            ESP_LOGD(TAG, "A2DP event %d", event);
            break;
    }
}

/* ── GAP callbacks ───────────────────────────────────────────────────── */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            if (!s_scanning || s_scan_count >= CARRADIO_BT_MAX_DEVICES) break;

            bt_device_t *dev = &s_scan_results[s_scan_count];
            memcpy(dev->mac, param->disc_res.bda, 6);
            snprintf(dev->mac_str, sizeof(dev->mac_str),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     dev->mac[0], dev->mac[1], dev->mac[2],
                     dev->mac[3], dev->mac[4], dev->mac[5]);
            dev->rssi = 0;
            strlcpy(dev->name, "Unknown", sizeof(dev->name));

            /* Try to find device name in EIR */
            for (int i = 0; i < param->disc_res.num_prop; i++) {
                esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
                if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->len > 0) {
                    size_t copy_len = (p->len < sizeof(dev->name) - 1)
                                     ? p->len : sizeof(dev->name) - 1;
                    memcpy(dev->name, p->val, copy_len);
                    dev->name[copy_len] = '\0';
                } else if (p->type == ESP_BT_GAP_DEV_PROP_RSSI && p->len == 1) {
                    dev->rssi = (int)((int8_t *)p->val)[0];
                }
            }

            ESP_LOGI(TAG, "Discovered: %s [%s] rssi=%d",
                     dev->name, dev->mac_str, dev->rssi);
            s_scan_count++;
            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                ESP_LOGI(TAG, "GAP scan complete, found %d devices", s_scan_count);
                s_scanning = false;
                if (s_scan_done_sem)
                    xSemaphoreGive(s_scan_done_sem);
            }
            break;

        default:
            break;
    }
}

/* ── Initialisation ──────────────────────────────────────────────────── */

esp_err_t bluetooth_init(void)
{
    if (s_bt_initialized) return ESP_OK;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_register_callback(gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap_register_callback failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_bt_dev_set_device_name("CarRadio");

    /* Set discoverability off — we're the source, not the target */
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    s_bt_initialized = true;
    ESP_LOGI(TAG, "Bluetooth initialized");
    return ESP_OK;
}

esp_err_t bluetooth_a2dp_start(void)
{
    if (s_a2dp_started) return ESP_OK;
    if (!s_bt_initialized) {
        esp_err_t err = bluetooth_init();
        if (err != ESP_OK) return err;
    }

    esp_err_t err = esp_a2d_register_callback(a2dp_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2dp_register_callback failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_a2d_source_register_data_callback(bt_a2d_data_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2dp_source_register_data_callback failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2dp_source_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_a2dp_started = true;
    ESP_LOGI(TAG, "A2DP source started");
    return ESP_OK;
}

/* ── GAP scan ────────────────────────────────────────────────────────── */

esp_err_t bluetooth_gap_scan(bt_device_t *out, int max_count, int *out_count)
{
    if (!s_bt_initialized) {
        esp_err_t err = bluetooth_init();
        if (err != ESP_OK) return err;
    }

    if (!s_scan_done_sem)
        s_scan_done_sem = xSemaphoreCreateBinary();

    memset(s_scan_results, 0, sizeof(s_scan_results));
    s_scan_count = 0;
    s_scanning   = true;

    esp_err_t err = esp_bt_gap_start_discovery(
        ESP_BT_INQ_MODE_GENERAL_INQUIRY,
        CARRADIO_BT_SCAN_SECS,  /* duration in 1.28s units — pass seconds directly */
        0 /* unlimited results */
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap_start_discovery failed: %s", esp_err_to_name(err));
        s_scanning = false;
        return err;
    }

    /* Wait for scan completion (timeout = scan duration + 2s) */
    xSemaphoreTake(s_scan_done_sem,
                   pdMS_TO_TICKS((CARRADIO_BT_SCAN_SECS * 1280) + 2000));

    int copy_count = (s_scan_count < max_count) ? s_scan_count : max_count;
    memcpy(out, s_scan_results, copy_count * sizeof(bt_device_t));
    *out_count = copy_count;
    return ESP_OK;
}

/* ── Connect ─────────────────────────────────────────────────────────── */

esp_err_t bluetooth_connect(const uint8_t mac[6])
{
    return bluetooth_a2dp_connect(mac);
}

esp_err_t bluetooth_a2dp_connect(const uint8_t mac[6])
{
    if (!s_a2dp_started) {
        esp_err_t err = bluetooth_a2dp_start();
        if (err != ESP_OK) return err;
    }

    ESP_LOGI(TAG, "Connecting A2DP to %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_err_t err = esp_a2d_source_connect((uint8_t *)mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2dp_source_connect failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait up to 10s for connection */
    for (int i = 0; i < 100; i++) {
        if (s_connected) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "A2DP connect timeout");
    return ESP_ERR_TIMEOUT;
}

bool bluetooth_is_connected(void)
{
    return s_connected;
}

void bluetooth_disconnect(void)
{
    if (s_connected) {
        /* esp_a2d_source_disconnect needs the remote address — not tracked here.
           A reboot will cleanly disconnect. */
        s_connected = false;
    }
}
