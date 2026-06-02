#pragma once
#ifndef HELIX_MP3_H
#define HELIX_MP3_H

/**
 * @file helix_mp3.h
 * @brief Minimal Helix MP3 decoder API.
 *
 * When the real Helix source (mp3dec.c, mp3common.c, etc.) is dropped into
 * this component directory the stub implementations in helix_mp3.c should
 * be replaced with real calls to HMP3Decoder / MP3Decode.
 *
 * Until then every decode call returns ESP_ERR_NOT_SUPPORTED and the audio
 * pipeline outputs silence.
 */

#include "esp_err.h"
#include <stdint.h>

typedef void *helix_mp3_handle_t;

/**
 * @brief Allocate and initialise a Helix MP3 decoder.
 * @return handle on success, NULL on allocation failure.
 */
helix_mp3_handle_t helix_mp3_init(void);

/**
 * @brief Decode one MP3 frame.
 *
 * @param handle     Decoder handle from helix_mp3_init().
 * @param input      Compressed data buffer.
 * @param input_len  Bytes available in input.
 * @param pcm_out    Output PCM buffer (stereo interleaved int16).
 * @param pcm_frames In: buffer capacity in frames. Out: frames written.
 * @param consumed   Out: bytes consumed from input.
 *
 * @return ESP_OK on success.
 *         ESP_ERR_NOT_SUPPORTED if Helix source not yet available.
 *         ESP_ERR_INVALID_ARG   on NULL arguments.
 */
esp_err_t helix_mp3_decode_frame(helix_mp3_handle_t handle,
                                  const uint8_t *input, int input_len,
                                  int16_t *pcm_out, int *pcm_frames,
                                  int *consumed);

/**
 * @brief Free decoder resources.
 */
void helix_mp3_deinit(helix_mp3_handle_t handle);

#endif /* HELIX_MP3_H */
