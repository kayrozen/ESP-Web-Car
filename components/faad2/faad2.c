/**
 * @file faad2.c
 * @brief AAC decoder wrapper — backed by Fraunhofer FDK AAC (fdk-aac).
 *
 * Exposes the same faad2_* API used by aac_decode.c, but calls the
 * fdk-aac aacDecoder_* functions underneath.  fdk-aac supports AAC-LC,
 * HE-AAC v1/v2 (SBR + PS), and xHE-AAC, and is licensed under the
 * Fraunhofer FDK license (free for non-commercial use; see LICENSE).
 *
 * Stream format: ADTS (the typical format for internet radio streams).
 * The decoder self-configures from the first ADTS frame header — no
 * explicit NeAACDecInit step needed.
 */

#include "faad2.h"
#include "aacdecoder_lib.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "fdkaac";

typedef struct {
    HANDLE_AACDECODER handle;
    int               disable_sbr;
    /* scratch buffer — reused across frames (allocated once) */
    INT_PCM           pcm_scratch[2048 * 2];  /* 2048 frames * stereo */
} fdkaac_ctx_t;

faad2_handle_t faad2_init(int disable_sbr)
{
    fdkaac_ctx_t *ctx = calloc(1, sizeof(fdkaac_ctx_t));
    if (!ctx) return NULL;

    /* TT_MP4_ADTS (2) = ADTS framing, used by internet radio streams */
    ctx->handle = aacDecoder_Open(TT_MP4_ADTS, 1 /* nrOfLayers */);
    if (!ctx->handle) {
        ESP_LOGE(TAG, "aacDecoder_Open() failed");
        free(ctx);
        return NULL;
    }

    ctx->disable_sbr = disable_sbr;

    if (disable_sbr) {
        /* Disable SBR / PS upsample — saves ~30% CPU, stays at base-band rate */
        aacDecoder_SetParam(ctx->handle, AAC_SBR_ENABLE, 0);
    }

    /* Downmix anything > 2ch to stereo */
    aacDecoder_SetParam(ctx->handle, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);

    /* Output 16-bit PCM (default, but be explicit) */
    aacDecoder_SetParam(ctx->handle, AAC_PCM_OUTPUT_INTERLEAVED, 1);

    ESP_LOGI(TAG, "fdk-aac decoder opened (SBR %s)",
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

    fdkaac_ctx_t *ctx = (fdkaac_ctx_t *)handle;

    /* Step 1 — feed compressed data into the decoder's internal buffer */
    UINT bytes_valid = (UINT)input_len;
    AAC_DECODER_ERROR err = aacDecoder_Fill(
        ctx->handle,
        (UCHAR **)&input,
        (UINT *)&input_len,
        &bytes_valid);

    if (err != AAC_DEC_OK) {
        ESP_LOGW(TAG, "aacDecoder_Fill error: 0x%x", err);
        *pcm_frames = 0;
        *consumed   = input_len;
        return ESP_FAIL;
    }
    *consumed = input_len - (int)bytes_valid;

    /* Step 2 — decode one frame into the scratch buffer */
    int pcm_buf_size = *pcm_frames * 2 * sizeof(INT_PCM);
    err = aacDecoder_DecodeFrame(
        ctx->handle,
        ctx->pcm_scratch,
        (INT)sizeof(ctx->pcm_scratch) / sizeof(INT_PCM),
        0 /* flags */);

    if (err == AAC_DEC_NOT_ENOUGH_BITS) {
        /* Need more data — caller should push more bytes */
        *pcm_frames = 0;
        return ESP_ERR_NOT_FOUND;
    }
    if (err != AAC_DEC_OK && !IS_OUTPUT_VALID(err)) {
        ESP_LOGW(TAG, "aacDecoder_DecodeFrame error: 0x%x", err);
        *pcm_frames = 0;
        return ESP_FAIL;
    }

    /* Step 3 — copy decoded PCM to caller's buffer */
    CStreamInfo *info = aacDecoder_GetStreamInfo(ctx->handle);
    if (!info) {
        *pcm_frames = 0;
        return ESP_FAIL;
    }

    int frames    = info->frameSize;
    int channels  = info->numChannels > 0 ? info->numChannels : 1;
    int copy_frames = (frames < *pcm_frames) ? frames : *pcm_frames;

    memcpy(pcm_out, ctx->pcm_scratch,
           copy_frames * channels * sizeof(INT_PCM));
    *pcm_frames = copy_frames;

    return ESP_OK;
}

void faad2_deinit(faad2_handle_t handle)
{
    if (!handle) return;
    fdkaac_ctx_t *ctx = (fdkaac_ctx_t *)handle;
    if (ctx->handle)
        aacDecoder_Close(ctx->handle);
    free(ctx);
}
