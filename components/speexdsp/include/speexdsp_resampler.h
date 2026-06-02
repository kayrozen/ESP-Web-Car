#pragma once
#ifndef SPEEXDSP_RESAMPLER_H
#define SPEEXDSP_RESAMPLER_H

/**
 * @file speexdsp_resampler.h
 * @brief Minimal SpeexDSP resampler API stub.
 *
 * When the real SpeexDSP source (resample.c and friends) is dropped into
 * this component directory, replace the stubs with real
 * speex_resampler_init / speex_resampler_process_interleaved_int calls.
 *
 * Real API reference:
 *   SpeexResamplerState *speex_resampler_init(spx_uint32_t nb_channels,
 *       spx_uint32_t in_rate, spx_uint32_t out_rate, int quality, int *err);
 *   void speex_resampler_destroy(SpeexResamplerState *st);
 *   int  speex_resampler_process_interleaved_int(SpeexResamplerState *st,
 *       const spx_int16_t *in, spx_uint32_t *in_len,
 *       spx_int16_t *out, spx_uint32_t *out_len);
 */

#include "esp_err.h"
#include <stdint.h>

typedef void *speexdsp_resampler_handle_t;

/**
 * @brief Create a SpeexDSP resampler.
 *
 * @param out_handle  Output handle.
 * @param channels    Number of interleaved channels.
 * @param in_rate     Input sample rate (Hz).
 * @param out_rate    Output sample rate (Hz).
 * @param quality     Resampler quality 0 (lowest) – 10 (highest).
 */
esp_err_t speexdsp_resampler_init(speexdsp_resampler_handle_t *out_handle,
                                   int channels,
                                   uint32_t in_rate, uint32_t out_rate,
                                   int quality);

/**
 * @brief Resample interleaved int16 audio.
 *
 * @param handle     Resampler handle.
 * @param input      Input samples.
 * @param in_frames  Number of input frames.
 * @param output     Output buffer.
 * @param out_frames In: capacity in frames. Out: frames written.
 */
esp_err_t speexdsp_resampler_process(speexdsp_resampler_handle_t handle,
                                      const int16_t *input, int in_frames,
                                      int16_t *output, int *out_frames);

/**
 * @brief Free resampler resources.
 */
void speexdsp_resampler_deinit(speexdsp_resampler_handle_t handle);

#endif /* SPEEXDSP_RESAMPLER_H */
