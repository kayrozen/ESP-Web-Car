#pragma once

#ifndef PROVISIONING_SERIAL_H
#define PROVISIONING_SERIAL_H

#include <stdbool.h>

/**
 * @brief Wait up to 30 seconds on UART0 for a PROVISION: line.
 *
 * Parses the JSON payload, validates it, writes device_name and
 * playlist_json to NVS, then prints "OK\n".
 *
 * On validation/parse error prints "ERR:<reason>\n".
 * If nothing arrives within 30 seconds, returns false silently.
 *
 * Must be called very early in app_main, before WiFi/BT init.
 *
 * @return true  Provisioning data was received and written to NVS.
 * @return false Timeout or error — NVS unchanged.
 */
bool provisioning_serial_wait(void);

#endif /* PROVISIONING_SERIAL_H */
