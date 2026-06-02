#pragma once
#ifndef RESAMPLE_H
#define RESAMPLE_H

#include "esp_err.h"
#include <stdint.h>

typedef void *resample_handle_t;

/**
 * @brief Initialise a SpeexDSP resampler.
 *
 * @param out_handle    Output handle.
 * @param out_rate      Target sample rate (e.g. 44100).
 * @param in_rate       Source sample rate (e.g. 44100, 48000, 22050).
 * @param in_channels   Input channel count (1 or 2).
 * @param out_channels  Output channel count (2 — mono→stereo duplication).
 */
esp_err_t resample_init(resample_handle_t *out_handle,
                         uint32_t out_rate, uint32_t in_rate,
                         int in_channels, int out_channels);

/**
 * @brief Resample a block of PCM audio.
 *
 * @param handle        Resampler handle.
 * @param input         Input samples (interleaved, int16).
 * @param in_frames     Number of input frames.
 * @param output        Output buffer.
 * @param out_frames    In: capacity in frames. Out: frames written.
 */
esp_err_t resample_process(resample_handle_t handle,
                            const int16_t *input, int in_frames,
                            int16_t *output, int *out_frames);

/**
 * @brief Free resampler resources.
 */
void resample_deinit(resample_handle_t handle);

#endif
