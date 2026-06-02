#include "supervisor.h"
#include "config.h"
#include "storage.h"
#include "wifi.h"
#include "bluetooth.h"
#include "audio_pipeline.h"
#include "avrcp.h"
#include "portal_phase1.h"
#include "portal_phase2.h"
#include "ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"

static const char *TAG = "supervisor";

static volatile bool s_good_boot_confirmed = false;
static volatile bool s_reboot_requested    = false;

/* ── Playback state ──────────────────────────────────────────────────── */

#define SOFT_PAUSE_ESCALATE_MS  (30 * 1000)

static volatile playback_state_t s_playback_state = PLAYBACK_STATE_PLAYING;
static volatile avrc_cmd_t       s_pending_cmd;
static volatile bool             s_has_pending_cmd = false;
static TimerHandle_t             s_soft_pause_timer = NULL;

static void soft_pause_timer_cb(TimerHandle_t xTimer)
{
    /* Escalate soft → hard pause after 30s */
    if (s_playback_state == PLAYBACK_STATE_SOFT_PAUSED) {
        ESP_LOGI(TAG, "Soft-pause timeout — escalating to hard pause");
        s_playback_state = PLAYBACK_STATE_HARD_PAUSED;
        audio_pipeline_hard_pause();
        avrcp_publish_playback_status(PLAYBACK_STATE_HARD_PAUSED);
    }
}

static void set_playback_state(playback_state_t new_state)
{
    playback_state_t old = s_playback_state;
    if (old == new_state) return;

    s_playback_state = new_state;

    switch (new_state) {
        case PLAYBACK_STATE_PLAYING:
            if (old == PLAYBACK_STATE_SOFT_PAUSED) {
                xTimerStop(s_soft_pause_timer, 0);
                audio_pipeline_resume_soft();
            } else if (old == PLAYBACK_STATE_HARD_PAUSED) {
                audio_pipeline_resume_hard();
            }
            break;

        case PLAYBACK_STATE_SOFT_PAUSED:
            audio_pipeline_soft_pause();
            xTimerReset(s_soft_pause_timer, 0);
            break;

        case PLAYBACK_STATE_HARD_PAUSED:
            if (s_soft_pause_timer) xTimerStop(s_soft_pause_timer, 0);
            audio_pipeline_hard_pause();
            break;
    }

    avrcp_publish_playback_status(new_state);
    ESP_LOGI(TAG, "Playback state: %d → %d", old, new_state);
}

void supervisor_avrcp_command(avrc_cmd_t cmd)
{
    /* Called from BT stack context; post to the supervisor main loop */
    s_pending_cmd     = cmd;
    s_has_pending_cmd = true;
}

playback_state_t supervisor_get_playback_state(void)
{
    return s_playback_state;
}

/* ── Backoff ─────────────────────────────────────────────────────────── */

uint32_t supervisor_backoff_next(uint32_t current_ms)
{
    if (current_ms == 0) return CARRADIO_BACKOFF_INIT_MS;
    uint32_t next = current_ms * 2;
    return (next > CARRADIO_BACKOFF_MAX_MS) ? CARRADIO_BACKOFF_MAX_MS : next;
}

uint32_t supervisor_backoff_reset(void)
{
    return CARRADIO_BACKOFF_INIT_MS;
}

void supervisor_confirm_good_boot(void)
{
    if (!s_good_boot_confirmed) {
        s_good_boot_confirmed = true;
        storage_set_boot_fail_count(0);
        ota_mark_valid();
        ESP_LOGI(TAG, "Good boot confirmed — OTA rollback guard cleared");
    }
}

void supervisor_request_reboot(void)
{
    s_reboot_requested = true;
}

/* ── Heap health check ───────────────────────────────────────────────── */

#define HEAP_LOW_WATER_BYTES  (32 * 1024)

static void check_heap_health(void)
{
    size_t free = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (free < HEAP_LOW_WATER_BYTES || largest < (HEAP_LOW_WATER_BYTES / 2)) {
        ESP_LOGE(TAG, "Heap low (free=%u largest=%u) — controlled restart",
                 (unsigned)free, (unsigned)largest);
        esp_restart();
    }
}

/* ── Phase runners ───────────────────────────────────────────────────── */

static void run_phase1(void)
{
    ESP_LOGI(TAG, "Entering PHASE 1 — WiFi + URL portal (SoftAP)");
    wifi_start_softap();
    portal_phase1_start();
    /* Blocks until the user submits credentials and device reboots */
    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void run_phase2(void)
{
    ESP_LOGI(TAG, "Entering PHASE 2 — Bluetooth pairing portal (STA)");

    uint32_t backoff = 0;
    uint32_t total_wait = 0;

    /* Connect to the saved hotspot with backoff */
    while (wifi_connect_sta() != ESP_OK) {
        esp_task_wdt_reset();
        backoff = supervisor_backoff_next(backoff);
        total_wait += backoff;
        if (total_wait >= CARRADIO_FAIL_REBOOT_MS) {
            ESP_LOGE(TAG, "Phase 2 WiFi connect failed after 5 min — back to Phase 1");
            storage_reset_phase();
            esp_restart();
        }
        ESP_LOGW(TAG, "WiFi connect failed — retry in %u ms", (unsigned)backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }

    bluetooth_init();
    portal_phase2_start();

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void run_streaming(void)
{
    ESP_LOGI(TAG, "Entering STREAMING mode");

    /* AVRCP must init before A2DP */
    avrcp_init();

    s_soft_pause_timer = xTimerCreate("soft_pause", pdMS_TO_TICKS(SOFT_PAUSE_ESCALATE_MS),
                                       pdFALSE, NULL, soft_pause_timer_cb);

    /* BT before WiFi — see §8 of design doc */
    bluetooth_init();
    bluetooth_a2dp_start();

    uint32_t wifi_backoff = 0, bt_backoff = 0;
    uint32_t wifi_fail_ms = 0, bt_fail_ms = 0;

    /* Connect WiFi with backoff */
    while (wifi_connect_sta() != ESP_OK) {
        esp_task_wdt_reset();
        wifi_backoff = supervisor_backoff_next(wifi_backoff);
        wifi_fail_ms += wifi_backoff;
        if (wifi_fail_ms >= CARRADIO_FAIL_REBOOT_MS) {
            ESP_LOGE(TAG, "WiFi connect failed — rebooting");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(wifi_backoff));
    }
    wifi_backoff = 0; wifi_fail_ms = 0;

    /* Connect A2DP with backoff */
    uint8_t mac[6];
    if (storage_get_bt_mac_bytes(mac) != ESP_OK) {
        ESP_LOGE(TAG, "No BT MAC saved — falling back to Phase 1");
        storage_reset_phase();
        esp_restart();
    }
    while (bluetooth_a2dp_connect(mac) != ESP_OK) {
        esp_task_wdt_reset();
        bt_backoff = supervisor_backoff_next(bt_backoff);
        bt_fail_ms += bt_backoff;
        if (bt_fail_ms >= CARRADIO_FAIL_REBOOT_MS) {
            ESP_LOGE(TAG, "BT connect failed — rebooting");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(bt_backoff));
    }

    char stream_url[STORAGE_URL_MAX] = {0};
    storage_get_stream_url(stream_url, sizeof(stream_url));
    audio_pipeline_start(stream_url);

    TickType_t stream_start = xTaskGetTickCount();
    bool good_boot_signalled = false;

    while (1) {
        esp_task_wdt_reset();
        check_heap_health();

        if (s_reboot_requested) {
            ESP_LOGW(TAG, "Reboot requested");
            esp_restart();
        }

        /* Process AVRCP commands from the BT stack */
        if (s_has_pending_cmd) {
            s_has_pending_cmd = false;
            avrc_cmd_t cmd = s_pending_cmd;
            switch (cmd) {
                case AVRC_CMD_PLAY:
                    if (s_playback_state != PLAYBACK_STATE_PLAYING)
                        set_playback_state(PLAYBACK_STATE_PLAYING);
                    break;
                case AVRC_CMD_PAUSE:
                    if (s_playback_state == PLAYBACK_STATE_PLAYING)
                        set_playback_state(PLAYBACK_STATE_SOFT_PAUSED);
                    break;
                case AVRC_CMD_STOP:
                    if (s_playback_state != PLAYBACK_STATE_HARD_PAUSED)
                        set_playback_state(PLAYBACK_STATE_HARD_PAUSED);
                    break;
            }
        }

        /* Good boot: 60s of streaming without a restart */
        if (!good_boot_signalled) {
            TickType_t elapsed = xTaskGetTickCount() - stream_start;
            if (elapsed >= pdMS_TO_TICKS(CARRADIO_GOOD_BOOT_MS)) {
                supervisor_confirm_good_boot();
                good_boot_signalled = true;
            }
        }

        /* Reconnect if WiFi dropped */
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi lost — hard pausing pipeline");
            set_playback_state(PLAYBACK_STATE_HARD_PAUSED);
            wifi_backoff = supervisor_backoff_next(wifi_backoff);
            wifi_fail_ms += wifi_backoff;
            if (wifi_fail_ms >= CARRADIO_FAIL_REBOOT_MS) {
                ESP_LOGE(TAG, "WiFi recovery failed — rebooting");
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(wifi_backoff));
            if (wifi_connect_sta() == ESP_OK) {
                wifi_backoff = 0; wifi_fail_ms = 0;
                set_playback_state(PLAYBACK_STATE_PLAYING);
            }
        } else {
            wifi_backoff = 0; wifi_fail_ms = 0;
        }

        /* Reconnect if BT dropped */
        if (!bluetooth_is_connected()) {
            ESP_LOGW(TAG, "BT lost — hard pausing pipeline");
            set_playback_state(PLAYBACK_STATE_HARD_PAUSED);
            bt_backoff = supervisor_backoff_next(bt_backoff);
            bt_fail_ms += bt_backoff;
            if (bt_fail_ms >= CARRADIO_FAIL_REBOOT_MS) {
                ESP_LOGE(TAG, "BT recovery failed — rebooting");
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(bt_backoff));
            if (bluetooth_a2dp_connect(mac) == ESP_OK) {
                bt_backoff = 0; bt_fail_ms = 0;
                set_playback_state(PLAYBACK_STATE_PLAYING);
            }
        } else {
            bt_backoff = 0; bt_fail_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── Supervisor task ─────────────────────────────────────────────────── */

static void supervisor_task(void *arg)
{
    esp_task_wdt_add(NULL);

    uint8_t phase = storage_get_phase();
    ESP_LOGI(TAG, "Boot phase = %u", phase);

    switch (phase) {
        case CARRADIO_PHASE_WIFI:  run_phase1();     break;
        case CARRADIO_PHASE_BT:    run_phase2();     break;
        case CARRADIO_PHASE_READY: run_streaming();  break;
        default:
            ESP_LOGE(TAG, "Unknown phase %u — resetting to Phase 1", phase);
            storage_reset_phase();
            esp_restart();
    }

    vTaskDelete(NULL);
}

esp_err_t supervisor_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        supervisor_task, "supervisor",
        TASK_STACK_SUPERVISOR, NULL,
        TASK_PRIO_SUPERVISOR, NULL,
        1 /* Core 1 */
    );
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
