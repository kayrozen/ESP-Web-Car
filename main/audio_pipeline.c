/**
 * @file audio_pipeline.c
 * @brief Two-stage audio pipeline: HTTP fetch → decode/resample → A2DP.
 *
 * Ring buffers are allocated in PSRAM:
 *   raw_ringbuf  (32 KB) — compressed audio from HTTP stream
 *   pcm_ringbuf  (64 KB) — decoded + resampled PCM for A2DP
 *
 * The decode task runs on Core 1, reads from raw_ringbuf,
 * dispatches to MP3 or AAC decoder, resamples to 44100 Hz stereo,
 * and writes to pcm_ringbuf.
 */

#include "audio_pipeline.h"
#include "config.h"
#include "http_stream.h"
#include "mp3_decode.h"
#include "aac_decode.h"
#include "resample.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

static const char *TAG = "audio_pipeline";

/* ── Ring buffers ────────────────────────────────────────────────────── */

static RingbufHandle_t s_raw_ringbuf = NULL;
static RingbufHandle_t s_pcm_ringbuf = NULL;

/* ── State ───────────────────────────────────────────────────────────── */

static volatile bool s_running = false;
static volatile bool s_paused  = false;
static TaskHandle_t  s_decode_task = NULL;

static char s_stream_url[256] = {0};

/* ── Decoder state ───────────────────────────────────────────────────── */

static mp3_decoder_handle_t s_mp3_dec = NULL;
static aac_decoder_handle_t s_aac_dec = NULL;
static resample_handle_t    s_resamp  = NULL;

/* ── PCM output buffer (allocated once) ─────────────────────────────── */
#define PCM_FRAME_BYTES  (4096 * 2 * 2)   /* 4096 samples * stereo * 16-bit */
static int16_t *s_pcm_buf = NULL;

/* ── Silence padding ─────────────────────────────────────────────────── */

static void write_silence(int num_samples)
{
    if (!s_pcm_ringbuf) return;
    int16_t silence[256] = {0};
    int remaining = num_samples * 2;   /* stereo */
    while (remaining > 0 && s_running) {
        int chunk = remaining > 256 ? 256 : remaining;
        xRingbufferSend(s_pcm_ringbuf, silence, chunk * sizeof(int16_t),
                        pdMS_TO_TICKS(100));
        remaining -= chunk;
    }
}

/* ── Decode task ─────────────────────────────────────────────────────── */

static void decode_task(void *arg)
{
    ESP_LOGI(TAG, "Decode task started");

    /* Wait for format detection */
    audio_format_t fmt = AUDIO_FORMAT_UNKNOWN;
    int wait_count = 0;
    while (fmt == AUDIO_FORMAT_UNKNOWN && s_running) {
        vTaskDelay(pdMS_TO_TICKS(100));
        fmt = http_stream_get_format();
        if (++wait_count > 100) {
            ESP_LOGW(TAG, "Format detection timeout — defaulting to MP3");
            fmt = AUDIO_FORMAT_MP3;
        }
    }

    ESP_LOGI(TAG, "Audio format: %d", fmt);

    /* Init decoder */
    esp_err_t err = ESP_OK;
    if (fmt == AUDIO_FORMAT_MP3) {
        err = mp3_decoder_init(&s_mp3_dec);
    } else {
        err = aac_decoder_init(&s_aac_dec);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Decoder init failed: %s", esp_err_to_name(err));
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Init resampler */
    err = resample_init(&s_resamp, 44100, CARRADIO_SAMPLE_RATE,
                        CARRADIO_CHANNELS, CARRADIO_CHANNELS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Resample init failed — will use raw sample rate");
    }

    uint8_t input_buf[2048];
    int pcm_frames = 0;

    while (s_running) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Read compressed data from raw_ringbuf */
        size_t received = 0;
        void *item = xRingbufferReceiveUpTo(s_raw_ringbuf, &received,
                                             pdMS_TO_TICKS(200),
                                             sizeof(input_buf));
        if (!item || received == 0) {
            /* Underrun — write silence to keep A2DP happy */
            write_silence(512);
            continue;
        }

        memcpy(input_buf, item, received);
        vRingbufferReturnItem(s_raw_ringbuf, item);

        /* Decode frame(s) */
        int consumed = 0;
        while (consumed < (int)received && s_running) {
            if (fmt == AUDIO_FORMAT_MP3) {
                pcm_frames = PCM_FRAME_BYTES / (CARRADIO_CHANNELS * sizeof(int16_t));
                err = mp3_decode_frame(s_mp3_dec,
                                       input_buf + consumed,
                                       (int)received - consumed,
                                       s_pcm_buf, &pcm_frames,
                                       &consumed);
            } else {
                pcm_frames = PCM_FRAME_BYTES / (CARRADIO_CHANNELS * sizeof(int16_t));
                err = aac_decode_frame(s_aac_dec,
                                       input_buf + consumed,
                                       (int)received - consumed,
                                       s_pcm_buf, &pcm_frames,
                                       &consumed);
            }

            if (err != ESP_OK || consumed <= 0) {
                /* Skip a byte on sync error */
                consumed = (consumed <= 0) ? 1 : consumed;
                continue;
            }

            if (pcm_frames <= 0) continue;

            /* Resample if needed */
            int16_t *out_ptr  = s_pcm_buf;
            int      out_frames = pcm_frames;

            if (s_resamp) {
                static int16_t resamp_out[PCM_FRAME_BYTES / sizeof(int16_t)];
                out_frames = sizeof(resamp_out) / (CARRADIO_CHANNELS * sizeof(int16_t));
                err = resample_process(s_resamp, s_pcm_buf, pcm_frames,
                                       resamp_out, &out_frames);
                if (err == ESP_OK) {
                    out_ptr = resamp_out;
                } else {
                    out_ptr   = s_pcm_buf;
                    out_frames = pcm_frames;
                }
            }

            /* Write PCM to pcm_ringbuf */
            size_t bytes = out_frames * CARRADIO_CHANNELS * sizeof(int16_t);
            BaseType_t ret = xRingbufferSend(s_pcm_ringbuf, out_ptr, bytes,
                                              pdMS_TO_TICKS(100));
            if (ret != pdTRUE) {
                ESP_LOGD(TAG, "pcm_buf full, dropping frame");
            }
        }
    }

    /* Cleanup */
    if (s_mp3_dec) { mp3_decoder_deinit(s_mp3_dec); s_mp3_dec = NULL; }
    if (s_aac_dec) { aac_decoder_deinit(s_aac_dec); s_aac_dec = NULL; }
    if (s_resamp)  { resample_deinit(s_resamp);      s_resamp  = NULL; }

    ESP_LOGI(TAG, "Decode task ended");
    s_decode_task = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t audio_pipeline_start(const char *stream_url)
{
    if (s_running) return ESP_OK;

    /* Allocate ring buffers in PSRAM */
    void *raw_storage = heap_caps_malloc(CARRADIO_RAW_BUF_SIZE, MALLOC_CAP_SPIRAM);
    void *pcm_storage = heap_caps_malloc(CARRADIO_PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);

    if (!raw_storage || !pcm_storage) {
        ESP_LOGE(TAG, "PSRAM alloc failed (raw=%p pcm=%p)", raw_storage, pcm_storage);
        free(raw_storage);
        free(pcm_storage);
        return ESP_ERR_NO_MEM;
    }

    /* Note: xRingbufferCreateStatic isn't available in all IDF versions.
       Use dynamic allocation with StaticRingbuffer_t workaround. */
    s_raw_ringbuf = xRingbufferCreate(CARRADIO_RAW_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    s_pcm_ringbuf = xRingbufferCreate(CARRADIO_PCM_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);

    /* Free the manually allocated blocks — xRingbufferCreate uses heap internally */
    heap_caps_free(raw_storage);
    heap_caps_free(pcm_storage);

    if (!s_raw_ringbuf || !s_pcm_ringbuf) {
        ESP_LOGE(TAG, "Ring buffer creation failed");
        if (s_raw_ringbuf) { vRingbufferDelete(s_raw_ringbuf); s_raw_ringbuf = NULL; }
        if (s_pcm_ringbuf) { vRingbufferDelete(s_pcm_ringbuf); s_pcm_ringbuf = NULL; }
        return ESP_ERR_NO_MEM;
    }

    /* PCM decode output buffer */
    s_pcm_buf = heap_caps_malloc(PCM_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_buf) {
        s_pcm_buf = malloc(PCM_FRAME_BYTES);
        if (!s_pcm_buf) {
            ESP_LOGE(TAG, "PCM buf alloc failed");
            vRingbufferDelete(s_raw_ringbuf); s_raw_ringbuf = NULL;
            vRingbufferDelete(s_pcm_ringbuf); s_pcm_ringbuf = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_running = true;
    s_paused  = false;

    strlcpy(s_stream_url, stream_url, sizeof(s_stream_url));

    /* Start HTTP stream task */
    esp_err_t err = http_stream_start(stream_url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_stream_start failed: %s", esp_err_to_name(err));
        s_running = false;
        vRingbufferDelete(s_raw_ringbuf); s_raw_ringbuf = NULL;
        vRingbufferDelete(s_pcm_ringbuf); s_pcm_ringbuf = NULL;
        free(s_pcm_buf); s_pcm_buf = NULL;
        return err;
    }

    /* Start decode task on Core 1 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        decode_task, "decode",
        TASK_STACK_DECODE, NULL,
        TASK_PRIO_DECODE, &s_decode_task,
        1   /* Core 1 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "decode task create failed");
        http_stream_stop();
        s_running = false;
        vRingbufferDelete(s_raw_ringbuf); s_raw_ringbuf = NULL;
        vRingbufferDelete(s_pcm_ringbuf); s_pcm_ringbuf = NULL;
        free(s_pcm_buf); s_pcm_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio pipeline started");
    return ESP_OK;
}

void audio_pipeline_soft_pause(void)
{
    s_paused = true;
    /* Keep HTTP stream open — A2DP cb will output silence on underrun */
}

void audio_pipeline_hard_pause(void)
{
    s_paused = true;
    http_stream_stop();
}

void audio_pipeline_pause(void)
{
    audio_pipeline_hard_pause();
}

void audio_pipeline_resume_soft(void)
{
    s_paused = false;
}

void audio_pipeline_resume_hard(void)
{
    if (s_stream_url[0] == '\0') {
        ESP_LOGW(TAG, "No URL cached for hard resume");
        return;
    }
    s_paused = false;
    http_stream_start(s_stream_url);
}

void audio_pipeline_resume(void)
{
    audio_pipeline_resume_soft();
}

void audio_pipeline_stop(void)
{
    s_running = false;
    s_paused  = false;
    http_stream_stop();
    /* Tasks will exit; wait a moment */
    vTaskDelay(pdMS_TO_TICKS(500));
    if (s_raw_ringbuf) { vRingbufferDelete(s_raw_ringbuf); s_raw_ringbuf = NULL; }
    if (s_pcm_ringbuf) { vRingbufferDelete(s_pcm_ringbuf); s_pcm_ringbuf = NULL; }
    if (s_pcm_buf)     { free(s_pcm_buf); s_pcm_buf = NULL; }
}

bool audio_pipeline_is_running(void)
{
    return s_running && !s_paused;
}

RingbufHandle_t audio_pipeline_get_raw_ringbuf(void)
{
    return s_raw_ringbuf;
}

RingbufHandle_t audio_pipeline_get_pcm_ringbuf(void)
{
    return s_pcm_ringbuf;
}
