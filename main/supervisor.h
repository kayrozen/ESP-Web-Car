#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "avrcp.h"

typedef enum {
    AVRC_CMD_PLAY,
    AVRC_CMD_PAUSE,
    AVRC_CMD_STOP,
    AVRC_CMD_NEXT_STATION,
    AVRC_CMD_PREV_STATION,
} avrc_cmd_t;

esp_err_t supervisor_start(void);           /* create supervisor task; call after storage_init() */
void      supervisor_confirm_good_boot(void);  /* clears boot-fail counter + marks OTA valid */

uint32_t supervisor_backoff_next(uint32_t current_ms);  /* doubles, capped at BACKOFF_MAX */
uint32_t supervisor_backoff_reset(void);

void supervisor_request_reboot(void);
void supervisor_avrcp_command(avrc_cmd_t cmd);   /* safe to call from BT stack context */
playback_state_t supervisor_get_playback_state(void);
