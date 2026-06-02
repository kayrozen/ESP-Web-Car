/**
 * @file faad2.c
 * @brief Stub FAAD2 AAC decoder.
 *
 * Replace the stub bodies with real NeAACDecOpen / NeAACDecDecode2 calls
 * once the FAAD2 source is available.
 */

#include "faad2.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "faad2";

typedef struct {
    int disable_sbr;
    int initialized;
    /* Real FAAD2: NeAACDecHandle handle; */
} faad2_ctx_t;

faad2_handle_t faad2_init(int disable_sbr)
{
    faad2_ctx_t *ctx = calloc(1, sizeof(faad2_ctx_t));
    if (!ctx) return NULL;
    ctx->disable_sbr = disable_sbr;
    ctx->initialized = 0;   /* Real FAAD2 needs NeAACDecInit after first frame */
    ESP_LOGW(TAG, "FAAD2 stub — real FAAD2 source not yet available (SBR %s)",
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

    /* Stub: output silence and advance past the ADTS header */
    /* ADTS header is 7 or 9 bytes; frame length is in bits 30-43 */
    int frame_len = 7;  /* default minimum */
    if (input_len >= 7 && (input[0] == 0xFF) && ((input[1] & 0xF0) == 0xF0)) {
        frame_len = ((input[3] & 0x03) << 11) |
                     (input[4] << 3) |
                     ((input[5] >> 5) & 0x07);
        if (frame_len <= 0 || frame_len > input_len) frame_len = input_len;
    }

    /* 1024 samples per AAC frame */
    int frames = (*pcm_frames < 1024) ? *pcm_frames : 1024;
    memset(pcm_out, 0, frames * 2 * sizeof(int16_t));
    *pcm_frames = frames;
    *consumed   = frame_len;

    return ESP_ERR_NOT_SUPPORTED;
}

void faad2_deinit(faad2_handle_t handle)
{
    free(handle);
}
