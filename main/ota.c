/**
 * @file ota.c
 * @brief A/B OTA firmware update over HTTP/HTTPS.
 *
 * Uses esp_https_ota (which wraps esp_http_client) to download the firmware
 * binary to the inactive OTA partition.  Verifies the image before marking
 * it for boot, then reboots.
 *
 * The task watchdog is temporarily unsubscribed during the write so that
 * large flash write operations do not trigger a reset.
 */

#include "ota.h"
#include "config.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_task_wdt.h"
#include "esp_app_desc.h"

static const char *TAG = "ota";

/* ── OTA progress callback ───────────────────────────────────────────── */

static void ota_http_event_handler(esp_http_client_event_t *evt)
{
    (void)evt;  /* We rely on esp_https_ota for progress tracking */
}

/* ── Perform OTA ─────────────────────────────────────────────────────── */

esp_err_t ota_perform_update(const char *url)
{
    if (!url || strlen(url) == 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    /* Unsubscribe from WDT during OTA — flash writes can be slow */
    esp_task_wdt_delete(NULL);

    esp_http_client_config_t http_cfg = {
        .url              = url,
        .timeout_ms       = 30000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        esp_task_wdt_add(NULL);
        return err;
    }

    /* Fetch the app description to log version info */
    esp_app_desc_t new_app_info;
    if (esp_https_ota_get_img_desc(ota_handle, &new_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
    }

    /* Download and write in a loop */
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        ESP_LOGD(TAG, "OTA progress: %d bytes",
                 esp_https_ota_get_image_len_read(ota_handle));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        esp_task_wdt_add(NULL);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA: incomplete data received");
        esp_https_ota_abort(ota_handle);
        esp_task_wdt_add(NULL);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded — rebooting to new firmware");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        /* Does not return */
    } else if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGE(TAG, "OTA image validation failed");
    } else {
        ESP_LOGE(TAG, "esp_https_ota_finish error: %s", esp_err_to_name(err));
    }

    esp_task_wdt_add(NULL);
    return err;
}

/* ── Mark valid ──────────────────────────────────────────────────────── */

esp_err_t ota_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "App marked valid — rollback cancelled");
    }
    return err;
}

/* ── Version string ──────────────────────────────────────────────────── */

const char *ota_get_app_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}
