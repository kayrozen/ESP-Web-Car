/**
 * @file faad2.c
 * @brief Wrapper around the real FAAD2 AAC-LC/HE-AAC decoder.
 *
 * Maps the project's faad2_* API onto NeAACDecOpen / NeAACDecInit /
 * NeAACDecDecode2 / NeAACDecClose.
 *
 * SBR is controlled at build time by CONFIG_AAC_DISABLE_SBR (sdkconfig).
 * The wrapper also re-initialises the decoder on sample-rate or channel
 * changes (common when switching streams).
 */

#include "faad2.h"
#include "neaacdec.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "faad2";

typedef struct {
    NeAACDecHandle  handle;
    int             disable_sbr;
    int             initialized;   /* 1 after NeAACDecInit succeeds */
    unsigned long   samplerate;
    unsigned char   channels;
} faad2_ctx_t;

faad2_handle_t faad2_init(int disable_sbr)
{
    faad2_ctx_t *ctx = calloc(1, sizeof(faad2_ctx_t));
    if (!ctx) return NULL;

    ctx->handle = NeAACDecOpen();
    if (!ctx->handle) {
        ESP_LOGE(TAG, "NeAACDecOpen() failed");
        free(ctx);
        return NULL;
    }

    /* Configure decoder */
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(ctx->handle);
    cfg->outputFormat = FAAD_FMT_16BIT;
    cfg->downMatrix   = 1;   /* downmix >2ch to stereo */
    if (disable_sbr) {
        /* NO_SBR: decode base-band only, skip spectral band replication */
        cfg->dontUpSampleImplicitSBR = 1;
    }
    NeAACDecSetConfiguration(ctx->handle, cfg);

    ctx->disable_sbr = disable_sbr;
    ctx->initialized = 0;

    ESP_LOGI(TAG, "FAAD2 decoder opened (SBR %s)",
             disable_sbr ? "disabled" : "enabled");
    return (faad2_handle_t)ctx;
}

esp_err_t faad2_decode_frame(faad2_handle_t handle,
                              const uint8_t *input, int input_len,
                              int16_t *pcm_out, int *pcm_frames,
                              int *consumed)
{
    if (!handle || !input || !pcm_out || !pcm_frames || !consumed)
        return ESP_ERR_INVALID_ARG;

    faad2_ctx_t *ctx = (faad2_ctx_t *)handle;

    /* First call: initialise with the stream's ADTS header */
    if (!ctx->initialized) {
        long ret = NeAACDecInit(ctx->handle,
                                (unsigned char *)input, (unsigned long)input_len,
                                &ctx->samplerate, &ctx->channels);
        if (ret < 0) {
            ESP_LOGW(TAG, "NeAACDecInit failed: %s",
                     NeAACDecGetErrorMessage((unsigned char)-ret));
            *pcm_frames = 0;
            *consumed = (input_len > 7) ? 7 : input_len;
            return ESP_FAIL;
        }
        ctx->initialized = 1;
        ESP_LOGI(TAG, "AAC stream: %lu Hz, %d ch", ctx->samplerate, ctx->channels);
        /* NeAACDecInit consumed `ret` bytes of sync/header */
        if (ret > 0) {
            input      += ret;
            input_len  -= (int)ret;
            *consumed   = (int)ret;
        }
    }

    NeAACDecFrameInfo info;
    void *pcm_buf = NeAACDecDecode2(ctx->handle, &info,
                                    (unsigned char *)input,
                                    (unsigned long)input_len,
                                    (void **)&pcm_out,
                                    (unsigned long)(*pcm_frames * 2 * sizeof(int16_t)));

    if (info.error != 0) {
        ESP_LOGW(TAG, "NeAACDecDecode2 error %d: %s",
                 info.error, NeAACDecGetErrorMessage(info.error));
        /* On error, skip a minimal ADTS frame to stay in sync */
        *pcm_frames = 0;
        *consumed += (input_len > 7) ? 7 : input_len;
        return ESP_FAIL;
    }

    /* Detect sample-rate/channel change → reinitialise next call */
    if (info.samplerate != ctx->samplerate || info.channels != ctx->channels) {
        ESP_LOGI(TAG, "AAC stream format change: %lu→%lu Hz, %d→%d ch",
                 ctx->samplerate, info.samplerate, ctx->channels, info.channels);
        ctx->samplerate = info.samplerate;
        ctx->channels   = info.channels;
        ctx->initialized = 0;
    }

    *pcm_frames  = (int)(info.samples / (info.channels > 0 ? info.channels : 1));
    *consumed   += (int)info.bytesconsumed;

    (void)pcm_buf;   /* NeAACDecDecode2 writes directly into pcm_out via the pointer arg */
    return ESP_OK;
}

void faad2_deinit(faad2_handle_t handle)
{
    if (!handle) return;
    faad2_ctx_t *ctx = (faad2_ctx_t *)handle;
    if (ctx->handle)
        NeAACDecClose(ctx->handle);
    free(ctx);
}
