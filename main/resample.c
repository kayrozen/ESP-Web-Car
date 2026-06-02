/**
 * @file resample.c
 * @brief SpeexDSP resampler wrapper with mono→stereo duplication.
 *
 * Delegates to the speexdsp component.
 */

#include "resample.h"
#include "speexdsp_resampler.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "resample";

typedef struct {
    speexdsp_resampler_handle_t spx;
    uint32_t in_rate;
    uint32_t out_rate;
    int      in_channels;
    int      out_channels;
} resample_ctx_t;

esp_err_t resample_init(resample_handle_t *out_handle,
                         uint32_t out_rate, uint32_t in_rate,
                         int in_channels, int out_channels)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

    resample_ctx_t *ctx = calloc(1, sizeof(resample_ctx_t));
    if (!ctx) return ESP_ERR_NO_MEM;

    ctx->in_rate      = in_rate;
    ctx->out_rate     = out_rate;
    ctx->in_channels  = in_channels;
    ctx->out_channels = out_channels;

    esp_err_t err = speexdsp_resampler_init(&ctx->spx, in_channels,
                                             in_rate, out_rate,
                                             5 /* quality 0-10 */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speexdsp_resampler_init failed: %s — passthrough mode",
                 esp_err_to_name(err));
        ctx->spx = NULL;
    }

    *out_handle = (resample_handle_t)ctx;
    ESP_LOGI(TAG, "Resampler: %u Hz → %u Hz, %dch → %dch",
             in_rate, out_rate, in_channels, out_channels);
    return ESP_OK;
}

esp_err_t resample_process(resample_handle_t handle,
                            const int16_t *input, int in_frames,
                            int16_t *output, int *out_frames)
{
    if (!handle || !input || !output || !out_frames) return ESP_ERR_INVALID_ARG;

    resample_ctx_t *ctx = (resample_ctx_t *)handle;

    int actual_out = *out_frames;

    if (ctx->spx) {
        esp_err_t err = speexdsp_resampler_process(ctx->spx,
                                                    input, in_frames,
                                                    output, &actual_out);
        if (err != ESP_OK) {
            /* Fallback to passthrough */
            actual_out = in_frames < *out_frames ? in_frames : *out_frames;
            memcpy(output, input,
                   actual_out * ctx->in_channels * sizeof(int16_t));
        }
    } else {
        /* Passthrough */
        actual_out = in_frames < *out_frames ? in_frames : *out_frames;
        memcpy(output, input, actual_out * ctx->in_channels * sizeof(int16_t));
    }

    /* Mono → Stereo duplication */
    if (ctx->in_channels == 1 && ctx->out_channels == 2) {
        /* Expand mono to stereo in-place (work from end to start) */
        for (int i = actual_out - 1; i >= 0; i--) {
            output[i * 2 + 1] = output[i];
            output[i * 2]     = output[i];
        }
    }

    *out_frames = actual_out;
    return ESP_OK;
}

void resample_deinit(resample_handle_t handle)
{
    if (!handle) return;
    resample_ctx_t *ctx = (resample_ctx_t *)handle;
    if (ctx->spx) speexdsp_resampler_deinit(ctx->spx);
    free(ctx);
}
