#pragma once
#ifndef SPEEXDSP_RESAMPLER_H
#define SPEEXDSP_RESAMPLER_H

#include "esp_err.h"
#include <stdint.h>

typedef void *speexdsp_resampler_handle_t;

/**
 * @param channels   Number of channels (1 or 2)
 * @param in_rate    Input sample rate
 * @param out_rate   Output sample rate
 * @param quality    Quality level 0–10 (4 recommended)
 */
esp_err_t speexdsp_resampler_init(speexdsp_resampler_handle_t *out,
                                   int channels,
                                   uint32_t in_rate, uint32_t out_rate,
                                   int quality);

/**
 * @param in_frames   Input frames available
 * @param out_frames  In: output buffer capacity; Out: frames written
 */
esp_err_t speexdsp_resampler_process(speexdsp_resampler_handle_t h,
                                      const int16_t *input, int in_frames,
                                      int16_t *output, int *out_frames);

void speexdsp_resampler_deinit(speexdsp_resampler_handle_t h);

#endif
