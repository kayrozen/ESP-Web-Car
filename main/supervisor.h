#pragma once

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start the supervisor task.
 *        The supervisor owns the top-level state machine and drives
 *        PHASE1 → PHASE2 → READY transitions.
 *        Must be called from app_main after storage_init().
 */
esp_err_t supervisor_start(void);

/**
 * @brief Called by the streaming layer once audio has been flowing
 *        for CARRADIO_GOOD_BOOT_MS milliseconds without error.
 *        Clears the boot-fail counter and calls esp_ota_mark_app_valid.
 */
void supervisor_confirm_good_boot(void);

/**
 * @brief Compute next exponential-backoff delay (doubles each call, capped).
 * @param current_ms  Current backoff value in ms (0 → use CARRADIO_BACKOFF_INIT_MS).
 * @return New backoff delay in ms, capped at CARRADIO_BACKOFF_MAX_MS.
 */
uint32_t supervisor_backoff_next(uint32_t current_ms);

/**
 * @brief Reset backoff counter to initial value.
 */
uint32_t supervisor_backoff_reset(void);

/**
 * @brief Request supervisor to trigger a full reboot after saving state.
 */
void supervisor_request_reboot(void);

#endif /* SUPERVISOR_H */
