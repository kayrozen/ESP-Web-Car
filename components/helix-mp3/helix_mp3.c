/**
 * @file helix_mp3.c
 * @brief Thin wrapper around the real Helix fixed-point MP3 decoder.
 *
 * Maps the project's helix_mp3_* API onto the upstream
 * MP3InitDecoder / MP3Decode / MP3FreeDecoder API.
 */

#include "helix_mp3.h"
#include "mp3dec.h"   /* Helix public API */
#include <string.h>
#include "esp_log.h"

static const char *TAG = "helix_mp3";

helix_mp3_handle_t helix_mp3_init(void)
{
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGE(TAG, "MP3InitDecoder() failed (out of memory)");
        return NULL;
    }
    return (helix_mp3_handle_t)dec;
}

esp_err_t helix_mp3_decode_frame(helix_mp3_handle_t handle,
                                  const uint8_t *input, int input_len,
                                  int16_t *pcm_out, int *pcm_frames,
                                  int *consumed)
{
    if (!handle || !input || !pcm_out || !pcm_frames || !consumed)
        return ESP_ERR_INVALID_ARG;

    HMP3Decoder dec = (HMP3Decoder)handle;

    /* Find sync word before passing to decoder */
    int offset = MP3FindSyncWord(input, input_len);
    if (offset < 0) {
        /* No valid frame in this buffer — caller should refill */
        *pcm_frames = 0;
        *consumed = input_len;
        return ESP_ERR_NOT_FOUND;
    }

    const unsigned char *buf_ptr = input + offset;
    size_t bytes_left = (size_t)(input_len - offset);

    int err = MP3Decode(dec, &buf_ptr, &bytes_left, pcm_out, 0);

    if (err != ERR_MP3_NONE && err != ERR_MP3_MAINDATA_UNDERFLOW) {
        ESP_LOGW(TAG, "MP3Decode error %d", err);
        *pcm_frames = 0;
        *consumed = input_len;   /* skip bad frame */
        return ESP_FAIL;
    }

    MP3FrameInfo info;
    MP3GetLastFrameInfo(dec, &info);

    *pcm_frames = info.outputSamps / (info.nChans > 0 ? info.nChans : 1);
    *consumed = (int)(input_len - (int)bytes_left);

    return ESP_OK;
}

void helix_mp3_deinit(helix_mp3_handle_t handle)
{
    if (handle)
        MP3FreeDecoder((HMP3Decoder)handle);
}
