#include "stats_monitor.h"
#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "stats";

static stats_snapshot_t s_snap;
static SemaphoreHandle_t s_mutex;

void stats_record_glitch(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_snap.audio_glitches++;
    xSemaphoreGive(s_mutex);
}

void stats_record_reconnect(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_snap.reconnects++;
    xSemaphoreGive(s_mutex);
}

void stats_get(stats_snapshot_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_snap, sizeof(*out));
    xSemaphoreGive(s_mutex);
}

static void stats_task(void *arg)
{
    /*
     * FreeRTOS run-time stats require CONFIG_FREERTOS_USE_TRACE_FACILITY=y
     * and CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y (set in sdkconfig.defaults).
     *
     * We read idle task counters directly: the idle task name is "IDLE0" / "IDLE1".
     * This is a lightweight proxy for CPU load without a second pass through all tasks.
     */
    const TickType_t interval = pdMS_TO_TICKS(STATS_INTERVAL_MS);
    static char buf[2048];

    while (1) {
        vTaskDelay(interval);

        uint32_t free_int  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t free_spi  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t min_int   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        uint32_t min_spi   = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        uint64_t now_ms    = esp_timer_get_time() / 1000ULL;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_snap.free_internal_bytes     = free_int;
        s_snap.free_spiram_bytes       = free_spi;
        s_snap.min_free_internal_bytes = min_int;
        s_snap.min_free_spiram_bytes   = min_spi;
        s_snap.uptime_ms               = now_ms;
        xSemaphoreGive(s_mutex);

        ESP_LOGI(TAG, "--- STATS @ %llus ---", now_ms / 1000);
        ESP_LOGI(TAG, "  RAM internal : %7lu B free  (min %lu B)",
                 (unsigned long)free_int, (unsigned long)min_int);
        ESP_LOGI(TAG, "  RAM SPIRAM   : %7lu B free  (min %lu B)",
                 (unsigned long)free_spi, (unsigned long)min_spi);
        ESP_LOGI(TAG, "  Glitches: %lu   Reconnects: %lu",
                 (unsigned long)s_snap.audio_glitches,
                 (unsigned long)s_snap.reconnects);

        /* Print per-task CPU stats for Phase C analysis */
        vTaskGetRunTimeStats(buf);
        ESP_LOGD(TAG, "CPU:\n%s", buf);
    }
}

void stats_monitor_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_snap, 0, sizeof(s_snap));
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 1, NULL, 1);
}
