/**
 * @file aac_decode.c
 * @brief FAAD2 AAC-LC / HE-AAC decoder wrapper.
 *
 * Delegates to the faad2 component.  If the real FAAD2 source has not
 * been dropped in, aac_decode_frame() returns ESP_ERR_NOT_SUPPORTED.
 */

#include "aac_decode.h"
#include "faad2.h"

#include <stdlib.h>
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "aac_dec";

esp_err_t aac_decoder_init(aac_decoder_handle_t *out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

#ifdef CONFIG_AAC_DISABLE_SBR
    int disable_sbr = CONFIG_AAC_DISABLE_SBR;
#else
    int disable_sbr = 0;
#endif

    faad2_handle_t h = faad2_init(disable_sbr);
    if (!h) {
        ESP_LOGE(TAG, "faad2_init returned NULL");
        return ESP_FAIL;
    }

    *out_handle = (aac_decoder_handle_t)h;
    ESP_LOGI(TAG, "AAC decoder initialized (SBR %s)",
             disable_sbr ? "disabled" : "enabled");
    return ESP_OK;
}

esp_err_t aac_decode_frame(aac_decoder_handle_t handle,
                            const uint8_t *input, int input_len,
                            int16_t *pcm_out, int *pcm_frames,
                            int *consumed)
{
    if (!handle || !input || !pcm_out || !pcm_frames || !consumed)
        return ESP_ERR_INVALID_ARG;

    return faad2_decode_frame((faad2_handle_t)handle,
                               input, input_len,
                               pcm_out, pcm_frames,
                               consumed);
}

void aac_decoder_deinit(aac_decoder_handle_t handle)
{
    if (handle) {
        faad2_deinit((faad2_handle_t)handle);
    }
}
