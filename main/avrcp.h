#pragma once
#ifndef AVRCP_H
#define AVRCP_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    PLAYBACK_STATE_PLAYING,
    PLAYBACK_STATE_SOFT_PAUSED,
    PLAYBACK_STATE_HARD_PAUSED,
} playback_state_t;

/**
 * @brief Initialise AVRCP TG. MUST be called before A2DP init.
 */
esp_err_t avrcp_init(void);

/**
 * @brief Publish current track metadata to the car's display.
 *        Pass NULL for any field to leave it unchanged.
 */
void avrcp_publish_metadata(const char *title, const char *artist, const char *genre);

/**
 * @brief Publish playback status change (updates car's play/pause icon).
 */
void avrcp_publish_playback_status(playback_state_t state);

#endif /* AVRCP_H */
