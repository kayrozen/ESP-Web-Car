#pragma once
#ifndef COMMAND_POLL_H
#define COMMAND_POLL_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Start the background command polling task.
 *        Call after WiFi connects and telemetry_init().
 *        Polls GET /api/v1/commands every COMMAND_POLL_INTERVAL_MS.
 */
esp_err_t command_poll_init(void);

/**
 * @brief Check if a force_ota_check command has been received.
 *        Copies the OTA URL into buf and clears the pending flag.
 * @return true if a URL was pending, false if not.
 */
bool command_poll_consume_ota_url(char *buf, size_t len);

#endif /* COMMAND_POLL_H */
