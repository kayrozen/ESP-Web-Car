/**
 * @file speexdsp_resampler.c
 * @brief Wrapper around the real SpeexDSP fixed-point resampler.
 *
 * Maps the project's speexdsp_resampler_* API onto
 * speex_resampler_init / speex_resampler_process_interleaved_int /
 * speex_resampler_destroy from xiph/speexdsp.
 *
 * Quality level 4 is used (squeezelite-esp32 default): good audio/CPU
 * balance under WiFi+BT coexistence load. Increase to 5–6 only if the
 * CPU budget allows after measuring underrun frequency.
 */

#include "speexdsp_resampler.h"
#include "speex_resampler.h"   /* xiph SpeexDSP public header */
#include <inttypes.h>
#include "esp_log.h"

static const char *TAG = "speexdsp";

esp_err_t speexdsp_resampler_init(speexdsp_resampler_handle_t *out_handle,
                                   int channels,
                                   uint32_t in_rate, uint32_t out_rate,
                                   int quality)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

    int err = RESAMPLER_ERR_SUCCESS;
    SpeexResamplerState *st = speex_resampler_init(
        (spx_uint32_t)channels,
        (spx_uint32_t)in_rate,
        (spx_uint32_t)out_rate,
        quality,
        &err);

    if (!st || err != RESAMPLER_ERR_SUCCESS) {
        ESP_LOGE(TAG, "speex_resampler_init failed: err=%d", err);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "resampler ready: %"PRIu32"->%"PRIu32" Hz, %dch, quality=%d",
             in_rate, out_rate, channels, quality);

    *out_handle = (speexdsp_resampler_handle_t)st;
    return ESP_OK;
}

esp_err_t speexdsp_resampler_process(speexdsp_resampler_handle_t handle,
                                      const int16_t *input, int in_frames,
                                      int16_t *output, int *out_frames)
{
    if (!handle || !input || !output || !out_frames) return ESP_ERR_INVALID_ARG;

    SpeexResamplerState *st = (SpeexResamplerState *)handle;
    spx_uint32_t in_len  = (spx_uint32_t)in_frames;
    spx_uint32_t out_len = (spx_uint32_t)*out_frames;

    int err = speex_resampler_process_interleaved_int(st, input, &in_len, output, &out_len);
    if (err != RESAMPLER_ERR_SUCCESS) {
        ESP_LOGW(TAG, "resample error: %d", err);
        return ESP_FAIL;
    }

    *out_frames = (int)out_len;
    return ESP_OK;
}

void speexdsp_resampler_deinit(speexdsp_resampler_handle_t handle)
{
    if (handle)
        speex_resampler_destroy((SpeexResamplerState *)handle);
}
