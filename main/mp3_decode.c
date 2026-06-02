/**
 * @file mp3_decode.c
 * @brief Helix MP3 decoder wrapper.
 *
 * Delegates to the helix-mp3 component.  If the real Helix source has not
 * been dropped in, mp3_decode_frame() returns ESP_ERR_NOT_SUPPORTED and
 * the audio pipeline outputs silence.
 */

#include "mp3_decode.h"
#include "helix_mp3.h"

#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "mp3_dec";

esp_err_t mp3_decoder_init(mp3_decoder_handle_t *out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

    helix_mp3_handle_t h = helix_mp3_init();
    if (!h) {
        ESP_LOGE(TAG, "helix_mp3_init returned NULL");
        return ESP_FAIL;
    }

    *out_handle = (mp3_decoder_handle_t)h;
    ESP_LOGI(TAG, "MP3 decoder initialized");
    return ESP_OK;
}

esp_err_t mp3_decode_frame(mp3_decoder_handle_t handle,
                            const uint8_t *input, int input_len,
                            int16_t *pcm_out, int *pcm_frames,
                            int *consumed)
{
    if (!handle || !input || !pcm_out || !pcm_frames || !consumed)
        return ESP_ERR_INVALID_ARG;

    return helix_mp3_decode_frame((helix_mp3_handle_t)handle,
                                   input, input_len,
                                   pcm_out, pcm_frames,
                                   consumed);
}

void mp3_decoder_deinit(mp3_decoder_handle_t handle)
{
    if (handle) {
        helix_mp3_deinit((helix_mp3_handle_t)handle);
    }
}
