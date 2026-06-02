/*
 * SpeexDSP resampler stub.
 *
 * Replace with the real SpeexDSP source from squeezelite-esp32.
 * Until then process() returns input unchanged (passthrough),
 * which causes a chipmunk effect for 48kHz streams but keeps
 * the pipeline alive for testing.
 */

#include "speexdsp_resampler.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "speexdsp_stub";

typedef struct {
    int      channels;
    uint32_t in_rate;
    uint32_t out_rate;
} speexdsp_ctx_t;

esp_err_t speexdsp_resampler_init(speexdsp_resampler_handle_t *out,
                                   int channels,
                                   uint32_t in_rate, uint32_t out_rate,
                                   int quality)
{
    (void)quality;
    speexdsp_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->channels = channels;
    ctx->in_rate  = in_rate;
    ctx->out_rate = out_rate;
    *out = (speexdsp_resampler_handle_t)ctx;
    ESP_LOGW(TAG, "SpeexDSP stub: passthrough (%u→%u Hz)", in_rate, out_rate);
    return ESP_OK;
}

esp_err_t speexdsp_resampler_process(speexdsp_resampler_handle_t h,
                                      const int16_t *input, int in_frames,
                                      int16_t *output, int *out_frames)
{
    speexdsp_ctx_t *ctx = (speexdsp_ctx_t *)h;
    int n = (in_frames < *out_frames) ? in_frames : *out_frames;
    memcpy(output, input, n * ctx->channels * sizeof(int16_t));
    *out_frames = n;
    return ESP_OK;
}

void speexdsp_resampler_deinit(speexdsp_resampler_handle_t h)
{
    free(h);
}
