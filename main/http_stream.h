#pragma once
#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_AAC,
    AUDIO_FORMAT_HE_AAC,
} audio_format_t;

/**
 * @brief Start streaming from the given URL into raw_ringbuf.
 *        Resolves M3U/PLS playlists automatically.
 *        Detects audio format from Content-Type header and sync words.
 *        Reconnects with exponential backoff on error.
 *
 * This function creates an internal FreeRTOS task and returns immediately.
 * Call http_stream_stop() to halt the task.
 */
esp_err_t http_stream_start(const char *url);

/**
 * @brief Stop the streaming task and free resources.
 */
void http_stream_stop(void);

/**
 * @brief Returns the detected audio format (valid after first data arrives).
 */
audio_format_t http_stream_get_format(void);

/**
 * @brief Returns true if the stream task is currently running.
 */
bool http_stream_is_running(void);

/**
 * @brief Copy the current "now playing" title (artist - title or station name).
 *        Thread-safe. Returns false if nothing is available yet.
 */
bool http_stream_get_now_playing(char *buf, size_t len);

#endif
