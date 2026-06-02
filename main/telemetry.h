#pragma once
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Initialize telemetry: init log NVS partition, register device,
 *        start session on server, log boot event, start flush task.
 *        Call after WiFi connects and device_identity_init().
 */
esp_err_t telemetry_init(void);

/**
 * @brief Append a structured event to the in-memory ring and local flash log.
 *        Thread-safe. Non-blocking (drops oldest entry when ring is full).
 *
 * @param event_type   Short ASCII string, e.g. "state_transition".
 * @param payload_json JSON object string, e.g. {"from":0,"to":1}.
 *                     NULL or empty treated as {}.
 */
void telemetry_log(const char *event_type, const char *payload_json);

/**
 * @brief Enable or disable sending events to the backend.
 *        Local flash logging always continues regardless.
 */
void telemetry_set_enabled(bool enabled);
bool telemetry_is_enabled(void);

/**
 * @brief Hash a raw identifier with the per-device salt.
 *        sha256(salt+raw) first 16 bytes as lowercase hex (32 chars + NUL).
 *
 * @param raw     Input string (e.g. Bluetooth MAC "AA:BB:CC:DD:EE:FF").
 * @param out     Output buffer (>= 33 bytes).
 * @param outlen  Size of out.
 */
void telemetry_hash_id(const char *raw, char *out, size_t outlen);

/**
 * @brief Read up to max_events log entries from flash into buf (caller-allocated).
 *        Used by command_poll for upload_full_log.
 *        Returns number of entries read.
 */
int telemetry_log_read_all(char *buf, size_t buf_size);

#endif /* TELEMETRY_H */
