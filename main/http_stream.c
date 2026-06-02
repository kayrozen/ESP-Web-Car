/**
 * @file http_stream.c
 * @brief HTTP audio stream fetcher with M3U/PLS playlist resolution,
 *        format detection, ICY in-band metadata parsing, and automatic
 *        reconnection with backoff.
 *
 * ICY metadata flow:
 *   1. Request header "Icy-MetaData: 1" opts in.
 *   2. Response headers icy-metaint / icy-name / icy-genre are parsed.
 *   3. Every icy-metaint bytes a metadata block is spliced in:
 *        1-byte length L; L*16 bytes of "StreamTitle='...';".
 *      The block is stripped before pushing data to raw_ringbuf.
 *   4. On title change, avrcp_publish_metadata() is called.
 */

#include "http_stream.h"
#include "config.h"
#include "audio_pipeline.h"
#include "supervisor.h"
#include "avrcp.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"

static const char *TAG = "http_stream";

static volatile audio_format_t s_format       = AUDIO_FORMAT_UNKNOWN;
static volatile bool           s_running       = false;
static TaskHandle_t            s_task_handle   = NULL;

/* ── Now-playing (shared, mutex-protected) ───────────────────────────── */

#define NOW_PLAYING_MAX 128
static char              s_now_playing[NOW_PLAYING_MAX] = {0};
static SemaphoreHandle_t s_now_playing_mutex            = NULL;

/* ── ICY state ───────────────────────────────────────────────────────── */

typedef struct {
    int  metaint;               /* 0 = no ICY */
    int  bytes_until_meta;
    bool reading_meta;          /* true = in ICY_READ_LEN or ICY_READ_META state */
    int  meta_remaining;
    int  meta_used;
    char meta_buf[256];
    char last_title[128];
    char station_name[64];
    char station_genre[32];
} icy_state_t;

/* ── Title normalisation ─────────────────────────────────────────────── */

static void decode_html_entities(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if (strncmp(r, "&amp;",  5) == 0) { *w++ = '&'; r += 5; }
            else if (strncmp(r, "&apos;", 6) == 0 ||
                     strncmp(r, "&#39;",  5) == 0) { *w++ = '\''; r += (r[1]=='#' ? 5 : 6); }
            else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"';  r += 6; }
            else if (strncmp(r, "&lt;",   4) == 0) { *w++ = '<';  r += 4; }
            else if (strncmp(r, "&gt;",   4) == 0) { *w++ = '>';  r += 4; }
            else { *w++ = *r++; }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void normalize_title(char *s, size_t max_len)
{
    if (!s || !s[0]) return;

    /* Trim leading whitespace */
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    /* Trim trailing whitespace */
    int l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) s[--l] = '\0';

    decode_html_entities(s);

    /* Validate UTF-8: replace bad bytes with '?' */
    unsigned char *u = (unsigned char *)s;
    while (*u) {
        if (*u < 0x80) {
            u++;
        } else if ((*u & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
            u += 2;
        } else if ((*u & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 &&
                   (u[2] & 0xC0) == 0x80) {
            u += 3;
        } else if ((*u & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 &&
                   (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
            u += 4;
        } else {
            *u++ = '?';
        }
    }

    /* Truncate to max_len - 1 on a char boundary */
    if (strlen(s) >= max_len) {
        s[max_len - 1] = '\0';
        /* Back off any split multi-byte sequence */
        while (max_len > 1 && (s[max_len - 2] & 0x80)) {
            s[--max_len - 1] = '\0';
        }
    }
}

/* Parse "StreamTitle='Artist - Title';" from meta_buf.
   Fills title_out (artist + " - " + title or just title or station_name). */
static void process_icy_meta(icy_state_t *icy)
{
    char *raw = strstr(icy->meta_buf, "StreamTitle='");
    if (!raw) return;
    raw += 13;  /* skip StreamTitle=' */
    char *end = strrchr(raw, '\'');
    if (!end || end == raw) return;
    *end = '\0';

    char title[128];
    strlcpy(title, raw, sizeof(title));
    normalize_title(title, sizeof(title));

    if (title[0] == '\0') return;
    if (strcmp(title, icy->last_title) == 0) return;   /* deduplicate */

    strlcpy(icy->last_title, title, sizeof(icy->last_title));

    /* Split "Artist - Title" on first " - " */
    char artist_buf[64] = {0};
    char title_buf[96]  = {0};
    char *sep = strstr(title, " - ");
    if (sep) {
        size_t alen = (size_t)(sep - title);
        if (alen >= sizeof(artist_buf)) alen = sizeof(artist_buf) - 1;
        memcpy(artist_buf, title, alen);
        artist_buf[alen] = '\0';
        strlcpy(title_buf, sep + 3, sizeof(title_buf));
    } else {
        strlcpy(title_buf, title, sizeof(title_buf));
    }

    /* Build now-playing string */
    char np[NOW_PLAYING_MAX];
    if (artist_buf[0])
        snprintf(np, sizeof(np), "%s - %s", artist_buf, title_buf);
    else
        strlcpy(np, title_buf, sizeof(np));

    if (s_now_playing_mutex) {
        xSemaphoreTake(s_now_playing_mutex, portMAX_DELAY);
        strlcpy(s_now_playing, np, sizeof(s_now_playing));
        xSemaphoreGive(s_now_playing_mutex);
    }

    avrcp_publish_metadata(title_buf, artist_buf[0] ? artist_buf : NULL,
                           icy->station_genre[0] ? icy->station_genre : NULL);

    ESP_LOGI(TAG, "Now playing: %s", np);
}

/* ── ICY byte-level filter ───────────────────────────────────────────── */

/* ICY byte stream states */
#define ICY_AUDIO        0   /* counting down to next meta block */
#define ICY_READ_LEN     1   /* next byte is the L prefix */
#define ICY_READ_META    2   /* consuming L*16 bytes of metadata */

/* Filter a buffer through the ICY state machine.
   Strips metadata blocks, writing clean audio bytes to dst.
   Returns number of audio bytes written to dst. */
static int icy_filter(icy_state_t *icy, const uint8_t *src, int src_len,
                       uint8_t *dst)
{
    int out   = 0;
    int state = icy->reading_meta ? ICY_READ_META : ICY_AUDIO;

    for (int i = 0; i < src_len; i++) {
        uint8_t b = src[i];

        switch (state) {
            case ICY_AUDIO:
                dst[out++] = b;
                icy->bytes_until_meta--;
                if (icy->bytes_until_meta == 0) {
                    state = ICY_READ_LEN;
                }
                break;

            case ICY_READ_LEN:
                icy->meta_remaining = (int)b * 16;
                icy->meta_used      = 0;
                if (icy->meta_remaining == 0) {
                    /* Empty meta block — skip straight back to audio */
                    icy->bytes_until_meta = icy->metaint;
                    state = ICY_AUDIO;
                } else {
                    state = ICY_READ_META;
                }
                break;

            case ICY_READ_META:
                if (icy->meta_used < (int)sizeof(icy->meta_buf) - 1)
                    icy->meta_buf[icy->meta_used] = (char)b;
                icy->meta_used++;
                icy->meta_remaining--;
                if (icy->meta_remaining == 0) {
                    icy->meta_buf[icy->meta_used < (int)sizeof(icy->meta_buf)
                                  ? icy->meta_used : (int)sizeof(icy->meta_buf) - 1] = '\0';
                    process_icy_meta(icy);
                    icy->bytes_until_meta = icy->metaint;
                    icy->meta_used        = 0;
                    state = ICY_AUDIO;
                }
                break;
        }
    }

    icy->reading_meta = (state == ICY_READ_META || state == ICY_READ_LEN);
    return out;
}


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
    if (buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0) return AUDIO_FORMAT_MP3;
    if (buf[0] == 0xFF && (buf[1] & 0xF0) == 0xF0) return AUDIO_FORMAT_AAC;
    return AUDIO_FORMAT_UNKNOWN;
}

/* ── Playlist resolution ─────────────────────────────────────────────── */

static bool resolve_playlist(const char *body, int len,
                              char *resolved_url, size_t url_size)
{
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

    const char *p = body;
    while (p && p < body + len) {
        if (*p == '#' || *p == '\r' || *p == '\n') {
            p = strpbrk(p, "\n");
            if (p) p++;
            continue;
        }
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
    icy_state_t     icy;
} stream_ctx_t;

/* Scratch buffer for ICY-filtered audio data — stack-local would be too large */
static uint8_t s_icy_scratch[4096];

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    stream_ctx_t *ctx = (stream_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER: {
            if (strcasecmp(evt->header_key, "Content-Type") == 0) {
                audio_format_t fmt = detect_format_from_content_type(evt->header_value);
                if (fmt != AUDIO_FORMAT_UNKNOWN) {
                    ctx->detected_format = fmt;
                    ctx->format_detected = true;
                }
                if (strcasestr(evt->header_value, "mpegurl") ||
                    strcasestr(evt->header_value, "scpls") ||
                    strcasestr(evt->header_value, "x-pls")) {
                    ctx->is_playlist = true;
                }
            } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
                int mi = atoi(evt->header_value);
                if (mi > 0 && mi < 65536) {
                    ctx->icy.metaint         = mi;
                    ctx->icy.bytes_until_meta = mi;
                    ESP_LOGI(TAG, "ICY metaint=%d", mi);
                } else {
                    ESP_LOGW(TAG, "ICY metaint out of range (%d) — disabling", mi);
                }
            } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
                strlcpy(ctx->icy.station_name, evt->header_value,
                        sizeof(ctx->icy.station_name));
                /* Use station name as initial now-playing until a real title arrives */
                if (s_now_playing_mutex) {
                    xSemaphoreTake(s_now_playing_mutex, portMAX_DELAY);
                    if (s_now_playing[0] == '\0') {
                        strlcpy(s_now_playing, ctx->icy.station_name,
                                sizeof(s_now_playing));
                    }
                    xSemaphoreGive(s_now_playing_mutex);
                }
                avrcp_publish_metadata(ctx->icy.station_name, NULL, NULL);
            } else if (strcasecmp(evt->header_key, "icy-genre") == 0) {
                strlcpy(ctx->icy.station_genre, evt->header_value,
                        sizeof(ctx->icy.station_genre));
            }
            break;
        }

        case HTTP_EVENT_ON_DATA: {
            if (evt->data_len <= 0) break;
            uint8_t *data = (uint8_t *)evt->data;
            int      dlen = evt->data_len;

            if (ctx->is_playlist) {
                int copy = dlen;
                if (ctx->playlist_len + copy >= (int)sizeof(ctx->playlist_body) - 1)
                    copy = sizeof(ctx->playlist_body) - 1 - ctx->playlist_len;
                if (copy > 0) {
                    memcpy(ctx->playlist_body + ctx->playlist_len, data, copy);
                    ctx->playlist_len += copy;
                    ctx->playlist_body[ctx->playlist_len] = '\0';
                }
                break;
            }

            /* Strip ICY metadata from the byte stream */
            int audio_len;
            if (ctx->icy.metaint > 0) {
                int chunk = dlen;
                if (chunk > (int)sizeof(s_icy_scratch))
                    chunk = (int)sizeof(s_icy_scratch);
                audio_len = icy_filter(&ctx->icy, data, chunk, s_icy_scratch);
                data = s_icy_scratch;
                dlen = audio_len;
            } else {
                audio_len = dlen;
            }

            if (audio_len <= 0) break;

            /* Detect format from first audio bytes */
            if (!ctx->format_detected) {
                ctx->detected_format = detect_format_from_sync(data, audio_len);
                if (ctx->detected_format != AUDIO_FORMAT_UNKNOWN)
                    ctx->format_detected = true;
            }

            if (ctx->raw_buf) {
                BaseType_t ret = xRingbufferSend(ctx->raw_buf, data, audio_len,
                                                  pdMS_TO_TICKS(200));
                if (ret != pdTRUE)
                    ESP_LOGD(TAG, "raw_buf full, dropping %d bytes", audio_len);
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
            .icy             = { .metaint = 0, .bytes_until_meta = 0 },
        };

        if (strcasestr(url, ".m3u") || strcasestr(url, ".pls"))
            ctx.is_playlist = true;

        esp_http_client_config_t http_cfg = {
            .url               = url,
            .event_handler     = http_event_handler,
            .user_data         = &ctx,
            .timeout_ms        = CARRADIO_HTTP_TIMEOUT_MS,
            .buffer_size       = CARRADIO_HTTP_BUF_SIZE,
            .keep_alive_enable = true,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) {
            ESP_LOGE(TAG, "http_client_init failed");
            goto retry;
        }

        /* Request ICY metadata */
        esp_http_client_set_header(client, "Icy-MetaData", "1");

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

            if (ctx.format_detected) s_format = ctx.detected_format;
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

    s_running     = false;
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t http_stream_start(const char *url)
{
    if (s_running) return ESP_OK;

    if (!s_now_playing_mutex)
        s_now_playing_mutex = xSemaphoreCreateMutex();

    char *url_copy = strdup(url);
    if (!url_copy) return ESP_ERR_NO_MEM;

    s_running = true;
    s_format  = AUDIO_FORMAT_UNKNOWN;
    memset(s_now_playing, 0, sizeof(s_now_playing));

    BaseType_t ret = xTaskCreatePinnedToCore(
        http_stream_task, "http_stream",
        TASK_STACK_HTTP_STREAM, url_copy,
        TASK_PRIO_HTTP_STREAM, &s_task_handle,
        1
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
}

audio_format_t http_stream_get_format(void)
{
    return s_format;
}

bool http_stream_is_running(void)
{
    return s_running;
}

bool http_stream_get_now_playing(char *buf, size_t len)
{
    if (!s_now_playing_mutex || !buf || len == 0) return false;
    xSemaphoreTake(s_now_playing_mutex, portMAX_DELAY);
    bool has = s_now_playing[0] != '\0';
    if (has) strlcpy(buf, s_now_playing, len);
    xSemaphoreGive(s_now_playing_mutex);
    return has;
}
