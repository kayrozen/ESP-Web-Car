#pragma once
#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Maximum name length for a discovered BT device */
#define BT_DEVICE_NAME_MAX  64
#define BT_MAC_STR_MAX      18

typedef struct {
    char    name[BT_DEVICE_NAME_MAX];
    char    mac_str[BT_MAC_STR_MAX];
    uint8_t mac[6];
    int     rssi;
} bt_device_t;

/**
 * @brief Initialise BT controller + Bluedroid stack.
 *        Must be called before any other bluetooth_ function.
 *        Safe to call multiple times (idempotent).
 */
esp_err_t bluetooth_init(void);

/**
 * @brief Start A2DP source role and register data callback.
 */
esp_err_t bluetooth_a2dp_start(void);

/**
 * @brief Perform GAP discovery for CARRADIO_BT_SCAN_SECS seconds.
 *        Fills `out` with up to `max_count` devices.
 *        Blocks until scan is complete.
 */
esp_err_t bluetooth_gap_scan(bt_device_t *out, int max_count, int *out_count);

/**
 * @brief Connect A2DP to the device with the given MAC.
 */
esp_err_t bluetooth_connect(const uint8_t mac[6]);
esp_err_t bluetooth_a2dp_connect(const uint8_t mac[6]);

/**
 * @brief Returns true if A2DP is currently connected and streaming.
 */
bool bluetooth_is_connected(void);

/**
 * @brief Disconnect A2DP.
 */
void bluetooth_disconnect(void);

#endif
