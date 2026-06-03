#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "config.h"
#include "storage.h"
#include "supervisor.h"
#include "device_identity.h"
#include "provisioning_serial.h"
#include "led_status.h"

static const char *TAG = "main";

static void check_reset_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CARRADIO_RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    if (gpio_get_level(CARRADIO_RESET_GPIO) == 0) {
        ESP_LOGW(TAG, "BOOT button held — waiting %d ms for reset confirmation",
                 CARRADIO_RESET_HOLD_MS);
        vTaskDelay(pdMS_TO_TICKS(CARRADIO_RESET_HOLD_MS));
        if (gpio_get_level(CARRADIO_RESET_GPIO) == 0) {
            ESP_LOGW(TAG, "Factory reset triggered — erasing config");
            led_status_set(LED_STATE_FACTORY_RESET);
            vTaskDelay(pdMS_TO_TICKS(1500)); /* let the burst play out */
            storage_erase_all();
            esp_restart();
        }
    }
}

static void apply_boot_fail_guard(void)
{
    uint8_t fail_count = storage_get_boot_fail_count();
    fail_count++;
    storage_set_boot_fail_count(fail_count);
    ESP_LOGI(TAG, "boot_fail_count = %u (max %u)", fail_count, CARRADIO_BOOT_FAIL_MAX);

    if (fail_count > CARRADIO_BOOT_FAIL_MAX) {
        ESP_LOGE(TAG, "Too many failed boots — falling back to Phase 1 portal");
        led_status_set(LED_STATE_ERROR_LOOP);
        vTaskDelay(pdMS_TO_TICKS(6000)); /* one full SOS cycle visible */
        storage_reset_phase();
        storage_set_boot_fail_count(0);
    }
}

void app_main(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", reason);

    /* Serial provisioning window: must run before WiFi/BT init.
       nvs_flash_init() is needed first so NVS writes succeed. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash init failed (%s) — erasing", esp_err_to_name(ret));
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(storage_init());
    led_status_init();
    led_status_set(LED_STATE_BOOT);
    device_identity_init();   /* load/generate identity; no WiFi needed */

    /* Wait for serial provisioning (up to 30s). Must run before WiFi/BT. */
    provisioning_serial_wait();

    check_reset_button();
    apply_boot_fail_guard();

    ESP_ERROR_CHECK(supervisor_start());

    /* supervisor_start() creates a task and returns; app_main can exit */
    vTaskDelete(NULL);
}
