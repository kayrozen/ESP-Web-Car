/**
 * @file helix_mp3.c
 * @brief Stub Helix MP3 decoder.
 *
 * Replace the stub bodies below with real HMP3Decoder calls once the
 * Helix MP3 source files (mp3dec.c, mp3common.c, mp3tabs.c, …) are
 * placed in this directory and added to CMakeLists.txt SRCS.
 *
 * Real API reference:
 *   HMP3Decoder MP3InitDecoder(void);
 *   void        MP3FreeDecoder(HMP3Decoder hMP3Decoder);
 *   int         MP3Decode(HMP3Decoder hMP3Decoder,
 *                         unsigned char **inbuf, int *bytesLeft,
 *                         short *outbuf, int useSize);
 *   void        MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder,
 *                                   MP3FrameInfo *mp3FrameInfo);
 */

#include "helix_mp3.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "helix_mp3";

/* Placeholder struct — replaced by real HMP3Decoder when Helix is available */
typedef struct {
    int placeholder;
} helix_mp3_ctx_t;

helix_mp3_handle_t helix_mp3_init(void)
{
    helix_mp3_ctx_t *ctx = calloc(1, sizeof(helix_mp3_ctx_t));
    if (!ctx) return NULL;
    ESP_LOGW(TAG, "Helix MP3 stub — real Helix source not yet available");
    return (helix_mp3_handle_t)ctx;
}

esp_err_t helix_mp3_decode_frame(helix_mp3_handle_t handle,
                                  const uint8_t *input, int input_len,
                                  int16_t *pcm_out, int *pcm_frames,
                                  int *consumed)
{
    if (!handle || !input || !pcm_out || !pcm_frames || !consumed)
        return ESP_ERR_INVALID_ARG;

    /* Stub: output silence and consume all input */
    int frames = (*pcm_frames < 1152) ? *pcm_frames : 1152;
    memset(pcm_out, 0, frames * 2 * sizeof(int16_t));   /* stereo */
    *pcm_frames = frames;
    *consumed   = (input_len < 417) ? input_len : 417;   /* typical MP3 frame size */

    return ESP_ERR_NOT_SUPPORTED;
}

void helix_mp3_deinit(helix_mp3_handle_t handle)
{
    free(handle);
}
