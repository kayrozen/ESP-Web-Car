/**
 * @file http_stream.c
 * @brief HTTP audio stream fetcher with M3U/PLS playlist resolution,
 *        format detection, and automatic reconnection with backoff.
 *
 * Runs on Core 1.  Writes compressed audio data to raw_ringbuf.
 */

#include "http_stream.h"
#include "config.h"
#include "audio_pipeline.h"
#include "supervisor.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"

static const char *TAG = "http_stream";

static volatile audio_format_t s_format       = AUDIO_FORMAT_UNKNOWN;
static volatile bool           s_running       = false;
static TaskHandle_t            s_task_handle   = NULL;
static char                    s_url[STORAGE_URL_MAX_HTTP];

#define STORAGE_URL_MAX_HTTP  CARRADIO_HTTP_BUF_SIZE

/* ── Format detection ────────────────────────────────────────────────── */

static audio_format_t detect_format_from_content_type(const char *ct)
{
    if (!ct) return AUDIO_FORMAT_UNKNOWN;
    if (strcasestr(ct, "mpeg"))   return AUDIO_FORMAT_MP3;
    if (strcasestr(ct, "mp3"))    return AUDIO_FORMAT_MP3;
    if (strcasestr(ct, "aac"))    return AUDIO_FORMAT_AAC;
    if (strcasestr(ct, "mp4"))    return AUDIO_FORMAT_AAC;
    if (strcasestr(ct, "m4a"))    return AUDIO_FORMAT_AAC;
    return AUDIO_FORMAT_UNKNOWN;
}

static audio_format_t detect_format_from_sync(const uint8_t *buf, int len)
{
    if (len < 4) return AUDIO_FORMAT_UNKNOWN;

    /* MP3: sync word 0xFFEx or 0xFFFx */
    if (buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0)
        return AUDIO_FORMAT_MP3;

    /* AAC ADTS: sync word 0xFFF */
    if (buf[0] == 0xFF && (buf[1] & 0xF0) == 0xF0)
        return AUDIO_FORMAT_AAC;

    return AUDIO_FORMAT_UNKNOWN;
}

/* ── Playlist resolution ─────────────────────────────────────────────── */

/* Returns true and fills resolved_url if the response looks like an M3U/PLS playlist */
static bool resolve_playlist(const char *body, int len,
                              char *resolved_url, size_t url_size)
{
    /* PLS: look for File1= */
    const char *file1 = strcasestr(body, "File1=");
    if (file1) {
        file1 += 6;
        const char *end = strpbrk(file1, "\r\n");
        size_t copy_len = end ? (size_t)(end - file1) : strlen(file1);
        if (copy_len > 0 && copy_len < url_size) {
            memcpy(resolved_url, file1, copy_len);
            resolved_url[copy_len] = '\0';
            return true;
        }
    }

    /* M3U: first non-comment line starting with http */
    const char *p = body;
    while (p && p < body + len) {
        /* Skip comment/directive lines */
        if (*p == '#' || *p == '\r' || *p == '\n') {
            p = strpbrk(p, "\n");
            if (p) p++;
            continue;
        }
        /* Check if it's a URL */
        if (strncmp(p, "http", 4) == 0) {
            const char *end = strpbrk(p, "\r\n");
            size_t copy_len = end ? (size_t)(end - p) : strlen(p);
            if (copy_len > 0 && copy_len < url_size) {
                memcpy(resolved_url, p, copy_len);
                resolved_url[copy_len] = '\0';
                return true;
            }
        }
        p = strpbrk(p, "\n");
        if (p) p++;
    }
    return false;
}

/* ── HTTP event handler ──────────────────────────────────────────────── */

typedef struct {
    RingbufHandle_t raw_buf;
    audio_format_t  detected_format;
    bool            format_detected;
    bool            is_playlist;
    char            playlist_body[2048];
    int             playlist_len;
} stream_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    stream_ctx_t *ctx = (stream_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER: {
            if (strcasecmp(evt->header_key, "Content-Type") == 0) {
                /* Detect format from Content-Type */
                audio_format_t fmt = detect_format_from_content_type(evt->header_value);
                if (fmt != AUDIO_FORMAT_UNKNOWN) {
                    ctx->detected_format = fmt;
                    ctx->format_detected = true;
                }
                /* Detect if this is a playlist */
                if (strcasestr(evt->header_value, "mpegurl") ||
                    strcasestr(evt->header_value, "scpls") ||
                    strcasestr(evt->header_value, "x-pls")) {
                    ctx->is_playlist = true;
                }
            }
            break;
        }

        case HTTP_EVENT_ON_DATA: {
            if (evt->data_len <= 0) break;
            uint8_t *data = (uint8_t *)evt->data;

            if (ctx->is_playlist) {
                /* Buffer the playlist body */
                int copy = evt->data_len;
                if (ctx->playlist_len + copy >= (int)sizeof(ctx->playlist_body) - 1)
                    copy = sizeof(ctx->playlist_body) - 1 - ctx->playlist_len;
                if (copy > 0) {
                    memcpy(ctx->playlist_body + ctx->playlist_len, data, copy);
                    ctx->playlist_len += copy;
                    ctx->playlist_body[ctx->playlist_len] = '\0';
                }
                break;
            }

            /* Detect format from first bytes */
            if (!ctx->format_detected) {
                ctx->detected_format = detect_format_from_sync(data, evt->data_len);
                if (ctx->detected_format != AUDIO_FORMAT_UNKNOWN)
                    ctx->format_detected = true;
            }

            /* Write to raw ring buffer */
            if (ctx->raw_buf) {
                BaseType_t ret = xRingbufferSend(ctx->raw_buf, data, evt->data_len,
                                                  pdMS_TO_TICKS(200));
                if (ret != pdTRUE) {
                    ESP_LOGD(TAG, "raw_buf full, dropping %d bytes", evt->data_len);
                }
            }
            break;
        }

        default:
            break;
    }
    return ESP_OK;
}

/* ── Stream task ─────────────────────────────────────────────────────── */

static void http_stream_task(void *arg)
{
    char url[256];
    strlcpy(url, (char *)arg, sizeof(url));
    free(arg);

    uint32_t backoff = 0;

    while (s_running) {
        stream_ctx_t ctx = {
            .raw_buf         = audio_pipeline_get_raw_ringbuf(),
            .detected_format = AUDIO_FORMAT_UNKNOWN,
            .format_detected = false,
            .is_playlist     = false,
            .playlist_len    = 0,
        };

        /* Detect playlist from URL extension */
        if (strcasestr(url, ".m3u") || strcasestr(url, ".pls"))
            ctx.is_playlist = true;

        esp_http_client_config_t http_cfg = {
            .url            = url,
            .event_handler  = http_event_handler,
            .user_data      = &ctx,
            .timeout_ms     = CARRADIO_HTTP_TIMEOUT_MS,
            .buffer_size    = CARRADIO_HTTP_BUF_SIZE,
            .keep_alive_enable = true,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) {
            ESP_LOGE(TAG, "http_client_init failed");
            goto retry;
        }

        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP %d, format=%d", status, ctx.detected_format);

            if (ctx.is_playlist && ctx.playlist_len > 0) {
                char resolved[256] = {0};
                if (resolve_playlist(ctx.playlist_body, ctx.playlist_len,
                                     resolved, sizeof(resolved))) {
                    ESP_LOGI(TAG, "Playlist resolved to: %s", resolved);
                    strlcpy(url, resolved, sizeof(url));
                    esp_http_client_cleanup(client);
                    backoff = 0;
                    continue;
                }
            }

            if (ctx.format_detected) {
                s_format = ctx.detected_format;
            }

            backoff = supervisor_backoff_reset();
        } else {
            ESP_LOGW(TAG, "HTTP fetch error: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);

retry:
        if (!s_running) break;
        backoff = supervisor_backoff_next(backoff);
        ESP_LOGI(TAG, "Reconnecting in %u ms", (unsigned)backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }

    s_running = false;
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t http_stream_start(const char *url)
{
    if (s_running) return ESP_OK;

    char *url_copy = strdup(url);
    if (!url_copy) return ESP_ERR_NO_MEM;

    s_running = true;
    s_format  = AUDIO_FORMAT_UNKNOWN;

    BaseType_t ret = xTaskCreatePinnedToCore(
        http_stream_task, "http_stream",
        TASK_STACK_HTTP_STREAM, url_copy,
        TASK_PRIO_HTTP_STREAM, &s_task_handle,
        1   /* Core 1 */
    );

    if (ret != pdPASS) {
        s_running = false;
        free(url_copy);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void http_stream_stop(void)
{
    s_running = false;
    /* Task will exit on next loop iteration */
}

audio_format_t http_stream_get_format(void)
{
    return s_format;
}

bool http_stream_is_running(void)
{
    return s_running;
}
