#pragma once
#ifndef AAC_DECODE_H
#define AAC_DECODE_H

#include "esp_err.h"
#include <stdint.h>

typedef void *aac_decoder_handle_t;

/**
 * @brief Create and initialise a FAAD2 AAC/HE-AAC decoder instance.
 *        SBR (spectral band replication) is controlled by CONFIG_AAC_DISABLE_SBR.
 */
esp_err_t aac_decoder_init(aac_decoder_handle_t *out_handle);

/**
 * @brief Decode one AAC frame from input buffer.
 *
 * @param handle       Decoder handle.
 * @param input        Pointer to ADTS-framed AAC data.
 * @param input_len    Number of bytes available in input.
 * @param pcm_out      Output PCM buffer (interleaved stereo int16).
 * @param pcm_frames   In: capacity in frames. Out: frames decoded.
 * @param consumed     Out: bytes consumed from input.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if FAAD2 not available.
 */
esp_err_t aac_decode_frame(aac_decoder_handle_t handle,
                            const uint8_t *input, int input_len,
                            int16_t *pcm_out, int *pcm_frames,
                            int *consumed);

/**
 * @brief Destroy the decoder and free resources.
 */
void aac_decoder_deinit(aac_decoder_handle_t handle);

#endif
