#pragma once

#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Maximum field lengths (including null terminator) */
#define STORAGE_SSID_MAX        33
#define STORAGE_PASS_MAX        65
#define STORAGE_URL_MAX         256
#define STORAGE_BT_NAME_MAX     64
#define STORAGE_BT_MAC_MAX      18   /* "AA:BB:CC:DD:EE:FF\0" */

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/**
 * @brief Open NVS handle and migrate config if version mismatch.
 *        Must be called after nvs_flash_init().
 */
esp_err_t storage_init(void);

/**
 * @brief Erase all keys in the carradio namespace and reset to phase 0.
 */
esp_err_t storage_erase_all(void);

/* ── Phase ───────────────────────────────────────────────────────────── */

uint8_t   storage_get_phase(void);
esp_err_t storage_set_phase(uint8_t phase);
esp_err_t storage_reset_phase(void);   /* sets phase = CARRADIO_PHASE_WIFI */

/* ── WiFi credentials ────────────────────────────────────────────────── */

esp_err_t storage_get_wifi_ssid(char *buf, size_t len);
esp_err_t storage_set_wifi_ssid(const char *ssid);

esp_err_t storage_get_wifi_pass(char *buf, size_t len);
esp_err_t storage_set_wifi_pass(const char *pass);

/* ── Stream URL (legacy — kept for backward compat) ──────────────────── */

esp_err_t storage_get_stream_url(char *buf, size_t len);
esp_err_t storage_set_stream_url(const char *url);

/* ── Device name ─────────────────────────────────────────────────────── */

#define STORAGE_DEVICE_NAME_MAX  25   /* max 24 chars + null */

esp_err_t storage_get_device_name(char *buf, size_t len);
esp_err_t storage_set_device_name(const char *name);

/**
 * @brief Returns true if both playlist_json and WiFi credentials exist in NVS.
 */
bool storage_is_phase_ready(void);

/* ── Bluetooth device ────────────────────────────────────────────────── */

esp_err_t storage_get_bt_name(char *buf, size_t len);
esp_err_t storage_set_bt_name(const char *name);

/**
 * @brief Bluetooth MAC as colon-separated string "AA:BB:CC:DD:EE:FF".
 */
esp_err_t storage_get_bt_mac(char *buf, size_t len);
esp_err_t storage_set_bt_mac(const char *mac_str);

/**
 * @brief Parse stored MAC string into 6-byte array.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if format invalid.
 */
esp_err_t storage_get_bt_mac_bytes(uint8_t mac[6]);

/* ── Boot-fail counter ───────────────────────────────────────────────── */

uint8_t   storage_get_boot_fail_count(void);
esp_err_t storage_set_boot_fail_count(uint8_t count);

/* ── Device identity & telemetry ─────────────────────────────────────── */

esp_err_t storage_get_device_id(char *buf, size_t len);
esp_err_t storage_set_device_id(const char *id);

esp_err_t storage_get_api_key(char *buf, size_t len);
esp_err_t storage_set_api_key(const char *key);

esp_err_t storage_get_tm_salt(char *buf, size_t len);
esp_err_t storage_set_tm_salt(const char *salt);

esp_err_t storage_get_api_base_url(char *buf, size_t len);
esp_err_t storage_set_api_base_url(const char *url);

/* 0 = disabled, 1 = enabled (default 1) */
esp_err_t storage_get_tm_enabled(uint8_t *out);
esp_err_t storage_set_tm_enabled(bool enabled);

/* ── Validation helpers ──────────────────────────────────────────────── */

/**
 * @brief Validate SSID: non-empty, <= 32 chars.
 */
bool storage_validate_ssid(const char *ssid);

/**
 * @brief Validate URL: starts with "http://" or "https://".
 */
bool storage_validate_url(const char *url);

/**
 * @brief Validate MAC string "AA:BB:CC:DD:EE:FF".
 */
bool storage_validate_mac(const char *mac_str);

#endif /* STORAGE_H */
