#pragma once
#ifndef FAAD2_H
#define FAAD2_H

#include "esp_err.h"
#include <stdint.h>

typedef void *faad2_handle_t;

/**
 * @param disable_sbr  Set non-zero to disable SBR (HE-AAC high band).
 *                     Recommended for ESP32 CPU budget.
 */
faad2_handle_t faad2_init(int disable_sbr);
void           faad2_deinit(faad2_handle_t h);

esp_err_t faad2_decode_frame(faad2_handle_t h,
                              const uint8_t *input, int input_len,
                              int16_t *pcm_out, int *pcm_frames,
                              int *consumed);

#endif
