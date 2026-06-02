/**
 * @file speexdsp_resampler.c
 * @brief SpeexDSP resampler stub.
 *
 * Replace stub bodies with real speex_resampler_* calls once the
 * SpeexDSP source is available.
 */

#include "speexdsp_resampler.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "speexdsp";

typedef struct {
    int      channels;
    uint32_t in_rate;
    uint32_t out_rate;
    int      quality;
    /* Real SpeexDSP: SpeexResamplerState *state; */
} speexdsp_ctx_t;

esp_err_t speexdsp_resampler_init(speexdsp_resampler_handle_t *out_handle,
                                   int channels,
                                   uint32_t in_rate, uint32_t out_rate,
                                   int quality)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

    speexdsp_ctx_t *ctx = calloc(1, sizeof(speexdsp_ctx_t));
    if (!ctx) return ESP_ERR_NO_MEM;

    ctx->channels = channels;
    ctx->in_rate  = in_rate;
    ctx->out_rate = out_rate;
    ctx->quality  = quality;

    ESP_LOGW(TAG, "SpeexDSP stub — passthrough mode (%u→%u Hz, %dch)",
             in_rate, out_rate, channels);

    *out_handle = (speexdsp_resampler_handle_t)ctx;
    return ESP_ERR_NOT_SUPPORTED;  /* signal to caller that this is passthrough */
}

esp_err_t speexdsp_resampler_process(speexdsp_resampler_handle_t handle,
                                      const int16_t *input, int in_frames,
                                      int16_t *output, int *out_frames)
{
    if (!handle || !input || !output || !out_frames) return ESP_ERR_INVALID_ARG;

    speexdsp_ctx_t *ctx = (speexdsp_ctx_t *)handle;

    /* Passthrough — copy input to output, no rate conversion */
    int copy = (in_frames < *out_frames) ? in_frames : *out_frames;
    memcpy(output, input, copy * ctx->channels * sizeof(int16_t));
    *out_frames = copy;
    return ESP_OK;
}

void speexdsp_resampler_deinit(speexdsp_resampler_handle_t handle)
{
    free(handle);
}
