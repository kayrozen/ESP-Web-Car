#pragma once
#ifndef FAAD2_H
#define FAAD2_H

/**
 * @file faad2.h
 * @brief Minimal FAAD2 AAC-LC / HE-AAC decoder API stub.
 *
 * When the real FAAD2 source is added to this component directory,
 * replace the stub bodies in faad2.c with real NeAACDecOpen /
 * NeAACDecDecode2 calls.
 *
 * Real API reference:
 *   NeAACDecHandle NeAACDecOpen(void);
 *   void           NeAACDecClose(NeAACDecHandle hDecoder);
 *   long           NeAACDecInit(NeAACDecHandle hDecoder,
 *                               unsigned char *buffer, unsigned long buflen,
 *                               unsigned long *samplerate, unsigned char *channels);
 *   void          *NeAACDecDecode2(NeAACDecHandle hDecoder,
 *                                  NeAACDecFrameInfo *hInfo,
 *                                  unsigned char *buffer, unsigned long buflen,
 *                                  void **sample_buffer, unsigned long sample_buffer_size);
 */

#include "esp_err.h"
#include <stdint.h>

typedef void *faad2_handle_t;

/**
 * @brief Allocate and initialise a FAAD2 decoder.
 *
 * @param disable_sbr  Non-zero to disable SBR (HE-AAC v1/v2 extension).
 *                     Reduces CPU load at the cost of audio quality for
 *                     HE-AAC streams.  Set via CONFIG_AAC_DISABLE_SBR.
 */
faad2_handle_t faad2_init(int disable_sbr);

/**
 * @brief Decode one AAC ADTS frame.
 *
 * @param handle     Decoder handle.
 * @param input      ADTS-framed AAC data.
 * @param input_len  Bytes available in input.
 * @param pcm_out    Output PCM buffer (stereo interleaved int16).
 * @param pcm_frames In: buffer capacity in frames. Out: frames written.
 * @param consumed   Out: bytes consumed from input.
 *
 * @return ESP_OK on success.
 *         ESP_ERR_NOT_SUPPORTED if FAAD2 source not yet available.
 */
esp_err_t faad2_decode_frame(faad2_handle_t handle,
                              const uint8_t *input, int input_len,
                              int16_t *pcm_out, int *pcm_frames,
                              int *consumed);

/**
 * @brief Free decoder resources.
 */
void faad2_deinit(faad2_handle_t handle);

#endif /* FAAD2_H */
