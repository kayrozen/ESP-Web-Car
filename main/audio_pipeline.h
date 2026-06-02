#pragma once
#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include "esp_err.h"
#include "freertos/ringbuf.h"
#include <stdbool.h>

/**
 * @brief Initialise PSRAM ring buffers and create decode/resample task.
 *        http_stream_start() is called internally.
 *
 * @param stream_url  The HTTP(S) URL to stream from.
 */
esp_err_t audio_pipeline_start(const char *stream_url);

/**
 * @brief Pause decoding (stops writing to pcm_ringbuf).
 */
void audio_pipeline_pause(void);

/**
 * @brief Resume decoding after a pause.
 */
void audio_pipeline_resume(void);

/**
 * @brief Stop pipeline and free resources.
 */
void audio_pipeline_stop(void);

/**
 * @brief Returns true if the pipeline is currently running.
 */
bool audio_pipeline_is_running(void);

/**
 * @brief Returns the raw (compressed) ring buffer handle.
 *        Used by http_stream to write incoming data.
 */
RingbufHandle_t audio_pipeline_get_raw_ringbuf(void);

/**
 * @brief Returns the PCM ring buffer handle.
 *        Used by bluetooth A2DP data callback.
 */
RingbufHandle_t audio_pipeline_get_pcm_ringbuf(void);

#endif
