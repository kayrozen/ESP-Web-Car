#pragma once
#ifndef MP3_DECODE_H
#define MP3_DECODE_H

#include "esp_err.h"
#include <stdint.h>

typedef void *mp3_decoder_handle_t;

/**
 * @brief Create and initialise a Helix MP3 decoder instance.
 */
esp_err_t mp3_decoder_init(mp3_decoder_handle_t *out_handle);

/**
 * @brief Decode one MP3 frame from input buffer.
 *
 * @param handle       Decoder handle.
 * @param input        Pointer to compressed data.
 * @param input_len    Number of bytes available in input.
 * @param pcm_out      Output PCM buffer (interleaved stereo int16).
 * @param pcm_frames   In: capacity in frames. Out: frames decoded.
 * @param consumed     Out: bytes consumed from input.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if Helix not available.
 */
esp_err_t mp3_decode_frame(mp3_decoder_handle_t handle,
                            const uint8_t *input, int input_len,
                            int16_t *pcm_out, int *pcm_frames,
                            int *consumed);

/**
 * @brief Destroy the decoder and free resources.
 */
void mp3_decoder_deinit(mp3_decoder_handle_t handle);

#endif
