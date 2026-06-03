#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"

/* ESP-ADF */
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "audio_decoder.h"   /* auto-detect decoder */
#include "rsp_filter.h"      /* resampler */
#include "a2dp_stream.h"
#include "bluetooth_service.h"
#include "esp_peripherals.h"

#include "app_config.h"
#include "station_list.h"
#include "stats_monitor.h"

static const char *TAG = "adf_proto";

/* ------------------------------------------------------------------ */
/*  WiFi                                                               */
/* ------------------------------------------------------------------ */

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d", s_retry, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h2));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected to SSID: %s", WIFI_SSID);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi connection FAILED");
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  Pipeline state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t  http_stream;
    audio_element_handle_t  decoder;
    audio_element_handle_t  resample;
    audio_element_handle_t  bt_stream;
    audio_event_iface_handle_t evt;
    int                     station_idx;
    bool                    bt_connected;
    int64_t                 stream_start_us;
} pipeline_ctx_t;

static pipeline_ctx_t s_ctx;

/* ------------------------------------------------------------------ */
/*  Bluetooth callbacks                                                 */
/* ------------------------------------------------------------------ */

static void bt_app_av_state_cb(esp_a2d_connection_state_t state,
                                void *pdata)
{
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        ESP_LOGI(TAG, "A2DP CONNECTED");
        s_ctx.bt_connected = true;
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "A2DP DISCONNECTED");
        s_ctx.bt_connected = false;
    }
}

/* ------------------------------------------------------------------ */
/*  Pipeline build / teardown                                           */
/* ------------------------------------------------------------------ */

static void pipeline_build(pipeline_ctx_t *ctx, int station_idx)
{
    const station_t *st = &g_stations[station_idx];
    ESP_LOGI(TAG, "Building pipeline for: %s", st->name);
    ESP_LOGI(TAG, "  URL: %s", st->url);

    /* --- HTTP stream --- */
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type                   = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = true;   /* handles HLS m3u8 */
    ctx->http_stream = http_stream_init(&http_cfg);

    /* --- Auto decoder (MP3 / AAC selected by Content-Type / header) --- */
    audio_decoder_cfg_t dec_cfg = DEFAULT_AUDIO_DECODER_CONFIG();
    ctx->decoder = audio_decoder_init(&dec_cfg);

    /* --- Resampler → 44100 Hz stereo for A2DP --- */
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.dest_rate    = RSP_OUT_SAMPLE_RATE;
    rsp_cfg.dest_ch      = RSP_OUT_CHANNELS;
    ctx->resample = rsp_filter_init(&rsp_cfg);

    /* --- A2DP source stream --- */
    a2dp_stream_cfg_t a2dp_cfg = A2DP_STREAM_CFG_DEFAULT();
    a2dp_cfg.type = AUDIO_STREAM_WRITER;
    ctx->bt_stream = a2dp_stream_init(&a2dp_cfg);

    /* --- Pipeline --- */
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    ctx->pipeline = audio_pipeline_init(&pipeline_cfg);

    audio_pipeline_register(ctx->pipeline, ctx->http_stream, "http");
    audio_pipeline_register(ctx->pipeline, ctx->decoder,     "dec");
    audio_pipeline_register(ctx->pipeline, ctx->resample,    "rsp");
    audio_pipeline_register(ctx->pipeline, ctx->bt_stream,   "bt");

    const char *links[] = {"http", "dec", "rsp", "bt"};
    audio_pipeline_link(ctx->pipeline, links, 4);

    audio_element_set_uri(ctx->http_stream, st->url);

    /* --- Event interface --- */
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    ctx->evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(ctx->pipeline, ctx->evt);

    ctx->station_idx    = station_idx;
    ctx->stream_start_us = esp_timer_get_time();
}

static void pipeline_start(pipeline_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Starting pipeline");
    audio_pipeline_run(ctx->pipeline);
}

static void pipeline_stop_and_destroy(pipeline_ctx_t *ctx)
{
    audio_pipeline_stop(ctx->pipeline);
    audio_pipeline_wait_for_stop(ctx->pipeline);
    audio_pipeline_terminate(ctx->pipeline);

    audio_pipeline_unregister(ctx->pipeline, ctx->http_stream);
    audio_pipeline_unregister(ctx->pipeline, ctx->decoder);
    audio_pipeline_unregister(ctx->pipeline, ctx->resample);
    audio_pipeline_unregister(ctx->pipeline, ctx->bt_stream);

    audio_event_iface_destroy(ctx->evt);
    audio_pipeline_deinit(ctx->pipeline);

    audio_element_deinit(ctx->http_stream);
    audio_element_deinit(ctx->decoder);
    audio_element_deinit(ctx->resample);
    audio_element_deinit(ctx->bt_stream);
}

/* ------------------------------------------------------------------ */
/*  Station switch (Phase D test)                                       */
/* ------------------------------------------------------------------ */

void switch_station(int new_idx)
{
    ESP_LOGI(TAG, "Switching to station %d: %s",
             new_idx, g_stations[new_idx].name);
    stats_record_reconnect();
    pipeline_stop_and_destroy(&s_ctx);
    pipeline_build(&s_ctx, new_idx);
    pipeline_start(&s_ctx);
}

/* ------------------------------------------------------------------ */
/*  Event loop                                                          */
/* ------------------------------------------------------------------ */

static void run_event_loop(pipeline_ctx_t *ctx)
{
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(ctx->evt, &msg,
                                                  pdMS_TO_TICKS(5000));
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }

        /* Pipeline finished (end of stream or error) */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)ctx->http_stream &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {

            audio_element_state_t st = (audio_element_state_t)msg.data;
            if (st == AEL_STATE_FINISHED || st == AEL_STATE_ERROR) {
                ESP_LOGW(TAG, "HTTP stream ended (state=%d) — reconnecting", st);
                stats_record_reconnect();

                int64_t start_us = esp_timer_get_time();
                audio_pipeline_stop(ctx->pipeline);
                audio_pipeline_wait_for_stop(ctx->pipeline);
                audio_pipeline_reset_ringbuffer(ctx->pipeline);
                audio_pipeline_reset_elements(ctx->pipeline);
                audio_element_set_uri(ctx->http_stream,
                                      g_stations[ctx->station_idx].url);
                audio_pipeline_run(ctx->pipeline);

                ESP_LOGI(TAG, "Reconnect done in %lld ms",
                         (esp_timer_get_time() - start_us) / 1000);
            }
        }

        /* Under-run — audio glitch */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)ctx->bt_stream &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            audio_element_state_t st = (audio_element_state_t)msg.data;
            if (st == AEL_STATE_RUNNING) {
                /* first sound: log latency */
                int64_t latency_ms =
                    (esp_timer_get_time() - ctx->stream_start_us) / 1000;
                ESP_LOGI(TAG, ">>> FIRST SOUND latency: %lld ms", latency_ms);
            }
        }

        /* A2DP connection events come through bluetooth_service */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_SERVICE &&
            msg.cmd == BT_APP_EVT) {
            /* logged in bt_app_av_state_cb */
        }
    }
}

/* ------------------------------------------------------------------ */
/*  app_main                                                            */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP-ADF Prototype — Phase A/B/C/D ===");
    ESP_LOGI(TAG, "Station count: %d", STATION_COUNT);

    /* NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Stats monitor starts early so we capture baseline RAM */
    stats_monitor_start();

    /* WiFi — must be up before pipeline initialises http_stream */
    ESP_ERROR_CHECK(wifi_init_sta());

    /* Bluetooth — initialise A2DP source service */
    bluetooth_service_cfg_t bt_cfg = {
        .device_name = "ESP-ADF-Prototype",
        .mode        = BLUETOOTH_A2DP_SOURCE,
        .remote_name = BT_SINK_NAME,
    };
    ESP_ERROR_CHECK(bluetooth_service_start(&bt_cfg));
    ESP_ERROR_CHECK(bluetooth_service_connect());

    /* Wait for BT to connect (up to 15 s, then proceed anyway for testing) */
    int wait = 0;
    while (!s_ctx.bt_connected && wait < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }
    if (!s_ctx.bt_connected) {
        ESP_LOGW(TAG, "BT not yet connected — starting pipeline anyway");
    }

    /* Build and start the pipeline at startup station (MP3 Icecast first) */
    pipeline_build(&s_ctx, STATION_IDX_MP3_ICECAST);
    pipeline_start(&s_ctx);

    /* Main event loop — runs indefinitely */
    run_event_loop(&s_ctx);
}
