#pragma once
#ifndef OTA_H
#define OTA_H

#include "esp_err.h"

/**
 * @brief Perform an A/B OTA update from the given HTTPS URL.
 *
 * Downloads firmware to the inactive OTA slot, verifies it,
 * marks it for boot, and reboots.
 *
 * This function does NOT return on success (it reboots).
 * On failure it returns an error code.
 *
 * @param url  Firmware binary URL (http:// or https://).
 */
esp_err_t ota_perform_update(const char *url);

/**
 * @brief Mark the current running app as valid (cancel rollback).
 *        Should be called after CARRADIO_GOOD_BOOT_MS of stable operation.
 *        Wrapper around esp_ota_mark_app_valid_cancel_rollback().
 */
esp_err_t ota_mark_valid(void);

/**
 * @brief Return the current firmware version string.
 */
const char *ota_get_app_version(void);

#endif
