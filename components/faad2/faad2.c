/*
 * FAAD2 AAC decoder stub.
 *
 * Replace with the real FAAD2/libfaad source from squeezelite-esp32.
 * Until then all decode calls return ESP_ERR_NOT_SUPPORTED and
 * the pipeline outputs silence for AAC streams.
 */

#include "faad2.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "faad2_stub";

faad2_handle_t faad2_init(int disable_sbr)
{
    ESP_LOGW(TAG, "FAAD2 stub — real decoder not installed (SBR %s)",
             disable_sbr ? "disabled" : "enabled");
    return (faad2_handle_t)1;
}

void faad2_deinit(faad2_handle_t h)
{
    (void)h;
}

esp_err_t faad2_decode_frame(faad2_handle_t h,
                              const uint8_t *input, int input_len,
                              int16_t *pcm_out, int *pcm_frames,
                              int *consumed)
{
    (void)h; (void)input; (void)pcm_out;
    *pcm_frames = 0;
    *consumed   = input_len;
    return ESP_ERR_NOT_SUPPORTED;
}
